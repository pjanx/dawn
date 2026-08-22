//
// app.cpp: process-wide Vulkan, GPU, and top-level windows
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "app.hpp"

#include "dawn-config.h"
#include "vk-device.hpp"
#include "window.hpp"

#if DN_WITH_WAYLAND
#include "wayland-color-bridge.hpp"
#include "wayland-window.hpp"
#endif

#include <QByteArray>
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMetaObject>
#include <QVersionNumber>

#include <cstdio>

#if defined(Q_OS_MACOS)
#include "app-menu-macos.hpp"

#include <dlfcn.h>
#endif

using namespace std;

namespace dn
{

bool
App::init()
{
#if DN_WITH_WAYLAND
	// Vulkan content is a wl_subsurface. Qt's presentAboutToBeQueued waits on
	// wl_surface.frame and marks the window unexposed on timeout; that
	// callback is unreliable for desync subsurfaces under Sway. Disable the
	// wait and the timeout so presentation does not stall. Process-global;
	// must run before the first Wayland window.
	qputenv("QT_WAYLAND_FRAME_CALLBACK_TIMEOUT", QByteArrayLiteral("0"));
	this->needs_csd_ = wayland_needs_csd();
#endif
	QGuiApplication::setQuitOnLastWindowClosed(false);
#if defined(Q_OS_MACOS)
	// QNSView backs a VulkanSurface with QMetalLayer, which arbitrates
	// presentation between Qt's display cycle and a Qt render thread through
	// a display lock. We present the drawable ourselves from the GUI thread,
	// and Qt's side of that arbitration withholds expose and update requests.
	// Process-global; must run before the first window.
	qputenv("QT_MTL_NO_TRANSACTION", QByteArrayLiteral("1"));
	// QCocoaVulkanInstance loadVulkanLibrary("vulkan") — not a dylib name.
	if (!qEnvironmentVariableIsSet("QT_VULKAN_LIB")) {
		if (void *sym = dlsym(RTLD_DEFAULT, "vkGetInstanceProcAddr")) {
			Dl_info info{};
			if (dladdr(sym, &info) && info.dli_fname && info.dli_fname[0])
				qputenv("QT_VULKAN_LIB", info.dli_fname);
		}
	}
#endif
	this->vulkan_instance_.setApiVersion(QVersionNumber(1, 1));
	vk_add_bundled_driver_files();
	this->vulkan_instance_.setExtensions(
		{VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME});
	if (!this->vulkan_instance_.create()) {
		fprintf(stderr, "Qt Vulkan instance creation failed: VkResult %d\n",
			static_cast<int>(this->vulkan_instance_.errorCode()));
		return false;
	}
#if defined(Q_OS_MACOS)
	install_macos_app_menu(this);
#endif
	this->display_profiles_.start();
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

OpenResult
App::open(
	const QString &path, const QString &activation_token, BrowseSetup setup)
{
	const QString resolved = path.isEmpty() ? QDir::currentPath() : path;
	const QFileInfo info(resolved);
	if (!info.exists()) {
		fprintf(stderr, "%s: not found\n", qUtf8Printable(resolved));
		return OpenResult::NotFound;
	}
	if (!info.isReadable()) {
		fprintf(stderr, "%s: permission denied\n", qUtf8Printable(resolved));
		return OpenResult::PermissionDenied;
	}

#if DN_WITH_WAYLAND
	if (QGuiApplication::platformName() == QStringLiteral("wayland")) {
		auto window = make_unique<WaylandWindow>(this);
		if (!window->initialize(resolved, setup))
			return OpenResult::Internal;
		apply_activation_token(activation_token);
		window->show();
		this->windows_.push_back(std::move(window));
		return OpenResult::Ok;
	}
#endif

	auto window = make_unique<Window>(this);
	if (!window->initialize(resolved, setup))
		return OpenResult::Internal;
	apply_activation_token(activation_token);
	window->show();
	this->windows_.push_back(std::move(window));
#if defined(Q_OS_MACOS)
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
		qGuiApp, [this, top] { close(top); }, Qt::QueuedConnection);
}

void
App::quit()
{
	QMetaObject::invokeMethod(
		qGuiApp,
		[this] {
			// Unmap first. The shell is a black SHM buffer; destroying the
			// Vulkan subsurface while it is still mapped is the black window.
			for (unique_ptr<QWindow> &w : this->windows_)
				w->hide();
			QGuiApplication::sync();
			this->windows_.clear();
			QCoreApplication::quit();
		},
		Qt::QueuedConnection);
}

GpuContext &
App::gpu()
{
	return this->gpu_;
}

QVulkanInstance *
App::vulkan_instance()
{
	return &this->vulkan_instance_;
}

Window *
App::key_window() const
{
	QWindow *focus = QGuiApplication::focusWindow();
	Window *fallback = nullptr;
	for (const unique_ptr<QWindow> &w : this->windows_) {
		auto *win = dynamic_cast<Window *>(w.get());
		if (!win)
			continue;
		if (!fallback)
			fallback = win;
		if (w.get() == focus)
			return win;
	}
	return fallback;
}

}  // namespace dn
