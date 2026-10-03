// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// _GNU_SOURCE must be defined before any include: glibc only exposes pipe2()
// under it, and pipe2 is what makes CLOEXEC atomic on Linux (see make_pipe
// below). g++'s -std=gnu++17 already defines it on the command line, so
// guard the definition to avoid a "redefined" warning under -Werror.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

// POSIX child process (macOS tests and the macOS plugin build; also Linux
// core-tests in CI, since this file serves both).
#include "airplay/process.hpp"

#include <cerrno>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef __APPLE__
#include <crt_externs.h> // bundles such as an OBS plugin cannot reference environ directly
#define AIRPLAY_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define AIRPLAY_ENVIRON environ
#endif

namespace airplay {
namespace {

#ifdef __linux__
// pipe2(..., O_CLOEXEC) sets close-on-exec atomically, so another OBS thread
// spawning a child between pipe() and a separate fcntl() call can never leak
// one of these descriptors into it.
bool make_pipe(int fds[2], std::string *error)
{
	if (::pipe2(fds, O_CLOEXEC) != 0) {
		if (error)
			*error = std::string("pipe2: ") + std::strerror(errno);
		return false;
	}
	return true;
}
#else
void set_cloexec(int fd)
{
	fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

bool make_pipe(int fds[2], std::string *error)
{
	if (::pipe(fds) != 0) {
		if (error)
			*error = std::string("pipe: ") + std::strerror(errno);
		return false;
	}
	set_cloexec(fds[0]);
	set_cloexec(fds[1]);
	return true;
}
#endif

// posix_spawn_file_actions_adddup2 mirrors dup2, and dup2 is documented to do
// nothing when source and destination are equal. Since make_pipe's fds are
// always created CLOEXEC, a no-op dup2 would leave that flag set across
// exec and silently close the child's stdio. Move a child-end fd above 2
// first if it already happens to sit at 0, 1 or 2 (fd 0/1/2 having been
// closed by something else before this pipe() call).
bool ensure_above_stdio(int *fd, std::string *error)
{
	if (*fd > 2)
		return true;
	const int moved = fcntl(*fd, F_DUPFD_CLOEXEC, 3);
	if (moved < 0) {
		if (error)
			*error = std::string("fcntl F_DUPFD_CLOEXEC: ") + std::strerror(errno);
		return false;
	}
	::close(*fd);
	*fd = moved;
	return true;
}

void pump(int fd, const std::function<void(const char *, size_t)> &fn)
{
	char buf[8192];
	for (;;) {
		const ssize_t n = ::read(fd, buf, sizeof(buf));
		if (n > 0) {
			if (fn)
				fn(buf, static_cast<size_t>(n));
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		break;
	}
	::close(fd);
}

class PosixProcess final : public ChildProcess {
public:
	PosixProcess(pid_t pid, int in_fd, int out_fd, int err_fd, ProcessCallbacks callbacks)
		: pid_(pid), in_fd_(in_fd), callbacks_(std::move(callbacks))
	{
		out_thread_ = std::thread([this, out_fd] { pump(out_fd, callbacks_.on_stdout); });
		err_thread_ = std::thread([this, err_fd] { pump(err_fd, callbacks_.on_stderr); });
		wait_thread_ = std::thread([this] { wait_loop(); });
	}

	~PosixProcess() override
	{
		kill();
		if (wait_thread_.joinable())
			wait_thread_.join();
		close_stdin();
	}

	bool write_all(const uint8_t *data, size_t size) override
	{
		if (in_fd_ < 0)
			return false;
#ifndef F_SETNOSIGPIPE
		// No per-descriptor SIGPIPE switch here: block it on this thread
		// so a dead helper gives EPIPE instead of killing OBS. This is the
		// dedicated plugin writer thread, so blocking it here is safe and
		// does not affect signal delivery anywhere else.
		static thread_local bool blocked = false;
		if (!blocked) {
			sigset_t set;
			sigemptyset(&set);
			sigaddset(&set, SIGPIPE);
			pthread_sigmask(SIG_BLOCK, &set, nullptr);
			blocked = true;
		}
#endif
		while (size > 0) {
			const ssize_t n = ::write(in_fd_, data, size);
			if (n > 0) {
				data += n;
				size -= static_cast<size_t>(n);
				continue;
			}
			if (n < 0 && errno == EINTR)
				continue;
			return false;
		}
		return true;
	}

	void close_stdin() override
	{
		if (in_fd_ >= 0) {
			::close(in_fd_);
			in_fd_ = -1;
		}
	}

	bool wait_for_exit(std::chrono::milliseconds timeout) override
	{
		std::unique_lock<std::mutex> lock(mu_);
		return cv_.wait_for(lock, timeout, [this] { return exited_; });
	}

	void kill() override
	{
		// reaped_ is set to true inside this same lock, in wait_loop, at
		// the point pid_ is actually reaped (waitpid), so this check and
		// the signal below can never race with the PID being freed for
		// reuse by the kernel.
		std::lock_guard<std::mutex> lock(mu_);
		if (!reaped_) // never signal a PID that may have been reused
			::kill(pid_, SIGKILL);
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
		// First wait WITHOUT reaping (WNOWAIT): this blocks until the
		// child exits but leaves it a zombie, so its PID cannot yet be
		// recycled. Only once we are ready to actually reap it, under
		// the same lock kill() takes, do we call waitpid and free the
		// PID. That closes the race where the child was reaped (making
		// its PID reusable) before reaped_ was set to true, during which
		// kill() could see !reaped_ and signal a PID the kernel had
		// already handed to an unrelated process.
		siginfo_t info;
		int wr;
		do {
			wr = ::waitid(P_PID, pid_, &info, WEXITED | WNOWAIT);
		} while (wr < 0 && errno == EINTR);

		int status = 0;
		pid_t r = -1;
		{
			std::lock_guard<std::mutex> lock(mu_);
			do {
				r = ::waitpid(pid_, &status, 0);
			} while (r < 0 && errno == EINTR);
			reaped_ = true;
		}
		int code = -1;
		if (r == pid_) {
			if (WIFEXITED(status))
				code = WEXITSTATUS(status);
			else if (WIFSIGNALED(status))
				code = 128 + WTERMSIG(status);
		}
		out_thread_.join();
		err_thread_.join();
		// on_exit runs before exited_ is published: a thread woken from
		// wait_for_exit() must see the callback as already having run,
		// not race it.
		if (callbacks_.on_exit)
			callbacks_.on_exit(code);
		{
			std::lock_guard<std::mutex> lock(mu_);
			exit_code_ = code;
			exited_ = true;
		}
		cv_.notify_all();
	}

	pid_t pid_;
	int in_fd_;
	ProcessCallbacks callbacks_;
	mutable std::mutex mu_;
	std::condition_variable cv_;
	bool reaped_ = false;
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
	int in[2], out[2], err[2];
	if (!make_pipe(in, error))
		return nullptr;
	if (!make_pipe(out, error)) {
		::close(in[0]);
		::close(in[1]);
		return nullptr;
	}
	if (!make_pipe(err, error)) {
		for (int fd : {in[0], in[1], out[0], out[1]})
			::close(fd);
		return nullptr;
	}

	auto close_pipes = [&] {
		for (int fd : {in[0], in[1], out[0], out[1], err[0], err[1]})
			::close(fd);
	};
	if (!ensure_above_stdio(&in[0], error) || !ensure_above_stdio(&out[1], error) ||
	    !ensure_above_stdio(&err[1], error)) {
		close_pipes();
		return nullptr;
	}

	posix_spawn_file_actions_t actions;
	bool actions_ok = false;
	posix_spawnattr_t attr;
	bool attr_ok = false;
	auto fail = [&](const char *what, int rc) -> std::unique_ptr<ChildProcess> {
		if (error)
			*error = std::string(what) + ": " + std::strerror(rc);
		if (attr_ok)
			posix_spawnattr_destroy(&attr);
		if (actions_ok)
			posix_spawn_file_actions_destroy(&actions);
		close_pipes();
		return nullptr;
	};

	int rc = posix_spawn_file_actions_init(&actions);
	if (rc != 0)
		return fail("posix_spawn_file_actions_init", rc);
	actions_ok = true;
	if ((rc = posix_spawn_file_actions_adddup2(&actions, in[0], 0)) != 0)
		return fail("posix_spawn_file_actions_adddup2 stdin", rc);
	if ((rc = posix_spawn_file_actions_adddup2(&actions, out[1], 1)) != 0)
		return fail("posix_spawn_file_actions_adddup2 stdout", rc);
	if ((rc = posix_spawn_file_actions_adddup2(&actions, err[1], 2)) != 0)
		return fail("posix_spawn_file_actions_adddup2 stderr", rc);

	rc = posix_spawnattr_init(&attr);
	if (rc != 0)
		return fail("posix_spawnattr_init", rc);
	attr_ok = true;

	// The writer thread (write_all, above) blocks SIGPIPE on itself so a
	// dead helper reports EPIPE instead of killing OBS. A blocked signal is
	// inherited across posix_spawn, so without SETSIGMASK/SETSIGDEF the
	// child would start with SIGPIPE blocked too and could never be killed
	// by it.
	int flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef POSIX_SPAWN_CLOEXEC_DEFAULT
	flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
	if ((rc = posix_spawnattr_setflags(&attr, static_cast<short>(flags))) != 0)
		return fail("posix_spawnattr_setflags", rc);
	sigset_t empty_mask;
	sigemptyset(&empty_mask);
	if ((rc = posix_spawnattr_setsigmask(&attr, &empty_mask)) != 0)
		return fail("posix_spawnattr_setsigmask", rc);
	sigset_t default_sigs;
	sigemptyset(&default_sigs);
	sigaddset(&default_sigs, SIGPIPE);
	if ((rc = posix_spawnattr_setsigdefault(&attr, &default_sigs)) != 0)
		return fail("posix_spawnattr_setsigdefault", rc);

	std::vector<std::string> argv_storage;
	argv_storage.push_back(exe);
	argv_storage.insert(argv_storage.end(), args.begin(), args.end());
	std::vector<char *> argv;
	for (std::string &s : argv_storage)
		argv.push_back(s.data());
	argv.push_back(nullptr);

	pid_t pid = 0;
	rc = posix_spawn(&pid, exe.c_str(), &actions, &attr, argv.data(), AIRPLAY_ENVIRON);
	posix_spawnattr_destroy(&attr);
	posix_spawn_file_actions_destroy(&actions);
	::close(in[0]);
	::close(out[1]);
	::close(err[1]);
	if (rc != 0) {
		if (error)
			*error = "posix_spawn " + exe + ": " + std::strerror(rc);
		::close(in[1]);
		::close(out[0]);
		::close(err[0]);
		return nullptr;
	}
#ifdef F_SETNOSIGPIPE
	fcntl(in[1], F_SETNOSIGPIPE, 1);
#endif
	return std::make_unique<PosixProcess>(pid, in[1], out[0], err[0], std::move(callbacks));
}

} // namespace airplay
