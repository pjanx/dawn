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
#include "thumbnailer.hpp"

#include <QEvent>
#include <QGuiApplication>
#include <QPointer>
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

enum class OpenResult : uint8_t {
	Ok,
	NotFound,
	PermissionDenied,
	InvalidArgument,
	Internal
};

/// Process-wide user settings and change notification.
/// Notification is coarse: any change notifies all listeners.
class Settings
{
	std::vector<std::string> bookmarks_;
	std::vector<std::pair<void *, std::function<void()>>> listeners_;

	void notify() const;

public:
	[[nodiscard]] const std::vector<std::string> &bookmarks() const
	{
		return this->bookmarks_;
	}
	[[nodiscard]] bool bookmarked(const std::string &path) const;
	void toggle_bookmark(const std::string &path);
	void listen(void *key, std::function<void()> fn);
	void unlisten(const void *key);
};

class App : public QGuiApplication
{
	std::vector<std::unique_ptr<QWindow>> windows_;

protected:
	bool event(QEvent *event) override;

public:
	App(int &argc, char **argv) : QGuiApplication(argc, argv) {}

	QVulkanInstance vulkan_instance;
	GpuContext gpu;
	Thumbnailer thumbnailer;
	DisplayProfileWatch display_profiles;
	Settings settings;
	// macOS may open us blank before passing us association-opened files.
	QPointer<Window> default_window;
	bool needs_csd = false;

	bool init();
	OpenResult open(const QUrl &url, const QString &activation_token = {},
		BrowseSetup setup = {}, bool browse = false);
	void close(const QWindow *top);
	void close_later(const QWindow *top);
	// Not quit(): that name is taken by a static QCoreApplication slot,
	// which Qt's own quit paths go through, skipping the unmapping below.
	void shutdown();
	[[nodiscard]] Window *key_window() const;
};

}  // namespace dn
