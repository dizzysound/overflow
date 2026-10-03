// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/supervisor.hpp"

#include "airplay/commands.hpp"
#include "airplay/frame.hpp"

namespace airplay {

namespace {

// Set around every user-callback invocation (on_event, on_log, on_status), on
// whichever thread is making that call. stop() checks this to tell whether it
// is being called back from inside one of its own callbacks -- which may be
// the supervisor thread itself, or one of the child process's reader/wait
// threads -- and if so avoids blocking or joining (see the header contract).
thread_local bool g_in_callback = false;

struct CallbackGuard {
	CallbackGuard() { g_in_callback = true; }
	~CallbackGuard() { g_in_callback = false; }
	CallbackGuard(const CallbackGuard &) = delete;
};

} // namespace

std::vector<std::string> helper_args(const HelperLaunch &launch)
{
	std::vector<std::string> args{"-creds", launch.creds_path, "-port-range", launch.port_range, "-fps",
				      std::to_string(launch.fps)};
	if (launch.timing == TimingMode::Ntp) {
		args.push_back("-timing");
		args.push_back("ntp");
	}
	if (launch.target_latency_ms > 0) {
		args.push_back("-target-latency-ms");
		args.push_back(std::to_string(launch.target_latency_ms));
	}
	if (!launch.eld_encoder_path.empty()) {
		args.push_back("-eld-encoder");
		args.push_back(launch.eld_encoder_path);
	}
	if (launch.debug)
		args.push_back("-debug");
	args.insert(args.end(), launch.extra_args.begin(), launch.extra_args.end());
	return args;
}

const char *helper_status_name(HelperStatus status)
{
	switch (status) {
	case HelperStatus::Stopped:
		return "stopped";
	case HelperStatus::Starting:
		return "starting";
	case HelperStatus::Running:
		return "running";
	case HelperStatus::Ready:
		return "ready";
	case HelperStatus::Restarting:
		return "restarting";
	case HelperStatus::Failed:
		break;
	}
	return "failed";
}

HelperSupervisor::HelperSupervisor(SupervisorCallbacks callbacks, std::function<uint64_t()> now_ns,
				   std::vector<std::chrono::milliseconds> backoff_steps, MediaQueueLimits limits,
				   uint64_t stuck_write_ms)
	: callbacks_(std::move(callbacks)),
	  now_ns_(std::move(now_ns)),
	  backoff_steps_(std::move(backoff_steps)),
	  media_(limits),
	  stuck_write_ms_(stuck_write_ms)
{
	watchdog_thread_ = std::thread([this] { watchdog_loop(); });
}

HelperSupervisor::~HelperSupervisor()
{
	stop();
	{
		std::lock_guard<std::mutex> lock(watchdog_mu_);
		watchdog_stop_ = true;
	}
	watchdog_cv_.notify_all();
	watchdog_thread_.join();
}

void HelperSupervisor::start(const HelperLaunch &launch)
{
	std::unique_lock<std::mutex> lock(mu_);
	if (thread_.joinable()) {
		if (!run_done_)
			return;
		lock.unlock(); // a previous run ended (Failed); reap it
		thread_.join();
		lock.lock();
	}
	run_done_ = false;
	stop_requested_ = false;
	restart_requested_ = false;
	thread_ = std::thread([this, launch] { run(launch); });
}

void HelperSupervisor::stop(std::chrono::milliseconds grace)
{
	{
		std::lock_guard<std::mutex> lock(mu_);
		if (!thread_.joinable())
			return;
		grace_ = grace;
	}
	stop_requested_ = true;
	stop_cv_.notify_all();
	media_.notify();
	if (g_in_callback)
		// Called back from inside one of our own callbacks: that thread may
		// be the supervisor thread itself, or one of the child's reader/wait
		// threads, so waiting here for run_done_ or joining thread_ could be
		// a self-join or a deadlock (this thread may be one the supervisor
		// thread needs to finish before run_done_ can become true). Only the
		// request is made; the owning thread's later stop() call, or the
		// destructor, performs the actual wait and join once this callback
		// has returned.
		return;
	std::unique_lock<std::mutex> lock(mu_);
	// Backstop: a write stuck on a helper that stopped reading only returns
	// once the process dies (the always-on watchdog also kills it, but this
	// covers the case where grace is shorter than the watchdog's threshold).
	if (!done_cv_.wait_for(lock, grace + std::chrono::seconds(3), [this] { return run_done_; }) && current_)
		current_->kill();
	lock.unlock();
	thread_.join();
	lock.lock();
	stop_requested_ = false;
	run_done_ = false;
}

void HelperSupervisor::restart(const HelperLaunch &launch)
{
	std::unique_lock<std::mutex> lock(mu_);
	if (!thread_.joinable() || run_done_) {
		lock.unlock();
		start(launch);
		return;
	}
	pending_launch_ = launch;
	restart_requested_ = true;
	lock.unlock();
	stop_cv_.notify_all();
	media_.notify();
}

void HelperSupervisor::send_command(std::string json)
{
	// Minor 6: while nothing is running (or coming up will never drain this),
	// drop it instead of growing the queue unboundedly; the controller
	// resends the current selection on every `ready`, so nothing is lost.
	const HelperStatus s = status_.load();
	if (s == HelperStatus::Stopped || s == HelperStatus::Failed)
		return;
	{
		std::lock_guard<std::mutex> lock(cmd_mu_);
		commands_.push_back(std::move(json));
	}
	media_.notify();
}

size_t HelperSupervisor::queued_command_count() const
{
	std::lock_guard<std::mutex> lock(cmd_mu_);
	return commands_.size();
}

std::chrono::milliseconds HelperSupervisor::grace() const
{
	std::lock_guard<std::mutex> lock(mu_);
	return grace_;
}

void HelperSupervisor::log(LogLevel level, const std::string &message)
{
	if (callbacks_.on_log) {
		CallbackGuard guard;
		callbacks_.on_log(level, message);
	}
}

void HelperSupervisor::set_status(HelperStatus status, const std::string &detail)
{
	status_ = status;
	std::lock_guard<std::mutex> lock(status_mu_);
	if (callbacks_.on_status) {
		CallbackGuard guard;
		callbacks_.on_status(status, detail);
	}
}

void HelperSupervisor::set_status_if(HelperStatus expected, HelperStatus status, const std::string &detail)
{
	HelperStatus current = expected;
	if (!status_.compare_exchange_strong(current, status))
		return; // status_ already moved on (e.g. to Ready); don't clobber it
	std::lock_guard<std::mutex> lock(status_mu_);
	if (callbacks_.on_status) {
		CallbackGuard guard;
		callbacks_.on_status(status, detail);
	}
}

void HelperSupervisor::handle_stdout_line(const std::string &line)
{
	if (line.empty())
		return;
	const Event event = parse_event(line);
	if (std::holds_alternative<ReadyEvent>(event)) {
		ready_ = true;
		set_status(HelperStatus::Ready, "");
	} else if (const auto *fatal = std::get_if<FatalEvent>(&event)) {
		log(LogLevel::Error, "overflow-helper fatal error: " + fatal->error);
	}
	if (callbacks_.on_event) {
		CallbackGuard guard;
		callbacks_.on_event(event);
	}
}

bool HelperSupervisor::write_watched(ChildProcess &child, const std::vector<uint8_t> &frame)
{
	watchdog_fired_ = false;
	write_since_ns_ = now_ns_();
	const bool ok = child.write_all(frame.data(), frame.size());
	write_since_ns_ = 0;
	return ok;
}

void HelperSupervisor::watchdog_loop()
{
	std::unique_lock<std::mutex> lock(watchdog_mu_);
	for (;;) {
		if (watchdog_cv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return watchdog_stop_; }))
			return;
		lock.unlock();
		const uint64_t since = write_since_ns_.load();
		if (since != 0) {
			const uint64_t now = now_ns_();
			if (now > since && (now - since) / 1000000ull >= stuck_write_ms_ && !watchdog_fired_.exchange(true)) {
				log(LogLevel::Warning, "overflow-helper stopped reading its input; restarting it");
				std::lock_guard<std::mutex> child_lock(mu_);
				if (current_)
					current_->kill();
			}
		}
		lock.lock();
	}
}

