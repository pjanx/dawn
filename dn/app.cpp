//
// app.cpp: process-wide Vulkan, GPU, and top-level windows
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "app.hpp"

#include "libdn/vk-device.hpp"
#include "url.hpp"
#include "window.hpp"

#if DN_WITH_WAYLAND
#include "wayland-color-bridge.hpp"
#include "wayland-window.hpp"
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QUrl>
#include <QVersionNumber>
#include <QtLogging>

#include <algorithm>
#include <charconv>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#ifdef Q_OS_MACOS
#include "app-menu-macos.hpp"

#include <dlfcn.h>
#endif

using namespace std;

namespace dn
{

namespace
{

constexpr string_view kBookmarksKey = "dn/Bookmarks";
constexpr string_view kDitheringKey = "dn/DisableDithering";
constexpr string_view kFilenamesKey = "dn/BrowserShowFilenames";
constexpr string_view kThumbnailKey = "dn/BrowserThumbnailSize";
constexpr string_view kProfileKey = "dn/ICCProfileOverride";

void
config_warn(string_view key, const char *message)
{
	qWarning("configuration %.*s: %s", int(key.size()), key.data(), message);
}

optional<string>
setting(string_view key)
{
	dawn::Error error;
	optional<string> value = dawn::config_get(key, &error);
	if (error)
		config_warn(key, error.message.c_str());
	return value;
}

void
set_setting(string_view key, string_view value)
{
	dawn::Error error;
	if (!dawn::config_set(key, value, &error))
		config_warn(key, error.message.c_str());
}

bool
boolean_setting(string_view key, bool fallback)
{
	const optional<string> value = setting(key);
	if (!value)
		return fallback;
	if (*value == "true" || *value == "1")
		return true;
	if (*value == "false" || *value == "0")
		return false;
	config_warn(key, "expected true, false, 1, or 0");
	return fallback;
}

void
set_boolean_setting(string_view key, bool value)
{
	set_setting(key, value ? "true" : "false");
}

// Answers 0 for anything the browser has no thumbnails for.
int
parse_thumbnail_size(const string &value)
{
	int size = 0;
	const auto parsed =
		from_chars(value.data(), value.data() + value.size(), size);
	if (parsed.ec != errc{} || parsed.ptr != value.data() + value.size())
		return 0;
	for (const int candidate : kThumbSizes) {
		if (candidate == size)
			return size;
	}
	return 0;
}

char
bookmark_separator()
{
#ifdef Q_OS_WIN
	return ';';
#else
	return ':';
#endif
}

vector<string>
split_bookmarks(const string &value)
{
	vector<string> out;
	const char separator = bookmark_separator();
	for (size_t offset = 0; offset <= value.size();) {
		const size_t end = value.find(separator, offset);
		const string item = value.substr(
			offset, end == string::npos ? string::npos : end - offset);
		if (!item.empty())
			out.push_back(item);
		if (end == string::npos)
			break;
		offset = end + 1;
	}
	return out;
}

string
join_bookmarks(const vector<string> &bookmarks)
{
	string value;
	for (const string &bookmark : bookmarks) {
		if (!value.empty())
			value += bookmark_separator();
		value += bookmark;
	}
	return value;
}

}  // namespace

// Bookmarks are compared by path, so they are stored canonicalised: the
// sidebar highlights the open directory by std::filesystem::equivalent,
// and a trailing separator or an unresolved symlink must not disagree.
static string
canonical_dir(const string &path)
{
	error_code ec;
	filesystem::path p = filesystem::weakly_canonical(path, ec);
	if (ec)
		p = path;
	while (p.filename().empty()) {
		const filesystem::path parent = p.parent_path();
		if (parent.empty() || parent == p)
			break;
		p = parent;
	}
	return p.string();
}

void
Settings::load_icc_override(const string &path)
{
	this->icc_profile_override.clear();
	this->icc_profile_override_path.clear();
	if (path.empty())
		return;

	QFile file(QString::fromUtf8(path.data(), qsizetype(path.size())));
	if (!file.open(QIODevice::ReadOnly)) {
		config_warn(kProfileKey, ("cannot open " + path).c_str());
		return;
	}
	const QByteArray bytes = file.readAll();
	if (bytes.isEmpty()) {
		config_warn(kProfileKey, "empty ICC profile");
		return;
	}
	this->icc_profile_override.assign(bytes.begin(), bytes.end());
	this->icc_profile_override_path = path;
}

void
Settings::load()
{
	this->disable_dithering = boolean_setting(kDitheringKey, false);
	this->browser_show_filenames = boolean_setting(kFilenamesKey, true);
	if (const optional<string> value = setting(kThumbnailKey)) {
		if (const int size = parse_thumbnail_size(*value))
			this->browser_thumbnail_size = size;
		else
			config_warn(kThumbnailKey, "unsupported size");
	}

	this->bookmarks.clear();
	if (const optional<string> value = setting(kBookmarksKey))
		this->bookmarks = split_bookmarks(*value);

	load_icc_override(setting(kProfileKey).value_or(string()));
}

void
Settings::save(const SettingsDraft &draft)
{
	this->browser_thumbnail_size = draft.thumbnail_size;
	this->browser_show_filenames = draft.show_filenames;
	this->disable_dithering = draft.disable_dithering;
	set_setting(kThumbnailKey, to_string(draft.thumbnail_size));
	set_boolean_setting(kFilenamesKey, draft.show_filenames);
	set_boolean_setting(kDitheringKey, draft.disable_dithering);

	const string path = draft.icc_profile_path.toStdString();
	set_setting(kProfileKey, path);
	load_icc_override(path);
	notify();
}

bool
Settings::bookmarked(const string &path) const
{
	const string want = canonical_dir(path);
	return find(this->bookmarks.begin(), this->bookmarks.end(), want) !=
		this->bookmarks.end();
}

void
Settings::toggle_bookmark(const string &path)
{
	const string want = canonical_dir(path);
	const auto it =
		find(this->bookmarks.begin(), this->bookmarks.end(), want);
	if (it != this->bookmarks.end())
		this->bookmarks.erase(it);
	else
		this->bookmarks.push_back(want);
	notify();
	set_setting(kBookmarksKey, join_bookmarks(this->bookmarks));
}

void
Settings::listen(void *key, function<void()> fn)
{
	unlisten(key);
	this->listeners_.emplace_back(key, std::move(fn));
}

void
Settings::unlisten(const void *key)
{
	erase_if(this->listeners_,
		[key](const auto &item) { return item.first == key; });
}

void
Settings::notify() const
{
	// All mutations originate on the GUI thread, so unlike DisplayProfileWatch
	// this needs no queued hop.  Copy: a callback may unlisten.
	const auto copy = this->listeners_;
	for (const auto &item : copy)
		if (item.second)
			item.second();
}

// Finder delivers a document to open as a QFileOpenEvent, not an argument.
bool
App::event(QEvent *event)
{
	if (event->type() == QEvent::Quit) {
		shutdown();
		return true;
	}
	if (event->type() == QEvent::FileOpen) {
		open(url_normalized(((QFileOpenEvent *) event)->url()));
		return true;
	}
	return QGuiApplication::event(event);
}

bool
App::init()
{
	this->settings.load();
#if DN_WITH_WAYLAND
	// Vulkan content is a wl_subsurface. Qt's presentAboutToBeQueued waits on
	// wl_surface.frame and marks the window unexposed on timeout; that
	// callback is unreliable for desync subsurfaces under Sway. Disable the
	// wait and the timeout so presentation does not stall. Process-global;
	// must run before the first Wayland window.
	qputenv("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT", "0");
	this->needs_csd = wayland_needs_csd();
#endif
	// The Wayland close dance hides the shell before closing it, so Qt never
	// emits lastWindowClosed; App::close() counts windows itself.
	// See WaylandWindow::finish_close().
	QGuiApplication::setQuitOnLastWindowClosed(false);
#ifdef Q_OS_MACOS
	// QNSView backs a VulkanSurface with QMetalLayer, which arbitrates
	// presentation between Qt's display cycle and a Qt render thread through
	// a display lock. We present the drawable ourselves from the GUI thread,
	// and Qt's side of that arbitration withholds expose and update requests.
	// Process-global; must run before the first window.
	qputenv("QT_MTL_NO_TRANSACTION", "1");
	// QCocoaVulkanInstance loadVulkanLibrary("vulkan") — not a dylib name.
	if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) {
		if (void *sym = dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")) {
			Dl_info info{};
			if (dladdr(sym, &info) && info.dli_fname && info.dli_fname[0])
				qputenv("QT_VULKAN_LIB", info.dli_fname);
		}
	}
#endif
	this->vulkan_instance.setApiVersion(QVersionNumber(1, 1));
	dawn::vk_add_bundled_driver_files();
	this->vulkan_instance.setExtensions(
		{VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME});
	if (!this->vulkan_instance.create()) {
		qWarning("Qt Vulkan instance creation failed: VkResult %d",
			int(this->vulkan_instance.errorCode()));
		return false;
	}
#ifdef Q_OS_MACOS
	install_macos_app_menu(this);
#endif
	this->display_profiles.start();
	return true;
}

