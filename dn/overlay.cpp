//
// overlay.cpp: tinted textured-quad overlay on dn's Vulkan device
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "overlay.hpp"

#include "dn-overlay-frag-spv.h"
#include "dn-overlay-vert-spv.h"
#include "vk-device.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace dn
{
namespace
{

constexpr VkFormat kOverlayTexFormat = VK_FORMAT_R16G16B16A16_UNORM;
constexpr VkDeviceSize kOverlayBpp = 8;
constexpr uint32_t kThumbAtlasBase = 2048;
constexpr VkDeviceSize kThumbAtlasBudgetCap = 512ull * 1024 * 1024;
constexpr VkDeviceSize kThumbAtlasHeapFrac = 4;

VkImageCreateInfo
sampled_info(int width, int height)
{
	return {
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kOverlayTexFormat,
		.extent = {uint32_t(width), uint32_t(height), 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
}

void
check_vk(VkResult result, const char *what)
{
	if (result != VK_SUCCESS) {
		fprintf(
			stderr, "%s failed: VkResult %d\n", what, static_cast<int>(result));
		exit(1);
	}
}

struct PushConstant {
	float scale[2];
	float translate[2];
};

float
snap_fb(float v, float s)
{
	if (s <= 0.0f)
		s = 1.0f;
	return std::round(v * s) / s;
}

}  // namespace

void
OverlayList::sync_clip()
{
	const Clip &clip = this->clip_stack_.back();
	if (this->cmd_.idx_count > 0 &&
		(this->cmd_.clip_x0 != clip.x0 || this->cmd_.clip_y0 != clip.y0 ||
			this->cmd_.clip_x1 != clip.x1 || this->cmd_.clip_y1 != clip.y1 ||
			this->cmd_.tex != this->tex_)) {
		this->mesh_.cmds.push_back(this->cmd_);
		this->cmd_.idx_count = 0;
	}
	if (this->cmd_.idx_count == 0)
		this->cmd_.idx_offset = uint32_t(this->mesh_.indices.size());
	this->cmd_.clip_x0 = clip.x0;
	this->cmd_.clip_y0 = clip.y0;
	this->cmd_.clip_x1 = clip.x1;
	this->cmd_.clip_y1 = clip.y1;
	this->cmd_.tex = this->tex_;
}

void
OverlayList::begin(
	float width_pts, float height_pts, float dpr, float white_u, float white_v)
{
	this->mesh_.vertices.clear();
	this->mesh_.indices.clear();
	this->mesh_.cmds.clear();
	this->mesh_.display_w = width_pts;
	this->mesh_.display_h = height_pts;
	this->mesh_.fb_scale = dpr > 0.0f ? dpr : 1.0f;
	this->white_u_ = white_u;
	this->white_v_ = white_v;
	this->tex_ = kOverlayTexFont;
	this->clip_stack_.clear();
	this->clip_stack_.push_back({0.0f, 0.0f, width_pts, height_pts});
	this->cmd_ = {};
	sync_clip();
}

void
OverlayList::end()
{
	if (this->cmd_.idx_count > 0)
		this->mesh_.cmds.push_back(this->cmd_);
	this->cmd_ = {};
}

void
OverlayList::push_clip(float x0, float y0, float x1, float y1)
{
	const Clip &prev = this->clip_stack_.back();
	Clip next{
		std::max(prev.x0, x0),
		std::max(prev.y0, y0),
		std::min(prev.x1, x1),
		std::min(prev.y1, y1),
	};
	const float s = this->mesh_.fb_scale;
	next.x0 = snap_fb(next.x0, s);
	next.y0 = snap_fb(next.y0, s);
	next.x1 = snap_fb(next.x1, s);
	next.y1 = snap_fb(next.y1, s);
	if (next.x1 < next.x0)
		next.x1 = next.x0;
	if (next.y1 < next.y0)
		next.y1 = next.y0;
	this->clip_stack_.push_back(next);
	sync_clip();
}

void
OverlayList::pop_clip()
{
	if (this->clip_stack_.size() <= 1)
		return;
	this->clip_stack_.pop_back();
	sync_clip();
}

void
OverlayList::add_quad(float x0, float y0, float x1, float y1, float u0,
	float v0, float u1, float v1, Colour c00, Colour c10, Colour c11,
	Colour c01)
{
	const float s = this->mesh_.fb_scale;
	x0 = snap_fb(x0, s);
	y0 = snap_fb(y0, s);
	x1 = snap_fb(x1, s);
	y1 = snap_fb(y1, s);
	sync_clip();
	const uint32_t i = uint32_t(this->mesh_.vertices.size());
	this->mesh_.vertices.push_back({x0, y0, u0, v0, c00});
	this->mesh_.vertices.push_back({x1, y0, u1, v0, c10});
	this->mesh_.vertices.push_back({x1, y1, u1, v1, c11});
	this->mesh_.vertices.push_back({x0, y1, u0, v1, c01});
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 1);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i + 3);
	this->cmd_.idx_count += 6;
}

void
OverlayList::add_rect_filled(float x0, float y0, float x1, float y1, Colour col)
{
	this->tex_ = kOverlayTexFont;
	add_quad(x0, y0, x1, y1, this->white_u_, this->white_v_, this->white_u_,
		this->white_v_, col, col, col, col);
}

void
OverlayList::add_rect_filled_vgradient(
	float x0, float y0, float x1, float y1, Colour top, Colour bottom)
{
	this->tex_ = kOverlayTexFont;
	add_quad(x0, y0, x1, y1, this->white_u_, this->white_v_, this->white_u_,
		this->white_v_, top, top, bottom, bottom);
}

void
OverlayList::add_line(
	float x0, float y0, float x1, float y1, Colour col, float thickness)
{
	this->tex_ = kOverlayTexFont;
	const float dx = x1 - x0;
	const float dy = y1 - y0;
	const float len = std::sqrt(dx * dx + dy * dy);
	if (len <= 0.0f || thickness <= 0.0f)
		return;
	const float s = this->mesh_.fb_scale > 0.0f ? this->mesh_.fb_scale : 1.0f;
	const float th = std::max(1.0f, std::round(thickness * s)) / s;
	if (std::abs(dy) * s < 0.5f) {
		x0 = snap_fb(x0, s);
		x1 = snap_fb(x1, s);
		y0 = y1 = (std::floor(y0 * s) + 0.5f) / s;
	} else if (std::abs(dx) * s < 0.5f) {
		y0 = snap_fb(y0, s);
		y1 = snap_fb(y1, s);
		x0 = x1 = (std::floor(x0 * s) + 0.5f) / s;
	}
	const float hx = (-dy / len) * (th * 0.5f);
	const float hy = (dx / len) * (th * 0.5f);
	sync_clip();
	const uint32_t i = uint32_t(this->mesh_.vertices.size());
	this->mesh_.vertices.push_back(
		{x0 + hx, y0 + hy, this->white_u_, this->white_v_, col});
	this->mesh_.vertices.push_back(
		{x1 + hx, y1 + hy, this->white_u_, this->white_v_, col});
	this->mesh_.vertices.push_back(
		{x1 - hx, y1 - hy, this->white_u_, this->white_v_, col});
	this->mesh_.vertices.push_back(
		{x0 - hx, y0 - hy, this->white_u_, this->white_v_, col});
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 1);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i + 3);
	this->cmd_.idx_count += 6;
}

