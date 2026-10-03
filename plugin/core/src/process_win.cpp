// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Windows child process: CreateProcessW with three anonymous pipes, a Job
// Object with KILL_ON_JOB_CLOSE, and CREATE_NO_WINDOW (docs/protocol.md,
// Orphan protection). libobs's os_process_pipe_create is one-directional, so
// it cannot be used here (research notes, section 7).
#include "airplay/process.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace airplay {
namespace {

std::wstring widen(const std::string &s)
{
	if (s.empty())
		return std::wstring();
	const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
	std::wstring w(static_cast<size_t>(n), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
	return w;
}

std::string win_error(const char *what)
{
	return std::string(what) + " failed (Windows error " + std::to_string(GetLastError()) + ")";
}

void pump(HANDLE h, const std::function<void(const char *, size_t)> &fn)
{
	char buf[8192];
	for (;;) {
		DWORD n = 0;
		if (!ReadFile(h, buf, sizeof(buf), &n, nullptr) || n == 0)
			break; // ERROR_BROKEN_PIPE: the child closed its end
		if (fn)
			fn(buf, static_cast<size_t>(n));
	}
	CloseHandle(h);
}

class WinProcess final : public ChildProcess {
public:
	WinProcess(HANDLE process, HANDLE job, HANDLE in_w, HANDLE out_r, HANDLE err_r, ProcessCallbacks callbacks)
		: process_(process), job_(job), in_w_(in_w), callbacks_(std::move(callbacks))
	{
		out_thread_ = std::thread([this, out_r] { pump(out_r, callbacks_.on_stdout); });
		err_thread_ = std::thread([this, err_r] { pump(err_r, callbacks_.on_stderr); });
		wait_thread_ = std::thread([this] { wait_loop(); });
	}

	~WinProcess() override
	{
		kill();
		if (wait_thread_.joinable())
			wait_thread_.join();
		close_stdin();
		CloseHandle(process_);
		CloseHandle(job_); // kills anything left in the job (eld-encoder)
	}

	bool write_all(const uint8_t *data, size_t size) override
	{
		if (!in_w_)
			return false;
		while (size > 0) {
			const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size, 1u << 20));
			DWORD written = 0;
			if (!WriteFile(in_w_, data, chunk, &written, nullptr))
				return false; // ERROR_NO_DATA / ERROR_BROKEN_PIPE: the child is gone
			data += written;
			size -= written;
		}
		return true;
	}

	void close_stdin() override
	{
		if (in_w_) {
			CloseHandle(in_w_);
			in_w_ = nullptr;
		}
	}

	bool wait_for_exit(std::chrono::milliseconds timeout) override
	{
		std::unique_lock<std::mutex> lock(mu_);
		return cv_.wait_for(lock, timeout, [this] { return exited_; });
	}

	void kill() override
	{
		// The process handle stays open until the destructor, so the PID
		// cannot be reused under us. Terminate the whole Job Object, not
		// just the process: a descendant (eld-encoder) can be holding one
		// of our pipe handles, which would otherwise leave the reader
		// threads blocked until the job closes at destruction and hang
		// the destructor. 137 matches the POSIX SIGKILL exit code
		// (128 + SIGKILL) so callers see one "killed" convention on both
		// platforms.
		if (WaitForSingleObject(process_, 0) == WAIT_TIMEOUT) {
			if (job_)
				TerminateJobObject(job_, 137);
			else
				TerminateProcess(process_, 137);
		}
	}

	bool exited() const override
	{
		std::lock_guard<std::mutex> lock(mu_);
		return exited_;
	}

	int exit_code() const override
	{
		std::lock_guard<std::mutex> lock(mu_);
		return exit_code_;
	}

private:
	void wait_loop()
	{
		WaitForSingleObject(process_, INFINITE);
		DWORD code = 0;
		const int exit_code = GetExitCodeProcess(process_, &code) ? static_cast<int>(code) : -1;
		out_thread_.join();
		err_thread_.join();
		// on_exit runs before exited_ is published: a thread woken from
		// wait_for_exit() must see the callback as already having run,
		// not race it.
		if (callbacks_.on_exit)
			callbacks_.on_exit(exit_code);
		{
			std::lock_guard<std::mutex> lock(mu_);
			exit_code_ = exit_code;
			exited_ = true;
		}
		cv_.notify_all();
	}