// xdg-shell requestActivate accepts a foreign token only via
// XDG_ACTIVATION_TOKEN; there is no public setter. show() consumes it.
static void
apply_activation_token(const QString &token)
{
	if (!token.isEmpty())
		qputenv("XDG_ACTIVATION_TOKEN", token.toUtf8());
}

// Windows has no token to carry: the process that asked for the window
// hands over its foreground right first, and only then does this work.
// See AllowSetForegroundWindow in main.cpp.
static void
raise_window(Window *window)
{
#ifdef Q_OS_WIN
	window->requestActivate();
#else
	(void) window;
#endif
}

OpenResult
App::open(const QUrl &url, const QString &activation_token, BrowseSetup setup,
	bool browse)
{
	// Until there is a VFS, this is where anything but file:// stops.
	const QString path = url_to_path(url);
	if (path.isEmpty()) {
		qWarning("%s: unsupported location",
			qUtf8Printable(url.toString(QUrl::PrettyDecoded)));
		return OpenResult::InvalidArgument;
	}

	const QFileInfo info(path);
	if (!info.exists()) {
		qWarning("%s: not found", qUtf8Printable(path));
		return OpenResult::NotFound;
	}
	if (!info.isReadable()) {
		qWarning("%s: permission denied", qUtf8Printable(path));
		return OpenResult::PermissionDenied;
	}

	// Windows compare and store this, so hand them a canonical form.
	const QUrl resolved = QUrl::fromLocalFile(info.absoluteFilePath());
	if (Window *target = this->default_window) {
		this->default_window = nullptr;
		if (target->current_url() == path_to_url(QDir::currentPath())) {
			target->open_any(resolved, browse);
			apply_activation_token(activation_token);
			raise_window(target);
			return OpenResult::Ok;
		}
	}

#if DN_WITH_WAYLAND
	if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
		auto window = make_unique<WaylandWindow>(this);
		if (!window->initialize(resolved, setup, browse))
			return OpenResult::Internal;
		apply_activation_token(activation_token);
		window->show();
		this->windows_.push_back(std::move(window));
		return OpenResult::Ok;
	}
