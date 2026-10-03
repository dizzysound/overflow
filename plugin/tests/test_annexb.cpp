// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/annexb.hpp"

#include <doctest/doctest.h>

#include <vector>

using Bytes = std::vector<uint8_t>;

namespace {
const Bytes kSps{0x67, 0x42, 0x00, 0x1f, 0xe9};
const Bytes kPps{0x68, 0xce, 0x3c, 0x80};

Bytes annexb_headers()
{
	Bytes h{0, 0, 0, 1};
	h.insert(h.end(), kSps.begin(), kSps.end());
	h.insert(h.end(), {0, 0, 0, 1});
	h.insert(h.end(), kPps.begin(), kPps.end());
	return h;
}
} // namespace

TEST_CASE("has_start_code recognizes 3- and 4-byte start codes")
{
	const Bytes three{0, 0, 1, 0x65};
	const Bytes four{0, 0, 0, 1, 0x65};
	const Bytes avcc{1, 0x42, 0x00, 0x1f};
	CHECK(airplay::has_start_code(three.data(), three.size()));
	CHECK(airplay::has_start_code(four.data(), four.size()));
	CHECK_FALSE(airplay::has_start_code(avcc.data(), avcc.size()));
	CHECK_FALSE(airplay::has_start_code(nullptr, 0));
}

TEST_CASE("nal_types lists every NAL unit type")
{
	const Bytes au{0, 0, 0, 1, 0x09, 0xf0, 0, 0, 1, 0x67, 0x42, 0, 0, 0, 1, 0x68, 0xce, 0, 0, 1, 0x65, 0x88};
	CHECK(airplay::nal_types(au.data(), au.size()) == Bytes{9, 7, 8, 5});
	CHECK(airplay::contains_nal_type(au.data(), au.size(), airplay::kNalSps));
	CHECK_FALSE(airplay::contains_nal_type(au.data(), au.size(), 1));
}

TEST_CASE("extradata_to_annexb passes Annex-B through")
{
	const Bytes h = annexb_headers();
	CHECK(airplay::extradata_to_annexb(h.data(), h.size()) == h);
}

TEST_CASE("extradata_to_annexb converts avcC")
{
	Bytes avcc{0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, static_cast<uint8_t>(kSps.size())};
	avcc.insert(avcc.end(), kSps.begin(), kSps.end());
	avcc.insert(avcc.end(), {0x01, 0x00, static_cast<uint8_t>(kPps.size())});
	avcc.insert(avcc.end(), kPps.begin(), kPps.end());
	CHECK(airplay::extradata_to_annexb(avcc.data(), avcc.size()) == annexb_headers());
}

TEST_CASE("extradata_to_annexb rejects truncated or unknown data")
{
	const Bytes truncated{0x01, 0x42, 0x00, 0x1f, 0xff, 0xe1, 0x00, 0x09, 0x67};
	const Bytes garbage{0x02, 0x03};
	CHECK(airplay::extradata_to_annexb(truncated.data(), truncated.size()).empty());
	CHECK(airplay::extradata_to_annexb(garbage.data(), garbage.size()).empty());
	CHECK(airplay::extradata_to_annexb(nullptr, 0).empty());
}

TEST_CASE("build_access_unit prepends SPS/PPS to a keyframe that lacks them")
{
	const Bytes idr{0, 0, 0, 1, 0x65, 0x88, 0x84};
	Bytes want = annexb_headers();
	want.insert(want.end(), idr.begin(), idr.end());
	CHECK(airplay::build_access_unit(idr.data(), idr.size(), true, annexb_headers()) == want);
}

TEST_CASE("build_access_unit leaves non-keyframes and in-band headers alone")
{
	const Bytes p{0, 0, 0, 1, 0x41, 0x9a};
	CHECK(airplay::build_access_unit(p.data(), p.size(), false, annexb_headers()) == p);

	Bytes inband = annexb_headers();
	inband.insert(inband.end(), {0, 0, 0, 1, 0x65, 0x88});
	CHECK(airplay::build_access_unit(inband.data(), inband.size(), true, annexb_headers()) == inband);

	const Bytes idr{0, 0, 1, 0x65, 0x88};
	CHECK(airplay::build_access_unit(idr.data(), idr.size(), true, Bytes{}) == idr);
}

TEST_CASE("build_access_unit keeps an access unit delimiter first")
{
	const Bytes aud{0, 0, 0, 1, 0x09, 0xf0};
	const Bytes idr{0, 0, 0, 1, 0x65, 0x88};
	Bytes au = aud;
	au.insert(au.end(), idr.begin(), idr.end());
	Bytes want = aud;
	const Bytes h = annexb_headers();
	want.insert(want.end(), h.begin(), h.end());
	want.insert(want.end(), idr.begin(), idr.end());
	CHECK(airplay::build_access_unit(au.data(), au.size(), true, h) == want);
}
