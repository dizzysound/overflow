// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/commands.hpp"
#include "airplay/supervisor.hpp"
#include "test_util.hpp"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

using namespace airplay;
using std::chrono::milliseconds;

namespace {

struct Recorder {
	Lines events; // raw lines of stub_* (Unknown) events
	Lines logs;
	Lines statuses;
	std::atomic<int> ready{0};

	SupervisorCallbacks callbacks()
	{
		SupervisorCallbacks cb;
		cb.on_event = [this](const Event &e) {
			if (std::holds_alternative<ReadyEvent>(e))
				++ready;
			if (const auto *u = std::get_if<UnknownEvent>(&e))
				events.add(u->raw);
		};
		cb.on_log = [this](LogLevel, const std::string &m) { logs.add(m); };
		cb.on_status = [this](HelperStatus s, const std::string &d) {
			statuses.add(std::string(helper_status_name(s)) + ": " + d);
		};
		return cb;
	}
};

HelperLaunch stub(std::vector<std::string> extra = {})
{
	HelperLaunch l;
	l.exe = STUB_HELPER_PATH;
	l.creds_path = "credentials.json";
	l.extra_args = std::move(extra);
	return l;
}

std::vector<milliseconds> fast_backoff()
{
	return {milliseconds(20), milliseconds(40)};
}

} // namespace

TEST_CASE("helper_args: defaults")
{
	HelperLaunch l;
	l.creds_path = "C:/Users/x/AppData/Roaming/obs-studio/plugin_config/obs-overflow/credentials.json";
	CHECK(helper_args(l) == std::vector<std::string>{"-creds", l.creds_path, "-port-range", "60000-60099", "-fps", "30"});
}

TEST_CASE("helper_args: every option")
{
	HelperLaunch l;
	l.creds_path = "c.json";
	l.fps = 60;
	l.timing = TimingMode::Ntp;
	l.target_latency_ms = 250;
	l.eld_encoder_path = "C:/p/eld-encoder.disabled";
	l.debug = true;
	CHECK(helper_args(l) == std::vector<std::string>{"-creds", "c.json", "-port-range", "60000-60099", "-fps", "60",
							 "-timing", "ntp", "-target-latency-ms", "250", "-eld-encoder",
							 "C:/p/eld-encoder.disabled", "-debug"});
}

TEST_CASE("supervisor: sends hello first and reports ready")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	CHECK(wait_until([&] { return r.ready.load() >= 1 && r.events.any_contains("\"stub_hello\""); }));
	CHECK(r.events.any_contains("\"ok\":true"));
	CHECK(sup.status() == HelperStatus::Ready);
	CHECK(r.logs.any_contains("[helper] stub: started"));
	sup.stop(std::chrono::seconds(2));
	CHECK(sup.status() == HelperStatus::Stopped);
}

TEST_CASE("supervisor: the helper gets the launch flags")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	HelperLaunch l = stub();
	l.fps = 50;
	sup.start(l);
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_args\""); }));
	CHECK(r.events.any_contains(R"("-fps","50")"));
	CHECK(r.events.any_contains(R"("-port-range","60000-60099")"));
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: commands arrive in order")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_hello\""); }));
	sup.send_command(set_displays_command({}));
	sup.send_command(restart_command("A"));
	REQUIRE(wait_until([&] { return r.events.count_containing("\"stub_command\"") >= 2; }));
	std::vector<std::string> cmds;
	for (const std::string &line : r.events.snapshot()) {
		const auto j = nlohmann::json::parse(line, nullptr, false);
		if (j.is_object() && j.value("event", "") == "stub_command")
			cmds.push_back(j.value("cmd", ""));
	}
	CHECK(cmds == std::vector<std::string>{"set_displays", "restart"});
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: media gets a fresh send time and valid age")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_hello\""); }));
	sup.media().push_video({0, 0, 0, 1, 0x65, 0x88}, test_now_ns(), true);
	sup.media().push_audio(std::vector<uint8_t>(4096, 0), test_now_ns());
	REQUIRE(wait_until([&] { return r.events.count_containing("\"stub_media\"") >= 2; }));
	CHECK(r.events.any_contains(R"("age_ok":true,"bytes":6,"event":"stub_media","keyframe":true,"type":2)"));
	CHECK(r.events.any_contains(R"("age_ok":true,"bytes":4096,"event":"stub_media","keyframe":false,"type":3)"));
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: stale media is dropped, not sent")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_hello\""); }));
	sup.media().push_video({0, 0, 0, 1, 0x65}, test_now_ns() - 5000000000ull, true);
	sup.media().push_audio(std::vector<uint8_t>(8, 0), test_now_ns());
	REQUIRE(wait_until([&] { return r.events.count_containing("\"stub_media\"") >= 1; }));
	std::this_thread::sleep_for(milliseconds(100));
	CHECK(r.events.count_containing("\"stub_media\"") == 1);
	CHECK(r.events.any_contains("\"type\":3"));
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: drains a helper that floods stderr")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub({"--stderr-flood", "2048"}));
	CHECK(wait_until([&] { return r.ready.load() >= 1; }));
	CHECK(wait_until([&] { return r.logs.size() >= 2048; })); // stderr and stdout are read on separate threads
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: restarts after an unexpected exit, with backoff")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub({"--exit-after-hello", "3"}));
	CHECK(wait_until([&] { return sup.spawn_count() >= 3; }));
	CHECK(r.statuses.any_contains("restarting: overflow-helper exited with code 3"));
	sup.stop(std::chrono::seconds(2));
	CHECK(sup.status() == HelperStatus::Stopped);
}

