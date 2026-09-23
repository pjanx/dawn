//
// display-profile-windows.cpp: display ICC via Windows ICM
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <QAbstractNativeEventFilter>
#include <QCoreApplication>
#include <QFile>
#include <QObject>
#include <QScreen>
#include <QWinEventNotifier>
#include <QtGui/qscreen_platform.h>
#include <QtLogging>

#include <dxgi1_6.h>
#include <windows.h>

#include <array>
#include <cwchar>
#include <functional>
#include <memory>
#include <utility>
#include <vector>

using namespace std;

namespace dn
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

// The DisplayConfig target that shows the GDI device `name`.
static bool
display_config_target(
	const wchar_t *name, DISPLAYCONFIG_DEVICE_INFO_HEADER *out)
{
	UINT32 path_count = 0, mode_count = 0;
	if (GetDisplayConfigBufferSizes(
			QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count) != ERROR_SUCCESS)
		return false;
	vector<DISPLAYCONFIG_PATH_INFO> paths(path_count);
	vector<DISPLAYCONFIG_MODE_INFO> modes(mode_count);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(),
			&mode_count, modes.data(), nullptr) != ERROR_SUCCESS)
		return false;
	for (UINT32 i = 0; i < path_count; i++) {
		DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
		source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		source.header.size = sizeof source;
		source.header.adapterId = paths[i].sourceInfo.adapterId;
		source.header.id = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
			wcscmp(source.viewGdiDeviceName, name) != 0)
			continue;
		out->adapterId = paths[i].targetInfo.adapterId;
		out->id = paths[i].targetInfo.id;
		return true;
	}
	return false;
}

// The HDR form, the primaries and the peak of the output showing `monitor`.
static bool
dxgi_output_desc(HMONITOR monitor, DXGI_OUTPUT_DESC1 *out)
{
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
		return false;
	bool found = false;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT i = 0; !found && SUCCEEDED(factory->EnumAdapters1(i, &adapter));
		i++) {
		IDXGIOutput *output = nullptr;
		for (UINT j = 0; !found && SUCCEEDED(adapter->EnumOutputs(j, &output));
			j++) {
			DXGI_OUTPUT_DESC desc{};
			IDXGIOutput6 *output6 = nullptr;
			if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor &&
				SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6)))) {
				found = SUCCEEDED(output6->GetDesc1(out));
				output6->Release();
			}
			output->Release();
		}
		adapter->Release();
	}
	factory->Release();
	return found;
}

// Advanced Color is on in its HDR form, and in its wide-gamut SDR form,
// which is Windows 11's automatic colour management on SDR displays.
// DisplayConfig tells that it is on, DXGI which form it takes.
static AdvancedColor
load_advanced_color(HMONITOR monitor, const wchar_t *name)
{
	AdvancedColor result;
	DISPLAYCONFIG_DEVICE_INFO_HEADER target{};
	if (!display_config_target(name, &target))
		return result;

	// Set in both forms from Windows 11 22H2 on: observed, not documented.
	DISPLAYCONFIG_GET_ADVANCED_COLOR_INFO info{};
	info.header = target;
	info.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_ADVANCED_COLOR_INFO;
	info.header.size = sizeof info;
	if (DisplayConfigGetDeviceInfo(&info.header) != ERROR_SUCCESS ||
		!info.advancedColorEnabled)
		return result;

	DXGI_OUTPUT_DESC1 desc{};
	if (!dxgi_output_desc(monitor, &desc))
		return result;
	result.active = true;
	result.range.hdr =
		desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
	result.range.primaries =
		array<double, 8>{desc.RedPrimary[0], desc.RedPrimary[1],
			desc.GreenPrimary[0], desc.GreenPrimary[1], desc.BluePrimary[0],
			desc.BluePrimary[1], desc.WhitePoint[0], desc.WhitePoint[1]};

	// In units of 80/1000 cd/m², so that the UI matches the system's.
	DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
	white.header = target;
	white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
	white.header.size = sizeof white;
	if (DisplayConfigGetDeviceInfo(&white.header) == ERROR_SUCCESS &&
		white.SDRWhiteLevel)
		result.white = float(white.SDRWhiteLevel) / 1000;

	// The wide-gamut form has nothing above SDR white, and a high SDR white
	// on a dim panel can take the HDR form to 1 or below.
	if (result.range.hdr)
		result.range.headroom = desc.MaxLuminance / (result.white * 80);
	return result;
}

// The monitor showing `screen`, and its GDI device name in `monitor`.
static HMONITOR
screen_monitor(QScreen *screen, MONITORINFOEXW *monitor)
{
	auto *native = screen
		? screen->nativeInterface<QNativeInterface::QWindowsScreen>()
		: nullptr;
	monitor->cbSize = sizeof *monitor;
	if (!native || !GetMonitorInfoW(native->handle(), monitor))
		return nullptr;
	return native->handle();
}

AdvancedColor
windows_advanced_color(QScreen *screen)
{
	MONITORINFOEXW monitor{};
	HMONITOR handle = screen_monitor(screen, &monitor);
	return handle ? load_advanced_color(handle, monitor.szDevice)
				  : AdvancedColor{};
}

static DisplayProfile
load_display_profile(QScreen *screen)
{
	DisplayProfile result;
	MONITORINFOEXW monitor{};
	HMONITOR handle = screen_monitor(screen, &monitor);
	if (!handle)
		return result;
	result.advanced_color = load_advanced_color(handle, monitor.szDevice);
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
		return result;
	result.source = "Windows ICM";
	result.label = filename.toUtf8().toStdString();
	qInfo("ICC source: Windows ICM (%s)", result.label.c_str());
	return result;
}

namespace
{

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

}  // namespace

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

namespace
{

// Advanced Color toggles change the profile without touching the registry.
struct DisplayChangeFilter final : QAbstractNativeEventFilter {
	function<void()> on_change;

	bool nativeEventFilter(
		const QByteArray &type, void *message, qintptr *) override
	{
		if (type == "windows_generic_MSG" &&
			static_cast<const MSG *>(message)->message == WM_DISPLAYCHANGE &&
			this->on_change)
			this->on_change();
		return false;
	}
};

struct WcsSource final : DisplayProfileSource {
	function<void()> on_change;
	Watch system;
	Watch user;
	unique_ptr<DisplayChangeFilter> display_change;

	~WcsSource() override;

	void start(function<void()> fn) override;
	DisplayProfile load(QScreen *screen) override;
	bool bind(Watch &watch, HKEY root, const wchar_t *path);
};

}  // namespace

WcsSource::~WcsSource()
{
	if (this->display_change && QCoreApplication::instance())
		QCoreApplication::instance()->removeNativeEventFilter(
			this->display_change.get());
}

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
	if (!this->display_change) {
		this->display_change = make_unique<DisplayChangeFilter>();
		QCoreApplication::instance()->installNativeEventFilter(
			this->display_change.get());
	}
	this->display_change->on_change = this->on_change;
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

unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<WcsSource>();
}

}  // namespace dn
