// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/redact.hpp"

#include <doctest/doctest.h>

using namespace airplay;

TEST_CASE("a 32-hex-char key is redacted")
{
	const std::string line = "AesKey (raw): 3fa1cd9e0b7c2a4d8e6f1b9c0d2a3e4f";
	CHECK(redact_secrets(line) == "AesKey (raw): <redacted>");
}

TEST_CASE("a longer hex key is redacted as one run")
{
	const std::string line =
		"eKey: 0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
	CHECK(redact_secrets(line) == "eKey: <redacted>");
}

TEST_CASE("a 40+ char base64-like run is redacted")
{
	const std::string line = "Secret (32 bytes): AbCdEfGh1234567890+/AbCdEfGh1234567890==";
	CHECK(redact_secrets(line) == "Secret (32 bytes): <redacted>");
}

TEST_CASE("a short hex run under 32 chars is left alone")
{
	const std::string line = "frame hdr[4:6]=0a1b";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("a short base64-like run under 40 chars is left alone")
{
	const std::string line = "session=AbCdEfGh1234567890+/AbCd";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("device IDs like MAC addresses are preserved")
{
	const std::string line = "connected to display 9E:B8:AE:9A:2A:CF";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("IPv4 addresses are preserved")
{
	const std::string line = "receiver at 192.168.1.100:7000";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("timestamps are preserved")
{
	const std::string line = "[2026-09-28 14:32:07] session started";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("plain prose with no secret-shaped runs is untouched")
{
	const std::string line = "helper connected, streaming started successfully";
	CHECK(redact_secrets(line) == line);
}

TEST_CASE("empty string redacts to empty string")
{
	CHECK(redact_secrets("") == "");
}

TEST_CASE("multiple secrets on one line are each redacted independently")
{
	// A space (or any non-base64 separator) precedes each value, matching the
	// real "label: value" shape of the fixed helper log lines (e.g. "cipher
	// key: <hex> cipher IV: <hex>"); a run only merges with adjacent text
	// when there is no separator at all.
	const std::string line =
		"key: 0123456789abcdef0123456789abcdef iv: fedcba9876543210fedcba9876543210";
	CHECK(redact_secrets(line) == "key: <redacted> iv: <redacted>");
}
