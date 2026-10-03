// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/frame.hpp"

namespace airplay {
namespace {

void put_le32(std::vector<uint8_t> &out, uint32_t v)
{
	for (int i = 0; i < 4; ++i)
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

void put_le64(std::vector<uint8_t> &out, uint64_t v)
{
	for (int i = 0; i < 8; ++i)
		out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

} // namespace

bool append_frame(std::vector<uint8_t> &out, MsgType type, const uint8_t *payload, size_t size)
{
	if (size >= kMaxMessage)
		return false;
	out.reserve(out.size() + 5 + size);
	put_le32(out, static_cast<uint32_t>(size + 1));
	out.push_back(static_cast<uint8_t>(type));
	if (size > 0)
		out.insert(out.end(), payload, payload + size);
	return true;
}

bool append_frame(std::vector<uint8_t> &out, MsgType type, const std::string &payload)
{
	return append_frame(out, type, reinterpret_cast<const uint8_t *>(payload.data()), payload.size());
}

bool append_media_frame(std::vector<uint8_t> &out, MsgType type, uint64_t capture_ns, uint64_t send_ns,
			bool keyframe, const uint8_t *data, size_t size)
{
	if (type != MsgType::VideoAU && type != MsgType::AudioPCM)
		return false;
	if (size == 0)
		return false;
	if (type == MsgType::AudioPCM && size % 4 != 0)
		return false;
	if (size + kMediaHeaderLen >= kMaxMessage)
		return false;
	out.reserve(out.size() + 5 + kMediaHeaderLen + size);
	put_le32(out, static_cast<uint32_t>(1 + kMediaHeaderLen + size));
	out.push_back(static_cast<uint8_t>(type));
	put_le64(out, capture_ns);
	put_le64(out, send_ns);
	out.push_back(keyframe ? kFlagKeyframe : 0);
	out.insert(out.end(), data, data + size);
	return true;
}

std::string hello_json()
{
	return R"({"version":1,"video":{"codec":"h264"},"audio":{"sample_rate":44100,"channels":2,"format":"s16le"}})";
}

} // namespace airplay
