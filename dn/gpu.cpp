//
// gpu.cpp: shared Vulkan device and queue
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "gpu.hpp"
#include "libdn/vk-device.hpp"

#include <QtLogging>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

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
	// Vulkan 1.1 only knows the driver through this extension.
	uint32_t count = 0;
	vkEnumerateDeviceExtensionProperties(this->phys_, nullptr, &count, nullptr);
	vector<VkExtensionProperties> extensions(count);
	vkEnumerateDeviceExtensionProperties(
		this->phys_, nullptr, &count, extensions.data());
	const bool tells_driver = any_of(extensions.begin(), extensions.end(),
		[](const VkExtensionProperties &e) {
			return !strcmp(e.extensionName,
				VK_KHR_DRIVER_PROPERTIES_EXTENSION_NAME);
		});
	VkPhysicalDeviceDriverProperties driver{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES,
	};
	VkPhysicalDeviceProperties2 properties{
		.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
		.pNext = tells_driver ? &driver : nullptr,
	};
	vkGetPhysicalDeviceProperties2(this->phys_, &properties);
	this->device_name_ = properties.properties.deviceName;
	if (tells_driver)
		this->driver_ =
			string(driver.driverName) + " " + string(driver.driverInfo);
	qInfo("device: %s (%s)", this->device_name_.c_str(),
		this->driver_.c_str());
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
	this->driver_.clear();
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