	HANDLE process_;
	HANDLE job_;
	HANDLE in_w_;
	ProcessCallbacks callbacks_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	bool exited_ = false;
	int exit_code_ = -1;
	std::thread out_thread_;
	std::thread err_thread_;
	std::thread wait_thread_;
};

} // namespace

std::unique_ptr<ChildProcess> spawn_process(const std::string &exe, const std::vector<std::string> &args,
					    ProcessCallbacks callbacks, std::string *error)
{
	// Every pipe handle is created non-inheritable. A handle that starts
	// inheritable and is only demoted afterward leaves a window where an
	// unrelated CreateProcess call on another OBS thread (default
	// inheritance, no PROC_THREAD_ATTRIBUTE_HANDLE_LIST) could inherit our
	// parent-side handles. Only the three child ends are marked inheritable
	// below, and only immediately before CreateProcessW.
	SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, FALSE};
	HANDLE in_r = nullptr, in_w = nullptr, out_r = nullptr, out_w = nullptr, err_r = nullptr, err_w = nullptr;
	auto close_all = [&] {
		for (HANDLE *h : {&in_r, &in_w, &out_r, &out_w, &err_r, &err_w}) {
			if (*h) {
				CloseHandle(*h);
				*h = nullptr;
			}
		}
	};
	if (!CreatePipe(&in_r, &in_w, &sa, 0) || !CreatePipe(&out_r, &out_w, &sa, 0) ||
	    !CreatePipe(&err_r, &err_w, &sa, 0)) {
		if (error)
			*error = win_error("CreatePipe");
		close_all();
		return nullptr;
	}

	// Inherit only the three child ends, not every inheritable handle OBS has.
	SIZE_T attr_size = 0;
	InitializeProcThreadAttributeList(nullptr, 1, 0, &attr_size);
	std::vector<unsigned char> attr_buf(attr_size);
	auto *attrs = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attr_buf.data());
	if (!InitializeProcThreadAttributeList(attrs, 1, 0, &attr_size)) {
		if (error)
			*error = win_error("InitializeProcThreadAttributeList");
		close_all();
		return nullptr;
	}
	HANDLE inherit[3] = {in_r, out_w, err_w};
	if (!UpdateProcThreadAttribute(attrs, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherit, sizeof(inherit), nullptr,
				       nullptr)) {
		if (error)
			*error = win_error("UpdateProcThreadAttribute");
		DeleteProcThreadAttributeList(attrs);
		close_all();
		return nullptr;
	}

	STARTUPINFOEXW si{};
	si.StartupInfo.cb = sizeof(si);
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	si.StartupInfo.hStdInput = in_r;
	si.StartupInfo.hStdOutput = out_w;
	si.StartupInfo.hStdError = err_w;
	si.lpAttributeList = attrs;

	// Mark only the three child ends inheritable, right before spawning, so
	// the inheritable window is as short as CreateProcessW itself.
	SetHandleInformation(in_r, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(out_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);
	SetHandleInformation(err_w, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT);

	const std::wstring app = widen(exe);
	std::wstring cmd = widen(windows_command_line(exe, args));
	PROCESS_INFORMATION pi{};
	const BOOL created = CreateProcessW(app.c_str(), cmd.data(), nullptr, nullptr, TRUE,
					    CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT, nullptr,
					    nullptr, &si.StartupInfo, &pi);
	if (!created && error)
		*error = win_error(("CreateProcessW " + exe).c_str());
	DeleteProcThreadAttributeList(attrs);
	CloseHandle(in_r);
	CloseHandle(out_w);
	CloseHandle(err_w);
	in_r = out_w = err_w = nullptr;
	if (!created) {
		close_all();
		return nullptr;
	}

	// Assign the job before the helper runs, so eld-encoder (its child) is in it too.
	HANDLE job = CreateJobObjectW(nullptr, nullptr);
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!job || !SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)) ||
	    !AssignProcessToJobObject(job, pi.hProcess)) {
		if (error)
			*error = win_error("Job Object setup");
		TerminateProcess(pi.hProcess, 1);
		CloseHandle(pi.hThread);
		CloseHandle(pi.hProcess);
		if (job)
			CloseHandle(job);
		close_all();
		return nullptr;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	return std::make_unique<WinProcess>(pi.hProcess, job, in_w, out_r, err_r, std::move(callbacks));
}

} // namespace airplay