#endif

	auto window = make_unique<Window>(this);
	if (!window->initialize(resolved, setup, browse))
		return OpenResult::Internal;
	apply_activation_token(activation_token);
	window->show();
	raise_window(window.get());
	this->windows_.push_back(std::move(window));
#ifdef Q_OS_MACOS
	sync_macos_app_menu(this);
#endif
	return OpenResult::Ok;
}

void
App::close(const QWindow *top)
{
	erase_if(this->windows_, [top](const unique_ptr<QWindow> &window) {
		return window.get() == top;
	});
	if (this->windows_.empty())
		QCoreApplication::quit();
}

void
App::close_later(const QWindow *top)
{
	QMetaObject::invokeMethod(
		this, [this, top] { close(top); }, Qt::QueuedConnection);
}

void
App::shutdown()
{
	QMetaObject::invokeMethod(
		this,
		[this] {
			// Unmap first. The shell is a black SHM buffer; destroying the
			// Vulkan subsurface while it is still mapped is the black window.
			for (unique_ptr<QWindow> &w : this->windows_)
				w->hide();
			QGuiApplication::sync();
			this->windows_.clear();
			QCoreApplication::exit(0);
		},
		Qt::QueuedConnection);
}

// A Wayland shell carries the viewer as a child window; elsewhere the
// top-level is the viewer itself. Nothing here has Q_OBJECT, so qobject_cast
// and findChild() would fall back to QWindow and match anything.
static Window *
content_window(QObject *window)
{
	if (auto *win = dynamic_cast<Window *>(window))
		return win;
	for (QObject *child : window->children())
		if (auto *win = dynamic_cast<Window *>(child))
			return win;
	return nullptr;
}

Window *
App::key_window() const
{
	if (QWindow *focus = QGuiApplication::focusWindow())
		if (Window *win = content_window(focus))
			return win;
	for (const unique_ptr<QWindow> &w : this->windows_)
		if (Window *win = content_window(w.get()))
			return win;
	return nullptr;
}

}  // namespace dn
