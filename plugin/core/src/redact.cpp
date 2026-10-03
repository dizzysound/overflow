// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/redact.hpp"

#include <cctype>

namespace airplay {

namespace {

bool is_base64_char(char c)
{
	return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '+' || c == '/' || c == '=';
}

bool is_hex_char(char c)
{
	return std::isxdigit(static_cast<unsigned char>(c)) != 0;
}

} // namespace

std::string redact_secrets(const std::string &line)
{
	std::string out;
	out.reserve(line.size());

	size_t i = 0;
	while (i < line.size()) {
		if (!is_base64_char(line[i])) {
			out += line[i];
			++i;
			continue;
		}

		size_t j = i;
		bool all_hex = true;
		while (j < line.size() && is_base64_char(line[j])) {
			if (all_hex && !is_hex_char(line[j]))
				all_hex = false;
			++j;
		}
		const size_t run_len = j - i;

		// A run this long is secret-shaped regardless of alphabet: a hex run
		// of 32+ chars (an AES-256 key, a SHA-256 digest, ...), or any
		// base64-like run of 40+ chars (a longer key or a wrapped payload).
		// Device IDs, IPs, and timestamps are interrupted by ':' / '.' / '-'
		// / ' ' often enough that their digit runs never reach either
		// threshold, so they pass through untouched.
		if (run_len >= 40 || (all_hex && run_len >= 32))
			out += "<redacted>";
		else
			out.append(line, i, run_len);

		i = j;
	}

	return out;
}

} // namespace airplay
