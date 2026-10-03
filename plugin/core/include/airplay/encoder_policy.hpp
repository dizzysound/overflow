// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Which H.264 encoder the AirPlay output uses and how it is configured.
#pragma once

#include "airplay/settings.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace airplay {

// NVENC, then AMD AMF, then Intel Quick Sync, then Apple VideoToolbox
// (hardware only), then VAAPI, then x264 (OBS 32.2.2 IDs).
const std::vector<std::string> &encoder_fallback_order();

struct EncoderPlan {
	std::vector<std::string> candidates; // try in this order
	bool override_ignored = false;       // an override was set but is not available
};

EncoderPlan plan_encoders(const std::vector<std::string> &available, const std::string &override_id);

struct PresetSpec {
	uint32_t max_width;
	uint32_t max_height;
	int bitrate_kbps;
};

PresetSpec preset_spec(QualityPreset p);

struct ScaledSize {
	uint32_t width;
	uint32_t height;
	bool scaled;
};

// Fits width x height inside max_width x max_height, keeping the aspect ratio.
// Never upscales; scaled dimensions are even.
ScaledSize fit_within(uint32_t width, uint32_t height, uint32_t max_width, uint32_t max_height);

using SettingValue = std::variant<int64_t, bool, std::string>;
using EncoderSettings = std::vector<std::pair<std::string, SettingValue>>;

// obs_data settings for encoder_id: CBR at bitrate_kbps, no B-frames, 1 s
// keyframes, SPS/PPS repeated in-band, low-latency tuning.
//
// `repeat_headers` (set true below for every family) is what makes each
// encoder put SPS/PPS on every keyframe instead of once at stream start.
// Task 3's extradata_to_annexb handles both Annex-B and avcC.
EncoderSettings encoder_settings(const std::string &encoder_id, int bitrate_kbps);

} // namespace airplay