void
OverlayList::add_rect_stroke(
	float x0, float y0, float x1, float y1, Colour col, float thickness)
{
	add_line(x0, y0, x1, y0, col, thickness);
	add_line(x1, y0, x1, y1, col, thickness);
	add_line(x1, y1, x0, y1, col, thickness);
	add_line(x0, y1, x0, y0, col, thickness);
}

void
OverlayList::add_image(float x0, float y0, float x1, float y1, float u0,
	float v0, float u1, float v1, Colour col)
{
	this->tex_ = kOverlayTexFont;
	add_quad(x0, y0, x1, y1, u0, v0, u1, v1, col, col, col, col);
}

void
OverlayList::add_thumb(float x0, float y0, float x1, float y1, float u0,
	float v0, float u1, float v1, Colour col)
{
	this->tex_ = kOverlayTexThumbs;
	add_quad(x0, y0, x1, y1, u0, v0, u1, v1, col, col, col, col);
}

bool
OverlayVulkan::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	uint32_t queue_family, VkFormat format, VkImageLayout initial_layout,
	VkImageLayout final_layout)
{
	destroy();
	this->phys_ = phys;
	this->device_ = device;
	this->queue_ = queue;
	this->queue_family_ = queue_family;
	this->format_ = format;
	if (!this->phys_ || !this->device_ || !this->queue_)
		return false;

	VkCommandPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = this->queue_family_,
	};
	check_vk(vkCreateCommandPool(
				 this->device_, &pool_info, nullptr, &this->upload_pool_),
		"vkCreateCommandPool overlay upload");
	compute_thumb_atlas_max();

	VkAttachmentDescription color{
		.format = this->format_,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = initial_layout,
		.finalLayout = final_layout,
	};
	VkAttachmentReference color_ref{
		.attachment = 0,
		.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
	};
	VkSubpassDescription subpass{
		.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
		.colorAttachmentCount = 1,
		.pColorAttachments = &color_ref,
	};
	VkSubpassDependency dependency{
		.srcSubpass = VK_SUBPASS_EXTERNAL,
		.dstSubpass = 0,
		.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
	};
	VkRenderPassCreateInfo render_pass_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency,
	};
	check_vk(vkCreateRenderPass(this->device_, &render_pass_info, nullptr,
				 &this->render_pass_),
		"vkCreateRenderPass overlay");

	VkSamplerCreateInfo sampler_info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 1.0f,
	};
	check_vk(
		vkCreateSampler(this->device_, &sampler_info, nullptr, &this->sampler_),
		"vkCreateSampler overlay");

	VkDescriptorSetLayoutBinding binding{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo layout_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	check_vk(vkCreateDescriptorSetLayout(
				 this->device_, &layout_info, nullptr, &this->set_layout_),
		"vkCreateDescriptorSetLayout overlay");

	VkDescriptorPoolSize pool_size{
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 2,
	};
	VkDescriptorPoolCreateInfo descriptor_pool_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	check_vk(vkCreateDescriptorPool(this->device_, &descriptor_pool_info,
				 nullptr, &this->descriptor_pool_),
		"vkCreateDescriptorPool overlay");
	VkDescriptorSetLayout layouts[2] = {this->set_layout_, this->set_layout_};
	VkDescriptorSetAllocateInfo allocate_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = this->descriptor_pool_,
		.descriptorSetCount = 2,
		.pSetLayouts = layouts,
	};
	check_vk(vkAllocateDescriptorSets(
				 this->device_, &allocate_info, this->descriptor_sets_),
		"vkAllocateDescriptorSets overlay");

	return create_pipeline();
}

