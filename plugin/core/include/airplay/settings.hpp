// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Persisted plugin settings: global options and one entry per display, keyed by
// the receiver's device ID.
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace airplay {

inline constexpr const char *kLeadModeDynamic = "dynamic";

enum class IdlePolicy { StayConnected, DisconnectAfterIdle, DisconnectWhenOutputStops };
// One shared encoder, set globally: not per display.
enum class QualityPreset { P1080_6Mbps, P1080_8Mbps, P1080_10Mbps, P720_4Mbps };
enum class TimingMode { Auto, Ntp };

const char *to_string(IdlePolicy p);
const char *to_string(QualityPreset p);
const char *to_string(TimingMode t);
bool parse_idle_policy(const std::string &s, IdlePolicy *out);
bool parse_quality_preset(const std::string &s, QualityPreset *out);
bool parse_timing_mode(const std::string &s, TimingMode *out);

constexpr double kMinVolumeDb = -30.0;
constexpr double kMaxVolumeDb = 0.0;

struct DisplaySettings {
	std::string device_id;    // uppercased ASCII; the helper normalizes to uppercase on the wire
	std::string display_name;
	std::string location; // optional group heading in the dock, e.g. "Friendship Hall"
	bool enabled = false;
	bool auto_reconnect = true;
	bool audio_enabled = true; // false: video only for this display; volume is never touched
	bool wifi_tolerant = false; // true: a Wi-Fi display; 50 ms more TV delay to ride out brief stalls (lead_policy)
	int latency_ms = 0;      // this display's TV delay; 0 = Auto (lead_policy)
	std::string lead_mode;   // Auto TV delay: "" = raise only (default), "dynamic" = slides down too
	int saved_lead_ms = 0;   // persisted: Auto's starting TV delay for the next session (0 = none yet)
	std::string audio_format; // forced screen-audio codec: "alac" or "aac-eld"; empty = Auto (the receiver's /info)
	int video_need_ms = 0;   // persisted, non-user-facing: learned capture-to-write p99
	int last_run_p99_ms = 0; // persisted, non-user-facing: this run's max p99, for learning at next start
	int late_floor_ms = 0;   // persisted, non-user-facing: lead reached by a late-frame raise (lead_policy)
	int last_run_late_windows = 0; // persisted, non-user-facing: last run's delivery windows with late frames
	std::optional<double> volume_db; // unset: never change the receiver's volume
	std::string manual_ip;           // empty: use discovery
	int manual_port = 0;             // 0: the helper's default, 7000
	IdlePolicy idle_policy = IdlePolicy::DisconnectWhenOutputStops;
	int idle_minutes = 30;
	std::string password_protected; // base64 of a DPAPI blob; empty = none
};

struct GlobalSettings {
	int audio_track = 1; // OBS track 1-6
	QualityPreset preset = QualityPreset::P1080_6Mbps;
	std::string encoder_override; // empty: NVENC > AMF > QSV > x264
	bool start_on_launch = false;
	bool start_with_streaming = false;
	bool start_with_recording = false;
	// 0: each display's own TV delay (DisplaySettings::latency_ms, Auto by
	// default). Otherwise a legacy explicit lead that every Auto display uses.
	int target_latency_ms = 0;
	// Unused since per-display leads (spec 2026-09-28 section 6): kept only so
	// settings files keep loading and saving the field unchanged. The
	// controller neither reads nor writes it.
	int auto_latency_ms = 0;
	// Persisted, non-user-facing: max OBS audio age seen in the last run, ms;
	// used until the live tap reports.
	int last_audio_age_ms = 0;
	TimingMode timing = TimingMode::Auto;
	bool eld_encoder = true;
	// When true, the controller launches the helper with -debug
	// (HelperLaunch::debug); for troubleshooting only. Changing it restarts
	// the helper, like timing and eld_encoder.
	bool verbose_helper_log = false;
};

bool operator==(const DisplaySettings &a, const DisplaySettings &b);
bool operator!=(const DisplaySettings &a, const DisplaySettings &b);
bool operator==(const GlobalSettings &a, const GlobalSettings &b);
bool operator!=(const GlobalSettings &a, const GlobalSettings &b);

struct Settings {
	int version = 1;
	GlobalSettings global;
	std::vector<DisplaySettings> displays;

	// device_id is uppercased (ASCII) before lookup.
	DisplaySettings *find_display(const std::string &device_id);
	const DisplaySettings *find_display(const std::string &device_id) const;
	// Returns the display, adding it (disabled, with display_name) if missing.
	// device_id is uppercased (ASCII) before lookup or insertion.
	DisplaySettings &ensure_display(const std::string &device_id, const std::string &display_name);
	// device_id is uppercased (ASCII) before lookup.
	bool remove_display(const std::string &device_id);
};

std::string settings_to_json(const Settings &settings);
// Tolerant: bad fields, including an unrecognized quality preset, fall back to
// their defaults silently. Invalid JSON yields all defaults and sets *warning.
Settings settings_from_json(const std::string &text, std::string *warning);

} // namespace airplay
