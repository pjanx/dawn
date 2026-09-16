//
// app.hpp: process-wide Vulkan, GPU, and top-level windows
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "display-profile.hpp"
#include "gpu.hpp"
#include "kit-browser.hpp"
#include "kit-chrome.hpp"
#include "thumbnailer.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QString>
#include <QUrl>
#include <QVulkanInstance>
#include <QWindow>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace dn
{

class Window;

// A Wayland shell carries the viewer as a child window; elsewhere the
// top-level is the viewer itself. Nothing here has Q_OBJECT, so qobject_cast
// and findChild() would fall back to QWindow and match anything.
Window *content_window(QObject *window);

enum class SettingsChange : uint8_t { Bookmarks, Preferences };

/// Process-wide user settings and categorized change notification.
class Settings
{
	std::vector<std::pair<void *, std::function<void(SettingsChange)>>>
		listeners_;

	void notify(SettingsChange change) const;
	void update_enabled_loaders();
	// Reads the profile in, so that a path that cannot be used says so when
	// it is chosen rather than at the next repaint.
	void load_icc_override(const std::string &path);

public:
	std::vector<std::string> bookmarks;
	std::vector<unsigned char> icc_profile_override;
	std::string icc_profile_override_path;
	// All loaders this build has, in the configured order.
	std::vector<SettingsDraft::Loader> loaders;
	// The enabled ones, for OpenContext::loaders.  Replaced rather than
	// rewritten, so that a decoding thread may hold on to an older list.
	std::shared_ptr<const std::vector<std::string>> enabled_loaders;
	bool disable_dithering = false;
	bool browser_show_filenames = true;
	int browser_thumbnail_size = 256;

	void load();
	void save(const SettingsDraft &draft);
	[[nodiscard]] bool bookmarked(const std::string &path) const;
	void toggle_bookmark(const std::string &path);
	void listen(void *key, std::function<void(SettingsChange)> fn);
	void unlisten(const void *key);
};

class App : public QGuiApplication
{
	std::vector<std::unique_ptr<QWindow>> windows_;

protected:
	bool event(QEvent *event) override;

public:
	App(int &argc, char **argv)
		: QGuiApplication(argc, argv), thumbnailer(nullptr, 0)
	{
	}

	~App() override
	{
		// Destroy windows before tearing down the GpuContext.
		windows_.clear();
	}

	QVulkanInstance vulkan_instance;
	GpuContext gpu;
	Thumbnailer thumbnailer;
	DisplayProfileWatch display_profiles;
	Settings settings;
	bool needs_csd = false;

	bool init();
	/// Returns whether the window could be created.
	bool open(const QUrl &url, const QString &activation_token,
		BrowseSetup setup, Mode mode);
	void close(const QWindow *top);
	void close_later(const QWindow *top);
	// Not quit(): that name is taken by a static QCoreApplication slot,
	// which Qt's own quit paths go through, skipping the unmapping below.
	void shutdown();
	[[nodiscard]] Window *key_window() const;
};

}  // namespace dn
