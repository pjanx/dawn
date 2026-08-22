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

#include <QString>
#include <QVulkanInstance>
#include <QWindow>

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
	OpenResult open(const QString &path, const QString &activation_token = {},
		BrowseSetup setup = {});
	void close(const QWindow *top);
	void close_later(const QWindow *top);
	void quit();
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
	bool needs_csd_ = false;
};

}  // namespace dn
