// SPDX-License-Identifier: GPL-3.0-or-later
#include "airplay/ceiling_guard.hpp"
#include "airplay/lead_policy.hpp"
#include "airplay/live_lead.hpp"

#include <doctest/doctest.h>

using airplay::CeilingGuard;
using airplay::CeilingWindow;

namespace {
constexpr uint64_t kSec = 1000ull * 1000 * 1000;
constexpr uint64_t kMin = 60 * kSec;

// Narthex on 2026-10-04: an Auto Wi-Fi display that started at 197 ms with a
// 297 ms ceiling. At 10:57:02 OBS added 362 ms of audio buffering; the audio
// age went from about 30 ms to about 389 ms (need 400) and stayed there.
constexpr int kStart = 197;
constexpr int kCeiling = 297;
constexpr int kAgeAfterJump = 389;
constexpr int kNeedAfterJump = kAgeAfterJump + airplay::kEldFrameMs;

int wanted_after_jump()
{
	airplay::LeadInputs in;
	in.wifi = true;
	in.audio_age_ns = static_cast<uint64_t>(kAgeAfterJump) * 1000000ull;
	return airplay::display_lead_ms(in);
}

// Feeds 5 s over-ceiling windows from t0 to t1 (exclusive); returns the first
// reconnect lead asked for and sets *at to its time.
int feed_over(CeilingGuard &g, uint64_t t0, uint64_t t1, bool may, uint64_t *at = nullptr, int ceiling = kCeiling)
{
	for (uint64_t t = t0; t < t1; t += 5 * kSec) {
		const CeilingGuard::Step s = g.on_window({t, 625, kNeedAfterJump, ceiling}, may, wanted_after_jump());
		if (s.reconnect_ms > 0) {
			if (at)
				*at = t;
			return s.reconnect_ms;
		}
	}
	return 0;
}
} // namespace

TEST_CASE("CeilingGuard: the 2026-10-04 jump: LiveLead alone stops at the ceiling and audio stays lost")
{
	airplay::LiveLead live(true, kStart, kCeiling, 0);
	// Clean windows before the jump (need 75 ms).
	for (uint64_t t = 5 * kSec; t < 2 * kMin; t += 5 * kSec)
		live.on_window({t, 0, live.target_ms(), 75});
	// After the jump every window drops audio late. Even a lead that slid at
	// once to each target never gets above the ceiling, so it never meets the need.
	for (uint64_t t = 2 * kMin; t < 70 * kMin; t += 5 * kSec)
		live.on_window({t, 625, live.target_ms(), kNeedAfterJump});
	CHECK(live.target_ms() == kCeiling);
	CHECK(live.at_ceiling());
	CHECK(live.floor_ms() > live.ceiling_ms());
}

TEST_CASE("CeilingGuard: the 2026-10-04 jump warns within 10 s and reconnects above the need after 30 s")
{
	CeilingGuard g;
	for (uint64_t t = 5 * kSec; t < 2 * kMin; t += 5 * kSec) {
		const CeilingGuard::Step s = g.on_window({t, 0, 75, kCeiling}, true, kStart);
		CHECK_FALSE(s.warning_started);
		CHECK(s.reconnect_ms == 0);
	}
	const uint64_t jump = 2 * kMin;
	CeilingGuard::Step s = g.on_window({jump, 248, kNeedAfterJump, kCeiling}, true, wanted_after_jump());
	CHECK_FALSE(s.warning_started); // one window is not enough
	s = g.on_window({jump + 5 * kSec, 625, kNeedAfterJump, kCeiling}, true, wanted_after_jump());
	CHECK(s.warning_started);
	CHECK(g.warning());
	CHECK(g.need_ms() == kNeedAfterJump);
	CHECK(g.ceiling_ms() == kCeiling);

	uint64_t at = 0;
	const int lead = feed_over(g, jump + 10 * kSec, jump + 2 * kMin, true, &at);
	CHECK(at == jump + 30 * kSec);
	CHECK(lead >= kNeedAfterJump + CeilingGuard::kReconnectMarginMs);
	CHECK(lead == wanted_after_jump()); // 389 + 11 + 50 + 70 = 520
	CHECK(lead > kCeiling);
	CHECK(lead % 5 == 0);
	CHECK(g.reconnects() == 1);

	// The new session (ceiling lead + 100) meets the need: the warning clears.
	const int new_ceiling = lead + airplay::kWifiLeadHeadroomMs;
	s = g.on_window({at + 10 * kSec, 0, kNeedAfterJump, new_ceiling}, true, wanted_after_jump());
	CHECK_FALSE(s.warning_cleared);
	s = g.on_window({at + 15 * kSec, 0, kNeedAfterJump, new_ceiling}, true, wanted_after_jump());
	CHECK(s.warning_cleared);
	CHECK_FALSE(g.warning());
}