bool
OverlayVulkan::create_pipeline()
{
	VkShaderModuleCreateInfo vert_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = dn_overlay_vert_words * sizeof(uint32_t),
		.pCode = dn_overlay_vert,
	};
	VkShaderModuleCreateInfo frag_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = dn_overlay_frag_words * sizeof(uint32_t),
		.pCode = dn_overlay_frag,
	};
	check_vk(
		vkCreateShaderModule(this->device_, &vert_info, nullptr, &this->vert_),
		"vkCreateShaderModule overlay vert");
	check_vk(
		vkCreateShaderModule(this->device_, &frag_info, nullptr, &this->frag_),
		"vkCreateShaderModule overlay frag");

	VkPushConstantRange push{
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
		.offset = 0,
		.size = sizeof(PushConstant),
	};
	VkPipelineLayoutCreateInfo pipeline_layout_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &this->set_layout_,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	check_vk(vkCreatePipelineLayout(this->device_, &pipeline_layout_info,
				 nullptr, &this->pipeline_layout_),
		"vkCreatePipelineLayout overlay");

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = this->vert_;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = this->frag_;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding{
		.binding = 0,
		.stride = sizeof(OverlayVertex),
		.inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
	};
	VkVertexInputAttributeDescription attributes[3]{
		{.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = offsetof(OverlayVertex, x)},
		{.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32_SFLOAT,
			.offset = offsetof(OverlayVertex, u)},
		{.location = 2,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = offsetof(OverlayVertex, col)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = 3,
		.pVertexAttributeDescriptions = attributes,
	};
	VkPipelineInputAssemblyStateCreateInfo input_assembly{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo viewport{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo raster{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo multisample{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_attachment{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
			VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendStateCreateInfo blend{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &blend_attachment,
	};
	VkDynamicState dynamic_states[] = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dynamic{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = 2,
		.pDynamicStates = dynamic_states,
	};
	VkGraphicsPipelineCreateInfo pipeline_info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &input_assembly,
		.pViewportState = &viewport,
		.pRasterizationState = &raster,
		.pMultisampleState = &multisample,
		.pColorBlendState = &blend,
		.pDynamicState = &dynamic,
		.layout = this->pipeline_layout_,
		.renderPass = this->render_pass_,
	};
	check_vk(vkCreateGraphicsPipelines(this->device_, VK_NULL_HANDLE, 1,
				 &pipeline_info, nullptr, &this->pipeline_),
		"vkCreateGraphicsPipelines overlay");
	return true;
}

void
OverlayVulkan::set_swapchain(
	const std::vector<VkImageView> &views, VkExtent2D extent)
{
	destroy_swapchain();
	this->extent_ = extent;
	if (!this->device_ || !this->render_pass_ || views.empty() ||
		!extent.width || !extent.height)
		return;
	this->framebuffers_.resize(views.size());
	for (size_t i = 0; i < views.size(); ++i) {
		VkFramebufferCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = this->render_pass_,
			.attachmentCount = 1,
			.pAttachments = &views[i],
			.width = extent.width,
			.height = extent.height,
			.layers = 1,
		};
		check_vk(vkCreateFramebuffer(
					 this->device_, &info, nullptr, &this->framebuffers_[i]),
			"vkCreateFramebuffer overlay");
	}
}

void
OverlayVulkan::compute_thumb_atlas_max()
{
	uint32_t dim = 4096;
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(this->phys_, &props);
	if (props.limits.maxImageDimension2D)
		dim = props.limits.maxImageDimension2D;

	VkDeviceSize max_resource = 0;
	VkImageFormatProperties fmt{};
	if (vkGetPhysicalDeviceImageFormatProperties(this->phys_, kOverlayTexFormat,
			VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
			VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0,
			&fmt) == VK_SUCCESS) {
		if (fmt.maxExtent.width)
			dim = std::min(dim, fmt.maxExtent.width);
		if (fmt.maxExtent.height)
			dim = std::min(dim, fmt.maxExtent.height);
		max_resource = fmt.maxResourceSize;
	}

	uint32_t best = kThumbAtlasBase;
	for (uint32_t side = kThumbAtlasBase; side <= dim;) {
		VkImage image = VK_NULL_HANDLE;
		const VkImageCreateInfo info = sampled_info(int(side), int(side));
		if (vkCreateImage(this->device_, &info, nullptr, &image) != VK_SUCCESS)
			break;
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(this->device_, image, &requirements);
		vkDestroyImage(this->device_, image, nullptr);
		VkDeviceSize heap = 0;
		if (vk_memory_type(this->phys_, requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr,
				&heap) == UINT32_MAX ||
			(max_resource && requirements.size > max_resource) ||
			requirements.size >
				std::min(kThumbAtlasBudgetCap, heap / kThumbAtlasHeapFrac))
			break;
		best = side;
		if (side > dim / 2)
			break;
		side *= 2;
	}
	this->thumb_atlas_max_ = int(best);
}

bool
OverlayVulkan::create_sampled(
	int width, int height, VkImage *image, VkDeviceMemory *memory) const
{
	if (!this->device_ || width <= 0 || height <= 0 || !image || !memory)
		return false;
	const VkImageCreateInfo image_info = sampled_info(width, height);
	check_vk(vkCreateImage(this->device_, &image_info, nullptr, image),
		"vkCreateImage overlay tex");
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(this->device_, *image, &requirements);
	const uint32_t image_type = vk_memory_type(this->phys_,
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (image_type == UINT32_MAX) {
		vkDestroyImage(this->device_, *image, nullptr);
		*image = VK_NULL_HANDLE;
		return false;
	}
	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = image_type,
	};
	check_vk(vkAllocateMemory(this->device_, &allocate, nullptr, memory),
		"vkAllocateMemory overlay tex");
	check_vk(vkBindImageMemory(this->device_, *image, *memory, 0),
		"vkBindImageMemory overlay tex");
	return true;
}

void
OverlayVulkan::bind_sampled(VkImage image, VkImageView *view,
	VkDescriptorSet set, VkComponentMapping swizzle) const
{
	VkImageViewCreateInfo view_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = kOverlayTexFormat,
		.components = swizzle,
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1},
	};
	check_vk(vkCreateImageView(this->device_, &view_info, nullptr, view),
		"vkCreateImageView overlay tex");
	if (!set)
		return;
	VkDescriptorImageInfo image_descriptor{
		.sampler = this->sampler_,
		.imageView = *view,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = set,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_descriptor,
	};
	vkUpdateDescriptorSets(this->device_, 1, &write, 0, nullptr);
}

bool
OverlayVulkan::copy_rgba16(const void *pixels, int width, int height,
	VkImage image, VkImageLayout layout, int dst_x, int dst_y) const
{
	if (!this->device_ || !pixels || !image || width <= 0 || height <= 0)
		return false;
	const VkDeviceSize size =
		VkDeviceSize(width) * VkDeviceSize(height) * kOverlayBpp;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	VkBufferCreateInfo buffer_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	check_vk(vkCreateBuffer(this->device_, &buffer_info, nullptr, &staging),
		"vkCreateBuffer overlay tex staging");
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(this->device_, staging, &requirements);
	const uint32_t host_type =
		vk_memory_type(this->phys_, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	if (host_type == UINT32_MAX) {
		vkDestroyBuffer(this->device_, staging, nullptr);
		return false;
	}
	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = host_type,
	};
	check_vk(
		vkAllocateMemory(this->device_, &allocate, nullptr, &staging_memory),
		"vkAllocateMemory overlay tex staging");
	check_vk(vkBindBufferMemory(this->device_, staging, staging_memory, 0),
		"vkBindBufferMemory overlay tex staging");
	void *mapped = nullptr;
	check_vk(vkMapMemory(this->device_, staging_memory, 0, size, 0, &mapped),
		"vkMapMemory overlay tex staging");
	memcpy(mapped, pixels, size_t(size));
	vkUnmapMemory(this->device_, staging_memory);

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo cmd_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = this->upload_pool_,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	check_vk(vkAllocateCommandBuffers(this->device_, &cmd_info, &cmd),
		"vkAllocateCommandBuffers overlay tex");
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	check_vk(
		vkBeginCommandBuffer(cmd, &begin), "vkBeginCommandBuffer overlay tex");
	VkImageMemoryBarrier to_dst{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = layout == VK_IMAGE_LAYOUT_UNDEFINED
			? VkAccessFlags(0)
			: VkAccessFlags(VK_ACCESS_SHADER_READ_BIT),
		.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
		.oldLayout = layout,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1},
	};
	vkCmdPipelineBarrier(cmd,
		layout == VK_IMAGE_LAYOUT_UNDEFINED
			? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
			: VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);
	VkBufferImageCopy copy{
		.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.layerCount = 1},
		.imageOffset = {int32_t(dst_x), int32_t(dst_y), 0},
		.imageExtent = {uint32_t(width), uint32_t(height), 1},
	};
	vkCmdCopyBufferToImage(
		cmd, staging, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
	VkImageMemoryBarrier to_shader = to_dst;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&to_shader);
	check_vk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer overlay tex");
	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	check_vk(vkQueueSubmit(this->queue_, 1, &submit, VK_NULL_HANDLE),
		"vkQueueSubmit overlay tex");
	check_vk(vkQueueWaitIdle(this->queue_), "vkQueueWaitIdle overlay tex");
	vkFreeCommandBuffers(this->device_, this->upload_pool_, 1, &cmd);
	vkDestroyBuffer(this->device_, staging, nullptr);
	vkFreeMemory(this->device_, staging_memory, nullptr);
	return true;
}

