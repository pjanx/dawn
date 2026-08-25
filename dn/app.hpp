//
// app.hpp: process-wide Vulkan, GPU, and top-level windows
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "browser.hpp"
#include "display-profile.hpp"
#include "gpu.hpp"
#include "thumbnailer.hpp"

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

class App
{
public:
	bool init();
	// browse routes a file argument to its parent directory, with the file
	// selected, rather than to the viewer. Directories are unaffected. If a
	// default window is registered, it is redirected in place instead of
	// opening a new one.
	OpenResult open(const QUrl &path, const QString &activation_token = {},
		BrowseSetup setup = {}, bool browse = false);
	void close(const QWindow *top);
	void close_later(const QWindow *top);
	void quit();
	// A window opened speculatively (a bare launch, before it's known
	// whether more is coming) is redirected rather than left as a stray
	// extra window, but only for as long as nothing else has repurposed it.
	[[nodiscard]] QPointer<Window> &default_window()
	{
		return this->default_window_;
	}
	[[nodiscard]] GpuContext &gpu();
	[[nodiscard]] Thumbnailer &thumbnailer() { return this->thumbnailer_; }
	[[nodiscard]] QVulkanInstance *vulkan_instance();
	[[nodiscard]] Window *key_window() const;
	[[nodiscard]] DisplayProfileWatch &display_profiles()
	{
		return this->display_profiles_;
	}
	[[nodiscard]] bool needs_csd() const { return this->needs_csd_; }

private:
	QVulkanInstance vulkan_instance_;
	GpuContext gpu_;
	Thumbnailer thumbnailer_;
	DisplayProfileWatch display_profiles_;
	std::vector<std::unique_ptr<QWindow>> windows_;
	QPointer<Window> default_window_;
	bool needs_csd_ = false;
};

}  // namespace dn
