// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/lead_policy.hpp"

#include <doctest/doctest.h>

using namespace airplay;

static constexpr uint64_t kMs = 1000000ull;

TEST_CASE("display_lead_ms: audio on uses audio age + 11 + 50 audio path + wired margin, rounded up to 5")
{
	LeadInputs in;
	in.audio_age_ns = 48 * kMs; // audio need 48 + 11 + 50 = 109
	in.video_need_ms = 25;
	CHECK(display_lead_ms(in) == 130); // 109 + 20 = 129 -> 130
}

TEST_CASE("display_lead_ms: audio off ignores audio age")
{
	LeadInputs in;
	in.audio_on = false;
	in.audio_age_ns = 400 * kMs;
	in.video_need_ms = 25;
	CHECK(display_lead_ms(in) == 45); // 25 + 20
}

TEST_CASE("display_lead_ms: unknown audio age and unknown video need use the 30 ms default")
{
	LeadInputs in; // audio on, age 0, need 0
	CHECK(display_lead_ms(in) == 85); // max(30, 0 + 11 + 50) + 20 = 81 -> 85
}

TEST_CASE("display_lead_ms: Wi-Fi adds 50 ms more margin; floor 40; ceiling 2000")
{
	LeadInputs wifi;
	wifi.audio_on = false;
	wifi.wifi = true;
	wifi.video_need_ms = 25;
	CHECK(display_lead_ms(wifi) == 95); // 25 + 70
	LeadInputs tiny;
	tiny.audio_on = false;
	tiny.video_need_ms = 1;
	CHECK(display_lead_ms(tiny) == 40);
	LeadInputs huge;
	huge.audio_age_ns = 5000 * kMs;
	CHECK(display_lead_ms(huge) == 2000);
}

TEST_CASE("display_lead_ms: fixed values win, per display before global")
{
	LeadInputs in;
	in.global_fixed_ms = 200;
	CHECK(display_lead_ms(in) == 200);
	in.fixed_ms = 120;
	CHECK(display_lead_ms(in) == 120);
}

TEST_CASE("display_lead_ms: an Auto lead never falls below the late floor; a fixed one ignores it")
{
	LeadInputs in;
	in.audio_on = false;
	in.video_need_ms = 25; // 45 without the floor
	in.late_floor_ms = 100;
	CHECK(display_lead_ms(in) == 100);
	in.late_floor_ms = 30;
	CHECK(display_lead_ms(in) == 45);
	in.late_floor_ms = 5000;
	CHECK(display_lead_ms(in) == 2000);
	in.late_floor_ms = 300;
	in.fixed_ms = 120;
	CHECK(display_lead_ms(in) == 120);
	in.fixed_ms = 0;
	in.global_fixed_ms = 200;
	CHECK(display_lead_ms(in) == 200);
}

TEST_CASE("late floor: a raise is the lead plus 20, cleared at start only after a run without late windows")
{
	CHECK(late_floor_after_raise(80) == 100);
	CHECK(late_floor_after_raise(1990) == 2000);
	CHECK(learn_late_floor_at_start(100, 0) == 0);
	CHECK(learn_late_floor_at_start(100, 2) == 100);
	CHECK(learn_late_floor_at_start(0, 0) == 0);
}

TEST_CASE("seed_audio_age_ms: the first run after install takes the audio age from the legacy auto latency")
{
	CHECK(seed_audio_age_ms(0, 214) == 64); // legacy auto = audio age + 150
	CHECK(seed_audio_age_ms(0, 150) == 0);
	CHECK(seed_audio_age_ms(0, 90) == 0);
	CHECK(seed_audio_age_ms(0, 0) == 0);
	CHECK(seed_audio_age_ms(40, 300) == 40); // a measured age wins
}

TEST_CASE("LateWindows: more than 3 late windows within 60 s raise; late-free windows do not count")
{
	constexpr uint64_t kS = 1000000000ull;
	LateWindows w;
	CHECK_FALSE(w.record(1 * kS, 2));
	CHECK_FALSE(w.record(6 * kS, 0));
	CHECK_FALSE(w.record(11 * kS, 1));
	CHECK_FALSE(w.record(16 * kS, 5));
	CHECK(w.record(21 * kS, 1)); // the 4th late window in 60 s
	CHECK(w.run_late_windows() == 4);
	// The history clears on a raise: three more are not enough.
	CHECK_FALSE(w.record(26 * kS, 1));
	CHECK_FALSE(w.record(31 * kS, 1));
	CHECK_FALSE(w.record(36 * kS, 1));
	// Windows older than 60 s fall out: at 97 s, 26, 31 and 36 s are gone.
	CHECK_FALSE(w.record(97 * kS, 1));
	CHECK_FALSE(w.record(98 * kS, 1));
	CHECK_FALSE(w.record(99 * kS, 1));
	CHECK(w.record(100 * kS, 1));
	CHECK(w.run_late_windows() == 11);
}

