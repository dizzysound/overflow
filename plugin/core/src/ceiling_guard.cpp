// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/ceiling_guard.hpp"

#include "airplay/lead_policy.hpp"

#include <algorithm>

namespace airplay {

namespace {
uint64_t since(uint64_t now, uint64_t then)
{
	return now >= then ? now - then : 0;
}
} // namespace

CeilingGuard::Step CeilingGuard::on_window(const CeilingWindow &w, bool may_reconnect, int wanted_ms)
{
	Step step;
	const bool over = w.trouble > 0 && w.ceiling_ms > 0 && w.need_ms > w.ceiling_ms;
	if (!over) {
		over_windows_ = 0;
		if (warning_ && ++good_windows_ >= kWindowsToClear) {
			warning_ = false;
			exhausted_ = false;
			step.warning_cleared = true;
		}
		return step;
	}
	good_windows_ = 0;
	if (over_windows_++ == 0) {
		over_since_ns_ = w.now_ns;
		need_ms_ = 0;
	}
	need_ms_ = std::max(need_ms_, w.need_ms);
	ceiling_ms_ = w.ceiling_ms;
	if (!warning_ && over_windows_ >= kWindowsToWarn) {
		warning_ = true;
		step.warning_started = true;
	}
	if (!warning_ || !may_reconnect || since(w.now_ns, over_since_ns_) < kStableNs)
		return step;
	const int lead = reconnect_lead_ms(wanted_ms, need_ms_);
	// At the 2000 ms limit a new session would get no more room than this one.
	exhausted_ = lead <= w.ceiling_ms || reconnects_ >= kMaxReconnects;
	if (exhausted_)
		return step;
	if (reconnects_ > 0 && since(w.now_ns, last_reconnect_ns_) < kBackoffNs[reconnects_])
		return step;
	++reconnects_;
	last_reconnect_ns_ = w.now_ns;
	over_windows_ = 0; // the new session's windows start a new span
	step.reconnect_ms = lead;
	return step;
}

int reconnect_lead_ms(int wanted_ms, int need_ms)
{
	int lead = std::max(wanted_ms, need_ms + CeilingGuard::kReconnectMarginMs);
	lead = ((lead + 4) / 5) * 5;
	return std::clamp(lead, kLeadFloorMs, kLeadCeilingMs);
}

std::string ceiling_warning_text(const std::string &display_name, const CeilingGuard &guard, bool fixed)
{
	std::string text = display_name + ": audio or video dropped late: needs about " +
			   std::to_string(guard.need_ms()) + " ms of TV delay, above the " +
			   std::to_string(guard.ceiling_ms()) + " ms this session can reach.";
	if (fixed)
		text += " The TV delay is fixed: raise it in Display settings.";
	else if (guard.exhausted())
		text += " Overflow cannot raise it again this run; restarting OBS clears audio buffering OBS added.";
	else
		text += " Overflow reconnects it with a higher TV delay if this lasts 30 s.";
	return text;
}

} // namespace airplay
