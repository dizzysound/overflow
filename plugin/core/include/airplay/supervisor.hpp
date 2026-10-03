// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Runs overflow-helper: spawns it, writes hello, commands and media to its
// stdin from one writer thread, turns its stdout into events and its stderr
// into log lines, restarts it with backoff when it exits unexpectedly, and
// shuts it down cleanly (shutdown command, close stdin, wait, then kill).
#pragma once

#include "airplay/backoff.hpp"
#include "airplay/events.hpp"
#include "airplay/media_queue.hpp"
#include "airplay/process.hpp"
#include "airplay/settings.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace airplay {

enum class LogLevel { Debug, Info, Warning, Error };

struct HelperLaunch {
	std::string exe;
	std::string creds_path;
	std::string port_range = "60000-60099";
	int fps = 30;
	TimingMode timing = TimingMode::Auto;
	int target_latency_ms = 0;          // 0: omit -target-latency-ms
	std::string eld_encoder_path;       // empty: the helper's default, next to overflow-helper
	bool debug = false;
	std::vector<std::string> extra_args; // appended last (tests)
};

std::vector<std::string> helper_args(const HelperLaunch &launch);

enum class HelperStatus { Stopped, Starting, Running, Ready, Restarting, Failed };
const char *helper_status_name(HelperStatus status);

struct SupervisorCallbacks {
	std::function<void(const Event &)> on_event; // the stdout reader thread
	std::function<void(LogLevel, const std::string &)> on_log; // any thread, including the watchdog threads
	// The supervisor thread, AND the stdout reader thread (set_status(Ready)
	// runs there, right when the "ready" line is parsed). Invocations are
	// serialized under one mutex, so callers never see them concurrently or
	// out of order, but which thread calls in can vary from one call to the
	// next.
	std::function<void(HelperStatus, const std::string &)> on_status;
};

// Media older than this when it reaches the pipe is dropped (the helper
// rejects ages over 30 s; anything this old is useless for live playback).
constexpr uint64_t kMaxMediaAgeNs = 2000000000ull;

// A write to the child's stdin stuck this long means the helper stopped
// reading (a hung or wedged process): the always-on watchdog kills it so the
// supervisor can restart.
constexpr uint64_t kStuckWriteMs = 10000;

// Contract:
//  - start(), stop() and restart() must all be called from one thread (the
//    UI thread). They are not meant to be called concurrently with each
//    other.
//  - Never destroy a HelperSupervisor from inside one of its own callbacks
//    (on_event, on_log, on_status). The calling thread may be the supervisor
//    thread or one of the child process's own reader/wait threads, and the
//    destructor joins them; destroying from inside one of them is undefined.
//  - stop() IS safe to call from inside a callback: detected via a
//    thread-local flag, it then only sets the stop request and returns,
//    without blocking or joining anything. The owning thread must call
//    stop() again (or let the destructor run) once the callback has returned
//    to actually wait for and join the supervisor thread.
//  - restart() called from on_status(Stopped) or on_status(Failed) is
//    ignored: at that point the supervisor thread is already tearing itself
//    down and will not act on it.
class HelperSupervisor {
public:
	HelperSupervisor(SupervisorCallbacks callbacks, std::function<uint64_t()> now_ns,
			 std::vector<std::chrono::milliseconds> backoff_steps = default_helper_backoff(),
			 MediaQueueLimits limits = {}, uint64_t stuck_write_ms = kStuckWriteMs);
	~HelperSupervisor();
	HelperSupervisor(const HelperSupervisor &) = delete;
	HelperSupervisor &operator=(const HelperSupervisor &) = delete;

	// Starts the supervisor thread. No-op while it is already running.
	void start(const HelperLaunch &launch);
	// Stops the helper (shutdown, close stdin, wait up to grace, then kill)
	// and joins the supervisor thread. Blocks the caller for at most about
	// grace + 3 s. Safe to call from inside a callback; see the contract
	// above.
	void stop(std::chrono::milliseconds grace = std::chrono::seconds(10));
	// Replaces the helper with one started from launch, without blocking the
	// caller. Starts it if the supervisor is not running.
	void restart(const HelperLaunch &launch);

	// Queues a command for the current helper. Commands queued before a
	// (re)spawn are dropped; resend state when the helper reports ready.
	// Dropped outright (never queued) while no helper is running or coming
	// up (status Stopped or Failed): nothing would ever drain the queue, and
	// the controller resends the current selection on every `ready` anyway.
	void send_command(std::string json);

	MediaQueue &media() { return media_; }
	HelperStatus status() const { return status_.load(); }
	bool ready() const { return ready_.load(); }
	uint64_t spawn_count() const { return spawn_count_.load(); }
	// Test-only: how many commands are queued for the next drain, so tests
	// can confirm send_command() drops rather than accumulates them while
	// stopped or failed.
	size_t queued_command_count() const;

private:
	void run(HelperLaunch launch);
	void session(ChildProcess &child, const std::atomic<bool> &exited);
	void shutdown_child(ChildProcess &child, std::chrono::milliseconds grace);
	void handle_stdout_line(const std::string &line);
	void set_status(HelperStatus status, const std::string &detail);
	// Transitions status_ from expected to status, and calls on_status, only
	// if status_ still equals expected at that instant (a compare-exchange).
	// Used for Starting -> Running so a Ready that arrives concurrently, on
	// the stdout reader thread, can never be clobbered back to Running.
	void set_status_if(HelperStatus expected, HelperStatus status, const std::string &detail);
	void log(LogLevel level, const std::string &message);
	std::chrono::milliseconds grace() const;
	// Writes frame to child's stdin, recording write_since_ns_ for the
	// stuck-write watchdog around the (possibly blocking) write.
	bool write_watched(ChildProcess &child, const std::vector<uint8_t> &frame);
	// Wakes about every 500 ms for the life of the supervisor and kills
	// current_ if a write has been stuck for stuck_write_ms_.
	void watchdog_loop();

	SupervisorCallbacks callbacks_;
	std::function<uint64_t()> now_ns_;
	std::vector<std::chrono::milliseconds> backoff_steps_;
	MediaQueue media_;
	uint64_t stuck_write_ms_;

	mutable std::mutex mu_; // guards thread_, current_, pending_launch_, grace_, run_done_
	std::condition_variable stop_cv_;
	std::condition_variable done_cv_;
	std::thread thread_;
	ChildProcess *current_ = nullptr;
	HelperLaunch pending_launch_;
	std::chrono::milliseconds grace_{std::chrono::seconds(10)};
	bool run_done_ = false;
	std::atomic<bool> stop_requested_{false};
	std::atomic<bool> restart_requested_{false};

	mutable std::mutex cmd_mu_;
	std::deque<std::string> commands_;

	// Serializes on_status invocations (never held while acquiring mu_; if
	// both are ever needed, take this one first, mu_ second, never the
	// other way around).
	std::mutex status_mu_;
	std::atomic<HelperStatus> status_{HelperStatus::Stopped};
	std::atomic<bool> ready_{false};
	std::atomic<uint64_t> spawn_count_{0};

	// 0: no write to the child's stdin is in progress. Set (to now_ns_())
	// just before write_all, cleared just after, on the supervisor thread.
	std::atomic<uint64_t> write_since_ns_{0};
	std::atomic<bool> watchdog_fired_{false}; // one warning/kill per stuck write
	std::thread watchdog_thread_;
	std::mutex watchdog_mu_;
	std::condition_variable watchdog_cv_;
	bool watchdog_stop_ = false;
};

} // namespace airplay