TEST_CASE("supervisor: stop sends shutdown and the helper exits on its own")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_hello\""); }));
	const auto t0 = std::chrono::steady_clock::now();
	sup.stop(std::chrono::seconds(5));
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(3));
	CHECK(r.logs.any_contains("[helper] stub: shutdown command"));
	CHECK_FALSE(r.logs.any_contains("killing it"));
	CHECK(sup.spawn_count() == 1);
}

TEST_CASE("supervisor: stop kills a helper that hangs")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub({"--hang-after-hello"}));
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_hello\""); }));
	const auto t0 = std::chrono::steady_clock::now();
	sup.stop(milliseconds(300));
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(4));
	CHECK(r.logs.any_contains("killing it"));
	CHECK(sup.status() == HelperStatus::Stopped);
}

TEST_CASE("supervisor: restart replaces the helper without blocking")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.ready.load() >= 1; }));
	HelperLaunch next = stub();
	next.fps = 25;
	const auto t0 = std::chrono::steady_clock::now();
	sup.restart(next);
	CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(100));
	CHECK(wait_until([&] { return sup.spawn_count() == 2 && r.events.any_contains(R"("-fps","25")"); }));
	CHECK(r.logs.any_contains("[helper] stub: shutdown command"));
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: a missing executable fails without retrying")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	HelperLaunch l = stub();
	l.exe = "/nonexistent/overflow-helper";
	sup.start(l);
	CHECK(wait_until([&] { return sup.status() == HelperStatus::Failed; }));
	std::this_thread::sleep_for(milliseconds(100));
	CHECK(sup.spawn_count() == 0);
	CHECK(r.logs.any_contains("could not start overflow-helper"));
	sup.stop();
	CHECK(sup.status() == HelperStatus::Failed);
}

TEST_CASE("Minor 6: commands are dropped, not queued, while stopped or failed")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff());
	CHECK(sup.status() == HelperStatus::Stopped);
	sup.send_command(set_displays_command({}));
	sup.send_command(restart_command("A"));
	CHECK(sup.queued_command_count() == 0);

	HelperLaunch l = stub();
	l.exe = "/nonexistent/overflow-helper";
	sup.start(l);
	CHECK(wait_until([&] { return sup.status() == HelperStatus::Failed; }));
	sup.send_command(set_displays_command({}));
	CHECK(sup.queued_command_count() == 0);
	sup.stop();
}

// Fix round 1 (review findings on Task 14):

TEST_CASE("repro: restart while a write is stuck is recovered by the stuck-write watchdog")
{
	// A short stuck-write threshold keeps this test fast; production uses
	// kStuckWriteMs (10 s) by default.
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff(), MediaQueueLimits{64u << 20, 64u << 20}, 300);
	sup.start(stub({"--hang-after-hello"}));
	REQUIRE(wait_until([&] { return r.ready.load() >= 1; }));
	sup.media().push_video(std::vector<uint8_t>(4u << 20, 1), test_now_ns(), true); // fills the pipe
	std::this_thread::sleep_for(milliseconds(300));
	HelperLaunch next = stub();
	next.fps = 33;
	const auto t0 = std::chrono::steady_clock::now();
	sup.restart(next);
	CHECK(std::chrono::steady_clock::now() - t0 < milliseconds(100)); // restart() itself never blocks
	CHECK(wait_until([&] { return sup.spawn_count() >= 2; }, milliseconds(15000)));
	CHECK(wait_until([&] { return r.events.any_contains(R"("-fps","33")"); }));
	CHECK(r.logs.any_contains("overflow-helper stopped reading its input; restarting it"));
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("supervisor: a normal large write does not trigger the stuck-write watchdog")
{
	Recorder r;
	HelperSupervisor sup(r.callbacks(), test_now_ns, fast_backoff(), MediaQueueLimits{64u << 20, 64u << 20}, 300);
	sup.start(stub());
	REQUIRE(wait_until([&] { return r.ready.load() >= 1; }));
	sup.media().push_video(std::vector<uint8_t>(4u << 20, 1), test_now_ns(), true);
	REQUIRE(wait_until([&] { return r.events.any_contains("\"stub_media\""); }, milliseconds(5000)));
	std::this_thread::sleep_for(milliseconds(500));
	CHECK_FALSE(r.logs.any_contains("stopped reading its input"));
	CHECK(sup.spawn_count() == 1);
	sup.stop(std::chrono::seconds(2));
}

TEST_CASE("repro2: stop() from on_status(Ready) does not self-join or deadlock")
{
	HelperSupervisor *self = nullptr;
	std::atomic<bool> once{false};
	SupervisorCallbacks cb;
	cb.on_status = [&](HelperStatus s, const std::string &) {
		if (s == HelperStatus::Ready && !once.exchange(true))
			self->stop(milliseconds(200));
	};
	HelperSupervisor sup(cb, test_now_ns, fast_backoff());
	self = &sup;
	sup.start(stub());
	CHECK(wait_until([&] { return sup.status() == HelperStatus::Stopped; }, milliseconds(5000)));
	const auto t0 = std::chrono::steady_clock::now();
	sup.stop();
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
}

TEST_CASE("repro3: stop() from on_event on the stdout thread does not self-join or deadlock")
{
	HelperSupervisor *self = nullptr;
	std::atomic<bool> once{false};
	SupervisorCallbacks cb;
	cb.on_event = [&](const Event &e) {
		if (std::holds_alternative<ReadyEvent>(e) && !once.exchange(true))
			self->stop(milliseconds(200));
	};
	HelperSupervisor sup(cb, test_now_ns, fast_backoff());
	self = &sup;
	sup.start(stub());
	CHECK(wait_until([&] { return sup.status() == HelperStatus::Stopped; }, milliseconds(5000)));
	const auto t0 = std::chrono::steady_clock::now();
	sup.stop();
	CHECK(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(1));
}
