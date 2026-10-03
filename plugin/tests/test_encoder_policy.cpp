// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/encoder_policy.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using namespace airplay;

namespace {
const SettingValue *find(const EncoderSettings &s, const std::string &key)
{
	for (const auto &kv : s)
		if (kv.first == key)
			return &kv.second;
	return nullptr;
}
std::string str(const EncoderSettings &s, const std::string &key)
{
	const SettingValue *v = find(s, key);
	return v && std::holds_alternative<std::string>(*v) ? std::get<std::string>(*v) : std::string("<missing>");
}
int64_t num(const EncoderSettings &s, const std::string &key)
{
	const SettingValue *v = find(s, key);
	return v && std::holds_alternative<int64_t>(*v) ? std::get<int64_t>(*v) : -999;
}
bool flag(const EncoderSettings &s, const std::string &key)
{
	const SettingValue *v = find(s, key);
	REQUIRE(v);
	REQUIRE(std::holds_alternative<bool>(*v));
	return std::get<bool>(*v);
}
} // namespace

TEST_CASE("fallback order is NVENC, AMF, QSV, VideoToolbox, VAAPI, x264")
{
	CHECK(encoder_fallback_order() ==
	      std::vector<std::string>{"obs_nvenc_h264_tex", "h264_texture_amf", "obs_qsv11_v2",
				       "com.apple.videotoolbox.videoencoder.ave.avc",
				       "com.apple.videotoolbox.videoencoder.h264.gva", "ffmpeg_vaapi_tex", "obs_x264"});
}

TEST_CASE("macOS: hardware VideoToolbox goes ahead of x264; software VideoToolbox is not chosen")
{
	// What OBS 32.2.2 registers for H.264 on an Apple Silicon Mac (macOS 26.6.2, 2026-10-03).
	const std::vector<std::string> apple_silicon{"com.apple.videotoolbox.videoencoder.ave.avc",
						     "com.apple.videotoolbox.videoencoder.h264", "obs_x264"};
	CHECK(plan_encoders(apple_silicon, "").candidates ==
	      std::vector<std::string>{"com.apple.videotoolbox.videoencoder.ave.avc", "obs_x264"});
	const std::vector<std::string> intel_mac{"obs_x264", "com.apple.videotoolbox.videoencoder.h264",
						 "com.apple.videotoolbox.videoencoder.h264.gva"};
	CHECK(plan_encoders(intel_mac, "").candidates ==
	      std::vector<std::string>{"com.apple.videotoolbox.videoencoder.h264.gva", "obs_x264"});
	// The software encoder is still reachable as an explicit override.
	CHECK(plan_encoders(apple_silicon, "com.apple.videotoolbox.videoencoder.h264").candidates ==
	      std::vector<std::string>{"com.apple.videotoolbox.videoencoder.h264",
				       "com.apple.videotoolbox.videoencoder.ave.avc", "obs_x264"});
}

TEST_CASE("Linux: NVENC, then QSV, then VAAPI, then x264")
{
	const std::vector<std::string> available{"obs_x264", "ffmpeg_vaapi_tex", "obs_nvenc_h264_tex"};
	CHECK(plan_encoders(available, "").candidates ==
	      std::vector<std::string>{"obs_nvenc_h264_tex", "ffmpeg_vaapi_tex", "obs_x264"});
	CHECK(plan_encoders({"obs_x264", "ffmpeg_vaapi_tex"}, "").candidates ==
	      std::vector<std::string>{"ffmpeg_vaapi_tex", "obs_x264"});
}

TEST_CASE("plan keeps only available encoders, in order")
{
	const std::vector<std::string> available{"obs_x264", "obs_qsv11_v2", "ffmpeg_aac", "obs_nvenc_h264_tex"};
	const EncoderPlan plan = plan_encoders(available, "");
	CHECK(plan.candidates == std::vector<std::string>{"obs_nvenc_h264_tex", "obs_qsv11_v2", "obs_x264"});
	CHECK_FALSE(plan.override_ignored);
}

