// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/lead_policy.hpp"

#include <algorithm>

namespace airplay {

int display_lead_ms(const LeadInputs &in)
{
	if (in.fixed_ms > 0)
		return std::clamp(in.fixed_ms, kLeadFloorMs, kLeadCeilingMs);
	if (in.global_fixed_ms > 0)
		return std::clamp(in.global_fixed_ms, kLeadFloorMs, kLeadCeilingMs);
	if (in.saved_lead_ms > 0)
		return std::clamp(in.saved_lead_ms, kLeadFloorMs, kLeadCeilingMs);
	const int video = in.video_need_ms > 0 ? in.video_need_ms : kDefaultVideoNeedMs;
	int need = video;
	if (in.audio_on) {
		const uint64_t age_ms = in.audio_age_ns / 1000000ull;
		const uint64_t audio = std::min<uint64_t>(age_ms, kLeadCeilingMs) + kEldFrameMs + kAudioPathMarginMs;
		need = std::max(need, static_cast<int>(audio));
	}
	int lead = need + (in.wifi ? kWifiMarginMs : kWiredMarginMs);
	lead = ((lead + 4) / 5) * 5;
	lead = std::max(lead, in.late_floor_ms);
	return std::clamp(lead, kLeadFloorMs, kLeadCeilingMs);
}

int raise_video_need(int stored_ms, int observed_p99_ms, int cap_ms)
{
	const int observed = cap_ms > 0 ? std::min(observed_p99_ms, cap_ms) : observed_p99_ms;
	return std::clamp(std::max(stored_ms, observed), 0, kLeadCeilingMs);
}

int learn_video_need_at_start(int stored_ms, int last_run_p99_ms)
{
	if (last_run_p99_ms > 0 && last_run_p99_ms + 30 <= stored_ms)
		return last_run_p99_ms;
	return stored_ms;
}

bool lead_needs_restart(int applied_ms, int wanted_ms)
{
	return wanted_ms >= applied_ms + kLeadRestartStepMs;
}

int late_floor_after_raise(int applied_ms)
{
	return std::clamp(applied_ms + kLateRaiseMs, 0, kLeadCeilingMs);
}

int learn_late_floor_at_start(int floor_ms, int last_run_late_windows)
{
	return last_run_late_windows == 0 ? 0 : floor_ms;
}

int seed_audio_age_ms(int last_audio_age_ms, int legacy_auto_latency_ms)
{
	if (last_audio_age_ms > 0 || legacy_auto_latency_ms <= kLegacyAutoLatencyMarginMs)
		return last_audio_age_ms;
	return std::min(legacy_auto_latency_ms - kLegacyAutoLatencyMarginMs, kLeadCeilingMs);
}

bool LateWindows::record(uint64_t now_ns, int late_frames)
{
	recent_ns_.erase(std::remove_if(recent_ns_.begin(), recent_ns_.end(),
					[now_ns](uint64_t t) { return now_ns - t > kWindowNs; }),
			 recent_ns_.end());
	if (late_frames <= 0)
		return false;
	++run_late_windows_;
	recent_ns_.push_back(now_ns);
	if (static_cast<int>(recent_ns_.size()) <= kWindowsToRaise)
		return false;
	recent_ns_.clear();
	return true;
}

void AudioLoss::record(uint64_t now_ns, int lost, int resent, int dropped)
{
	seen_ = true;
	recent_.erase(std::remove_if(recent_.begin(), recent_.end(),
				     [now_ns](const Sample &s) { return now_ns - s.ns > kWindowNs; }),
		      recent_.end());
	lost = std::max(0, lost);
	dropped = std::max(0, dropped);
	if (lost == 0 && dropped == 0)
		return;
	const Counts c{lost, std::max(0, lost - resent), dropped};
	recent_.push_back({now_ns, c});
	run_.lost += c.lost;
	run_.not_resent += c.not_resent;
	run_.dropped += c.dropped;
}

int lead_trouble(int late_frames, int audio_lost, int audio_resent, int audio_dropped)
{
	return std::max(0, late_frames) + std::max(0, audio_dropped) + std::max(0, audio_lost - audio_resent);
}

AudioLoss::Counts AudioLoss::last_minute(uint64_t now_ns) const
{
	Counts total;
	for (const Sample &s : recent_) {
		if (now_ns - s.ns > kWindowNs)
			continue;
		total.lost += s.counts.lost;
		total.not_resent += s.counts.not_resent;
		total.dropped += s.counts.dropped;
	}
	return total;
}

std::string audio_loss_text(const AudioLoss &loss, uint64_t now_ns)
{
	const AudioLoss::Counts minute = loss.last_minute(now_ns);
	if (minute.lost == 0 && minute.dropped == 0) {
		std::string text = "no audio loss in the last minute";
		if (loss.run().lost + loss.run().dropped > 0)
			text += ", " + std::to_string(loss.run().lost + loss.run().dropped) + " this run";
		return text;
	}
	std::string text = "audio";
	if (minute.lost > 0)
		text += " lost " + std::to_string(minute.lost);
	if (minute.dropped > 0)
		text += std::string(minute.lost > 0 ? "," : "") + " " + std::to_string(minute.dropped) + " dropped late";
	text += " in the last minute";
	if (minute.not_resent > 0)
		text += " (" + std::to_string(minute.not_resent) + " not resent)";
	const int run = loss.run().lost + loss.run().dropped;
	if (run > minute.lost + minute.dropped)
		text += ", " + std::to_string(run) + " this run";
	return text;
}

bool LateWindows::take_restart(uint64_t now_ns)
{
	if (restarted_ && now_ns - last_restart_ns_ < kRestartIntervalNs)
		return false;
	restarted_ = true;
	last_restart_ns_ = now_ns;
	return true;
}

} // namespace airplay
