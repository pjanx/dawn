//
// display-profile-linux.cpp: display ICC via _ICC_PROFILE, or from colord
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <colord.h>
#include <gio/gio.h>
#include <lcms2.h>

#include <QGuiApplication>
#include <QScreen>
#include <QtLogging>

#if QT_CONFIG(xcb)
#include <QAbstractNativeEventFilter>

#include <xcb/randr.h>
#include <xcb/xcb.h>

#include <algorithm>
#endif

#include <utility>

using namespace std;

namespace dn
{

// --- X11 ---------------------------------------------------------------------

// The OpenICC "ICC Profiles in X Specification" 0.4 publishes target profiles
// on the root window: _ICC_PROFILE for the first monitor, _ICC_PROFILE_<N> for
// any further one, numbered in Xinerama order.  Xorg derives that order from
// RandR monitors, whose names Qt also gives to its screens.

#if QT_CONFIG(xcb)

// Watch a generous fixed range of monitors, rather than interning atoms anew
// as displays come and go.
static constexpr int kWatchedMonitors = 16;

static string
icc_property(int monitor)
{
	string name = "_ICC_PROFILE";
	if (monitor > 0)
		name += "_" + to_string(monitor);
	return name;
}

static xcb_connection_t *
x11_connection()
{
	if (!qGuiApp)
		return nullptr;

	auto *x11 = qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
	return x11 ? x11->connection() : nullptr;
}

static xcb_atom_t
x11_atom(xcb_connection_t *c, const string &name)
{
	auto *reply = xcb_intern_atom_reply(c,
		xcb_intern_atom(c, false, uint16_t(name.length()), name.data()),
		nullptr);
	if (!reply)
		return XCB_ATOM_NONE;

	const xcb_atom_t atom = reply->atom;
	free(reply);
	return atom;
}

static int
x11_monitor_index(xcb_connection_t *c, xcb_window_t root, xcb_atom_t name)
{
	// Collect the error: RandR 1.5 predates neither Qt 6.8 nor this project,
	// but an older server would have Qt log the protocol error for us.
	xcb_generic_error_t *error = nullptr;
	auto *reply = xcb_randr_get_monitors_reply(
		c, xcb_randr_get_monitors(c, root, true), &error);
	free(error);
	if (!reply)
		return -1;

	int index = -1, i = 0;
	for (auto it = xcb_randr_get_monitors_monitors_iterator(reply); it.rem;
		xcb_randr_monitor_info_next(&it), i++) {
		if (it.data->name == name) {
			index = i;
			break;
		}
	}
	free(reply);
	return index;
}

static vector<unsigned char>
x11_property(xcb_connection_t *c, xcb_window_t root, xcb_atom_t atom)
{
	// Profiles are large enough that their length needs to be asked for.
	auto *header = xcb_get_property_reply(c,
		xcb_get_property(c, false, root, atom, XCB_GET_PROPERTY_TYPE_ANY, 0, 0),
		nullptr);
	if (!header)
		return {};

	const uint32_t length = header->bytes_after;
	const uint8_t format = header->format;
	free(header);
	if (format != 8 || !length)
		return {};

	auto *reply = xcb_get_property_reply(c,
		xcb_get_property(c, false, root, atom, XCB_GET_PROPERTY_TYPE_ANY, 0,
			(length + 3) / 4),
		nullptr);
	if (!reply)
		return {};

	const auto *data =
		static_cast<const unsigned char *>(xcb_get_property_value(reply));
	vector<unsigned char> bytes(
		data, data + xcb_get_property_value_length(reply));
	free(reply);
	return bytes;
}

static DisplayProfile
load_from_x11(const QScreen *screen)
{
	DisplayProfile result;
	xcb_connection_t *c = x11_connection();
	if (!c || !screen)
		return result;

	// Qt tells nothing about X screens, and ":0.1" displays are a curiosity.
	auto roots = xcb_setup_roots_iterator(xcb_get_setup(c));
	if (!roots.rem)
		return result;

	const xcb_window_t root = roots.data->root;
	const string connector = screen->name().toStdString();
	const int monitor = x11_monitor_index(c, root, x11_atom(c, connector));
	if (monitor < 0)
		return result;

	const string property = icc_property(monitor);
	result.icc = x11_property(c, root, x11_atom(c, property));
	if (result.icc.empty())
		return {};
	if (result.icc.size() < 128) {
		qWarning("X11: %s is not an ICC profile", property.c_str());
		return {};
	}

	result.source = "X11";
	result.label = property;
	qInfo("ICC source: X11 (connector=%s, property=%s)", connector.c_str(),
		property.c_str());
	return result;
}

namespace
{

struct RootPropertyFilter final : QAbstractNativeEventFilter {
	vector<xcb_atom_t> atoms;
	function<void()> on_change;

