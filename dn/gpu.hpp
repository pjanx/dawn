//
// gpu.hpp: shared Vulkan device and queue
//
// Copyright The dawn Authors
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
	VkInstance instance_ = VK_NULL_HANDLE;  // borrowed from QVulkanInstance
	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	std::string device_name_;

public:
	GpuContext() = default;
	~GpuContext() { destroy(); }

	GpuContext(const GpuContext &) = delete;
	GpuContext &operator=(const GpuContext &) = delete;

	bool init(VkInstance instance, VkSurfaceKHR surface,
		std::function<bool(VkPhysicalDevice, uint32_t)> supports_present);
	void destroy();

	// Later windows: present support on the chosen family. False if not ready.
	[[nodiscard]] bool supports_present(VkSurfaceKHR surface) const;

	[[nodiscard]] VkInstance instance() const { return this->instance_; }
	[[nodiscard]] VkPhysicalDevice phys() const { return this->phys_; }
	[[nodiscard]] VkDevice device() const { return this->device_; }
	[[nodiscard]] VkQueue queue() const { return this->queue_; }
	[[nodiscard]] uint32_t queue_family() const { return this->queue_family_; }
	[[nodiscard]] const std::string &device_name() const
	{
		return this->device_name_;
	}
};

}  // namespace dn
