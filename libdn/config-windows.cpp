//
// config-windows.cpp: Windows registry configuration backend
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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
fail(Error *error, const char *operation, LSTATUS status = ERROR_SUCCESS)
{
	if (!error)
		return;
	error->code = Error::Code::Io;
	error->message = operation;
	if (status != ERROR_SUCCESS)
		error->message += ": Windows error " + to_string(status);
}

optional<wstring>
to_wide(string_view value, Error *error)
{
	if (value.empty())
		return wstring{};
	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		value.data(), int(value.size()), nullptr, 0);
	if (!size) {
		fail(error, "invalid UTF-8 configuration string", GetLastError());
		return nullopt;
	}
	wstring out(size_t(size), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
		int(value.size()), out.data(), size)) {
		fail(error, "cannot convert configuration string", GetLastError());
		return nullopt;
	}
	return out;
}

optional<string>
to_utf8(wstring_view value, Error *error)
{
	if (value.empty())
		return string{};
	const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
	if (!size) {
		fail(error, "invalid UTF-16 configuration string", GetLastError());
		return nullopt;
	}
	string out(size_t(size), '\0');
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
		int(value.size()), out.data(), size, nullptr, nullptr)) {
		fail(error, "cannot convert configuration string", GetLastError());
		return nullopt;
	}
	return out;
}

struct RegistryName {
	wstring subkey;
	wstring value;
};

optional<RegistryName>
registry_name(string_view key, Error *error)
{
	const size_t slash = key.rfind('/');
	if (slash == string_view::npos || slash == 0 || slash + 1 == key.size()) {
		fail(error, "invalid configuration key");
		return nullopt;
	}
	string subkey = "Software\\" DAWN_NAMESPACE "\\";
	subkey += key.substr(0, slash);
	for (char &c : subkey)
		if (c == '/')
			c = '\\';
	auto wide_subkey = to_wide(subkey, error);
	auto wide_value = to_wide(key.substr(slash + 1), error);
	if (!wide_subkey || !wide_value)
		return nullopt;
	return RegistryName{std::move(*wide_subkey), std::move(*wide_value)};
}

}  // namespace

optional<string>
config_get(string_view key, Error *error)
{
	if (error)
		*error = {};
	const auto name = registry_name(key, error);
	if (!name)
		return nullopt;
	DWORD bytes = 0;
	LSTATUS status = RegGetValueW(HKEY_CURRENT_USER, name->subkey.c_str(),
		name->value.c_str(), RRF_RT_REG_SZ, nullptr, nullptr, &bytes);
	if (status == ERROR_FILE_NOT_FOUND)
		return nullopt;
	if (status != ERROR_SUCCESS) {
		fail(error, "cannot read configuration value", status);
		return nullopt;
	}
	vector<wchar_t> data((bytes + sizeof(wchar_t) - 1) / sizeof(wchar_t));
	status = RegGetValueW(HKEY_CURRENT_USER, name->subkey.c_str(),
		name->value.c_str(), RRF_RT_REG_SZ, nullptr, data.data(), &bytes);
	if (status != ERROR_SUCCESS) {
		fail(error, "cannot read configuration value", status);
		return nullopt;
	}
	size_t length = bytes / sizeof(wchar_t);
	while (length && data[length - 1] == L'\0')
		--length;
	return to_utf8(wstring_view(data.data(), length), error);
}

bool
config_set(string_view key, string_view value, Error *error)
{
	if (error)
		*error = {};
	const auto name = registry_name(key, error);
	const auto data = to_wide(value, error);
	if (!name || !data)
		return false;
	HKEY handle = nullptr;
	LSTATUS status = RegCreateKeyExW(HKEY_CURRENT_USER, name->subkey.c_str(), 0,
		nullptr, 0, KEY_SET_VALUE, nullptr, &handle, nullptr);
	if (status != ERROR_SUCCESS) {
		fail(error, "cannot open configuration key", status);
		return false;
	}
	status = RegSetValueExW(handle, name->value.c_str(), 0, REG_SZ,
		reinterpret_cast<const BYTE *>(data->c_str()),
		DWORD((data->size() + 1) * sizeof(wchar_t)));
	RegCloseKey(handle);
	if (status != ERROR_SUCCESS) {
		fail(error, "cannot write configuration value", status);
		return false;
	}
	return true;
}

}  // namespace dawn
