// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// The plugin -> helper stdin framing from docs/protocol.md.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace airplay {

enum class MsgType : uint8_t {
	Hello = 0x01,
	VideoAU = 0x02,
	AudioPCM = 0x03,
	Command = 0x10,
};

// Upper bound on one message: the type byte plus the payload.
constexpr uint32_t kMaxMessage = 16u << 20;
constexpr size_t kMediaHeaderLen = 17;
constexpr uint8_t kFlagKeyframe = 0x01;

// Appends one frame (u32 little-endian length = 1 + size, the type byte, the
// payload) to out. Returns false and appends nothing if it would exceed
// kMaxMessage.
bool append_frame(std::vector<uint8_t> &out, MsgType type, const uint8_t *payload, size_t size);
bool append_frame(std::vector<uint8_t> &out, MsgType type, const std::string &payload);

// Appends a video_au or audio_pcm frame: the media header (capture_ns,
// send_ns, flags) followed by data. Returns false and appends nothing if type
// is not VideoAU or AudioPCM, size is 0, an audio size is not a multiple of 4,
// or the frame would exceed kMaxMessage.
bool append_media_frame(std::vector<uint8_t> &out, MsgType type, uint64_t capture_ns, uint64_t send_ns,
			bool keyframe, const uint8_t *data, size_t size);

// The hello payload: H.264 video, 44100 Hz stereo S16LE audio.
std::string hello_json();

} // namespace airplay