	bool nativeEventFilter(
		const QByteArray &type, void *message, qintptr *) override;
};

}  // namespace

bool
RootPropertyFilter::nativeEventFilter(
	const QByteArray &type, void *message, qintptr *)
{
	if (type != "xcb_generic_event_t")
		return false;

	auto *event = static_cast<xcb_generic_event_t *>(message);
	if ((event->response_type & ~0x80) != XCB_PROPERTY_NOTIFY)
		return false;

	const xcb_atom_t atom =
		static_cast<xcb_property_notify_event_t *>(message)->atom;
	if (this->on_change &&
		find(this->atoms.begin(), this->atoms.end(), atom) != this->atoms.end())
		this->on_change();
	return false;
}

static void
watch_x11(RootPropertyFilter *filter)
{
	xcb_connection_t *c = x11_connection();
	if (!c)
		return;

	for (int i = 0; i < kWatchedMonitors; i++)
		filter->atoms.push_back(x11_atom(c, icc_property(i)));

	// Qt already selects PropertyChange on root windows for itself.
	qGuiApp->installNativeEventFilter(filter);
}

#else

// Qt can be built without X11 support, and then there is nothing to read.
namespace
{

struct RootPropertyFilter {
	function<void()> on_change;
};

}  // namespace

static DisplayProfile
load_from_x11(const QScreen *)
{
	return {};
}

static void
watch_x11(RootPropertyFilter *)
{
}

#endif

// --- colord ------------------------------------------------------------------

static vector<unsigned char>
profile_bytes(cmsHPROFILE profile)
{
	cmsUInt32Number size = 0;
	if (!profile || !cmsSaveProfileToMem(profile, nullptr, &size) || !size)
		return {};

	vector<unsigned char> bytes(size);
	if (!cmsSaveProfileToMem(profile, bytes.data(), &size))
		return {};

	bytes.resize(size);
	return bytes;
}

static bool
display_device(CdDevice *device)
{
	if (!device)
		return true;

	const CdDeviceKind kind = cd_device_get_kind(device);
	return kind == CD_DEVICE_KIND_UNKNOWN || kind == CD_DEVICE_KIND_DISPLAY;
}

static DisplayProfile
load_from_client(CdClient *client, const QScreen *screen)
{
	DisplayProfile result;
	if (!screen) {
		qWarning("display profile: Qt has not assigned a screen yet");
		return result;
	}
	if (!client || !cd_client_get_connected(client))
		return result;
	const string connector = screen->name().toStdString();
	if (connector.empty()) {
		qWarning("display profile: Qt screen has no connector name yet");
		return result;
	}

	g_autoptr(GError) error = nullptr;
	g_autoptr(GPtrArray) devices = cd_client_get_devices_by_kind_sync(
		client, CD_DEVICE_KIND_DISPLAY, nullptr, &error);
	if (!devices) {
		qWarning("colord: get display devices: %s",
			error ? error->message : "failed");
		return result;
	}

	CdDevice *matched = nullptr;
	string method;
	for (guint i = 0; i < devices->len; i++) {
		auto *device = static_cast<CdDevice *>(g_ptr_array_index(devices, i));
		if (!cd_device_connect_sync(device, nullptr, &error)) {
			g_clear_error(&error);
			continue;
		}
		const char *name =
			cd_device_get_metadata_item(device, CD_DEVICE_METADATA_XRANDR_NAME);
		if (name && connector == name) {
			matched = device;
			method = "XRANDR_name";
			break;
		}
	}
	if (!matched) {
		qWarning("colord: no device for connector %s", connector.c_str());
		return result;
	}

	CdProfile *profile = cd_device_get_default_profile(matched);
	if (!profile || !cd_profile_connect_sync(profile, nullptr, &error)) {
		qWarning(
			"colord: display profile unavailable for %s", connector.c_str());
		return result;
	}
	CdIcc *icc =
		cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_ALL, nullptr, &error);
	if (!icc) {
		qWarning("colord: load ICC: %s", error ? error->message : "failed");
		return result;
	}
	result.icc =
		profile_bytes(static_cast<cmsHPROFILE>(cd_icc_get_handle(icc)));
	g_object_unref(icc);
	if (result.icc.empty())
		return {};

