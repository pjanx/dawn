//
// vk-device.cpp: pick a graphics device and create it
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "vk-device.hpp"

#include <cstring>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <cwchar>
#include <windows.h>
#elif defined(__APPLE__)
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#endif

using namespace std;

namespace dawn
{
namespace
{

int
type_rank(VkPhysicalDeviceType type)
{
	switch (type) {
	case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
		return 0;
	case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
		return 1;
	case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
		return 2;
	case VK_PHYSICAL_DEVICE_TYPE_CPU:
		return 4;
	default:
		return 3;
	}
}

bool
check_vk(VkResult r, const char *what, string *error)
{
	if (r == VK_SUCCESS)
		return true;
	if (error)
		*error = string(what) + " failed: VkResult " +
			to_string(static_cast<int>(r));
	return false;
}

bool
can_present(VkPhysicalDevice phys, uint32_t family, VkSurfaceKHR surface,
	const function<bool(VkPhysicalDevice, uint32_t)> &present)
{
	if (present)
		return present(phys, family);
	if (!surface)
		return true;
	VkBool32 ok = VK_FALSE;
	vkGetPhysicalDeviceSurfaceSupportKHR(phys, family, surface, &ok);
	return ok == VK_TRUE;
}

}  // namespace

uint32_t
vk_memory_type(VkPhysicalDevice phys, uint32_t bits,
	VkMemoryPropertyFlags flags, string *error, VkDeviceSize *heap_size)
{
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(phys, &properties);
	uint32_t best = UINT32_MAX;
	for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
		if (!(bits & (1u << i)) ||
			(properties.memoryTypes[i].propertyFlags & flags) != flags)
			continue;
		if (best == UINT32_MAX ||
			properties.memoryHeaps[properties.memoryTypes[i].heapIndex].size >
				properties.memoryHeaps[properties.memoryTypes[best].heapIndex]
					.size)
			best = i;
	}
	if (heap_size)
		*heap_size = best == UINT32_MAX
			? 0
			: properties.memoryHeaps[properties.memoryTypes[best].heapIndex]
				  .size;
	if (best == UINT32_MAX && error)
		*error = "no suitable Vulkan memory type";
	return best;
}

bool
vk_create_graphics_device(VkInstance instance, VkSurfaceKHR surface,
	function<bool(VkPhysicalDevice, uint32_t)> present,
	initializer_list<const char *> extra_exts, VkPhysicalDevice *phys,
	VkDevice *device, VkQueue *queue, uint32_t *queue_family, string *error)
{
	uint32_t pd_count = 0;
	if (!check_vk(vkEnumeratePhysicalDevices(instance, &pd_count, nullptr),
			"vkEnumeratePhysicalDevices count", error))
		return false;
	if (pd_count == 0) {
		if (error)
			*error = "no Vulkan physical devices";
		return false;
	}
	vector<VkPhysicalDevice> pds(pd_count);
	if (!check_vk(vkEnumeratePhysicalDevices(instance, &pd_count, pds.data()),
			"vkEnumeratePhysicalDevices", error))
		return false;

	VkPhysicalDevice best = VK_NULL_HANDLE;
	uint32_t best_family = 0;
	int best_rank = numeric_limits<int>::max();
	for (VkPhysicalDevice candidate : pds) {
		uint32_t qcount = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &qcount, nullptr);
		vector<VkQueueFamilyProperties> qprops(qcount);
		vkGetPhysicalDeviceQueueFamilyProperties(
			candidate, &qcount, qprops.data());
		for (uint32_t i = 0; i < qcount; i++) {
			if (!(qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) ||
				!can_present(candidate, i, surface, present))
				continue;
			VkPhysicalDeviceProperties info{};
			vkGetPhysicalDeviceProperties(candidate, &info);
			const int rank = type_rank(info.deviceType);
			if (rank < best_rank) {
				best_rank = rank;
				best = candidate;
				best_family = i;
			}
			break;
		}
	}
	if (!best) {
		if (error)
			*error = (present || surface) ? "no graphics+present queue family"
										  : "no graphics queue family";
		return false;
	}

	const float prio = 1.f;
	VkDeviceQueueCreateInfo qci{
		.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
		.queueFamilyIndex = best_family,
		.queueCount = 1,
		.pQueuePriorities = &prio,
	};
	vector<const char *> exts(extra_exts);
	uint32_t ext_count = 0;
	vkEnumerateDeviceExtensionProperties(best, nullptr, &ext_count, nullptr);
	vector<VkExtensionProperties> ext_props(ext_count);
	vkEnumerateDeviceExtensionProperties(
		best, nullptr, &ext_count, ext_props.data());
	constexpr const char *kPortabilitySubset = "VK_KHR_portability_subset";
	for (const auto &p : ext_props) {
		if (strcmp(p.extensionName, kPortabilitySubset) == 0) {
			exts.push_back(kPortabilitySubset);
			break;
		}
	}

	VkDeviceCreateInfo dci{
		.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
		.queueCreateInfoCount = 1,
		.pQueueCreateInfos = &qci,
		.enabledExtensionCount = uint32_t(exts.size()),
		.ppEnabledExtensionNames = exts.data(),
	};
	VkDevice dev = VK_NULL_HANDLE;
	if (!check_vk(
			vkCreateDevice(best, &dci, nullptr, &dev), "vkCreateDevice", error))
		return false;
	*phys = best;
	*device = dev;
	*queue_family = best_family;
	vkGetDeviceQueue(dev, best_family, 0, queue);
	return true;
}

void
vk_add_bundled_driver_files()
{
#if defined(_WIN32)
	HMODULE self = nullptr;
	wchar_t path[MAX_PATH];
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
				GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
			reinterpret_cast<LPCWSTR>(&vk_add_bundled_driver_files), &self) ||
		!GetModuleFileNameW(self, path, MAX_PATH))
		return;
	wchar_t *slash = wcsrchr(path, L'\\');
	if (!slash)
		return;
	wcscpy(slash + 1, L"vk_swiftshader_icd.json");
	SetEnvironmentVariableW(L"VK_ADD_DRIVER_FILES", path);
#elif defined(__APPLE__)
	// This library sits in Contents/Frameworks, the manifest in Resources.
	Dl_info info{};
	const char *slash = nullptr;
	char path[PATH_MAX], real[PATH_MAX];
	if (dladdr(reinterpret_cast<const void *>(&vk_add_bundled_driver_files),
			&info) &&
		(slash = strrchr(info.dli_fname, '/')) &&
		snprintf(path, sizeof path,
			"%.*s/../Resources/vulkan/icd.d/MoltenVK_icd.json",
			int(slash - info.dli_fname), info.dli_fname) > 0 &&
		realpath(path, real))
		setenv("VK_DRIVER_FILES", real, 0);
#endif
}

}  // namespace dawn
