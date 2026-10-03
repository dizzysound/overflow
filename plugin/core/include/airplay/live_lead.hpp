// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Auto TV delay for a helper that can slide a live session's lead (protocol
// 1.4, spec 2026-10-02 section 4): raise on trouble, lower while clean (dynamic
// mode), and the starting value to save for the next session.
#pragma once

#include <cstdint>
#include <deque>
#include <utility>

namespace airplay {

constexpr int kLeadHeadroomMs = 60;
// Wi-Fi displays get more room above their start before the ceiling.
constexpr int kWifiLeadHeadroomMs = 100;

struct LiveLeadWindow {
	uint64_t now_ns = 0;
	int trouble = 0;
	int effective_ms = 0;
	int need_ms = 0; // this window's need: max(delivery p99, audio age + 11)
};

class LiveLead {
public:
	static constexpr uint64_t kTroubleSpanNs = 60ull * 1000 * 1000 * 1000;
	static constexpr int kTroubleWindowsToRaise = 2;
	static constexpr int kRaiseMs = 20;
	static constexpr uint64_t kFirstLowerNs = 2ull * 60 * 1000 * 1000 * 1000;
	static constexpr uint64_t kNextLowerNs = 2ull * 60 * 1000 * 1000 * 1000;
	static constexpr int kMinLowerMs = 5;
	static constexpr int kMaxLowerMs = 20;
	// The floor follows the highest window need in this span, so one slow
	// window holds it only this long.
	static constexpr uint64_t kNeedWindowNs = 10ull * 60 * 1000 * 1000 * 1000;
	static constexpr int kNeedMarginMs = 10;
	static constexpr int kEdgeMarginMs = 10;
	static constexpr uint64_t kCleanRunNs = 10ull * 60 * 1000 * 1000 * 1000;
	static constexpr int kBetweenSessionLowerMs = 10;
	// The slide time of one 20 ms raise at 300 ppm: at the ceiling, count at most
	// one raise per interval.
	static constexpr uint64_t kRaisePaceNs = 67ull * 1000 * 1000 * 1000;

	LiveLead(bool dynamic, int start_ms, int ceiling_ms, uint64_t start_ns);

	// One 5 s delivery window. Returns the new target to send with set_lead, or 0.
	int on_window(const LiveLeadWindow &w);

	int target_ms() const { return target_ms_; }
	int ceiling_ms() const { return ceiling_ms_; }
	bool dynamic() const { return dynamic_; }
	bool at_ceiling() const { return at_ceiling_; }
	// The starting lead to save for this display's next session.
	int next_start_ms() const;
	// The highest window need in the last kNeedWindowNs (0 before any window).
	int recent_need_ms() const;
	// The lowest target a lowering may reach now: recent need + 10, edge floor, 40.
	int floor_ms() const;

private:
	bool dynamic_;
	int start_ms_;
	int ceiling_ms_;
	int target_ms_;
	uint64_t clean_since_ns_;
	uint64_t last_lower_ns_ = 0;
	std::deque<uint64_t> trouble_ns_;
	std::deque<std::pair<uint64_t, int>> clean_; // (window time, effective) since clean_since_ns_
	std::deque<std::pair<uint64_t, int>> needs_; // (window time, need) in the last kNeedWindowNs
	int troubled_windows_ = 0;
	int best_clean_ms_ = 0;
	int last_raised_ms_ = 0;
	int edge_floor_ms_ = 0;
	int ceiling_extra_ms_ = 0;
	uint64_t last_raise_ns_ = 0;
	bool have_raised_ = false;
	bool lowered_ = false;
	bool raised_ = false;
	bool at_ceiling_ = false;
};

} // namespace airplay
