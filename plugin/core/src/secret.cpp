// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

#include "airplay/secret.hpp"

#include <cstddef>
#include <random>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dpapi.h>
#endif

namespace airplay {
namespace {

const char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int decode_char(char c)
{
	if (c >= 'A' && c <= 'Z')
		return c - 'A';
	if (c >= 'a' && c <= 'z')
		return c - 'a' + 26;
	if (c >= '0' && c <= '9')
		return c - '0' + 52;
	if (c == '+')
		return 62;
	if (c == '/')
		return 63;
	return -1;
}

#ifdef _WIN32
// DATA_BLOB sizes are 32-bit; refuse anything larger instead of truncating.
constexpr std::size_t kMaxBlobBytes = 0xFFFFFFFFu;

class DpapiProtector final : public SecretProtector {
public:
	bool available() const override { return true; }

	std::optional<std::string> protect(const std::string &plain) const override
	{
		if (plain.size() > kMaxBlobBytes)
			return std::nullopt;
		DATA_BLOB in{static_cast<DWORD>(plain.size()),
			     reinterpret_cast<BYTE *>(const_cast<char *>(plain.data()))};
		DATA_BLOB out{};
		if (!CryptProtectData(&in, L"Overflow display password", nullptr, nullptr, nullptr,
				      CRYPTPROTECT_UI_FORBIDDEN, &out))
			return std::nullopt;
		std::vector<uint8_t> blob(out.pbData, out.pbData + out.cbData);
		LocalFree(out.pbData);
		return base64_encode(blob);
	}

	std::optional<std::string> unprotect(const std::string &blob) const override
	{
		auto raw = base64_decode(blob);
		if (!raw || raw->empty() || raw->size() > kMaxBlobBytes)
			return std::nullopt;
		DATA_BLOB in{static_cast<DWORD>(raw->size()), raw->data()};
		DATA_BLOB out{};
		if (!CryptUnprotectData(&in, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out))
			return std::nullopt;
		std::string plain(reinterpret_cast<const char *>(out.pbData), out.cbData);
		SecureZeroMemory(out.pbData, out.cbData);
		LocalFree(out.pbData);
		return plain;
	}
};
#endif

} // namespace

#if defined(__APPLE__)
std::unique_ptr<SecretProtector> make_keychain_protector(); // secret_macos.cpp
#elif !defined(_WIN32)
std::unique_ptr<SecretProtector> make_libsecret_protector(); // secret_linux.cpp
#endif

std::string store_item_id(const std::string &blob, const std::string &prefix)
{
	constexpr std::size_t kIdLength = 32;
	if (blob.size() != prefix.size() + 1 + kIdLength || blob.compare(0, prefix.size(), prefix) != 0 ||
	    blob[prefix.size()] != ':')
		return {};
	const std::string id = blob.substr(prefix.size() + 1);
	for (const char c : id)
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return {};
	return id;
}

std::string new_store_item_id()
{
	// An item name, not a secret: std::random_device is enough.
	static const char kHex[] = "0123456789abcdef";
	std::random_device rd;
	std::string id;
	id.reserve(32);
	for (int i = 0; i < 4; ++i) {
		uint32_t v = rd();
		for (int k = 0; k < 8; ++k, v >>= 4)
			id.push_back(kHex[v & 15]);
	}
	return id;
}

std::string base64_encode(const std::vector<uint8_t> &data)
{
	std::string out;
	out.reserve((data.size() + 2) / 3 * 4);
	size_t i = 0;
	for (; i + 3 <= data.size(); i += 3) {
		const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8) | data[i + 2];
		out.push_back(kAlphabet[(v >> 18) & 63]);
		out.push_back(kAlphabet[(v >> 12) & 63]);
		out.push_back(kAlphabet[(v >> 6) & 63]);
		out.push_back(kAlphabet[v & 63]);
	}
	const size_t rest = data.size() - i;
	if (rest == 1) {
		const uint32_t v = uint32_t(data[i]) << 16;
		out.push_back(kAlphabet[(v >> 18) & 63]);
		out.push_back(kAlphabet[(v >> 12) & 63]);
		out.append("==");
	} else if (rest == 2) {
		const uint32_t v = (uint32_t(data[i]) << 16) | (uint32_t(data[i + 1]) << 8);
		out.push_back(kAlphabet[(v >> 18) & 63]);
		out.push_back(kAlphabet[(v >> 12) & 63]);
		out.push_back(kAlphabet[(v >> 6) & 63]);
		out.push_back('=');
	}
	return out;
}

std::optional<std::vector<uint8_t>> base64_decode(const std::string &text)
{
	if (text.size() % 4 != 0)
		return std::nullopt;
	std::vector<uint8_t> out;
	out.reserve(text.size() / 4 * 3);
	for (size_t i = 0; i < text.size(); i += 4) {
		const bool last = i + 4 == text.size();
		int v[4];
		int pad = 0;
		for (size_t k = 0; k < 4; ++k) {
			const char c = text[i + k];
			if (c == '=' && last && k >= 2) {
				v[k] = 0;
				++pad;
				continue;
			}
			if (pad > 0)
				return std::nullopt; // data after padding
			v[k] = decode_char(c);
			if (v[k] < 0)
				return std::nullopt;
		}
		const uint32_t n = (uint32_t(v[0]) << 18) | (uint32_t(v[1]) << 12) | (uint32_t(v[2]) << 6) |
				   uint32_t(v[3]);
		out.push_back(static_cast<uint8_t>(n >> 16));
		if (pad < 2)
			out.push_back(static_cast<uint8_t>(n >> 8));
		if (pad < 1)
			out.push_back(static_cast<uint8_t>(n));
	}
	return out;
}

std::unique_ptr<SecretProtector> make_platform_protector()
{
#if defined(_WIN32)
	return std::make_unique<DpapiProtector>();
#elif defined(__APPLE__)
	return make_keychain_protector();
#else
	return make_libsecret_protector();
#endif
}

} // namespace airplay
