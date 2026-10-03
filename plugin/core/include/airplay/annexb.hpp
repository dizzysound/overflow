// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// H.264 Annex-B helpers: every keyframe the helper receives must carry SPS and
// PPS in-band (docs/protocol.md, video rules).
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace airplay {

constexpr uint8_t kNalIdr = 5;
constexpr uint8_t kNalSps = 7;
constexpr uint8_t kNalPps = 8;
constexpr uint8_t kNalAud = 9;

// True if data begins with 00 00 01 or 00 00 00 01.
bool has_start_code(const uint8_t *data, size_t size);

// The nal_unit_type of every NAL unit in an Annex-B buffer, in order.
std::vector<uint8_t> nal_types(const uint8_t *data, size_t size);
bool contains_nal_type(const uint8_t *data, size_t size, uint8_t type);

// Encoder extra data as Annex-B SPS/PPS. Annex-B input is returned unchanged;
// an avcC record is converted; anything else yields an empty vector.
std::vector<uint8_t> extradata_to_annexb(const uint8_t *data, size_t size);

// One access unit for the helper. A keyframe with no SPS NAL gets
// annexb_headers inserted after its leading AUD, or at the front.
std::vector<uint8_t> build_access_unit(const uint8_t *data, size_t size, bool keyframe,
					const std::vector<uint8_t> &annexb_headers);

} // namespace airplay
