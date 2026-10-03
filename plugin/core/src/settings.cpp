// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/settings.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <set>

namespace airplay {
namespace {

using nlohmann::json;
using OrderedJson = nlohmann::ordered_json;

std::string to_upper_ascii(const std::string &s)
{
	std::string out = s;
	for (char &c : out)
		c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
	return out;
}

bool get_bool(const json &j, const char *key, bool def)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_boolean()) ? it->get<bool>() : def;
}

int get_int(const json &j, const char *key, int def, int lo, int hi)
{
	const auto it = j.find(key);
	if (it == j.end() || !it->is_number_integer())
		return def;
	const int64_t v = it->get<int64_t>();
	return (v < lo || v > hi) ? def : static_cast<int>(v);
}

std::string get_str(const json &j, const char *key)
{
	const auto it = j.find(key);
	return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

std::optional<double> get_volume(const json &j)
{
	const auto it = j.find("volume_db");
	if (it == j.end() || !it->is_number())
		return std::nullopt;
	const double v = it->get<double>();
	if (!(v >= kMinVolumeDb && v <= kMaxVolumeDb))
		return std::nullopt;
	return v;
}

// Non-empty, distinct names, in saved order; anything else is dropped.
std::vector<std::string> get_scene_names(const json &j)
{
	std::vector<std::string> out;
	const auto it = j.find("skip_scenes");
	if (it == j.end() || !it->is_array())
		return out;
	for (const json &v : *it) {
		if (!v.is_string())
			continue;
		std::string name = v.get<std::string>();
		if (!name.empty() && std::find(out.begin(), out.end(), name) == out.end())
			out.push_back(std::move(name));
	}
	return out;
}

} // namespace

const char *to_string(IdlePolicy p)
{
	switch (p) {
	case IdlePolicy::StayConnected:
		return "stay";
	case IdlePolicy::DisconnectAfterIdle:
		return "after_idle";
	case IdlePolicy::DisconnectWhenOutputStops:
		break;
	}
	return "when_output_stops";
}

const char *to_string(QualityPreset p)
{
	switch (p) {
	case QualityPreset::P1080_8Mbps:
		return "1080p_8mbps";
	case QualityPreset::P1080_10Mbps:
		return "1080p_10mbps";
	case QualityPreset::P720_4Mbps:
		return "720p_4mbps";
	case QualityPreset::P1080_6Mbps:
		break;
	}
	return "1080p_6mbps";
}

const char *to_string(TimingMode t)
{
	return t == TimingMode::Ntp ? "ntp" : "auto";
}

bool parse_idle_policy(const std::string &s, IdlePolicy *out)
{
	for (IdlePolicy p : {IdlePolicy::StayConnected, IdlePolicy::DisconnectAfterIdle,
			     IdlePolicy::DisconnectWhenOutputStops}) {
		if (s == to_string(p)) {
			*out = p;
			return true;
		}
	}
	return false;
}

bool parse_quality_preset(const std::string &s, QualityPreset *out)
{
	for (QualityPreset p : {QualityPreset::P1080_6Mbps, QualityPreset::P1080_8Mbps, QualityPreset::P1080_10Mbps,
			     QualityPreset::P720_4Mbps}) {
		if (s == to_string(p)) {
			*out = p;
			return true;
		}
	}
	return false;
}

bool parse_timing_mode(const std::string &s, TimingMode *out)
{
	if (s == "auto" || s == "ntp") {
		*out = s == "ntp" ? TimingMode::Ntp : TimingMode::Auto;
		return true;
	}
	return false;
}

bool operator==(const DisplaySettings &a, const DisplaySettings &b)
{
	return a.device_id == b.device_id && a.display_name == b.display_name && a.location == b.location &&
	       a.enabled == b.enabled &&
	       a.auto_reconnect == b.auto_reconnect && a.audio_enabled == b.audio_enabled &&
	       a.wifi_tolerant == b.wifi_tolerant && a.latency_ms == b.latency_ms && a.lead_mode == b.lead_mode && a.saved_lead_ms == b.saved_lead_ms && a.audio_format == b.audio_format &&
	       a.video_need_ms == b.video_need_ms && a.last_run_p99_ms == b.last_run_p99_ms &&
	       a.late_floor_ms == b.late_floor_ms && a.last_run_late_windows == b.last_run_late_windows &&
	       a.volume_db == b.volume_db && a.manual_ip == b.manual_ip &&
	       a.manual_port == b.manual_port && a.idle_policy == b.idle_policy && a.idle_minutes == b.idle_minutes &&
	       a.password_protected == b.password_protected && a.skip_scenes == b.skip_scenes;
}

