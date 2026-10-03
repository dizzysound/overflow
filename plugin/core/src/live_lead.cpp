// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/live_lead.hpp"

#include "airplay/lead_policy.hpp"

#include <algorithm>

namespace airplay {

namespace {
// Elapsed time that cannot wrap when a window is stamped before 'then'.
uint64_t since(uint64_t now, uint64_t then)
{
	return now >= then ? now - then : 0;
}
} // namespace

LiveLead::LiveLead(bool dynamic, int start_ms, int ceiling_ms, uint64_t start_ns)
	: dynamic_(dynamic), start_ms_(start_ms), ceiling_ms_(std::max(ceiling_ms, start_ms)), target_ms_(start_ms),
	  clean_since_ns_(start_ns)
{
}

int LiveLead::on_window(const LiveLeadWindow &w)
{
	needs_.emplace_back(w.now_ns, w.need_ms);
	while (!needs_.empty() && since(w.now_ns, needs_.front().first) > kNeedWindowNs)
		needs_.pop_front();
	if (w.trouble > 0) {
		++troubled_windows_;
		clean_since_ns_ = w.now_ns;
		clean_.clear();
		last_lower_ns_ = 0;
		trouble_ns_.push_back(w.now_ns);
		while (!trouble_ns_.empty() && since(w.now_ns, trouble_ns_.front()) > kTroubleSpanNs)
			trouble_ns_.pop_front();
		if (static_cast<int>(trouble_ns_.size()) < kTroubleWindowsToRaise)
			return 0;
		if (w.effective_ms < target_ms_)
			return 0; // the previous raise is still sliding
		trouble_ns_.clear();
		const bool paced = have_raised_ && since(w.now_ns, last_raise_ns_) < kRaisePaceNs;
		if (target_ms_ >= ceiling_ms_ && paced)
			return 0; // at most one at-ceiling raise per pacing interval
		raised_ = true;
		have_raised_ = true;
		last_raise_ns_ = w.now_ns;
		if (lowered_)
			edge_floor_ms_ = std::max(edge_floor_ms_, w.effective_ms + kEdgeMarginMs);
		if (target_ms_ >= ceiling_ms_) {
			at_ceiling_ = true;
			ceiling_extra_ms_ = std::min(ceiling_extra_ms_ + kRaiseMs, kLeadHeadroomMs);
			last_raised_ms_ = target_ms_;
			return 0;
		}
		target_ms_ = std::min(std::max(target_ms_, w.effective_ms) + kRaiseMs, ceiling_ms_);
		at_ceiling_ = target_ms_ >= ceiling_ms_;
		last_raised_ms_ = target_ms_;
		return target_ms_;
	}

	clean_.emplace_back(w.now_ns, w.effective_ms);
	if (since(w.now_ns, clean_since_ns_) >= kCleanRunNs) {
		while (!clean_.empty() && since(w.now_ns, clean_.front().first) > kCleanRunNs)
			clean_.pop_front();
		int highest = 0;
		for (const auto &c : clean_)
			highest = std::max(highest, c.second);
		best_clean_ms_ = best_clean_ms_ > 0 ? std::min(best_clean_ms_, highest) : highest;
	}
	if (!dynamic_)
		return 0;
	const uint64_t since_ns = last_lower_ns_ == 0 ? clean_since_ns_ : last_lower_ns_;
	const uint64_t wait = last_lower_ns_ == 0 ? kFirstLowerNs : kNextLowerNs;
	if (since(w.now_ns, since_ns) < wait || w.effective_ms > target_ms_)
		return 0;
	const int floor = floor_ms();
	const int gap = target_ms_ - floor;
	if (gap <= 0)
		return 0;
	const int step = std::clamp(gap / 2 / 5 * 5, kMinLowerMs, kMaxLowerMs);
	const int lower = std::max(target_ms_ - step, floor);
	target_ms_ = lower;
	lowered_ = true;
	last_lower_ns_ = w.now_ns;
	at_ceiling_ = false;
	return target_ms_;
}

int LiveLead::recent_need_ms() const
{
	int highest = 0;
	for (const auto &n : needs_)
		highest = std::max(highest, n.second);
	return highest;
}

int LiveLead::floor_ms() const
{
	return std::max({recent_need_ms() + kNeedMarginMs, edge_floor_ms_, kLeadFloorMs});
}

int LiveLead::next_start_ms() const
{
	if (raised_)
		return std::min(last_raised_ms_ + ceiling_extra_ms_, kLeadCeilingMs);
	if (dynamic_)
		return best_clean_ms_ > 0 ? best_clean_ms_ : start_ms_;
	if (troubled_windows_ == 0 && best_clean_ms_ > 0)
		return std::min(start_ms_, std::max({start_ms_ - kBetweenSessionLowerMs, recent_need_ms() + kNeedMarginMs, kLeadFloorMs}));
	return start_ms_;
}

} // namespace airplay
