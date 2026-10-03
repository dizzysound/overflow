// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/encoder_policy.hpp"

#include <algorithm>

namespace airplay {
namespace {

enum class Family { Nvenc, Amf, Qsv, X264, Generic };

bool starts_with(const std::string &s, const char *prefix)
{
	return s.rfind(prefix, 0) == 0;
}

Family family_of(const std::string &id)
{
	if (starts_with(id, "obs_nvenc_"))
		return Family::Nvenc;
	if (id.find("_amf") != std::string::npos)
		return Family::Amf;
	if (starts_with(id, "obs_qsv11"))
		return Family::Qsv;
	if (id == "obs_x264")
		return Family::X264;
	return Family::Generic;
}

} // namespace

const std::vector<std::string> &encoder_fallback_order()
{
	static const std::vector<std::string> order{"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2",
						      "obs_x264"};
	return order;
}

EncoderPlan plan_encoders(const std::vector<std::string> &available, const std::string &override_id)
{
	EncoderPlan plan;
	auto has = [&](const std::string &id) {
		return std::find(available.begin(), available.end(), id) != available.end();
	};
	auto listed = [&](const std::string &id) {
		return std::find(plan.candidates.begin(), plan.candidates.end(), id) != plan.candidates.end();
	};
	if (!override_id.empty()) {
		if (has(override_id))
			plan.candidates.push_back(override_id);
		else
			plan.override_ignored = true;
	}
	for (const std::string &id : encoder_fallback_order())
		if (has(id) && !listed(id))
			plan.candidates.push_back(id);
	return plan;
}

PresetSpec preset_spec(QualityPreset p)
{
	// Every QualityPreset case is listed explicitly so a new enumerator
	// trips -Wswitch on Clang and GCC (an error via -Werror) instead of
	// silently falling through to the default below.
	switch (p) {
	case QualityPreset::P1080_6Mbps:
		return {1920, 1080, 6000};
	case QualityPreset::P1080_8Mbps:
		return {1920, 1080, 8000};
	case QualityPreset::P1080_10Mbps:
		return {1920, 1080, 10000};
	case QualityPreset::P720_4Mbps:
		return {1280, 720, 4000};
	}
	return {1920, 1080, 6000}; // default/fallthrough: P1080_6Mbps
}

ScaledSize fit_within(uint32_t width, uint32_t height, uint32_t max_width, uint32_t max_height)
{
	if (width == 0 || height == 0)
		return {width, height, false};
	if (width <= max_width && height <= max_height) {
		// No downscale needed, but H.264 4:2:0 needs even dimensions, so an odd
		// canvas is trimmed by one pixel (and reported as scaled).
		const uint32_t w = width & ~1u;
		const uint32_t h = height & ~1u;
		if (w == 0 || h == 0)
			return {width, height, false};
		return {w, h, w != width || h != height};
	}
	const double scale = std::min(static_cast<double>(max_width) / static_cast<double>(width),
				       static_cast<double>(max_height) / static_cast<double>(height));
	uint32_t w = static_cast<uint32_t>(static_cast<double>(width) * scale) & ~1u;
	uint32_t h = static_cast<uint32_t>(static_cast<double>(height) * scale) & ~1u;
	if (w < 2)
		w = 2;
	if (h < 2)
		h = 2;
	return {w, h, true};
}

// obs-nvenc's scheduled keyframe interval. Long, as Apple's senders run, because
// the helper requests IDRs on demand (join, backlog, receiver). Confirmed on the
// streaming PC 2026-10-01: every forced request produced an IDR in 40-49 ms.
constexpr int64_t kNvencKeyintSec = 10;

EncoderSettings encoder_settings(const std::string &encoder_id, int bitrate_kbps)
{
	EncoderSettings s;
	auto add_int = [&](const char *key, int64_t v) { s.emplace_back(key, SettingValue(v)); };
	auto add_bool = [&](const char *key, bool v) { s.emplace_back(key, SettingValue(v)); };
	auto add_str = [&](const char *key, const char *v) { s.emplace_back(key, SettingValue(std::string(v))); };

	const int64_t kbps = bitrate_kbps;
	add_int("bitrate", kbps);
	// obs-nvenc turns every live update into an IDR, so the helper can ask for
	// keyframes on demand (display join, backlog resync, receiver request) and
	// the scheduled interval can be long, as Apple's senders do. Other encoder
	// families reconfigure without an IDR, so they keep a 1 s interval.
	add_int("keyint_sec", family_of(encoder_id) == Family::Nvenc ? kNvencKeyintSec : 1);
	add_bool("repeat_headers", true);
	add_str("profile", "high");

	switch (family_of(encoder_id)) {
	case Family::Nvenc:
		add_str("rate_control", "cbr");
		add_int("max_bitrate", kbps);
		add_int("bf", 0);
		add_str("preset", "p4");
		add_str("tune", "ll");
		add_str("multipass", "disabled");
		add_bool("lookahead", false);
		break;
	case Family::Amf:
		add_str("rate_control", "CBR");
		add_int("bf", 0);
		add_str("preset", "balanced");
		break;
	case Family::Qsv:
		add_str("rate_control", "CBR");
		add_int("max_bitrate", kbps);
		add_int("bframes", 0);
		add_int("bf", 0);
		add_str("target_usage", "TU4");
		add_str("latency", "low");
		break;
	case Family::X264:
		add_str("rate_control", "CBR");
		add_int("bf", 0);
		add_str("preset", "veryfast");
		add_str("tune", "zerolatency");
		break;
	case Family::Generic:
		add_str("rate_control", "CBR");
		add_int("bf", 0);
		break;
	}
	return s;
}

} // namespace airplay
