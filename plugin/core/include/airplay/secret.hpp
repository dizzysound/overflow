// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Protection for cached AirPlay passwords. Windows: DPAPI, current user; the
// blob is the encrypted password. macOS: the login Keychain. Linux: the Secret
// Service (GNOME Keyring, KWallet) through libsecret, loaded at run time. On
// macOS and Linux the password lives in the OS store and the blob only names
// the item ("keychain:<id>", "secret-service:<id>").
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
	// Returns an opaque text blob for settings.json (see above).
	virtual std::optional<std::string> protect(const std::string &plain) const = 0;
	virtual std::optional<std::string> unprotect(const std::string &blob) const = 0;
	// Deletes what protect() stored for this blob, where that lives outside
	// the blob (the OS store). Called when a saved password is replaced or
	// forgotten. Unknown or already-deleted blobs are ignored.
	virtual void discard(const std::string &) const {}
};

std::unique_ptr<SecretProtector> make_platform_protector();

// The name a store-backed blob gives its item: "<prefix>:<id>" -> "<id>".
// Empty when the blob has another prefix or no usable id (32 hex digits).
std::string store_item_id(const std::string &blob, const std::string &prefix);
// A new random id for store_item_id: 32 lowercase hex digits.
std::string new_store_item_id();

} // namespace airplay
