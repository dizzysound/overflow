// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// A child process with three pipes. Its stdout and stderr are drained
// continuously on two dedicated threads from spawn to exit (docs/protocol.md,
// Process contract).
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace airplay {

struct ProcessCallbacks {
	// Called on the stdout or stderr reader thread with each chunk read.
	// Must not destroy the ChildProcess: that would join the very thread
	// calling this callback and deadlock.
	std::function<void(const char *data, size_t size)> on_stdout;
	std::function<void(const char *data, size_t size)> on_stderr;
	// Called once, on a background thread, after the process has exited and
	// both pipes have reached end of file. Must not destroy the ChildProcess
	// for the same reason: this callback runs on the thread that join()s in
	// the destructor.
	std::function<void(int exit_code)> on_exit;
};

class ChildProcess {
public:
	// Kills the process if it is still running, then joins its threads.
	virtual ~ChildProcess() = default;

	// Blocking write to the child's stdin. Call write_all and close_stdin
	// from one thread only. Returns false once the pipe is broken or closed.
	virtual bool write_all(const uint8_t *data, size_t size) = 0;
	virtual void close_stdin() = 0;

	// True once the process has exited and its pipes are drained.
	virtual bool wait_for_exit(std::chrono::milliseconds timeout) = 0;
	// Kills the process (and, on Windows, its whole Job Object, so a
	// descendant such as eld-encoder dies too) if it has not already
	// exited. exit_code() reports 137 for a process ended this way.
	virtual void kill() = 0;
	virtual bool exited() const = 0;
	// -1 until exited, or if the exit code could not be determined. 137
	// means the process was ended via kill(). Any other negative value is a
	// Windows crash exit code (an NTSTATUS such as 0xC0000005, reinterpreted
	// as a signed 32-bit int); it is reported as-is, not clamped.
	virtual int exit_code() const = 0;
};

// Starts exe with args. On Windows: CREATE_NO_WINDOW, parent pipe ends not
// inheritable, only the three child ends inherited, and the child placed in a
// Job Object with JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE before it runs. Returns
// nullptr and sets *error on failure. *error is written only when it is
// non-null.
std::unique_ptr<ChildProcess> spawn_process(const std::string &exe, const std::vector<std::string> &args,
					    ProcessCallbacks callbacks, std::string *error);

// Quoting for a Windows command line (the CommandLineToArgvW rules).
std::string quote_windows_arg(const std::string &arg);
std::string windows_command_line(const std::string &exe, const std::vector<std::string> &args);

} // namespace airplay
