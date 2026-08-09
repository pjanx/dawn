//
// config-macos.mm: macOS CFPreferences configuration backend
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn.h"

#include <CoreFoundation/CoreFoundation.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

namespace dawn
{
namespace
{

void
fail(Error *error, const char *message)
{
	if (error)
		*error = {Error::Code::Io, message};
}

CFStringRef
make_cfstring(string_view value, Error *error)
{
	CFStringRef result = CFStringCreateWithBytes(kCFAllocatorDefault,
		reinterpret_cast<const UInt8 *>(value.data()), CFIndex(value.size()),
		kCFStringEncodingUTF8, false);
	if (!result)
		fail(error, "invalid UTF-8 configuration string");
	return result;
}

optional<string>
to_utf8(CFStringRef value, Error *error)
{
	const CFIndex length = CFStringGetLength(value);
	const CFIndex maximum =
		CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
	vector<char> bytes(static_cast<size_t>(maximum));
	if (!CFStringGetCString(
			value, bytes.data(), maximum, kCFStringEncodingUTF8)) {
		fail(error, "cannot convert configuration string to UTF-8");
		return nullopt;
	}
	return string(bytes.data());
}

}  // namespace

optional<string>
config_get(string_view key, Error *error)
{
	if (error)
		*error = {};
	CFStringRef cf_key = make_cfstring(key, error);
	if (!cf_key)
		return nullopt;
	CFPropertyListRef value = CFPreferencesCopyValue(cf_key,
		CFSTR(DAWN_NAMESPACE), kCFPreferencesCurrentUser,
		kCFPreferencesAnyHost);
	CFRelease(cf_key);
	if (!value)
		return nullopt;
	if (CFGetTypeID(value) != CFStringGetTypeID()) {
		CFRelease(value);
		fail(error, "configuration value is not a string");
		return nullopt;
	}
	optional<string> result = to_utf8(CFStringRef(value), error);
	CFRelease(value);
	return result;
}

bool
config_set(string_view key, string_view value, Error *error)
{
	if (error)
		*error = {};
	CFStringRef cf_key = make_cfstring(key, error);
	CFStringRef cf_value = make_cfstring(value, error);
	if (!cf_key || !cf_value) {
		if (cf_key)
			CFRelease(cf_key);
		if (cf_value)
			CFRelease(cf_value);
		return false;
	}
	CFPreferencesSetValue(cf_key, cf_value, CFSTR(DAWN_NAMESPACE),
		kCFPreferencesCurrentUser, kCFPreferencesAnyHost);
	CFRelease(cf_key);
	CFRelease(cf_value);
	if (!CFPreferencesSynchronize(CFSTR(DAWN_NAMESPACE),
			kCFPreferencesCurrentUser, kCFPreferencesAnyHost)) {
		fail(error, "cannot synchronize configuration");
		return false;
	}
	return true;
}

}  // namespace dawn