bool operator!=(const DisplaySettings &a, const DisplaySettings &b)
{
	return !(a == b);
}

void merge_dialog_fields(DisplaySettings &target, const DisplaySettings &edited)
{
	target.display_name = edited.display_name;
	target.location = edited.location;
	target.auto_reconnect = edited.auto_reconnect;
	target.audio_enabled = edited.audio_enabled;
	target.wifi_tolerant = edited.wifi_tolerant;
	target.latency_ms = edited.latency_ms;
	target.lead_mode = edited.lead_mode;
	target.audio_format = edited.audio_format;
	target.volume_db = edited.volume_db;
	target.manual_ip = edited.manual_ip;
	target.manual_port = edited.manual_port;
	target.idle_policy = edited.idle_policy;
	target.idle_minutes = edited.idle_minutes;
	target.skip_scenes = edited.skip_scenes;
}

bool operator==(const GlobalSettings &a, const GlobalSettings &b)
{
	return a.audio_track == b.audio_track && a.preset == b.preset && a.encoder_override == b.encoder_override &&
	       a.start_on_launch == b.start_on_launch && a.start_with_streaming == b.start_with_streaming &&
	       a.start_with_recording == b.start_with_recording && a.target_latency_ms == b.target_latency_ms &&
	       a.auto_latency_ms == b.auto_latency_ms && a.last_audio_age_ms == b.last_audio_age_ms &&
	       a.timing == b.timing && a.eld_encoder == b.eld_encoder && a.verbose_helper_log == b.verbose_helper_log;
}

bool operator!=(const GlobalSettings &a, const GlobalSettings &b)
{
	return !(a == b);
}

DisplaySettings *Settings::find_display(const std::string &device_id)
{
	const std::string id = to_upper_ascii(device_id);
	for (DisplaySettings &r : displays)
		if (r.device_id == id)
			return &r;
	return nullptr;
}

const DisplaySettings *Settings::find_display(const std::string &device_id) const
{
	const std::string id = to_upper_ascii(device_id);
	for (const DisplaySettings &r : displays)
		if (r.device_id == id)
			return &r;
	return nullptr;
}

DisplaySettings &Settings::ensure_display(const std::string &device_id, const std::string &display_name)
{
	const std::string id = to_upper_ascii(device_id);
	if (DisplaySettings *r = find_display(id))
		return *r;
	DisplaySettings r;
	r.device_id = id;
	r.display_name = display_name;
	displays.push_back(std::move(r));
	return displays.back();
}

bool Settings::remove_display(const std::string &device_id)
{
	const std::string id = to_upper_ascii(device_id);
	const auto it = std::remove_if(displays.begin(), displays.end(),
				       [&](const DisplaySettings &r) { return r.device_id == id; });
	if (it == displays.end())
		return false;
	displays.erase(it, displays.end());
	return true;
}

std::string settings_to_json(const Settings &s)
{
	OrderedJson g;
	g["audio_track"] = s.global.audio_track;
	g["preset"] = to_string(s.global.preset);
	g["encoder_override"] = s.global.encoder_override;
	g["start_on_launch"] = s.global.start_on_launch;
	g["start_with_streaming"] = s.global.start_with_streaming;
	g["start_with_recording"] = s.global.start_with_recording;
	g["target_latency_ms"] = s.global.target_latency_ms;
	g["auto_latency_ms"] = s.global.auto_latency_ms;
	g["last_audio_age_ms"] = s.global.last_audio_age_ms;
	g["timing"] = to_string(s.global.timing);
	g["eld_encoder"] = s.global.eld_encoder;
	g["verbose_helper_log"] = s.global.verbose_helper_log;

	OrderedJson displays = OrderedJson::array();
	for (const DisplaySettings &r : s.displays) {
		OrderedJson e;
		e["device_id"] = r.device_id;
		e["display_name"] = r.display_name;
		e["location"] = r.location;
		e["enabled"] = r.enabled;
		e["auto_reconnect"] = r.auto_reconnect;
		e["audio_enabled"] = r.audio_enabled;
		e["wifi_tolerant"] = r.wifi_tolerant;
		e["latency_ms"] = r.latency_ms;
		e["audio_format"] = r.audio_format;
		if (!r.lead_mode.empty())
			e["lead_mode"] = r.lead_mode;
		e["saved_lead_ms"] = r.saved_lead_ms;
		e["video_need_ms"] = r.video_need_ms;
		e["last_run_p99_ms"] = r.last_run_p99_ms;
		e["late_floor_ms"] = r.late_floor_ms;
		e["last_run_late_windows"] = r.last_run_late_windows;
		e["volume_db"] = r.volume_db ? OrderedJson(*r.volume_db) : OrderedJson(nullptr);
		e["manual_ip"] = r.manual_ip;
		e["manual_port"] = r.manual_port;
		e["idle_policy"] = to_string(r.idle_policy);
		e["idle_minutes"] = r.idle_minutes;
		e["password_protected"] = r.password_protected;
		e["skip_scenes"] = r.skip_scenes;
		displays.push_back(std::move(e));
	}

	OrderedJson j;
	j["version"] = s.version;
	j["global"] = std::move(g);
	j["displays"] = std::move(displays);
	return j.dump(2, ' ', false, OrderedJson::error_handler_t::replace) + "\n";
}

