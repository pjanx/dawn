//
// gpu.hpp: shared Vulkan device and queue
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <functional>
#include <string>

namespace dn
{

// The one VkQueue is used only from the Qt main thread. Worker threads
// produce CPU pixels only.
class GpuContext
{
public:
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;
	std::string device_name;
	/// The driver's name and version, where the device can tell.
	std::string driver;

	GpuContext() = default;
	~GpuContext() { destroy(); }

	GpuContext(const GpuContext &) = delete;
	GpuContext &operator=(const GpuContext &) = delete;

	bool init(VkInstance instance, VkSurfaceKHR surface,
		std::function<bool(VkPhysicalDevice, uint32_t)> supports_present);
	void destroy();

	// Later windows: present support on the chosen family. False if not ready.
	[[nodiscard]] bool supports_present(VkSurfaceKHR surface) const;
};

}  // namespace dn
