// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/secret.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

using airplay::base64_decode;
using airplay::base64_encode;

namespace {
std::vector<uint8_t> bytes(const std::string &s)
{
	return std::vector<uint8_t>(s.begin(), s.end());
}
} // namespace

TEST_CASE("base64 RFC 4648 vectors")
{
	CHECK(base64_encode(bytes("")) == "");
	CHECK(base64_encode(bytes("f")) == "Zg==");
	CHECK(base64_encode(bytes("fo")) == "Zm8=");
	CHECK(base64_encode(bytes("foo")) == "Zm9v");
	CHECK(base64_encode(bytes("foobar")) == "Zm9vYmFy");
	CHECK(base64_decode("Zm9vYmFy") == bytes("foobar"));
	CHECK(base64_decode("Zg==") == bytes("f"));
	CHECK(base64_decode("") == bytes(""));
	const std::vector<uint8_t> binary{0x00, 0xff, 0x10, 0x80, 0x7f};
	CHECK(base64_decode(base64_encode(binary)) == binary);
}

TEST_CASE("base64_decode rejects malformed input")
{
	CHECK_FALSE(base64_decode("Zm9").has_value());
	CHECK_FALSE(base64_decode("Zm9v!A==").has_value());
	CHECK_FALSE(base64_decode("=Zm9").has_value());
}

TEST_CASE("platform protector")
{
	const auto p = airplay::make_platform_protector();
	REQUIRE(p);
#ifdef _WIN32
	REQUIRE(p->available());
	const auto blob = p->protect("hunter2");
	REQUIRE(blob.has_value());
	CHECK(blob->find("hunter2") == std::string::npos);
	CHECK(p->unprotect(*blob) == std::optional<std::string>("hunter2"));
	CHECK_FALSE(p->unprotect("bm90IGEgYmxvYg==").has_value());
#else
	CHECK_FALSE(p->available());
	CHECK_FALSE(p->protect("hunter2").has_value());
	CHECK_FALSE(p->unprotect("anything").has_value());
#endif
}
