// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/annexb.hpp"

namespace airplay {
namespace {

// Offset of the next start code at or after from, or size if none. Sets
// *code_len to 3 or 4.
size_t find_start_code(const uint8_t *d, size_t size, size_t from, size_t *code_len)
{
	for (size_t i = from; i + 3 <= size; ++i) {
		if (d[i] != 0 || d[i + 1] != 0)
			continue;
		if (d[i + 2] == 1) {
			*code_len = 3;
			return i;
		}
		if (i + 4 <= size && d[i + 2] == 0 && d[i + 3] == 1) {
			*code_len = 4;
			return i;
		}
	}
	return size;
}

} // namespace

bool has_start_code(const uint8_t *data, size_t size)
{
	if (!data)
		return false;
	if (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1)
		return true;
	return size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1;
}

std::vector<uint8_t> nal_types(const uint8_t *data, size_t size)
{
	std::vector<uint8_t> types;
	if (!data)
		return types;
	size_t code_len = 0;
	size_t i = find_start_code(data, size, 0, &code_len);
	while (i < size) {
		const size_t header = i + code_len;
		if (header < size)
			types.push_back(static_cast<uint8_t>(data[header] & 0x1f));
		i = find_start_code(data, size, header, &code_len);
	}
	return types;
}

bool contains_nal_type(const uint8_t *data, size_t size, uint8_t type)
{
	for (uint8_t t : nal_types(data, size))
		if (t == type)
			return true;
	return false;
}

std::vector<uint8_t> extradata_to_annexb(const uint8_t *data, size_t size)
{
	if (!data || size == 0)
		return {};
	if (has_start_code(data, size))
		return std::vector<uint8_t>(data, data + size);

	// avcC: version(1) profile compat level (0xFC | lengthSizeMinusOne)
	// (0xE0 | numSps) {u16 len, sps}... numPps {u16 len, pps}...
	if (size < 7 || data[0] != 1)
		return {};
	static const uint8_t start_code[4] = {0, 0, 0, 1};
	std::vector<uint8_t> out;
	size_t pos = 5;
	auto copy_sets = [&](size_t count) {
		for (size_t n = 0; n < count; ++n) {
			if (pos + 2 > size)
				return false;
			const size_t len = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
			pos += 2;
			if (len == 0 || pos + len > size)
				return false;
			out.insert(out.end(), start_code, start_code + 4);
			out.insert(out.end(), data + pos, data + pos + len);
			pos += len;
		}
		return true;
	};
	const size_t num_sps = data[pos] & 0x1f;
	++pos;
	if (!copy_sets(num_sps) || pos >= size)
		return {};
	const size_t num_pps = data[pos];
	++pos;
	if (!copy_sets(num_pps))
		return {};
	return out;
}

std::vector<uint8_t> build_access_unit(const uint8_t *data, size_t size, bool keyframe,
					const std::vector<uint8_t> &annexb_headers)
{
	if (!keyframe || annexb_headers.empty() || contains_nal_type(data, size, kNalSps))
		return std::vector<uint8_t>(data, data + size);

	size_t insert_at = 0;
	size_t code_len = 0;
	const size_t first = find_start_code(data, size, 0, &code_len);
	if (first < size && first + code_len < size && (data[first + code_len] & 0x1f) == kNalAud) {
		size_t next_len = 0;
		insert_at = find_start_code(data, size, first + code_len, &next_len);
	}

	std::vector<uint8_t> au;
	au.reserve(size + annexb_headers.size());
	au.insert(au.end(), data, data + insert_at);
	au.insert(au.end(), annexb_headers.begin(), annexb_headers.end());
	au.insert(au.end(), data + insert_at, data + size);
	return au;
}

} // namespace airplay
