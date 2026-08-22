//
// display-profile-linux.cpp: display ICC via a long-lived colord session
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <colord.h>
#include <gio/gio.h>
#include <lcms2.h>

#include <QScreen>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <utility>

using namespace std;

namespace fs = filesystem;

namespace dn
{
namespace
{

optional<string>
edid_md5_from_bytes(const vector<unsigned char> &edid)
{
	if (edid.empty())
		return nullopt;
	g_autofree gchar *md5 =
		g_compute_checksum_for_data(G_CHECKSUM_MD5, edid.data(), edid.size());
	return md5 ? optional<string>(md5) : nullopt;
}

optional<string>
edid_md5_for_connector(const string &connector)
{
	error_code error;
	for (const auto &entry : fs::directory_iterator("/sys/class/drm", error)) {
		if (!entry.is_directory())
			continue;
		const string dirname = entry.path().filename().string();
		const auto dash = dirname.find('-');
		if (dash == string::npos || dirname.substr(dash + 1) != connector)
			continue;
		ifstream input(entry.path() / "edid", ios::binary);
		if (!input)
			continue;
		vector<unsigned char> bytes(
			(istreambuf_iterator<char>(input)), istreambuf_iterator<char>());
		if (auto md5 = edid_md5_from_bytes(bytes))
			return md5;
	}
	return nullopt;
}

vector<unsigned char>
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

bool
display_device(CdDevice *device)
{
	if (!device)
		return true;
	const CdDeviceKind kind = cd_device_get_kind(device);
	return kind == CD_DEVICE_KIND_UNKNOWN || kind == CD_DEVICE_KIND_DISPLAY;
}

DisplayProfile
load_from_client(CdClient *client, const QScreen *screen)
{
	DisplayProfile result;
	if (!screen) {
		fprintf(stderr, "display profile: Qt has not assigned a screen yet\n");
		return result;
	}
	if (!client || !cd_client_get_connected(client))
		return result;
	const string connector = screen->name().toStdString();
	if (connector.empty()) {
		fprintf(
			stderr, "display profile: Qt screen has no connector name yet\n");
		return result;
	}

	g_autoptr(GError) error = nullptr;
	g_autoptr(GPtrArray) devices = cd_client_get_devices_by_kind_sync(
		client, CD_DEVICE_KIND_DISPLAY, nullptr, &error);
	if (!devices) {
		fprintf(stderr, "colord: get display devices: %s\n",
			error ? error->message : "failed");
		return result;
	}

	CdDevice *matched = nullptr;
	string method;
	for (guint i = 0; i < devices->len; ++i) {
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
		if (auto edid_md5 = edid_md5_for_connector(connector)) {
			for (guint i = 0; i < devices->len; ++i) {
				auto *device =
					static_cast<CdDevice *>(g_ptr_array_index(devices, i));
				const char *md5 = cd_device_get_metadata_item(
					device, CD_DEVICE_METADATA_OUTPUT_EDID_MD5);
				if (md5 && *edid_md5 == md5) {
					matched = device;
					method = "OutputEdidMd5";
					break;
				}
			}
		}
	}
	if (!matched) {
		fprintf(
			stderr, "colord: no device for connector %s\n", connector.c_str());
		return result;
	}

	CdProfile *profile = cd_device_get_default_profile(matched);
	if (!profile || !cd_profile_connect_sync(profile, nullptr, &error)) {
		fprintf(stderr, "colord: display profile unavailable for %s\n",
			connector.c_str());
		return result;
	}
	CdIcc *icc =
		cd_profile_load_icc(profile, CD_ICC_LOAD_FLAGS_ALL, nullptr, &error);
	if (!icc) {
		fprintf(stderr, "colord: load ICC: %s\n",
			error ? error->message : "failed");
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
	printf("ICC source: colord (connector=%s via %s, profile=%s)\n",
		connector.c_str(), method.c_str(), result.label.c_str());
	return result;
}

struct ColordSource final : DisplayProfileSource {
	CdClient *client = nullptr;
	guint name_watch = 0;
	bool signals_hooked = false;
	function<void()> on_change;

	~ColordSource() override;
	void start(function<void()> fn) override;
	DisplayProfile load(QScreen *screen) override;
	void notify() const;
	void hook_signals();
	void watch_name();
	void connect_async();
};

void
on_device(CdClient *, CdDevice *device, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	if (!display_device(device))
		return;
	src->notify();
}

void
on_changed(CdClient *, gpointer data)
{
	static_cast<ColordSource *>(data)->notify();
}

void
on_profile(CdClient *, CdProfile *, gpointer data)
{
	static_cast<ColordSource *>(data)->notify();
}

void
on_connect_ready(GObject *source, GAsyncResult *res, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	g_autoptr(GError) error = nullptr;
	if (!cd_client_connect_finish(CD_CLIENT(source), res, &error)) {
		fprintf(
			stderr, "colord: connect: %s\n", error ? error->message : "failed");
		return;
	}
	src->hook_signals();
	src->notify();
}

void
on_name_appeared(GDBusConnection *, const gchar *, const gchar *, gpointer data)
{
	auto *src = static_cast<ColordSource *>(data);
	if (!src->client || cd_client_get_connected(src->client))
		return;
	src->connect_async();
}

void
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
	if (!this->client || cd_client_get_connected(this->client))
		return;
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
	if (this->client)
		return;
	this->client = cd_client_new();
	g_autoptr(GError) error = nullptr;
	if (cd_client_connect_sync(this->client, nullptr, &error)) {
		this->hook_signals();
		return;
	}
	fprintf(stderr, "colord: connect: %s\n", error ? error->message : "failed");
	this->watch_name();
}

DisplayProfile
ColordSource::load(QScreen *screen)
{
	return load_from_client(this->client, screen);
}

}  // namespace

unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<ColordSource>();
}

}  // namespace dn