Settings settings_from_json(const std::string &text, std::string *warning)
{
	Settings s;
	const json j = json::parse(text, nullptr, false);
	if (j.is_discarded() || !j.is_object()) {
		if (warning)
			*warning = "Overflow settings are not valid JSON; using defaults";
		return s;
	}

	const auto g = j.find("global");
	if (g != j.end() && g->is_object()) {
		s.global.audio_track = get_int(*g, "audio_track", 1, 1, 6);

		// An unrecognized preset (an older name, or one from a newer build) falls
		// back to the default silently, like every other field.
		const auto preset_it = g->find("preset");
		if (preset_it != g->end() && preset_it->is_string() &&
		    !parse_quality_preset(preset_it->get<std::string>(), &s.global.preset))
			s.global.preset = QualityPreset::P1080_6Mbps;

		s.global.encoder_override = get_str(*g, "encoder_override");
		s.global.start_on_launch = get_bool(*g, "start_on_launch", false);
		s.global.start_with_streaming = get_bool(*g, "start_with_streaming", false);
		s.global.start_with_recording = get_bool(*g, "start_with_recording", false);
		s.global.target_latency_ms = get_int(*g, "target_latency_ms", 0, 0, 2000);
		s.global.auto_latency_ms = get_int(*g, "auto_latency_ms", 0, 0, 2000);
		s.global.last_audio_age_ms = get_int(*g, "last_audio_age_ms", 0, 0, 2000);
		parse_timing_mode(get_str(*g, "timing"), &s.global.timing);
		s.global.eld_encoder = get_bool(*g, "eld_encoder", true);
		s.global.verbose_helper_log = get_bool(*g, "verbose_helper_log", false);
	}

	const auto displays = j.find("displays");
	if (displays != j.end() && displays->is_array()) {
		std::set<std::string> seen;
		for (const json &e : *displays) {
			if (!e.is_object())
				continue;
			DisplaySettings r;
			r.device_id = to_upper_ascii(get_str(e, "device_id"));
			if (r.device_id.empty() || !seen.insert(r.device_id).second)
				continue;
			r.display_name = get_str(e, "display_name");
			r.location = get_str(e, "location");
			r.enabled = get_bool(e, "enabled", false);
			r.auto_reconnect = get_bool(e, "auto_reconnect", true);
			r.audio_enabled = get_bool(e, "audio_enabled", true);
			r.wifi_tolerant = get_bool(e, "wifi_tolerant", false);
			r.latency_ms = get_int(e, "latency_ms", 0, 0, 2000);
			r.audio_format = get_str(e, "audio_format");
			if (r.audio_format != "alac" && r.audio_format != "aac-eld")
				r.audio_format.clear();
			r.video_need_ms = get_int(e, "video_need_ms", 0, 0, 2000);
			r.last_run_p99_ms = get_int(e, "last_run_p99_ms", 0, 0, 2000);
			r.late_floor_ms = get_int(e, "late_floor_ms", 0, 0, 2000);
			r.saved_lead_ms = get_int(e, "saved_lead_ms", 0, 0, 2000);
			r.lead_mode = get_str(e, "lead_mode");
			if (r.lead_mode != kLeadModeDynamic)
				r.lead_mode.clear();
			if (r.saved_lead_ms == 0 && r.late_floor_ms > 0)
				r.saved_lead_ms = r.late_floor_ms; // pre-1.4 migration
			r.last_run_late_windows = get_int(e, "last_run_late_windows", 0, 0, 1000000);
			r.volume_db = get_volume(e);
			r.manual_ip = get_str(e, "manual_ip");
			r.manual_port = get_int(e, "manual_port", 0, 0, 65535);
			parse_idle_policy(get_str(e, "idle_policy"), &r.idle_policy);
			r.idle_minutes = get_int(e, "idle_minutes", 30, 1, 1440);
			r.password_protected = get_str(e, "password_protected");
			r.skip_scenes = get_scene_names(e);
			s.displays.push_back(std::move(r));
		}
	}
	return s;
}

} // namespace airplay
