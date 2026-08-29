//
// display-profile-windows.cpp: display ICC via Windows ICM
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <QFile>
#include <QObject>
#include <QScreen>
#include <QWinEventNotifier>
#include <QtGui/qscreen_platform.h>
#include <QtLogging>

#include <windows.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// Monitor class GUID {4d36e96e-e325-11ce-bfc1-08002be10318}
constexpr wchar_t kSystemClass[] =
	L"SYSTEM\\CurrentControlSet\\Control\\Class\\"
	L"{4d36e96e-e325-11ce-bfc1-08002be10318}";
constexpr wchar_t kUserLeaf[] =
	L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM\\"
	L"ProfileAssociations\\Display\\{4d36e96e-e325-11ce-bfc1-08002be10318}";
constexpr wchar_t kUserParent[] =
	L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\ICM";

DisplayProfile
load_display_profile(QScreen *screen)
{
	DisplayProfile result;
	if (!screen)
		return result;
	auto *native = screen->nativeInterface<QNativeInterface::QWindowsScreen>();
	if (!native)
		return result;

	MONITORINFOEXW monitor{};
	monitor.cbSize = sizeof(monitor);
	if (!GetMonitorInfoW(native->handle(), &monitor))
		return result;
	HDC dc = CreateDCW(L"DISPLAY", monitor.szDevice, nullptr, nullptr);
	if (!dc)
		return result;

	DWORD length = 0;
	GetICMProfileW(dc, &length, nullptr);
	vector<wchar_t> path(length ? length : 1);
	const bool found = length && GetICMProfileW(dc, &length, path.data());
	DeleteDC(dc);
	if (!found)
		return result;

	const QString filename = QString::fromWCharArray(path.data());
	QFile file(filename);
	if (!file.open(QIODevice::ReadOnly)) {
		qWarning("Windows ICM: cannot read %s", filename.toUtf8().constData());
		return result;
	}
	const QByteArray bytes = file.readAll();
	result.icc.assign(bytes.begin(), bytes.end());
	if (result.icc.empty())
		return {};
	result.source = "Windows ICM";
	result.label = filename.toUtf8().toStdString();
	qInfo("ICC source: Windows ICM (%s)", result.label.c_str());
	return result;
}

struct Watch {
	HKEY key = nullptr;
	HANDLE event = nullptr;
	unique_ptr<QWinEventNotifier> notifier;

	Watch() = default;
	Watch(const Watch &) = delete;
	Watch &operator=(const Watch &) = delete;
	~Watch() { this->close(); }
	void close();
	bool open(HKEY root, const wchar_t *path);
	bool arm();
};

void
Watch::close()
{
	this->notifier.reset();
	if (this->event) {
		CloseHandle(this->event);
		this->event = nullptr;
	}
	if (this->key) {
		RegCloseKey(this->key);
		this->key = nullptr;
	}
}

bool
Watch::open(HKEY root, const wchar_t *path)
{
	if (RegOpenKeyExW(root, path, 0, KEY_NOTIFY | KEY_READ, &this->key) !=
		ERROR_SUCCESS)
		return false;
	this->event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	return this->event != nullptr;
}

bool
Watch::arm()
{
	if (!this->key || !this->event)
		return false;
	return RegNotifyChangeKeyValue(this->key, TRUE,
			   REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET |
				   REG_NOTIFY_THREAD_AGNOSTIC,
			   this->event, TRUE) == ERROR_SUCCESS;
}

struct WcsSource final : DisplayProfileSource {
	function<void()> on_change;
	Watch system;
	Watch user;

	void start(function<void()> fn) override;
	DisplayProfile load(QScreen *screen) override;
	bool bind(Watch &watch, HKEY root, const wchar_t *path);
};

bool
WcsSource::bind(Watch &watch, HKEY root, const wchar_t *path)
{
	if (!watch.open(root, path) || !watch.arm()) {
		watch.close();
		return false;
	}
	watch.notifier = make_unique<QWinEventNotifier>(watch.event);
	QObject::connect(watch.notifier.get(), &QWinEventNotifier::activated,
		[this, &watch](HANDLE) {
			ResetEvent(watch.event);
			watch.arm();
			if (this->on_change)
				this->on_change();
		});
	return true;
}

void
WcsSource::start(function<void()> fn)
{
	this->on_change = std::move(fn);
	if (this->system.notifier || this->user.notifier)
		return;
	if (!this->bind(this->system, HKEY_LOCAL_MACHINE, kSystemClass))
		qWarning("Windows ICM: cannot watch system profile associations");
	if (!this->bind(this->user, HKEY_CURRENT_USER, kUserLeaf) &&
		!this->bind(this->user, HKEY_CURRENT_USER, kUserParent))
		qWarning("Windows ICM: cannot watch user profile associations");
}

DisplayProfile
WcsSource::load(QScreen *screen)
{
	return load_display_profile(screen);
}

}  // namespace
unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<WcsSource>();
}

}  // namespace dn
