// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Saved AirPlay passwords on Linux: one item per password in the Secret
// Service (GNOME Keyring, KWallet, KeePassXC) through libsecret. The settings
// blob is "secret-service:<id>"; the item carries the id as an attribute.
//
// libsecret is loaded with dlopen, not linked, so the plugin still loads
// where it is missing; then, or when no Secret Service answers on the session
// bus, available() is false and the dock offers no "Remember".

#include "airplay/secret.hpp"

#include <dlfcn.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>

namespace airplay {
namespace {

constexpr const char *kPrefix = "secret-service";

// The parts of the libsecret and GLib ABI used here (libsecret-1.so.0).
using gboolean = int;
struct GError {
	uint32_t domain;
	int code;
	char *message;
};
struct SecretSchemaAttribute {
	const char *name;
	int type; // SECRET_SCHEMA_ATTRIBUTE_STRING = 0
};
struct SecretSchema {
	const char *name;
	int flags; // SECRET_SCHEMA_NONE = 0
	SecretSchemaAttribute attributes[32];
	int reserved;
	void *reserved1, *reserved2, *reserved3, *reserved4, *reserved5, *reserved6, *reserved7;
};

using StoreFn = gboolean (*)(const SecretSchema *, const char *collection, const char *label, const char *password,
			     void *cancellable, GError **, ...);
using LookupFn = char *(*)(const SecretSchema *, void *cancellable, GError **, ...);
using ClearFn = gboolean (*)(const SecretSchema *, void *cancellable, GError **, ...);
using FreeFn = void (*)(char *);
using ErrorFreeFn = void (*)(GError *);

const SecretSchema kSchema = {
	"com.github.dizzysound.obs-overflow.Password", 0, {{"id", 0}, {nullptr, 0}}, 0, nullptr, nullptr,
	nullptr, nullptr, nullptr, nullptr, nullptr};

// The id available() looks up to see whether a Secret Service answers.
constexpr const char *kProbeId = "probe";

class LibsecretProtector final : public SecretProtector {
public:
	LibsecretProtector()
	{
		lib_ = dlopen("libsecret-1.so.0", RTLD_NOW | RTLD_LOCAL);
		if (!lib_)
			return;
		store_ = reinterpret_cast<StoreFn>(dlsym(lib_, "secret_password_store_sync"));
		lookup_ = reinterpret_cast<LookupFn>(dlsym(lib_, "secret_password_lookup_sync"));
		clear_ = reinterpret_cast<ClearFn>(dlsym(lib_, "secret_password_clear_sync"));
		free_ = reinterpret_cast<FreeFn>(dlsym(lib_, "secret_password_free"));
		error_free_ = reinterpret_cast<ErrorFreeFn>(dlsym(lib_, "g_error_free")); // from GLib, a dependency
	}

	~LibsecretProtector() override
	{
		// Never dlclose: GLib types libsecret registered stay registered.
	}

	// A success is kept for the session. A failure is retried at most once a
	// minute, so a keyring daemon that starts after OBS (login autostart) is
	// picked up without restarting OBS, and a hung service costs one D-Bus
	// timeout a minute at most.
	bool available() const override
	{
		if (!store_ || !lookup_ || !clear_ || !free_ || !error_free_)
			return false;
		std::lock_guard<std::mutex> lock(probe_mutex_);
		if (usable_)
			return true;
		const auto now = std::chrono::steady_clock::now();
		if (probed_ && now - last_probe_ < std::chrono::minutes(1))
			return false;
		probed_ = true;
		last_probe_ = now;
		GError *err = nullptr;
		char *found = lookup_(&kSchema, nullptr, &err, "id", kProbeId, nullptr);
		if (found)
			free_(found);
		if (err) {
			error_free_(err); // no Secret Service on the session bus
			return false;
		}
		usable_ = true;
		return true;
	}

	std::optional<std::string> protect(const std::string &plain) const override
	{
		if (!available())
			return std::nullopt;
		const std::string id = new_store_item_id();
		GError *err = nullptr;
		const gboolean ok = store_(&kSchema, nullptr, "Overflow display password", plain.c_str(), nullptr, &err,
					   "id", id.c_str(), nullptr);
		if (err)
			error_free_(err);
		if (!ok)
			return std::nullopt;
		return std::string(kPrefix) + ":" + id;
	}

	std::optional<std::string> unprotect(const std::string &blob) const override
	{
		const std::string id = store_item_id(blob, kPrefix);
		if (id.empty() || !available())
			return std::nullopt;
		GError *err = nullptr;
		char *found = lookup_(&kSchema, nullptr, &err, "id", id.c_str(), nullptr);
		if (err)
			error_free_(err);
		if (!found)
			return std::nullopt;
		std::string plain(found);
		free_(found); // wipes, then frees
		return plain;
	}

	void discard(const std::string &blob) const override
	{
		const std::string id = store_item_id(blob, kPrefix);
		if (id.empty() || !available())
			return;
		GError *err = nullptr;
		clear_(&kSchema, nullptr, &err, "id", id.c_str(), nullptr);
		if (err)
			error_free_(err);
	}

private:
	void *lib_ = nullptr;
	StoreFn store_ = nullptr;
	LookupFn lookup_ = nullptr;
	ClearFn clear_ = nullptr;
	FreeFn free_ = nullptr;
	ErrorFreeFn error_free_ = nullptr;
	mutable std::mutex probe_mutex_;
	mutable bool usable_ = false;
	mutable bool probed_ = false;
	mutable std::chrono::steady_clock::time_point last_probe_;
};

} // namespace

std::unique_ptr<SecretProtector> make_libsecret_protector()
{
	return std::make_unique<LibsecretProtector>();
}

} // namespace airplay
