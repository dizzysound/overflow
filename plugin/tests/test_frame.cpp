// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/frame.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using airplay::MsgType;

TEST_CASE("append_frame writes the length, the type and the payload")
{
	std::vector<uint8_t> out;
	REQUIRE(airplay::append_frame(out, MsgType::Command, std::string("{}")));
	const std::vector<uint8_t> want{3, 0, 0, 0, 0x10, '{', '}'};
	CHECK(out == want);
}

TEST_CASE("append_frame appends to what is already there")
{
	std::vector<uint8_t> out{0xAA};
	REQUIRE(airplay::append_frame(out, MsgType::Hello, std::string("x")));
	const std::vector<uint8_t> want{0xAA, 2, 0, 0, 0, 0x01, 'x'};
	CHECK(out == want);
}

TEST_CASE("append_frame enforces the 16 MiB limit on type byte plus payload")
{
	std::vector<uint8_t> big(airplay::kMaxMessage);
	std::vector<uint8_t> out;
	CHECK_FALSE(airplay::append_frame(out, MsgType::Command, big.data(), big.size()));
	CHECK(out.empty());
	REQUIRE(airplay::append_frame(out, MsgType::Command, big.data(), big.size() - 1));
	CHECK(out.size() == 4 + airplay::kMaxMessage);
	CHECK(out[0] == 0x00);
	CHECK(out[1] == 0x00);
	CHECK(out[2] == 0x00);
	CHECK(out[3] == 0x01); // 0x01000000 = 16 MiB, little-endian
}

TEST_CASE("append_media_frame writes the 17-byte media header")
{
	const uint8_t au[] = {0, 0, 0, 1, 0x65};
	std::vector<uint8_t> out;
	REQUIRE(airplay::append_media_frame(out, MsgType::VideoAU, 0x0102030405060708ull, 0x1112131415161718ull,
					    true, au, sizeof(au)));
	const std::vector<uint8_t> want{
		23,   0,    0,    0,    0x02,                         // length 1 + 17 + 5, type video_au
		0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,       // capture_ns
		0x18, 0x17, 0x16, 0x15, 0x14, 0x13, 0x12, 0x11,       // send_ns
		0x01,                                                 // flags: keyframe
		0,    0,    0,    1,    0x65,                         // data
	};
	CHECK(out == want);
}

TEST_CASE("append_media_frame rejects what the helper treats as a protocol error")
{
	const uint8_t pcm[8] = {};
	std::vector<uint8_t> out;
	CHECK_FALSE(airplay::append_media_frame(out, MsgType::VideoAU, 1, 1, false, pcm, 0));
	CHECK_FALSE(airplay::append_media_frame(out, MsgType::AudioPCM, 1, 1, false, pcm, 6));
	CHECK_FALSE(airplay::append_media_frame(out, MsgType::Hello, 1, 1, false, pcm, 8));
	CHECK(out.empty());
	CHECK(airplay::append_media_frame(out, MsgType::AudioPCM, 1, 1, false, pcm, 8));
	CHECK(out[21] == 0x00); // flags: not a keyframe
}

TEST_CASE("hello_json matches docs/protocol.md")
{
	CHECK(airplay::hello_json() ==
	      R"({"version":1,"video":{"codec":"h264"},"audio":{"sample_rate":44100,"channels":2,"format":"s16le"}})");
}