void HelperSupervisor::run(HelperLaunch launch)
{
	Backoff backoff(backoff_steps_);
	for (;;) {
		if (stop_requested_)
			break;
		if (restart_requested_.exchange(false)) {
			std::lock_guard<std::mutex> lock(mu_);
			launch = pending_launch_;
			backoff.reset(); // a restart arriving during the backoff wait starts fresh
		}

		set_status(HelperStatus::Starting, launch.exe);
		{
			// Stale commands and media belong to the previous helper.
			std::lock_guard<std::mutex> lock(cmd_mu_);
			commands_.clear();
		}
		media_.reset();
		ready_ = false;

		std::atomic<bool> exited{false};
		LineSplitter out_lines;
		LineSplitter err_lines;
		ProcessCallbacks cb;
		cb.on_stdout = [this, &out_lines](const char *d, size_t n) {
			out_lines.feed(d, n, [this](const std::string &line) { handle_stdout_line(line); });
		};
		cb.on_stderr = [this, &err_lines](const char *d, size_t n) {
			err_lines.feed(d, n, [this](const std::string &line) { log(LogLevel::Info, "[helper] " + line); });
		};
		cb.on_exit = [this, &exited](int) {
			exited = true;
			media_.notify();
		};

		std::string error;
		std::unique_ptr<ChildProcess> child = spawn_process(launch.exe, helper_args(launch), std::move(cb), &error);
		if (!child) {
			log(LogLevel::Error, "could not start overflow-helper: " + error);
			set_status(HelperStatus::Failed, error);
			break; // a missing or blocked executable will not fix itself
		}
		++spawn_count_;
		{
			std::lock_guard<std::mutex> lock(mu_);
			current_ = child.get();
		}
		const auto started = std::chrono::steady_clock::now();

		session(*child, exited);

		const bool stopping = stop_requested_;
		const bool restarting = restart_requested_;
		// ready_ reflects the helper that just stopped running; clear it
		// (and, for an unexpected exit, announce Restarting) as soon as
		// session() returns, rather than after the up-to-2 s wait below.
		const bool was_ready = ready_.exchange(false);
		if (!stopping && !restarting)
			set_status(HelperStatus::Restarting, "");
		if (stopping || restarting)
			shutdown_child(*child, stopping ? grace() : std::chrono::seconds(10));
		else
			child->wait_for_exit(std::chrono::seconds(2));
		const int code = child->exit_code();
		{
			std::lock_guard<std::mutex> lock(mu_);
			current_ = nullptr;
		}
		child.reset();

		if (stopping)
			break;
		if (restarting) {
			backoff.reset();
			continue;
		}
		if (was_ready && std::chrono::steady_clock::now() - started >= std::chrono::seconds(60))
			backoff.reset();
		const std::chrono::milliseconds delay = backoff.next();
		std::string detail = "overflow-helper exited with code " + std::to_string(code);
		if (!was_ready)
			detail += " before it was ready (see the log)";
		detail += "; restarting in " + std::to_string(delay.count()) + " ms";
		log(LogLevel::Warning, detail);
		set_status(HelperStatus::Restarting, detail);
		std::unique_lock<std::mutex> lock(mu_);
		stop_cv_.wait_for(lock, delay, [this] { return stop_requested_.load() || restart_requested_.load(); });
	}

	if (status_ != HelperStatus::Failed)
		set_status(HelperStatus::Stopped, "");
	{
		std::lock_guard<std::mutex> lock(mu_);
		run_done_ = true;
	}
	done_cv_.notify_all();
}

