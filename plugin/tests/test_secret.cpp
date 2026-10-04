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

TEST_CASE("store_item_id accepts only <prefix>:<32 lowercase hex digits>")
{
	const std::string id = "0123456789abcdef0123456789abcdef";
	CHECK(airplay::store_item_id("keychain:" + id, "keychain") == id);
	CHECK(airplay::store_item_id("secret-service:" + id, "secret-service") == id);
	CHECK(airplay::store_item_id("keychain:" + id, "secret-service").empty());
	CHECK(airplay::store_item_id("keychain:" + id.substr(1), "keychain").empty());
	CHECK(airplay::store_item_id("keychain:" + id + "0", "keychain").empty());
	CHECK(airplay::store_item_id("keychain:0123456789ABCDEF0123456789abcdef", "keychain").empty());
	CHECK(airplay::store_item_id("keychain;" + id, "keychain").empty());
	CHECK(airplay::store_item_id("aGVsbG8=", "keychain").empty()); // a DPAPI blob
}

TEST_CASE("new_store_item_id is 32 hex digits and differs each time")
{
	const std::string a = airplay::new_store_item_id();
	const std::string b = airplay::new_store_item_id();
	CHECK(airplay::store_item_id("x:" + a, "x") == a);
	CHECK(a != b);
}

TEST_CASE("platform protector")
{
	const auto p = airplay::make_platform_protector();
	REQUIRE(p);
#if defined(_WIN32) || defined(__APPLE__)
	REQUIRE(p->available());
#else
	// Linux: needs libsecret and a Secret Service on the session bus.
	if (!p->available()) {
		MESSAGE("no Secret Service here; checking the unavailable path only");
		CHECK_FALSE(p->protect("hunter2").has_value());
		CHECK_FALSE(p->unprotect("secret-service:0123456789abcdef0123456789abcdef").has_value());
		return;
	}
#endif
	const auto blob = p->protect("hunter2");
	REQUIRE(blob.has_value());
	CHECK(blob->find("hunter2") == std::string::npos);
	CHECK(p->unprotect(*blob) == std::optional<std::string>("hunter2"));
	CHECK_FALSE(p->unprotect("bm90IGEgYmxvYg==").has_value());
	p->discard(*blob);
#ifndef _WIN32
	// The password lived in the OS store, so discarding it is final.
	CHECK_FALSE(p->unprotect(*blob).has_value());
#endif
	p->discard(*blob); // a second discard is harmless
}
