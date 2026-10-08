// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/session_policy.hpp"

#include <doctest/doctest.h>

using namespace airplay;

namespace {
constexpr uint64_t kMinute = 60ull * 1000000000ull;

DisplaySettings display(IdlePolicy policy, int minutes = 10)
{
	DisplaySettings r;
	r.device_id = "A";
	r.enabled = true;
	r.idle_policy = policy;
	r.idle_minutes = minutes;
	return r;
}
} // namespace

TEST_CASE("auto-start: nothing configured, nothing runs")
{
	AutoStart a;
	a.obs_loaded();
	a.streaming(true);
	a.recording(true);
	CHECK_FALSE(a.should_run());
}

TEST_CASE("auto-start: when OBS launches")
{
	AutoStart a({true, false, false});
	CHECK_FALSE(a.should_run());
	a.obs_loaded();
	CHECK(a.should_run());
	a.user_stop();
	CHECK_FALSE(a.should_run());
	a.user_start();
	CHECK(a.should_run());
}

TEST_CASE("auto-start: with streaming starts and stops with the stream")
{
	AutoStart a({false, true, false});
	a.obs_loaded();
	a.streaming(true);
	CHECK(a.should_run());
	a.streaming(false);
	CHECK_FALSE(a.should_run());
}

TEST_CASE("auto-start: Stop suppresses until the next trigger")
{
	AutoStart a({false, true, false});
	a.streaming(true);
	a.user_stop();
	CHECK_FALSE(a.should_run());
	a.streaming(false);
	a.streaming(true);
	CHECK(a.should_run());
}

TEST_CASE("auto-start: a manual start outlives the stream")
{
	AutoStart a({false, true, false});
	a.streaming(true);
	a.user_start();
	a.streaming(false);
	CHECK(a.should_run());
}

TEST_CASE("auto-start: recording keeps it running after streaming stops")
{
	AutoStart a({false, true, true});
	a.streaming(true);
	a.recording(true);
	a.streaming(false);
	CHECK(a.should_run());
	a.recording(false);
	CHECK_FALSE(a.should_run());
}

TEST_CASE("idle: disabled displays are never selected")
{
	DisplaySettings r = display(IdlePolicy::StayConnected);
	r.enabled = false;
	IdleInputs in;
	in.output_running = true;
	CHECK_FALSE(display_wanted(r, in));
}

TEST_CASE("idle: while the output runs")
{
	IdleInputs in;
	in.output_running = true;
	in.ever_started = true;
	in.now_ns = 100 * kMinute;
	in.last_activity_ns = 95 * kMinute;
	CHECK(display_wanted(display(IdlePolicy::StayConnected), in));
	CHECK(display_wanted(display(IdlePolicy::DisconnectWhenOutputStops), in));
	CHECK(display_wanted(display(IdlePolicy::DisconnectAfterIdle, 10), in));
	CHECK_FALSE(display_wanted(display(IdlePolicy::DisconnectAfterIdle, 5), in));
	in.obs_busy = true;
	CHECK(display_wanted(display(IdlePolicy::DisconnectAfterIdle, 5), in));
}

TEST_CASE("idle: after the output stops")
{
	IdleInputs in;
	in.output_running = false;
	in.now_ns = 100 * kMinute;
	in.output_stopped_since_ns = 92 * kMinute;
	CHECK_FALSE(display_wanted(display(IdlePolicy::StayConnected), in)); // never started this session
	in.ever_started = true;
	CHECK(display_wanted(display(IdlePolicy::StayConnected), in));
	CHECK_FALSE(display_wanted(display(IdlePolicy::DisconnectWhenOutputStops), in));
	CHECK(display_wanted(display(IdlePolicy::DisconnectAfterIdle, 10), in));
	CHECK_FALSE(display_wanted(display(IdlePolicy::DisconnectAfterIdle, 8), in));
}

TEST_CASE("selection_for maps display settings to set_displays entries")
{
	Settings s;
	DisplaySettings &a = s.ensure_display("A", "Lobby");
	a.enabled = true;
	a.auto_reconnect = false;
	a.volume_db = -10.0;
	a.manual_ip = "10.20.0.178";
	a.manual_port = 7000;
	s.ensure_display("B", "Off"); // disabled
	IdleInputs in;
	in.output_running = true;
	const auto sel = selection_for(s, in, {});
	REQUIRE(sel.size() == 1);
	CHECK(sel[0].device_id == "A");
	CHECK_FALSE(sel[0].auto_reconnect);
	CHECK(sel[0].volume_db == std::optional<double>(-10.0));
	CHECK(sel[0].ip == "10.20.0.178");
	CHECK(sel[0].port == 7000);
}