TEST_CASE("an available override goes first; a missing one is reported")
{
	const std::vector<std::string> available{"obs_x264", "obs_nvenc_h264_tex"};
	EncoderPlan plan = plan_encoders(available, "obs_x264");
	CHECK(plan.candidates == std::vector<std::string>{"obs_x264", "obs_nvenc_h264_tex"});
	plan = plan_encoders(available, "h264_texture_amf");
	CHECK(plan.override_ignored);
	CHECK(plan.candidates == std::vector<std::string>{"obs_nvenc_h264_tex", "obs_x264"});
}

// R1: QualityPreset is {P1080_6Mbps, P1080_8Mbps, P1080_10Mbps, P720_4Mbps}.
// Cover all four exactly (width, height, bitrate), not just a sample.
TEST_CASE("presets")
{
	const PresetSpec p6 = preset_spec(QualityPreset::P1080_6Mbps);
	CHECK(p6.max_width == 1920);
	CHECK(p6.max_height == 1080);
	CHECK(p6.bitrate_kbps == 6000);

	const PresetSpec p8 = preset_spec(QualityPreset::P1080_8Mbps);
	CHECK(p8.max_width == 1920);
	CHECK(p8.max_height == 1080);
	CHECK(p8.bitrate_kbps == 8000);

	const PresetSpec p10 = preset_spec(QualityPreset::P1080_10Mbps);
	CHECK(p10.max_width == 1920);
	CHECK(p10.max_height == 1080);
	CHECK(p10.bitrate_kbps == 10000);

	const PresetSpec p720 = preset_spec(QualityPreset::P720_4Mbps);
	CHECK(p720.max_width == 1280);
	CHECK(p720.max_height == 720);
	CHECK(p720.bitrate_kbps == 4000);
}

TEST_CASE("fit_within")
{
	ScaledSize s = fit_within(3840, 2160, 1920, 1080);
	CHECK(s.width == 1920);
	CHECK(s.height == 1080);
	CHECK(s.scaled);
	s = fit_within(1920, 1080, 1920, 1080);
	CHECK_FALSE(s.scaled);
	CHECK(s.width == 1920);
	s = fit_within(1280, 720, 1920, 1080);
	CHECK_FALSE(s.scaled);
	s = fit_within(2560, 1080, 1920, 1080);
	CHECK(s.width == 1920);
	CHECK(s.height == 810);
	s = fit_within(4096, 2160, 1920, 1080);
	CHECK(s.width == 1920);
	CHECK(s.height == 1012);
	s = fit_within(1920, 1080, 1280, 720);
	CHECK(s.width == 1280);
	CHECK(s.height == 720);
	s = fit_within(1281, 721, 1920, 1080); // odd canvas: trimmed to even
	CHECK(s.width == 1280);
	CHECK(s.height == 720);
	CHECK(s.scaled);
	s = fit_within(0, 720, 1920, 1080);
	CHECK(s.width == 0);
	CHECK_FALSE(s.scaled);
}

TEST_CASE("NVENC settings: lowercase cbr, no B-frames, low latency")
{
	// bitrate_kbps is an explicit input to encoder_settings here, not the default preset.
	const EncoderSettings s = encoder_settings("obs_nvenc_h264_tex", 8000);
	CHECK(num(s, "bitrate") == 8000);
	CHECK(num(s, "max_bitrate") == 8000);
	CHECK(str(s, "rate_control") == "cbr");
	CHECK(num(s, "bf") == 0);
	CHECK(num(s, "keyint_sec") == 10);
	CHECK(flag(s, "repeat_headers"));
	CHECK(str(s, "tune") == "ll");
	CHECK(str(s, "multipass") == "disabled");
	CHECK_FALSE(flag(s, "lookahead"));
}

