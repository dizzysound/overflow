// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/session_policy.hpp"

namespace airplay {
namespace {

bool within(uint64_t now, uint64_t since, int minutes)
{
	const uint64_t window = static_cast<uint64_t>(minutes) * 60ull * 1000000000ull;
	const uint64_t elapsed = now > since ? now - since : 0;
	return elapsed < window;
}

} // namespace

AutoStartOptions autostart_options(const GlobalSettings &g)
{
	return {g.start_on_launch, g.start_with_streaming, g.start_with_recording};
}

AutoStart::AutoStart(AutoStartOptions options) : options_(options) {}

void AutoStart::set_options(AutoStartOptions options)
{
	options_ = options;
}

void AutoStart::obs_loaded()
{
	loaded_ = true;
}

void AutoStart::streaming(bool active)
{
	if (active && !streaming_ && options_.with_streaming)
		suppressed_ = false;
	streaming_ = active;
}

void AutoStart::recording(bool active)
{
	if (active && !recording_ && options_.with_recording)
		suppressed_ = false;
	recording_ = active;
}

void AutoStart::user_start()
{
	manual_ = true;
	suppressed_ = false;
}

void AutoStart::user_stop()
{
	manual_ = false;
	suppressed_ = true;
}

bool AutoStart::should_run() const
{
	if (manual_)
		return true;
	if (suppressed_)
		return false;
	return (options_.on_launch && loaded_) || (options_.with_streaming && streaming_) ||
	       (options_.with_recording && recording_);
}

bool display_wanted(const DisplaySettings &display, const IdleInputs &in)
{
	if (!display.enabled)
		return false;
	if (in.output_running) {
		if (display.idle_policy != IdlePolicy::DisconnectAfterIdle || in.obs_busy)
			return true;
		return within(in.now_ns, in.last_activity_ns, display.idle_minutes);
	}
	if (!in.ever_started)
		return false;
	switch (display.idle_policy) {
	case IdlePolicy::StayConnected:
		return true;
	case IdlePolicy::DisconnectWhenOutputStops:
		return false;
	case IdlePolicy::DisconnectAfterIdle:
		return within(in.now_ns, in.output_stopped_since_ns, display.idle_minutes);
	}
	return false;
}

std::vector<DisplaySelection> selection_for(const Settings &settings, const IdleInputs &in,
                                             const std::map<std::string, int> &lead_ms_by_device)
{
	std::vector<DisplaySelection> out;
	for (const DisplaySettings &r : settings.displays) {
		if (!display_wanted(r, in))
			continue;
		DisplaySelection s;
		s.device_id = r.device_id;
		s.auto_reconnect = r.auto_reconnect;
		s.audio = r.audio_enabled;
		s.wifi_tolerant = r.wifi_tolerant;
		s.audio_format = r.audio_format;
		const auto lead = lead_ms_by_device.find(r.device_id);
		s.latency_ms = lead == lead_ms_by_device.end() ? 0 : lead->second;
		s.volume_db = r.volume_db;
		s.ip = r.manual_ip;
		s.port = r.manual_ip.empty() ? 0 : r.manual_port;
		out.push_back(std::move(s));
	}
	return out;
}

} // namespace airplay