bool
OverlayVulkan::upload_rgba16(const void *pixels, int width, int height,
	VkImage *image, VkDeviceMemory *memory, VkImageView *view,
	VkDescriptorSet set, VkComponentMapping swizzle) const
{
	if (!this->device_ || !pixels || width <= 0 || height <= 0 || !image ||
		!memory || !view)
		return false;
	if (!create_sampled(width, height, image, memory))
		return false;
	if (!copy_rgba16(
			pixels, width, height, *image, VK_IMAGE_LAYOUT_UNDEFINED)) {
		destroy_sampled(image, memory, view);
		return false;
	}
	bind_sampled(*image, view, set, swizzle);
	return true;
}

bool
OverlayVulkan::upload_thumb(const uint16_t *pixels, int width, int height,
	int dst_x, int dst_y, int atlas_side, bool *recreated)
{
	if (recreated)
		*recreated = false;
	if (!this->device_ || !pixels || width <= 0 || height <= 0 || dst_x < 0 ||
		dst_y < 0 || atlas_side <= 0 || dst_x + width > atlas_side ||
		dst_y + height > atlas_side)
		return false;
	const bool fresh = !this->thumb_image_ || this->thumb_side_ != atlas_side;
	if (fresh) {
		vkDeviceWaitIdle(this->device_);
		destroy_thumbs();
		if (!create_sampled(atlas_side, atlas_side, &this->thumb_image_,
				&this->thumb_memory_))
			return false;
		this->thumb_side_ = atlas_side;
	}
	if (!copy_rgba16(pixels, width, height, this->thumb_image_,
			fresh ? VK_IMAGE_LAYOUT_UNDEFINED
				  : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			dst_x, dst_y)) {
		if (fresh)
			destroy_thumbs();
		return false;
	}
	if (fresh) {
		const VkComponentMapping bgra{VK_COMPONENT_SWIZZLE_B,
			VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R,
			VK_COMPONENT_SWIZZLE_A};
		bind_sampled(this->thumb_image_, &this->thumb_view_,
			this->descriptor_sets_[kOverlayTexThumbs], bgra);
		if (recreated)
			*recreated = true;
	}
	return true;
}

