// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// A display whose need is above the most TV delay its session can reach.
// The ceiling is announced to the receiver at connect (protocol 1.4), and a
// live slide never passes it, so when the OBS audio age jumps above it (OBS
// adding audio buffering mid-session, 2026-10-04: +362 ms) every audio frame
// is dropped late until the display reconnects with a higher lead. This
// watches the delivery windows, says so, and asks for that reconnect, with a
// backoff so it cannot loop.
#pragma once

#include <cstdint>
#include <string>

namespace airplay {

struct CeilingWindow {
	uint64_t now_ns = 0;
	int trouble = 0;    // lead_trouble() of the window
	int need_ms = 0;    // this window's need: max(delivery p99, audio age + 11)
	int ceiling_ms = 0; // the most the session's lead can reach
};

class CeilingGuard {
public:
	// Consecutive over-ceiling windows (5 s each) before the warning.
	static constexpr int kWindowsToWarn = 2;
	// Consecutive good windows before the warning clears.
	static constexpr int kWindowsToClear = 2;
	// Over the ceiling this long, without a break, before a reconnect.
	static constexpr uint64_t kStableNs = 30ull * 1000 * 1000 * 1000;
	// Reconnects per display per run, and the wait before each one after the first.
	static constexpr int kMaxReconnects = 3;
	static constexpr uint64_t kBackoffNs[kMaxReconnects] = {0, 5ull * 60 * 1000 * 1000 * 1000,
								 20ull * 60 * 1000 * 1000 * 1000};
	// A reconnect's lead is at least the need plus this.
	static constexpr int kReconnectMarginMs = 30;

	struct Step {
		bool warning_started = false;
		bool warning_cleared = false;
		int reconnect_ms = 0; // > 0: reconnect the display at this lead
	};

	// One delivery window. may_reconnect: an Auto display in a session that
	// can be restarted (never a fixed TV delay). wanted_ms: the lead a fresh
	// session would start at now (display_lead_ms without a saved lead).
	Step on_window(const CeilingWindow &w, bool may_reconnect, int wanted_ms);

	bool warning() const { return warning_; }
	// The highest need in the current over-ceiling span, and that span's ceiling.
	int need_ms() const { return need_ms_; }
	int ceiling_ms() const { return ceiling_ms_; }
	int reconnects() const { return reconnects_; }
	// True when the guard cannot help: the need is at the 2000 ms limit, or
	// the reconnects for this run are used up.
	bool exhausted() const { return exhausted_; }

private:
	bool warning_ = false;
	bool exhausted_ = false;
	int over_windows_ = 0;
	int good_windows_ = 0;
	uint64_t over_since_ns_ = 0;
	int need_ms_ = 0;
	int ceiling_ms_ = 0;
	int reconnects_ = 0;
	uint64_t last_reconnect_ns_ = 0;
};

// The lead to reconnect at: the larger of wanted_ms and need_ms plus the
// margin, rounded up to 5 ms, within [kLeadFloorMs, kLeadCeilingMs].
int reconnect_lead_ms(int wanted_ms, int need_ms);

// Dock and log text for a display in the warning state, e.g. "Narthex: audio
// dropped late: needs about 400 ms, above this display's most TV delay of 297
// ms. Reconnecting it with a higher TV delay."
std::string ceiling_warning_text(const std::string &display_name, const CeilingGuard &guard, bool fixed);

} // namespace airplay
