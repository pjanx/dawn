//
// app.hpp: process-wide Vulkan, GPU, and top-level windows
//
// Copyright The dawn Authors
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
#include <QVulkanInstance>
#include <QWindow>
#include <QUrl>

#include <cstdint>
#include <memory>
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
