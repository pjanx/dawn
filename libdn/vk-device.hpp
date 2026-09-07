//
// vk-device.hpp: pick a graphics device, and the state every pipeline shares
//
// Copyright The Dawn Authors
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
	VkMemoryPropertyFlags flags, std::string *error, VkDeviceSize *heap_size);

bool vk_create_graphics_device(VkInstance instance, VkSurfaceKHR surface,
	std::function<bool(VkPhysicalDevice, uint32_t)> present,
	std::initializer_list<const char *> extra_exts, VkPhysicalDevice *phys,
	VkDevice *device, VkQueue *queue, uint32_t *queue_family,
	std::string *error);

// --- Shared pipeline state ---------------------------------------------------

// Every pass Dawn draws is the same kind of pass: a flat 2D triangle list,
// no culling, no multisampling, viewport and scissor set at record time.
// Only the shaders, the vertex input, and the blend differ.

inline constexpr VkPipelineVertexInputStateCreateInfo kNoVertexInput{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
};

inline constexpr VkPipelineInputAssemblyStateCreateInfo kTriangleList{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
	.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
};

// Counts without arrays: valid only alongside kDynamicViewportScissor.
inline constexpr VkPipelineViewportStateCreateInfo kOneViewport{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
	.viewportCount = 1,
	.scissorCount = 1,
};

inline constexpr VkDynamicState kViewportScissor[]{
	VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};

inline constexpr VkPipelineDynamicStateCreateInfo kDynamicViewportScissor{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
	.dynamicStateCount = 2,
	.pDynamicStates = kViewportScissor,
};

inline constexpr VkPipelineRasterizationStateCreateInfo kRasterFill{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
	.polygonMode = VK_POLYGON_MODE_FILL,
	.cullMode = VK_CULL_MODE_NONE,
	.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
	.lineWidth = 1.f,
};

inline constexpr VkPipelineMultisampleStateCreateInfo kNoMultisample{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
	.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
};

// Overwrite the destination.
inline constexpr VkPipelineColorBlendAttachmentState kAttachmentReplace{
	.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};

inline constexpr VkPipelineColorBlendStateCreateInfo kBlendReplace{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	.attachmentCount = 1,
	.pAttachments = &kAttachmentReplace,
};

// Source over destination, both already premultiplied.
inline constexpr VkPipelineColorBlendAttachmentState kAttachmentPremulOver{
	.blendEnable = VK_TRUE,
	.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	.colorBlendOp = VK_BLEND_OP_ADD,
	.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
	.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
	.alphaBlendOp = VK_BLEND_OP_ADD,
	.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
		VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
};

inline constexpr VkPipelineColorBlendStateCreateInfo kBlendPremulOver{
	.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
	.attachmentCount = 1,
	.pAttachments = &kAttachmentPremulOver,
};

}  // namespace dawn
