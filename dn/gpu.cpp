//
// gpu.cpp: shared Vulkan device and queue
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "gpu.hpp"
#include "libdn/vk-device.hpp"

#include <QtLogging>

#include <string>
#include <utility>

using namespace std;

namespace dn
{

bool
GpuContext::init(VkInstance instance, VkSurfaceKHR surface,
	function<bool(VkPhysicalDevice, uint32_t)> supports_present)
{
	destroy();
	this->instance_ = instance;
	if (!this->instance_ || !surface)
		return false;
	string err;
	if (!dawn::vk_create_graphics_device(this->instance_, surface,
			std::move(supports_present), {VK_KHR_SWAPCHAIN_EXTENSION_NAME},
			&this->phys_, &this->device_, &this->queue_, &this->queue_family_,
			&err)) {
		qWarning("%s", err.c_str());
		return false;
	}
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(this->phys_, &properties);
	this->device_name_ = properties.deviceName;
	qInfo("device: %s", this->device_name_.c_str());
	return true;
}

void
GpuContext::destroy()
{
	if (this->device_) {
		vkDeviceWaitIdle(this->device_);
		vkDestroyDevice(this->device_, nullptr);
	}
	this->instance_ = VK_NULL_HANDLE;
	this->phys_ = VK_NULL_HANDLE;
	this->device_ = VK_NULL_HANDLE;
	this->queue_ = VK_NULL_HANDLE;
	this->queue_family_ = 0;
	this->device_name_.clear();
}

bool
GpuContext::supports_present(VkSurfaceKHR surface) const
{
	if (!this->phys_ || !surface)
		return false;
	VkBool32 present = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(
		this->phys_, this->queue_family_, surface, &present);
	return present == VK_TRUE;
}

}  // namespace dn