TEST_CASE("QSV settings use bframes (and bf)")
{
	const EncoderSettings s = encoder_settings("obs_qsv11_v2", 5000);
	CHECK(num(s, "bframes") == 0);
	CHECK(num(s, "bf") == 0);
	CHECK(str(s, "rate_control") == "CBR");
	CHECK(str(s, "latency") == "low");
	CHECK(str(s, "target_usage") == "TU4");
}

TEST_CASE("AMF, x264 and generic settings")
{
	// bitrate_kbps is an explicit input to encoder_settings here, not the default preset.
	EncoderSettings s = encoder_settings("h264_texture_amf", 8000);
	CHECK(str(s, "rate_control") == "CBR");
	CHECK(num(s, "bf") == 0);
	CHECK(str(s, "preset") == "balanced");
	s = encoder_settings("obs_x264", 8000);
	CHECK(str(s, "tune") == "zerolatency");
	CHECK(str(s, "preset") == "veryfast");
	CHECK(num(s, "bf") == 0);
	s = encoder_settings("ffmpeg_nvenc", 8000);
	CHECK(str(s, "rate_control") == "CBR");
	CHECK(num(s, "bf") == 0);
	CHECK(num(s, "keyint_sec") == 1);
	CHECK(find(s, "tune") == nullptr);
}

TEST_CASE("VideoToolbox settings: CBR, bool bframes off, 1 s keyframes, high profile")
{
	for (const char *id :
	     {"com.apple.videotoolbox.videoencoder.ave.avc", "com.apple.videotoolbox.videoencoder.h264.gva",
	      "com.apple.videotoolbox.videoencoder.h264"}) {
		CAPTURE(id);
		const EncoderSettings s = encoder_settings(id, 6000);
		CHECK(num(s, "bitrate") == 6000);
		// mac-videotoolbox reads "CBR" (uppercase) and falls back to ABR itself
		// where CBR is unsupported (Intel, or macOS before 13).
		CHECK(str(s, "rate_control") == "CBR");
		// The ABR fallback honors a cap; CBR ignores it.
		CHECK(flag(s, "limit_bitrate"));
		CHECK(num(s, "max_bitrate") == 6000);
		// mac-videotoolbox reads "bframes" as a bool (frame reordering), not an int.
		CHECK_FALSE(flag(s, "bframes"));
		CHECK(find(s, "bf") == nullptr);
		CHECK(num(s, "keyint_sec") == 1);
		CHECK(str(s, "profile") == "high");
	}
}

TEST_CASE("VAAPI settings: CBR, no B-frames, integer High profile")
{
	const EncoderSettings s = encoder_settings("ffmpeg_vaapi_tex", 8000);
	CHECK(num(s, "bitrate") == 8000);
	CHECK(str(s, "rate_control") == "CBR");
	CHECK(num(s, "bf") == 0);
	CHECK(num(s, "keyint_sec") == 1);
	// obs-ffmpeg-vaapi reads "profile" as an int (FFmpeg's AV_PROFILE_H264_HIGH = 100).
	CHECK(num(s, "profile") == 100);
	int profiles = 0;
	for (const auto &kv : s)
		profiles += kv.first == "profile";
	CHECK(profiles == 1);
}

TEST_CASE("keyint: long GOP only where a live update forces an IDR (obs-nvenc)")
{
	CHECK(num(encoder_settings("obs_nvenc_hevc_tex", 8000), "keyint_sec") == 10);
	CHECK(num(encoder_settings("obs_x264", 8000), "keyint_sec") == 1);
	CHECK(num(encoder_settings("obs_qsv11_v2", 8000), "keyint_sec") == 1);
	CHECK(num(encoder_settings("h264_texture_amf", 8000), "keyint_sec") == 1);
	CHECK(num(encoder_settings("com.apple.videotoolbox.videoencoder.ave.avc", 8000), "keyint_sec") == 1);
	CHECK(num(encoder_settings("ffmpeg_vaapi_tex", 8000), "keyint_sec") == 1);
}
