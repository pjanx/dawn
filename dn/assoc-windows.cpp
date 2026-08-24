//
// assoc-windows.cpp: IAssocHandler Open With (extension from the path)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "assoc.hpp"

#include <QDir>
#include <QFileInfo>
#include <QSet>

#include <windows.h>

#include <initguid.h>
#include <shlguid.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <string>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

void
ensure_com()
{
	static const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
	(void) hr;
}

QString
extension_of(const QString &path)
{
	// Windows doesn't really work this way.  We could use command-subkey verbs,
	// though it's a lot of code for little gain.
	if (QFileInfo(path).isDir())
		return {};

	const QString suffix = QFileInfo(path).suffix();
	if (suffix.isEmpty())
		return {};

	return QStringLiteral(".") + suffix.toLower();
}

QString
from_wide(const wchar_t *s)
{
	return s ? QString::fromWCharArray(s) : QString();
}

Handler
app_from_handler(IAssocHandler *handler)
{
	Handler a;
	if (!handler)
		return a;
	LPWSTR name = nullptr;
	if (SUCCEEDED(handler->GetName(&name)) && name) {
		a.id = from_wide(name);
		CoTaskMemFree(name);
	}
	LPWSTR ui = nullptr;
	if (SUCCEEDED(handler->GetUIName(&ui)) && ui) {
		a.name = from_wide(ui);
		CoTaskMemFree(ui);
	}
	LPWSTR icon = nullptr;
	int icon_index = 0;
	if (SUCCEEDED(handler->GetIconLocation(&icon, &icon_index)) && icon) {
		a.icon = from_wide(icon);
		if (icon_index)
			a.icon += QStringLiteral(",%1").arg(icon_index);
		CoTaskMemFree(icon);
	}
	if (a.name.isEmpty())
		a.name = a.id;
	return a;
}
vector<Handler>
enum_handlers(const QString &ext, ASSOC_FILTER filter)
{
	vector<Handler> out;
	if (ext.isEmpty())
		return out;
	const wstring wext = ext.toStdWString();
	IEnumAssocHandlers *en = nullptr;
	if (FAILED(SHAssocEnumHandlers(wext.c_str(), filter, &en)) || !en)
		return out;
	IAssocHandler *handler = nullptr;
	ULONG got = 0;
	while (en->Next(1, &handler, &got) == S_OK && handler) {
		Handler a = app_from_handler(handler);
		handler->Release();
		handler = nullptr;
		if (!a.id.isEmpty())
			out.push_back(std::move(a));
	}
	en->Release();
	return out;
}

IAssocHandler *
find_handler(const QString &ext, const QString &id)
{
	if (ext.isEmpty() || id.isEmpty())
		return nullptr;
	const wstring wext = ext.toStdWString();
	IEnumAssocHandlers *en = nullptr;
	if (FAILED(SHAssocEnumHandlers(wext.c_str(), ASSOC_FILTER_NONE, &en)) ||
		!en)
		return nullptr;
	IAssocHandler *found = nullptr;
	IAssocHandler *handler = nullptr;
	ULONG got = 0;
	while (en->Next(1, &handler, &got) == S_OK && handler) {
		LPWSTR name = nullptr;
		if (SUCCEEDED(handler->GetName(&name)) && name) {
			const bool match = from_wide(name) == id;
			CoTaskMemFree(name);
			if (match) {
				found = handler;
				break;
			}
		}
		handler->Release();
		handler = nullptr;
	}
	en->Release();
	return found;
}

}  // namespace

Handler
default_for(const QString &path)
{
	ensure_com();
	const QString ext = extension_of(path);
	if (ext.isEmpty())
		return {};
	const wstring wext = ext.toStdWString();
	wchar_t name[MAX_PATH] = {};
	DWORD name_n = MAX_PATH;
	Handler a;
	if (SUCCEEDED(AssocQueryStringW(
			0, ASSOCSTR_FRIENDLYAPPNAME, wext.c_str(), L"open", name, &name_n)))
		a.name = from_wide(name);
	wchar_t exe[MAX_PATH] = {};
	DWORD exe_n = MAX_PATH;
	if (SUCCEEDED(AssocQueryStringW(
			0, ASSOCSTR_EXECUTABLE, wext.c_str(), L"open", exe, &exe_n)))
		a.id = from_wide(exe);
	if (!a.id.isEmpty()) {
		if (a.name.isEmpty())
			a.name = a.id;
		return a;
	}
	const vector<Handler> rec = enum_handlers(ext, ASSOC_FILTER_RECOMMENDED);
	if (!rec.empty())
		return rec.front();
	return {};
}
vector<Handler>
recommended_for(const QString &path)
{
	ensure_com();
	const Handler def = default_for(path);
	vector<Handler> out;
	for (Handler &a :
		enum_handlers(extension_of(path), ASSOC_FILTER_RECOMMENDED)) {
		if (!def.id.isEmpty() && a.id == def.id)
			continue;
		out.push_back(std::move(a));
	}
	return out;
}
vector<Handler>
fallback_for(const QString &path)
{
	ensure_com();
	const QString ext = extension_of(path);
	const vector<Handler> rec = enum_handlers(ext, ASSOC_FILTER_RECOMMENDED);
	QSet<QString> seen;
	for (const Handler &a : rec)
		seen.insert(a.id);
	vector<Handler> out;
	for (Handler &a : enum_handlers(ext, ASSOC_FILTER_NONE)) {
		if (seen.contains(a.id))
			continue;
		seen.insert(a.id);
		out.push_back(std::move(a));
	}
	return out;
}

bool
launch(const Handler &app, const QString &path)
{
	ensure_com();
	if (app.id.isEmpty() || path.isEmpty())
		return false;
	const QString ext = extension_of(path);
	IAssocHandler *handler = find_handler(ext, app.id);
	if (!handler)
		return false;

	const QString abs = QFileInfo(path).absoluteFilePath();
	const wstring wpath = QDir::toNativeSeparators(abs).toStdWString();
	IShellItem *item = nullptr;
	HRESULT hr = SHCreateItemFromParsingName(
		wpath.c_str(), nullptr, IID_PPV_ARGS(&item));
	if (FAILED(hr) || !item) {
		handler->Release();
		return false;
	}
	IDataObject *data = nullptr;
	hr = item->BindToHandler(nullptr, BHID_DataObject, IID_PPV_ARGS(&data));
	item->Release();
	if (FAILED(hr) || !data) {
		handler->Release();
		return false;
	}
	hr = handler->Invoke(data);
	data->Release();
	handler->Release();
	return SUCCEEDED(hr);
}

void
set_last_used(const Handler &, const QString &)
{
}

}  // namespace dn