TEST_CASE("LateWindows: at most one late-driven restart per 10 minutes")
{
	constexpr uint64_t kS = 1000000000ull;
	LateWindows w;
	CHECK(w.take_restart(100 * kS));
	CHECK_FALSE(w.take_restart(101 * kS));
	CHECK_FALSE(w.take_restart(699 * kS));
	CHECK(w.take_restart(700 * kS));
}

TEST_CASE("video need learning")
{
	CHECK(raise_video_need(30, 41) == 41);
	CHECK(raise_video_need(41, 30) == 41);
	CHECK(raise_video_need(30, 900, 80) == 80); // a p99 above the lead is capped at the lead
	CHECK(raise_video_need(30, 60, 80) == 60);
	CHECK(raise_video_need(30, 5000) == 2000); // stored needs stay within [0, 2000]
	CHECK(raise_video_need(-5, -1) == 0);
	CHECK(learn_video_need_at_start(80, 40) == 40); // last run 40 ms under: lower
	CHECK(learn_video_need_at_start(80, 60) == 80); // only 20 ms under: keep
	CHECK(learn_video_need_at_start(80, 0) == 80);  // no data: keep
	CHECK_FALSE(lead_needs_restart(80, 95));
	CHECK(lead_needs_restart(80, 100));
	CHECK_FALSE(lead_needs_restart(80, 80));
	CHECK_FALSE(lead_needs_restart(80, 60)); // never lowered mid-session
}

TEST_CASE("AudioLoss counts resend requests over the last minute and the run")
{
	using airplay::AudioLoss;
	constexpr uint64_t s = 1000ull * 1000 * 1000;
	AudioLoss loss;
	CHECK_FALSE(loss.seen());
	loss.record(10 * s, 0, 0);
	CHECK(loss.seen());
	CHECK(airplay::audio_loss_text(loss, 10 * s) == "no audio loss in the last minute");

	loss.record(20 * s, 5, 5);
	loss.record(25 * s, 3, 1);
	CHECK(loss.last_minute(25 * s).lost == 8);
	CHECK(loss.last_minute(25 * s).not_resent == 2);
	CHECK(airplay::audio_loss_text(loss, 25 * s) == "audio lost 8 in the last minute (2 not resent)");

	// 61 s after the first loss it has left the window; the run total keeps it.
	loss.record(81 * s, 0, 0);
	CHECK(loss.last_minute(81 * s).lost == 3);
	CHECK(airplay::audio_loss_text(loss, 81 * s) == "audio lost 3 in the last minute (2 not resent), 8 this run");
	loss.record(200 * s, 0, 0);
	CHECK(airplay::audio_loss_text(loss, 200 * s) == "no audio loss in the last minute, 8 this run");
}

TEST_CASE("AudioLoss shows audio dropped late beside lost packets")
{
	using airplay::AudioLoss;
	constexpr uint64_t s = 1000ull * 1000 * 1000;
	AudioLoss loss;
	loss.record(10 * s, 0, 0, 3);
	CHECK(airplay::audio_loss_text(loss, 10 * s) == "audio 3 dropped late in the last minute");
	loss.record(15 * s, 2, 2, 1);
	CHECK(airplay::audio_loss_text(loss, 15 * s) == "audio lost 2, 4 dropped late in the last minute");
	loss.record(200 * s, 0, 0, 0);
	CHECK(airplay::audio_loss_text(loss, 200 * s) == "no audio loss in the last minute, 6 this run");
}

TEST_CASE("lead_trouble counts late video, late audio and unrecovered loss, not resent loss")
{
	CHECK(airplay::lead_trouble(0, 0, 0, 0) == 0);
	CHECK(airplay::lead_trouble(0, 5, 5, 0) == 0); // resent in time: shown, not counted
	CHECK(airplay::lead_trouble(0, 5, 3, 0) == 2);
	CHECK(airplay::lead_trouble(0, 0, 0, 4) == 4);
	CHECK(airplay::lead_trouble(1, 0, 0, 0) == 1);
}

TEST_CASE("display_lead_ms: a saved lead starts Auto; fixed values still win")
{
	airplay::LeadInputs in;
	in.wifi = true;
	in.saved_lead_ms = 75;
	CHECK(airplay::display_lead_ms(in) == 75);
	in.saved_lead_ms = 20;
	CHECK(airplay::display_lead_ms(in) == airplay::kLeadFloorMs);
	in.fixed_ms = 120;
	CHECK(airplay::display_lead_ms(in) == 120);
}
