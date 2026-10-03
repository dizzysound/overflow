// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// When the AirPlay output runs (auto-start) and which displays are selected
// (per-display idle policy). Pure logic; the controller feeds it OBS events.
#pragma once

#include "airplay/commands.hpp"
#include "airplay/settings.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace airplay {

struct AutoStartOptions {
	bool on_launch = false;
	bool with_streaming = false;
	bool with_recording = false;
};

AutoStartOptions autostart_options(const GlobalSettings &g);

class AutoStart {
public:
	explicit AutoStart(AutoStartOptions options = {});
	void set_options(AutoStartOptions options);
	void obs_loaded();
	void streaming(bool active);
	void recording(bool active);
	void user_start();
	void user_stop();
	bool should_run() const;

private:
	AutoStartOptions options_;
	bool loaded_ = false;
	bool streaming_ = false;
	bool recording_ = false;
	bool manual_ = false;
	bool suppressed_ = false;
};

// Idle is "OBS is neither streaming nor recording and the program scene has
// not changed for N minutes." Chosen by Chris (2026-09-26).
struct IdleInputs {
	bool output_running = false;
	bool ever_started = false; // the output has run in this OBS session
	bool obs_busy = false;     // OBS is streaming or recording
	uint64_t now_ns = 0;
	uint64_t last_activity_ns = 0;
	uint64_t output_stopped_since_ns = 0;
};

bool display_wanted(const DisplaySettings &display, const IdleInputs &in);
std::vector<DisplaySelection> selection_for(const Settings &settings, const IdleInputs &in,
                                             const std::map<std::string, int> &lead_ms_by_device);

} // namespace airplay