TEST_CASE("CeilingGuard: a short burst over the ceiling neither warns nor reconnects")
{
	CeilingGuard g;
	CHECK_FALSE(g.on_window({5 * kSec, 100, 400, kCeiling}, true, 520).warning_started);
	CHECK_FALSE(g.on_window({10 * kSec, 0, 400, kCeiling}, true, 520).warning_started);
	CHECK_FALSE(g.on_window({15 * kSec, 100, 400, kCeiling}, true, 520).warning_started);
	CHECK_FALSE(g.warning());
}

TEST_CASE("CeilingGuard: trouble below the ceiling is LiveLead's to raise, not a reconnect")
{
	CeilingGuard g;
	for (uint64_t t = 5 * kSec; t < 5 * kMin; t += 5 * kSec) {
		const CeilingGuard::Step s = g.on_window({t, 50, 250, kCeiling}, true, 300);
		CHECK_FALSE(s.warning_started);
		CHECK(s.reconnect_ms == 0);
	}
}

TEST_CASE("CeilingGuard: a need above the ceiling without trouble does not warn")
{
	CeilingGuard g;
	for (uint64_t t = 5 * kSec; t < 2 * kMin; t += 5 * kSec)
		CHECK_FALSE(g.on_window({t, 0, 400, kCeiling}, true, 520).warning_started);
}

TEST_CASE("CeilingGuard: a fixed TV delay warns but never reconnects")
{
	CeilingGuard g;
	CHECK(feed_over(g, 5 * kSec, 30 * kMin, false) == 0);
	CHECK(g.warning());
	CHECK(g.reconnects() == 0);
	const std::string text = airplay::ceiling_warning_text("Narthex", g, true);
	CHECK(text.find("needs about 400 ms") != std::string::npos);
	CHECK(text.find("297 ms") != std::string::npos);
	CHECK(text.find("fixed") != std::string::npos);
}

TEST_CASE("CeilingGuard: reconnects back off 5 then 20 minutes and stop after three")
{
	CeilingGuard g;
	uint64_t first = 0, second = 0, third = 0;
	// Each new session is still too short (the need keeps outrunning it).
	REQUIRE(feed_over(g, 5 * kSec, 10 * kMin, true, &first) > 0);
	CHECK(first == 35 * kSec);
	REQUIRE(feed_over(g, first + 5 * kSec, first + 60 * kMin, true, &second) > 0);
	CHECK(second - first >= CeilingGuard::kBackoffNs[1]);
	CHECK(second - first < CeilingGuard::kBackoffNs[1] + 10 * kSec);
	REQUIRE(feed_over(g, second + 5 * kSec, second + 60 * kMin, true, &third) > 0);
	CHECK(third - second >= CeilingGuard::kBackoffNs[2]);
	CHECK(feed_over(g, third + 5 * kSec, third + 300 * kMin, true) == 0);
	CHECK(g.reconnects() == CeilingGuard::kMaxReconnects);
	CHECK(g.exhausted());
	CHECK(airplay::ceiling_warning_text("Narthex", g, false).find("cannot raise it again") != std::string::npos);
}

TEST_CASE("CeilingGuard: at the 2000 ms limit there is nothing to reconnect to")
{
	CeilingGuard g;
	for (uint64_t t = 5 * kSec; t < 10 * kMin; t += 5 * kSec)
		CHECK(g.on_window({t, 500, 2100, airplay::kLeadCeilingMs}, true, airplay::kLeadCeilingMs).reconnect_ms == 0);
	CHECK(g.warning());
	CHECK(g.exhausted());
}

TEST_CASE("CeilingGuard: a clean break restarts the 30 s span")
{
	CeilingGuard g;
	CHECK(feed_over(g, 5 * kSec, 25 * kSec, true) == 0);
	CHECK(g.on_window({25 * kSec, 0, 75, kCeiling}, true, kStart).reconnect_ms == 0);
	uint64_t at = 0;
	CHECK(feed_over(g, 30 * kSec, 2 * kMin, true, &at) > 0);
	CHECK(at == 60 * kSec);
}

TEST_CASE("reconnect_lead_ms: need plus margin, rounded up to 5, within the limits")
{
	CHECK(airplay::reconnect_lead_ms(0, 400) == 430);
	CHECK(airplay::reconnect_lead_ms(0, 401) == 435);
	CHECK(airplay::reconnect_lead_ms(520, 400) == 520);
	CHECK(airplay::reconnect_lead_ms(0, 5) == airplay::kLeadFloorMs);
	CHECK(airplay::reconnect_lead_ms(0, 3000) == airplay::kLeadCeilingMs);
}