TEST_CASE("selection_for copies audio_enabled into the selection's audio field")
{
	Settings s;
	DisplaySettings &a = s.ensure_display("A", "Lobby");
	a.enabled = true;
	a.audio_enabled = false;
	DisplaySettings &b = s.ensure_display("B", "Main Room");
	b.enabled = true; // audio_enabled defaults to true
	IdleInputs in;
	in.output_running = true;
	const auto sel = selection_for(s, in, {});
	REQUIRE(sel.size() == 2);
	CHECK(sel[0].device_id == "A");
	CHECK_FALSE(sel[0].audio);
	CHECK(sel[1].device_id == "B");
	CHECK(sel[1].audio);
}

TEST_CASE("selection_for copies wifi_tolerant into the selection")
{
	Settings s;
	DisplaySettings &a = s.ensure_display("A", "Roku");
	a.enabled = true;
	a.wifi_tolerant = true;
	DisplaySettings &b = s.ensure_display("B", "Main Room");
	b.enabled = true; // wifi_tolerant defaults to false
	IdleInputs in;
	in.output_running = true;
	const auto sel = selection_for(s, in, {});
	REQUIRE(sel.size() == 2);
	CHECK(sel[0].device_id == "A");
	CHECK(sel[0].wifi_tolerant);
	CHECK(sel[1].device_id == "B");
	CHECK_FALSE(sel[1].wifi_tolerant);
}

TEST_CASE("selection_for carries each display's lead")
{
	Settings s;
	DisplaySettings &d = s.ensure_display("AA", "Studio");
	d.enabled = true;
	d.idle_policy = IdlePolicy::StayConnected;
	IdleInputs in;
	in.output_running = true;
	const auto sel = selection_for(s, in, {{"AA", 95}});
	REQUIRE(sel.size() == 1);
	CHECK(sel[0].latency_ms == 95);
	CHECK(selection_for(s, in, {})[0].latency_ms == 0);
}

TEST_CASE("selection_for copies audio_format into the selection")
{
	Settings s;
	DisplaySettings &a = s.ensure_display("A", "Roku");
	a.enabled = true;
	a.audio_format = "aac-eld";
	IdleInputs in;
	in.output_running = true;
	const auto sel = selection_for(s, in, {});
	REQUIRE(sel.size() == 1);
	CHECK(sel[0].audio_format == "aac-eld");
}

TEST_CASE("scene skip: a display drops on its skipped scenes only, whatever the idle policy")
{
	DisplaySettings r = display(IdlePolicy::StayConnected);
	r.skip_scenes = {"Lobby Wide"};
	IdleInputs in;
	in.output_running = true;
	in.ever_started = true;
	in.obs_busy = true;
	in.program_scene = "Stage";
	CHECK(display_wanted(r, in));
	in.program_scene = "Lobby Wide";
	CHECK_FALSE(display_wanted(r, in));
	CHECK(display_wanted(display(IdlePolicy::StayConnected), in)); // other displays stay
	in.program_scene.clear(); // unknown scene: never skips
	CHECK(display_wanted(r, in));
	CHECK_FALSE(skipped_on_scene(r, ""));
}

TEST_CASE("scene skip: selection_for leaves out a skipped display")
{
	Settings s;
	s.displays = {display(IdlePolicy::StayConnected), display(IdlePolicy::StayConnected)};
	s.displays[1].device_id = "B";
	s.displays[1].skip_scenes = {"Wide"};
	IdleInputs in;
	in.output_running = true;
	in.ever_started = true;
	in.obs_busy = true;
	in.program_scene = "Wide";
	const auto sel = selection_for(s, in, {});
	REQUIRE(sel.size() == 1);
	CHECK(sel[0].device_id == "A");
}

TEST_CASE("scene skip: rows of enabled skipped displays read off on this scene")
{
	Settings s;
	s.displays = {display(IdlePolicy::StayConnected), display(IdlePolicy::StayConnected)};
	s.displays[0].skip_scenes = {"Wide"};
	s.displays[1].device_id = "B";
	s.displays[1].skip_scenes = {"Wide"};
	s.displays[1].enabled = false;
	DisplayRow a;
	a.device_id = "A";
	a.enabled = true;
	a.state = "idle";
	DisplayRow b;
	b.device_id = "B";
	b.state = "";
	DisplayGroup g;
	g.displays = {a, b};
	std::vector<DisplayGroup> groups = {g};

	mark_scene_skips(groups, s, "Stage");
	CHECK(groups[0].displays[0].state == "idle");
	mark_scene_skips(groups, s, "Wide");
	CHECK(groups[0].displays[0].state == "off on this scene");
	CHECK(groups[0].displays[0].light == Light::Gray);
	CHECK(groups[0].displays[0].status_tooltip.find("\"Wide\"") != std::string::npos);
	CHECK(groups[0].displays[1].state.empty()); // unchecked displays are left alone
}