void
OverlayVulkan::reset_thumbs()
{
	if (this->device_)
		vkDeviceWaitIdle(this->device_);
	destroy_thumbs();
}

bool
OverlayVulkan::upload_font(const unsigned char *pixels, int width, int height)
{
	if (!this->device_ || !pixels || width <= 0 || height <= 0)
		return false;
	vkDeviceWaitIdle(this->device_);
	destroy_font();
	const VkComponentMapping identity{
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
		VK_COMPONENT_SWIZZLE_IDENTITY,
	};
	return upload_rgba16(pixels, width, height, &this->font_image_,
		&this->font_memory_, &this->font_view_,
		this->descriptor_sets_[kOverlayTexFont], identity);
}

void
OverlayVulkan::destroy_thumbs()
{
	destroy_sampled(
		&this->thumb_image_, &this->thumb_memory_, &this->thumb_view_);
	this->thumb_side_ = 0;
}

bool
OverlayVulkan::ensure_buffers(
	VkDeviceSize vertex_bytes, VkDeviceSize index_bytes)
{
	auto recreate = [&](VkBuffer *buffer, VkDeviceMemory *memory,
						VkDeviceSize *current, VkDeviceSize needed,
						VkBufferUsageFlags usage) {
		if (*current >= needed && *buffer)
			return true;
		if (*buffer) {
			vkDestroyBuffer(this->device_, *buffer, nullptr);
			*buffer = VK_NULL_HANDLE;
		}
		if (*memory) {
			vkFreeMemory(this->device_, *memory, nullptr);
			*memory = VK_NULL_HANDLE;
		}
		*current = needed + needed / 2;
		VkBufferCreateInfo info{
			.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
			.size = *current,
			.usage = usage,
		};
		check_vk(vkCreateBuffer(this->device_, &info, nullptr, buffer),
			"vkCreateBuffer overlay");
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(this->device_, *buffer, &requirements);
		const uint32_t type =
			vk_memory_type(this->phys_, requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
					VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		if (type == UINT32_MAX)
			return false;
		VkMemoryAllocateInfo allocate{
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = requirements.size,
			.memoryTypeIndex = type,
		};
		check_vk(vkAllocateMemory(this->device_, &allocate, nullptr, memory),
			"vkAllocateMemory overlay");
		check_vk(vkBindBufferMemory(this->device_, *buffer, *memory, 0),
			"vkBindBufferMemory overlay");
		return true;
	};
	return recreate(&this->vertex_buffer_, &this->vertex_memory_,
			   &this->vertex_size_, vertex_bytes,
			   VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) &&
		recreate(&this->index_buffer_, &this->index_memory_, &this->index_size_,
			index_bytes, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void
OverlayVulkan::record(
	VkCommandBuffer cmd, uint32_t image_index, const OverlayMesh &mesh)
{
	if (!cmd || image_index >= this->framebuffers_.size() ||
		!this->framebuffers_[image_index] || !this->pipeline_ ||
		!this->font_view_)
		return;
	if (mesh.vertices.empty() || mesh.indices.empty() || mesh.cmds.empty() ||
		this->extent_.width == 0 || mesh.display_w <= 0.0f ||
		mesh.display_h <= 0.0f)
		return;

	const VkDeviceSize vertex_bytes =
		VkDeviceSize(mesh.vertices.size()) * sizeof(OverlayVertex);
	const VkDeviceSize index_bytes =
		VkDeviceSize(mesh.indices.size()) * sizeof(uint32_t);
	if (!ensure_buffers(vertex_bytes, index_bytes))
		return;

	void *vertices = nullptr;
	void *indices = nullptr;
	check_vk(vkMapMemory(this->device_, this->vertex_memory_, 0, vertex_bytes,
				 0, &vertices),
		"vkMapMemory overlay vtx");
	check_vk(vkMapMemory(this->device_, this->index_memory_, 0, index_bytes, 0,
				 &indices),
		"vkMapMemory overlay idx");
	memcpy(vertices, mesh.vertices.data(), size_t(vertex_bytes));
	memcpy(indices, mesh.indices.data(), size_t(index_bytes));
	vkUnmapMemory(this->device_, this->vertex_memory_);
	vkUnmapMemory(this->device_, this->index_memory_);

	VkRenderPassBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = this->render_pass_,
		.framebuffer = this->framebuffers_[image_index],
		.renderArea = {.extent = this->extent_},
	};
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport viewport{
		.x = 0.0f,
		.y = 0.0f,
		.width = float(this->extent_.width),
		.height = float(this->extent_.height),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->pipeline_);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &this->vertex_buffer_, &offset);
	vkCmdBindIndexBuffer(cmd, this->index_buffer_, 0, VK_INDEX_TYPE_UINT32);

	PushConstant push{};
	push.scale[0] = 2.0f / mesh.display_w;
	push.scale[1] = 2.0f / mesh.display_h;
	push.translate[0] = -1.0f;
	push.translate[1] = -1.0f;
	vkCmdPushConstants(cmd, this->pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT,
		0, sizeof(push), &push);

	const float fb = mesh.fb_scale > 0.0f ? mesh.fb_scale : 1.0f;
	uint32_t bound_tex = ~0u;
	for (const OverlayCmd &draw_cmd : mesh.cmds) {
		if (draw_cmd.idx_count == 0)
			continue;
		if (draw_cmd.tex == kOverlayTexThumbs && !this->thumb_view_)
			continue;
		if (draw_cmd.tex > kOverlayTexThumbs ||
			(draw_cmd.tex == kOverlayTexFont && !this->font_view_))
			continue;
		if (draw_cmd.tex != bound_tex) {
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				this->pipeline_layout_, 0, 1,
				&this->descriptor_sets_[draw_cmd.tex], 0, nullptr);
			bound_tex = draw_cmd.tex;
		}
		float clip_min_x = draw_cmd.clip_x0 * fb;
		float clip_min_y = draw_cmd.clip_y0 * fb;
		float clip_max_x = draw_cmd.clip_x1 * fb;
		float clip_max_y = draw_cmd.clip_y1 * fb;
		if (clip_min_x < 0.0f)
			clip_min_x = 0.0f;
		if (clip_min_y < 0.0f)
			clip_min_y = 0.0f;
		if (clip_max_x > float(this->extent_.width))
			clip_max_x = float(this->extent_.width);
		if (clip_max_y > float(this->extent_.height))
			clip_max_y = float(this->extent_.height);
		if (clip_max_x <= clip_min_x || clip_max_y <= clip_min_y)
			continue;
		VkRect2D scissor{
			.offset = {int32_t(clip_min_x), int32_t(clip_min_y)},
			.extent = {uint32_t(clip_max_x - clip_min_x),
				uint32_t(clip_max_y - clip_min_y)},
		};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdDrawIndexed(cmd, draw_cmd.idx_count, 1, draw_cmd.idx_offset, 0, 0);
	}
	vkCmdEndRenderPass(cmd);
}

