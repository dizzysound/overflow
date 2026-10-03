// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Protection for cached AirPlay passwords. Windows: DPAPI, current user.
// Other platforms: not available yet (macOS will use the Keychain).
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace airplay {

std::string base64_encode(const std::vector<uint8_t> &data);
std::optional<std::vector<uint8_t>> base64_decode(const std::string &text);

class SecretProtector {
public:
	virtual ~SecretProtector() = default;
	// False when passwords cannot be stored on this platform.
	virtual bool available() const = 0;
	// Returns an opaque, base64 text blob for settings.json.
	virtual std::optional<std::string> protect(const std::string &plain) const = 0;
	virtual std::optional<std::string> unprotect(const std::string &blob) const = 0;
};

std::unique_ptr<SecretProtector> make_platform_protector();

} // namespace airplay