	result.source = "colord";
	const char *filename = cd_profile_get_filename(profile);
	const char *profile_id = cd_profile_get_id(profile);
	result.label = filename && *filename
		? filename
		: (profile_id && *profile_id ? profile_id : "colord");
	qInfo("ICC source: colord (connector=%s via %s, profile=%s)",
		connector.c_str(), method.c_str(), result.label.c_str());
	return result;
}

namespace
{

struct ColordSource final : DisplayProfileSource {
	CdClient *client = nullptr;
	guint name_watch = 0;
	bool signals_hooked = false;
	function<void()> on_change;
	RootPropertyFilter filter;

	~ColordSource() override;
	void start(function<void()> fn) override;
	DisplayProfile load(QScreen *screen) override;
	void notify() const;
	void hook_signals();
	void watch_name();
	void connect_async();
};

}  // namespace

static void
on_device(CdClient *, CdDevice *device, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	if (display_device(device))
		src->notify();
}

static void
on_changed(CdClient *, gpointer data)
{
	static_cast<ColordSource *>(data)->notify();
}

static void
on_profile(CdClient *, CdProfile *, gpointer data)
{
	static_cast<ColordSource *>(data)->notify();
}

static void
on_connect_ready(GObject *source, GAsyncResult *res, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	g_autoptr(GError) error = nullptr;
	if (!cd_client_connect_finish(CD_CLIENT(source), res, &error)) {
		qWarning("colord: connect: %s", error ? error->message : "failed");
		return;
	}
	src->hook_signals();
	src->notify();
}

static void
on_name_appeared(GDBusConnection *, const gchar *, const gchar *, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	if (src->client && !cd_client_get_connected(src->client))
		src->connect_async();
}

static void
on_name_vanished(GDBusConnection *, const gchar *, gpointer)
{
}

void
ColordSource::notify() const
{
	if (this->on_change)
		this->on_change();
}

void
ColordSource::hook_signals()
{
	if (this->signals_hooked || !this->client)
		return;

	this->signals_hooked = true;
	g_signal_connect(this->client, "device-added", G_CALLBACK(on_device), this);
	g_signal_connect(
		this->client, "device-removed", G_CALLBACK(on_device), this);
	g_signal_connect(
		this->client, "device-changed", G_CALLBACK(on_device), this);
	g_signal_connect(
		this->client, "profile-changed", G_CALLBACK(on_profile), this);
	g_signal_connect(this->client, "changed", G_CALLBACK(on_changed), this);
}

void
ColordSource::connect_async()
{
	if (this->client && !cd_client_get_connected(this->client))
		cd_client_connect(this->client, nullptr, on_connect_ready, this);
}

void
ColordSource::watch_name()
{
	if (this->name_watch)
		return;

	this->name_watch = g_bus_watch_name(G_BUS_TYPE_SYSTEM,
		"org.freedesktop.ColorManager", G_BUS_NAME_WATCHER_FLAGS_NONE,
		on_name_appeared, on_name_vanished, this, nullptr);
}

ColordSource::~ColordSource()
{
	if (this->name_watch)
		g_bus_unwatch_name(this->name_watch);
	if (this->client)
		g_object_unref(this->client);
}

void
ColordSource::start(function<void()> fn)
{
	this->on_change = std::move(fn);
	this->filter.on_change = [this] { this->notify(); };
	watch_x11(&this->filter);
	if (this->client)
		return;

	this->client = cd_client_new();
	g_autoptr(GError) error = nullptr;
	if (cd_client_connect_sync(this->client, nullptr, &error)) {
		this->hook_signals();
		return;
	}
	qWarning("colord: connect: %s", error ? error->message : "failed");
	this->watch_name();
}

DisplayProfile
ColordSource::load(QScreen *screen)
{
	// The X11 property is what the session advertises to all its clients,
	// and it needs no guessing at which colord device is this screen.
	if (auto profile = load_from_x11(screen); !profile.icc.empty())
		return profile;
	return load_from_client(this->client, screen);
}

unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<ColordSource>();
}

}  // namespace dn
