//
// libdnvk.hpp: helpers shared by libdn's Vulkan code
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dawn
{

/// Soft cap on device-local bytes for one scale() (tiles + mid + dest).
/// Tweak in source; no CLI yet.
inline constexpr uint64_t kMaxDeviceBytes = 4ull << 30;

inline bool
check_vk(VkResult r, const char *what, std::string *error)
{
	if (r == VK_SUCCESS)
		return true;
	if (error)
		*error =
			std::string(what) + " failed: VkResult " + std::to_string(int(r));
	return false;
}

/// Calls vk<name>() and checks the result, naming it "vk<name><suffix>".
/// Expects a `std::string *error` in scope.
#define CALL_VK(name, suffix, ...)                                             \
	check_vk(vk##name(__VA_ARGS__), "vk" #name suffix, error)

inline uint32_t
ceil_div(uint32_t a, uint32_t b)
{
	return b ? (a + b - 1) / b : 0;
}

inline VkShaderModule
make_shader(VkDevice device, const uint32_t *words, uint32_t word_count,
	std::string *error)
{
	VkShaderModuleCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = std::size_t(word_count) * sizeof(uint32_t),
		.pCode = words,
	};
	VkShaderModule shader = VK_NULL_HANDLE;
	if (!CALL_VK(CreateShaderModule, "", device, &ci, nullptr, &shader))
		return VK_NULL_HANDLE;
	return shader;
}

}  // namespace dawn
