//
// vk-device.hpp: pick a graphics device and create it
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <vulkan/vulkan.h>

#include <functional>
#include <initializer_list>
#include <string>

namespace dawn
{

// Rank: discrete → integrated → virtual → other → CPU.
// present empty: if surface, KHR present; else graphics only.
// extra_exts: e.g. VK_KHR_swapchain. VK_KHR_portability_subset is added if
// the device exposes it.
// Windows: sibling vk_swiftshader_icd.json via VK_ADD_DRIVER_FILES, ignored at
// High integrity. Apple: bundled MoltenVK_icd.json via VK_DRIVER_FILES, as the
// loader scans the bundle but does not skip system paths. No-op elsewhere.
void vk_add_bundled_driver_files();

uint32_t vk_memory_type(VkPhysicalDevice phys, uint32_t bits,
	VkMemoryPropertyFlags flags, std::string *error = nullptr,
	VkDeviceSize *heap_size = nullptr);

bool vk_create_graphics_device(VkInstance instance, VkSurfaceKHR surface,
	std::function<bool(VkPhysicalDevice, uint32_t)> present,
	std::initializer_list<const char *> extra_exts, VkPhysicalDevice *phys,
	VkDevice *device, VkQueue *queue, uint32_t *queue_family,
	std::string *error);

}  // namespace dawn
