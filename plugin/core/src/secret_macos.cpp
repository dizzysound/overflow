// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Christopher Gillespie

// Saved AirPlay passwords on macOS: one generic-password item per password in
// the login Keychain. The settings blob is "keychain:<id>"; the item's account
// is the id. OBS creates the item, so the item's access list trusts OBS and
// reading it back does not prompt.

#include "airplay/secret.hpp"

#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

#include <memory>

namespace airplay {
namespace {

constexpr const char *kPrefix = "keychain";
constexpr const char *kService = "com.github.dizzysound.obs-overflow";

// Owns one Core Foundation reference.
template<typename T> struct CfRef {
	T ref = nullptr;
	CfRef() = default;
	explicit CfRef(T r) : ref(r) {}
	CfRef(const CfRef &) = delete;
	CfRef &operator=(const CfRef &) = delete;
	~CfRef()
	{
		if (ref)
			CFRelease(ref);
	}
};

CFStringRef make_string(const std::string &s)
{
	return CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(s.data()),
				       static_cast<CFIndex>(s.size()), kCFStringEncodingUTF8, false);
}

// The query that names one item: class, service, account.
CFMutableDictionaryRef item_query(const std::string &id)
{
	CFMutableDictionaryRef q = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
							     &kCFTypeDictionaryValueCallBacks);
	CfRef<CFStringRef> service(make_string(kService));
	CfRef<CFStringRef> account(make_string(id));
	CFDictionarySetValue(q, kSecClass, kSecClassGenericPassword);
	CFDictionarySetValue(q, kSecAttrService, service.ref);
	CFDictionarySetValue(q, kSecAttrAccount, account.ref);
	return q;
}

class KeychainProtector final : public SecretProtector {
public:
	bool available() const override { return true; }

	std::optional<std::string> protect(const std::string &plain) const override
	{
		const std::string id = new_store_item_id();
		CfRef<CFMutableDictionaryRef> attrs(item_query(id));
		CfRef<CFStringRef> label(make_string("Overflow display password"));
		CfRef<CFDataRef> data(CFDataCreate(kCFAllocatorDefault, reinterpret_cast<const UInt8 *>(plain.data()),
						   static_cast<CFIndex>(plain.size())));
		if (!attrs.ref || !label.ref || !data.ref)
			return std::nullopt;
		CFDictionarySetValue(attrs.ref, kSecAttrLabel, label.ref);
		CFDictionarySetValue(attrs.ref, kSecValueData, data.ref);
		if (SecItemAdd(attrs.ref, nullptr) != errSecSuccess)
			return std::nullopt;
		return std::string(kPrefix) + ":" + id;
	}

	std::optional<std::string> unprotect(const std::string &blob) const override
	{
		const std::string id = store_item_id(blob, kPrefix);
		if (id.empty())
			return std::nullopt;
		CfRef<CFMutableDictionaryRef> q(item_query(id));
		if (!q.ref)
			return std::nullopt;
		CFDictionarySetValue(q.ref, kSecReturnData, kCFBooleanTrue);
		CFDictionarySetValue(q.ref, kSecMatchLimit, kSecMatchLimitOne);
		CFTypeRef out = nullptr;
		if (SecItemCopyMatching(q.ref, &out) != errSecSuccess || !out)
			return std::nullopt;
		CfRef<CFTypeRef> owned(out);
		if (CFGetTypeID(out) != CFDataGetTypeID())
			return std::nullopt;
		const auto data = static_cast<CFDataRef>(out);
		return std::string(reinterpret_cast<const char *>(CFDataGetBytePtr(data)),
				   static_cast<size_t>(CFDataGetLength(data)));
	}

	void discard(const std::string &blob) const override
	{
		const std::string id = store_item_id(blob, kPrefix);
		if (id.empty())
			return;
		CfRef<CFMutableDictionaryRef> q(item_query(id));
		if (q.ref)
			SecItemDelete(q.ref);
	}
};

} // namespace

std::unique_ptr<SecretProtector> make_keychain_protector()
{
	return std::make_unique<KeychainProtector>();
}

} // namespace airplay