void
OverlayVulkan::destroy_swapchain()
{
	for (VkFramebuffer framebuffer : this->framebuffers_)
		if (framebuffer)
			vkDestroyFramebuffer(this->device_, framebuffer, nullptr);
	this->framebuffers_.clear();
}

void
OverlayVulkan::destroy_sampled(
	VkImage *image, VkDeviceMemory *memory, VkImageView *view) const
{
	if (!this->device_ || !image || !memory || !view)
		return;
	if (*view)
		vkDestroyImageView(this->device_, *view, nullptr);
	if (*image)
		vkDestroyImage(this->device_, *image, nullptr);
	if (*memory)
		vkFreeMemory(this->device_, *memory, nullptr);
	*view = VK_NULL_HANDLE;
	*image = VK_NULL_HANDLE;
	*memory = VK_NULL_HANDLE;
}

void
OverlayVulkan::destroy_font()
{
	destroy_sampled(&this->font_image_, &this->font_memory_, &this->font_view_);
}

void
OverlayVulkan::destroy_buffers()
{
	if (!this->device_)
		return;
	if (this->vertex_buffer_)
		vkDestroyBuffer(this->device_, this->vertex_buffer_, nullptr);
	if (this->vertex_memory_)
		vkFreeMemory(this->device_, this->vertex_memory_, nullptr);
	if (this->index_buffer_)
		vkDestroyBuffer(this->device_, this->index_buffer_, nullptr);
	if (this->index_memory_)
		vkFreeMemory(this->device_, this->index_memory_, nullptr);
	this->vertex_buffer_ = VK_NULL_HANDLE;
	this->vertex_memory_ = VK_NULL_HANDLE;
	this->vertex_size_ = 0;
	this->index_buffer_ = VK_NULL_HANDLE;
	this->index_memory_ = VK_NULL_HANDLE;
	this->index_size_ = 0;
}