void HelperSupervisor::session(ChildProcess &child, const std::atomic<bool> &exited)
{
	std::vector<uint8_t> frame;
	append_frame(frame, MsgType::Hello, hello_json());
	if (!write_watched(child, frame))
		return;
	// Only move Starting -> Running; if the reader thread already saw
	// "ready" and moved status_ to Ready, this compare-exchange leaves it
	// alone instead of clobbering it back to Running.
	set_status_if(HelperStatus::Starting, HelperStatus::Running, "");

	while (!exited && !stop_requested_ && !restart_requested_) {
		std::deque<std::string> commands;
		{
			std::lock_guard<std::mutex> lock(cmd_mu_);
			commands.swap(commands_);
		}
		for (const std::string &c : commands) {
			frame.clear();
			if (!append_frame(frame, MsgType::Command, c))
				continue;
			if (!write_watched(child, frame))
				return;
		}

		std::optional<MediaItem> item = media_.pop(std::chrono::milliseconds(50));
		if (!item)
			continue;
		const uint64_t now = now_ns_();
		const uint64_t capture = item->capture_ns ? item->capture_ns : now;
		const uint64_t send = now < capture ? capture : now; // the age is never negative
		if (send - capture > kMaxMediaAgeNs) {
			if (item->type == MsgType::VideoAU)
				media_.drop_video_until_keyframe();
			continue;
		}
		frame.clear();
		if (!append_media_frame(frame, item->type, capture, send, item->keyframe, item->data.data(),
					item->data.size()))
			continue;
		if (!write_watched(child, frame))
			return;
	}
}

void HelperSupervisor::shutdown_child(ChildProcess &child, std::chrono::milliseconds grace)
{
	// The write below can block on a helper that stopped reading; the
	// watchdog kills it after grace, which unblocks the write.
	std::thread watchdog([this, &child, grace] {
		if (!child.wait_for_exit(grace)) {
			log(LogLevel::Warning, "overflow-helper did not exit in time; killing it");
			child.kill();
		}
	});
	std::vector<uint8_t> frame;
	if (append_frame(frame, MsgType::Command, shutdown_command()))
		write_watched(child, frame);
	child.close_stdin();
	watchdog.join();
	child.wait_for_exit(std::chrono::seconds(5));
}

} // namespace airplay