void
OverlayVulkan::destroy_pipeline()
{
	if (!this->device_)
		return;
	if (this->pipeline_)
		vkDestroyPipeline(this->device_, this->pipeline_, nullptr);
	if (this->pipeline_layout_)
		vkDestroyPipelineLayout(this->device_, this->pipeline_layout_, nullptr);
	if (this->vert_)
		vkDestroyShaderModule(this->device_, this->vert_, nullptr);
	if (this->frag_)
		vkDestroyShaderModule(this->device_, this->frag_, nullptr);
	this->pipeline_ = VK_NULL_HANDLE;
	this->pipeline_layout_ = VK_NULL_HANDLE;
	this->vert_ = VK_NULL_HANDLE;
	this->frag_ = VK_NULL_HANDLE;
}

void
OverlayVulkan::destroy()
{
	if (!this->device_)
		return;
	vkDeviceWaitIdle(this->device_);
	destroy_swapchain();
	destroy_buffers();
	destroy_font();
	destroy_thumbs();
	destroy_pipeline();
	if (this->descriptor_pool_)
		vkDestroyDescriptorPool(this->device_, this->descriptor_pool_, nullptr);
	if (this->set_layout_)
		vkDestroyDescriptorSetLayout(this->device_, this->set_layout_, nullptr);
	if (this->sampler_)
		vkDestroySampler(this->device_, this->sampler_, nullptr);
	if (this->render_pass_)
		vkDestroyRenderPass(this->device_, this->render_pass_, nullptr);
	if (this->upload_pool_)
		vkDestroyCommandPool(this->device_, this->upload_pool_, nullptr);
	this->descriptor_pool_ = VK_NULL_HANDLE;
	this->descriptor_sets_[0] = VK_NULL_HANDLE;
	this->descriptor_sets_[1] = VK_NULL_HANDLE;
	this->set_layout_ = VK_NULL_HANDLE;
	this->sampler_ = VK_NULL_HANDLE;
	this->render_pass_ = VK_NULL_HANDLE;
	this->upload_pool_ = VK_NULL_HANDLE;
	this->phys_ = VK_NULL_HANDLE;
	this->device_ = VK_NULL_HANDLE;
	this->queue_ = VK_NULL_HANDLE;
	this->format_ = VK_FORMAT_UNDEFINED;
	this->extent_ = {};
}

}  // namespace dn
