//
// scale-engine.cpp: shared H→V tile scale engine
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdnvk.h"

#include "fullscreen-vert-spv.h"
#include "scale-2d-bilinear-spv.h"
#include "scale-2d-nearest-spv.h"
#include "scale-2d-nohalo-spv.h"
#include "scale-h-bilinear-spv.h"
#include "scale-v-bilinear-spv.h"
#include "vk-device.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

constexpr uint32_t kMaxTiles = 256;
constexpr uint32_t kTileEdge = 4096;
constexpr VkDeviceSize kTileBytesPerPixel = 8;

constexpr VkFormat kTileFormat = VK_FORMAT_R16G16B16A16_UNORM;
constexpr VkFormat kMidFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

struct TileRect {
	int32_t ox = 0, oy = 0, w = 0, h = 0;
};

struct MidTile {
	uint32_t layer = 0;
	int32_t ox = 0, oy = 0, w = 0, h = 0;
};

struct YRange {
	int32_t lo = 0;
	int32_t hi = 0;
	[[nodiscard]] bool empty() const { return hi <= lo; }
};

struct PushConstants {
	float viewport_x = 0, viewport_y = 0;
	float scale = 1.0f;
	int32_t transfer = 0;
	int32_t image_w = 0, image_h = 0;
	int32_t grid_cols = 1, grid_rows = 1;
	int32_t mid_cols = 1, mid_rows = 1;
	int32_t mid_pad_w = 0, mid_pad_h = 0;
	int32_t layer_ox = 0, layer_oy = 0;
	float pan_x = 0, pan_y = 0;
	float angle = 0;
	float bg_r = 0, bg_g = 0, bg_b = 0;
	float checker_r = 0, checker_g = 0, checker_b = 0;
};
static_assert(sizeof(PushConstants) == 92);

constexpr float kAngleFast = 1e-5f;

bool
view_axis_aligned(const ScaleView &view)
{
	return fabs(view.angle) < kAngleFast;
}

bool
use_separable(const ScaleView &view)
{
	if (!view_axis_aligned(view))
		return false;
	if (view.filter == Filter::Bilinear)
		return true;
	return view.filter == Filter::Expensive && view.scale < 1.0f;
}

uint32_t
ceil_div(uint32_t a, uint32_t b)
{
	return b == 0 ? 0 : (a + b - 1) / b;
}

bool
check_vk(VkResult r, const char *what, string *error)
{
	if (r != VK_SUCCESS) {
		if (error)
			*error = string(what) + " failed: VkResult " +
				to_string(static_cast<int>(r));
		return false;
	}
	return true;
}

}  // namespace

Filter
preferred_filter(VkPhysicalDevice phys)
{
	if (!phys)
		return Filter::Expensive;
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(phys, &props);
	if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU)
		return Filter::Bilinear;
	return Filter::Expensive;
}

struct ScaleEngine::Impl {
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;

	VkFormat dest_format = VK_FORMAT_R8G8B8A8_UNORM;
	VkImageLayout dest_final_layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

	uint32_t max_image_dim_2d = 0;
	uint32_t max_image_array_layers = 0;

	VkCommandPool upload_pool = VK_NULL_HANDLE;
	VkCommandBuffer upload_cmd = VK_NULL_HANDLE;

	VkSampler sampler = VK_NULL_HANDLE;

	VkRenderPass dest_render_pass = VK_NULL_HANDLE;
	VkRenderPass mid_render_pass = VK_NULL_HANDLE;
	VkDescriptorSetLayout dset_layout_tiles = VK_NULL_HANDLE;
	VkDescriptorSetLayout dset_layout_horiz = VK_NULL_HANDLE;
	VkDescriptorPool dset_pool = VK_NULL_HANDLE;
	VkDescriptorSet dset_tiles = VK_NULL_HANDLE;
	VkDescriptorSet dset_horiz = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_tiles = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_horiz = VK_NULL_HANDLE;
	VkPipeline pipeline_h = VK_NULL_HANDLE;
	VkPipeline pipeline_v = VK_NULL_HANDLE;
	VkPipeline pipeline_2d_nearest = VK_NULL_HANDLE;
	VkPipeline pipeline_2d_bilinear = VK_NULL_HANDLE;
	VkPipeline pipeline_2d_nohalo = VK_NULL_HANDLE;
	VkPipeline pipe_2d = VK_NULL_HANDLE;

	VkImage tile_image = VK_NULL_HANDLE;
	VkDeviceMemory tile_memory = VK_NULL_HANDLE;
	VkImageView tile_view = VK_NULL_HANDLE;
	vector<TileRect> tile_rects;
	uint32_t tile_count = 0;
	uint32_t tile_pad_w = 0;
	uint32_t tile_pad_h = 0;

	VkImage mid_image = VK_NULL_HANDLE;
	VkDeviceMemory mid_memory = VK_NULL_HANDLE;
	VkImageView mid_array_view = VK_NULL_HANDLE;
	vector<VkImageView> mid_layer_views;
	vector<VkFramebuffer> mid_fbs;
	uint32_t mid_cols = 0;
	uint32_t mid_rows = 0;
	uint32_t mid_pad_w = 0;
	uint32_t mid_pad_h = 0;
	uint32_t mid_layers = 0;

	uint32_t image_w = 0;
	uint32_t image_h = 0;
	uint32_t grid_cols = 1;
	uint32_t grid_rows = 1;
	bool image_opaque = true;

	uint32_t viewport_w = 0;
	uint32_t viewport_h = 0;

	bool ready = false;

	void destroy_mid();
	void destroy_tiles();
	void destroy_pipeline();
	void destroy_all();
	VkShaderModule make_shader(
		const uint32_t *words, uint32_t word_count, string *error);
	bool create_pipeline_objects(string *error);
	static vector<TileRect> split_grid(
		uint32_t w, uint32_t h, uint32_t cols, uint32_t rows, string *error);
	bool submit_upload(auto &&record, string *error);
	bool upload_tiles(const uint8_t *pixels, size_t stride,
		const vector<TileRect> &rects, string *error);
	void write_descriptors();
	bool ensure_mid(uint32_t vp_w, uint32_t src_h, string *error);
	PushConstants make_push(const ScaleView &view, uint32_t vp_w, uint32_t vp_h,
		const float clear_rgba[4]) const;
	YRange visible_source_y_range(const ScaleView &view, uint32_t vp_h) const;
	void cmd_h_pass(
		VkCommandBuffer cmd, const PushConstants &pc_in, const MidTile &tile);
	void cmd_barrier_mid(
		VkCommandBuffer cmd, uint32_t first_layer, uint32_t last_layer);
	void cmd_v_pass(VkCommandBuffer cmd, const PushConstants &pc,
		VkFramebuffer dest_fb, uint32_t vp_w, uint32_t vp_h,
		const float clear_rgba[4]);
	void cmd_2d_pass(VkCommandBuffer cmd, const PushConstants &pc,
		VkFramebuffer dest_fb, uint32_t vp_w, uint32_t vp_h,
		const float clear_rgba[4]);
	pair<uint32_t, uint32_t> cmd_fill_visible_mid(VkCommandBuffer cmd,
		const PushConstants &pc, YRange need, uint32_t vp_w, uint32_t disp_h);
};

void
ScaleEngine::Impl::destroy_mid()
{
	if (!device)
		return;
	for (VkFramebuffer fb : mid_fbs) {
		if (fb)
			vkDestroyFramebuffer(device, fb, nullptr);
	}
	mid_fbs.clear();
	for (VkImageView v : mid_layer_views) {
		if (v)
			vkDestroyImageView(device, v, nullptr);
	}
	mid_layer_views.clear();
	if (mid_array_view) {
		vkDestroyImageView(device, mid_array_view, nullptr);
		mid_array_view = VK_NULL_HANDLE;
	}
	if (mid_image) {
		vkDestroyImage(device, mid_image, nullptr);
		mid_image = VK_NULL_HANDLE;
	}
	if (mid_memory) {
		vkFreeMemory(device, mid_memory, nullptr);
		mid_memory = VK_NULL_HANDLE;
	}
	mid_cols = mid_rows = mid_pad_w = mid_pad_h = mid_layers = 0;
}

void
ScaleEngine::Impl::destroy_tiles()
{
	if (!device)
		return;
	if (tile_view) {
		vkDestroyImageView(device, tile_view, nullptr);
		tile_view = VK_NULL_HANDLE;
	}
	if (tile_image) {
		vkDestroyImage(device, tile_image, nullptr);
		tile_image = VK_NULL_HANDLE;
	}
	if (tile_memory) {
		vkFreeMemory(device, tile_memory, nullptr);
		tile_memory = VK_NULL_HANDLE;
	}
	tile_rects.clear();
	tile_count = 0;
	tile_pad_w = tile_pad_h = 0;
	// Keep image_w/image_h: upload_tiles calls destroy_tiles before
	// replacing GPU tiles; dimensions stay owned by set_image.
}

void
ScaleEngine::Impl::destroy_pipeline()
{
	if (!device)
		return;
	auto kill = [&](VkPipeline &p) {
		if (p) {
			vkDestroyPipeline(device, p, nullptr);
			p = VK_NULL_HANDLE;
		}
	};
	kill(pipeline_h);
	kill(pipeline_v);
	kill(pipeline_2d_nearest);
	kill(pipeline_2d_bilinear);
	kill(pipeline_2d_nohalo);
	pipe_2d = VK_NULL_HANDLE;
	if (pipeline_layout_tiles) {
		vkDestroyPipelineLayout(device, pipeline_layout_tiles, nullptr);
		pipeline_layout_tiles = VK_NULL_HANDLE;
	}
	if (pipeline_layout_horiz) {
		vkDestroyPipelineLayout(device, pipeline_layout_horiz, nullptr);
		pipeline_layout_horiz = VK_NULL_HANDLE;
	}
	if (dest_render_pass) {
		vkDestroyRenderPass(device, dest_render_pass, nullptr);
		dest_render_pass = VK_NULL_HANDLE;
	}
	if (mid_render_pass) {
		vkDestroyRenderPass(device, mid_render_pass, nullptr);
		mid_render_pass = VK_NULL_HANDLE;
	}
}

void
ScaleEngine::Impl::destroy_all()
{
	if (!device)
		return;
	vkDeviceWaitIdle(device);
	destroy_mid();
	destroy_tiles();
	destroy_pipeline();
	if (dset_pool) {
		vkDestroyDescriptorPool(device, dset_pool, nullptr);
		dset_pool = VK_NULL_HANDLE;
	}
	if (dset_layout_tiles) {
		vkDestroyDescriptorSetLayout(device, dset_layout_tiles, nullptr);
		dset_layout_tiles = VK_NULL_HANDLE;
	}
	if (dset_layout_horiz) {
		vkDestroyDescriptorSetLayout(device, dset_layout_horiz, nullptr);
		dset_layout_horiz = VK_NULL_HANDLE;
	}
	if (sampler) {
		vkDestroySampler(device, sampler, nullptr);
		sampler = VK_NULL_HANDLE;
	}
	if (upload_pool) {
		vkDestroyCommandPool(device, upload_pool, nullptr);
		upload_pool = VK_NULL_HANDLE;
		upload_cmd = VK_NULL_HANDLE;
	}
	phys = VK_NULL_HANDLE;
	device = VK_NULL_HANDLE;
	queue = VK_NULL_HANDLE;
	viewport_w = viewport_h = 0;
	ready = false;
}

VkShaderModule
ScaleEngine::Impl::make_shader(
	const uint32_t *words, uint32_t word_count, string *error)
{
	VkShaderModuleCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = word_count * sizeof(uint32_t),
		.pCode = words,
	};
	VkShaderModule mod = VK_NULL_HANDLE;
	if (!check_vk(vkCreateShaderModule(device, &ci, nullptr, &mod),
			"vkCreateShaderModule", error))
		return VK_NULL_HANDLE;
	return mod;
}

bool
ScaleEngine::Impl::create_pipeline_objects(string *error)
{
	destroy_pipeline();

	auto make_rp = [&](VkFormat fmt, VkImageLayout final_layout,
					   VkRenderPass *out) -> bool {
		VkAttachmentDescription color_att{
			.format = fmt,
			.samples = VK_SAMPLE_COUNT_1_BIT,
			.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
			.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
			.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
			.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
			.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
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
		VkSubpassDependency dep{
			.srcSubpass = VK_SUBPASS_EXTERNAL,
			.dstSubpass = 0,
			.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
			.srcAccessMask = 0,
			.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		};
		VkRenderPassCreateInfo rpci{
			.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = &color_att,
			.subpassCount = 1,
			.pSubpasses = &subpass,
			.dependencyCount = 1,
			.pDependencies = &dep,
		};
		return check_vk(vkCreateRenderPass(device, &rpci, nullptr, out),
			"vkCreateRenderPass", error);
	};

	if (!make_rp(dest_format, dest_final_layout, &dest_render_pass) ||
		!make_rp(kMidFormat, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			&mid_render_pass))
		return false;

	VkPushConstantRange pcr{
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(PushConstants),
	};
	auto make_layout = [&](VkDescriptorSetLayout set_layout,
						   VkPipelineLayout *out) -> bool {
		VkPipelineLayoutCreateInfo plci{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.setLayoutCount = 1,
			.pSetLayouts = &set_layout,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &pcr,
		};
		return check_vk(vkCreatePipelineLayout(device, &plci, nullptr, out),
			"vkCreatePipelineLayout", error);
	};
	if (!make_layout(dset_layout_tiles, &pipeline_layout_tiles) ||
		!make_layout(dset_layout_horiz, &pipeline_layout_horiz))
		return false;

	VkShaderModule vert =
		make_shader(fullscreen_vert, fullscreen_vert_words, error);
	if (!vert)
		return false;

	VkPipelineVertexInputStateCreateInfo vi{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
	};
	VkPipelineInputAssemblyStateCreateInfo ia{
		.sType =
			VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
		.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
	};
	VkPipelineViewportStateCreateInfo vp{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
		.viewportCount = 1,
		.scissorCount = 1,
	};
	VkPipelineRasterizationStateCreateInfo rs{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
		.polygonMode = VK_POLYGON_MODE_FILL,
		.cullMode = VK_CULL_MODE_NONE,
		.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
		.lineWidth = 1.0f,
	};
	VkPipelineMultisampleStateCreateInfo ms{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
		.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
	};
	// H-pass writes the mid buffer as-is. V-pass is premul-over the
	// dest clear (view well, or transparent for offscreen readback) so
	// SVG/PNG alpha sits on that background instead of replacing it.
	VkPipelineColorBlendAttachmentState blend_replace{
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT,
	};
	VkPipelineColorBlendAttachmentState blend_premul_over{
		.blendEnable = VK_TRUE,
		.srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.colorBlendOp = VK_BLEND_OP_ADD,
		.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
		.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
		.alphaBlendOp = VK_BLEND_OP_ADD,
		.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
			VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
			VK_COLOR_COMPONENT_A_BIT,
	};
	array dyn_states = {
		VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
	VkPipelineDynamicStateCreateInfo dyn{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
		.dynamicStateCount = uint32_t(dyn_states.size()),
		.pDynamicStates = dyn_states.data(),
	};

	auto make_pipe =
		[&](VkShaderModule frag, VkPipelineLayout layout, VkRenderPass rp,
			const VkPipelineColorBlendAttachmentState *blend_att,
			VkPipeline *out) -> bool {
		VkPipelineColorBlendStateCreateInfo cb{
			.sType =
				VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
			.attachmentCount = 1,
			.pAttachments = blend_att,
		};
		VkPipelineShaderStageCreateInfo stages[2] = {
			{
				.sType =
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_VERTEX_BIT,
				.module = vert,
				.pName = "main",
			},
			{
				.sType =
					VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
				.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
				.module = frag,
				.pName = "main",
			},
		};
		VkGraphicsPipelineCreateInfo gpci{
			.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
			.stageCount = 2,
			.pStages = stages,
			.pVertexInputState = &vi,
			.pInputAssemblyState = &ia,
			.pViewportState = &vp,
			.pRasterizationState = &rs,
			.pMultisampleState = &ms,
			.pColorBlendState = &cb,
			.pDynamicState = &dyn,
			.layout = layout,
			.renderPass = rp,
			.subpass = 0,
		};
		return check_vk(vkCreateGraphicsPipelines(
							device, VK_NULL_HANDLE, 1, &gpci, nullptr, out),
			"vkCreateGraphicsPipelines", error);
	};

	const struct {
		VkPipeline *out;
		VkPipelineLayout layout;
		VkRenderPass rp;
		const VkPipelineColorBlendAttachmentState *blend;
		const uint32_t *code;
		uint32_t words;
	} variants[] = {
		{&pipeline_h, pipeline_layout_tiles, mid_render_pass,
			&blend_replace, scale_h_bilinear, scale_h_bilinear_words},
		{&pipeline_v, pipeline_layout_horiz, dest_render_pass,
			&blend_premul_over, scale_v_bilinear, scale_v_bilinear_words},
		{&pipeline_2d_nearest, pipeline_layout_tiles, dest_render_pass,
			&blend_premul_over, scale_2d_nearest, scale_2d_nearest_words},
		{&pipeline_2d_bilinear, pipeline_layout_tiles, dest_render_pass,
			&blend_premul_over, scale_2d_bilinear, scale_2d_bilinear_words},
		{&pipeline_2d_nohalo, pipeline_layout_tiles, dest_render_pass,
			&blend_premul_over, scale_2d_nohalo, scale_2d_nohalo_words},
	};

	bool ok = true;
	for (const auto &v : variants) {
		VkShaderModule frag = make_shader(v.code, v.words, error);
		if (!frag) {
			ok = false;
			break;
		}
		ok = make_pipe(frag, v.layout, v.rp, v.blend, v.out);
		vkDestroyShaderModule(device, frag, nullptr);
		if (!ok)
			break;
	}

	vkDestroyShaderModule(device, vert, nullptr);
	return ok;
}

vector<TileRect>
ScaleEngine::Impl::split_grid(
	uint32_t w, uint32_t h, uint32_t cols, uint32_t rows, string *error)
{
	if (cols < 1 || rows < 1) {
		if (error)
			*error = "tile grid must be at least 1×1";
		return {};
	}
	if (cols * rows > kMaxTiles) {
		if (error)
			*error = "tile grid exceeds kMaxTiles";
		return {};
	}
	vector<TileRect> out;
	out.reserve(cols * rows);
	const uint32_t tw = ceil_div(w, cols);
	const uint32_t th = ceil_div(h, rows);
	for (uint32_t ty = 0; ty < rows; ty++) {
		for (uint32_t tx = 0; tx < cols; tx++) {
			const uint32_t x0 = tx * tw;
			const uint32_t y0 = ty * th;
			if (x0 >= w || y0 >= h) {
				if (error)
					*error = "tile origin outside image";
				return {};
			}
			const uint32_t x1 = min(x0 + tw, w);
			const uint32_t y1 = min(y0 + th, h);
			out.push_back({int32_t(x0), int32_t(y0), int32_t(x1 - x0),
				int32_t(y1 - y0)});
		}
	}
	return out;
}

bool
ScaleEngine::Impl::submit_upload(auto &&record, string *error)
{
	if (!check_vk(vkResetCommandBuffer(upload_cmd, 0),
			"vkResetCommandBuffer", error))
		return false;
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (!check_vk(vkBeginCommandBuffer(upload_cmd, &begin),
			"vkBeginCommandBuffer", error))
		return false;
	record();
	if (!check_vk(
			vkEndCommandBuffer(upload_cmd), "vkEndCommandBuffer", error))
		return false;
	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &upload_cmd,
	};
	if (!check_vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE),
			"vkQueueSubmit", error))
		return false;
	return check_vk(vkQueueWaitIdle(queue), "vkQueueWaitIdle", error);
}

bool
ScaleEngine::Impl::upload_tiles(const uint8_t *pixels, size_t stride,
	const vector<TileRect> &rects, string *error)
{
	destroy_tiles();
	tile_count = uint32_t(rects.size());
	tile_rects = rects;
	if (tile_count == 0) {
		if (error)
			*error = "no tiles";
		return false;
	}

	for (const auto &r : rects) {
		tile_pad_w = max(tile_pad_w, uint32_t(r.w));
		tile_pad_h = max(tile_pad_h, uint32_t(r.h));
	}

	// One scan here buys a uniform flag that lets the shaders skip
	// the per-tap un-premultiply. Most photographs take it.
	image_opaque = true;
	for (uint32_t y = 0; y < image_h && image_opaque; y++) {
		const auto *row =
			reinterpret_cast<const uint16_t *>(pixels + size_t(y) * stride);
		for (uint32_t x = 0; x < image_w; x++) {
			if (row[x * 4 + 3] != 65535) {
				image_opaque = false;
				break;
			}
		}
	}
	if (tile_pad_w > max_image_dim_2d || tile_pad_h > max_image_dim_2d) {
		if (error)
			*error = "tile pad exceeds maxImageDimension2D";
		return false;
	}
	if (tile_count > max_image_array_layers) {
		if (error)
			*error = "tile count exceeds maxImageArrayLayers";
		return false;
	}

	const uint64_t tile_bytes =
		uint64_t(tile_count) * tile_pad_w * tile_pad_h * kTileBytesPerPixel;
	if (tile_bytes > kMaxDeviceBytes) {
		if (error)
			*error = "tiles exceed kMaxDeviceBytes (" +
				to_string(tile_bytes) + " > " + to_string(kMaxDeviceBytes) +
				")";
		return false;
	}

	VkImageCreateInfo ici{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kTileFormat,
		.extent = {tile_pad_w, tile_pad_h, 1},
		.mipLevels = 1,
		.arrayLayers = tile_count,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage =
			VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!check_vk(vkCreateImage(device, &ici, nullptr, &tile_image),
			"vkCreateImage tiles", error))
		return false;

	VkMemoryRequirements mr{};
	vkGetImageMemoryRequirements(device, tile_image, &mr);
	uint32_t mem_type = vk_memory_type(phys, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error);
	if (mem_type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo mai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!check_vk(vkAllocateMemory(device, &mai, nullptr, &tile_memory),
			"vkAllocateMemory tiles", error))
		return false;
	if (!check_vk(vkBindImageMemory(device, tile_image, tile_memory, 0),
			"vkBindImageMemory tiles", error))
		return false;

	VkImageViewCreateInfo vi{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = tile_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = kTileFormat,
		.components =
			{
				.r = VK_COMPONENT_SWIZZLE_B,
				.g = VK_COMPONENT_SWIZZLE_G,
				.b = VK_COMPONENT_SWIZZLE_R,
				.a = VK_COMPONENT_SWIZZLE_A,
			},
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = tile_count,
			},
	};
	if (!check_vk(vkCreateImageView(device, &vi, nullptr, &tile_view),
			"vkCreateImageView tiles", error))
		return false;

	const VkDeviceSize staging_bytes =
		VkDeviceSize(tile_pad_w) * tile_pad_h * kTileBytesPerPixel;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;

	VkBufferCreateInfo bci{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = staging_bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (!check_vk(vkCreateBuffer(device, &bci, nullptr, &staging),
			"vkCreateBuffer staging", error))
		return false;

	VkMemoryRequirements smr{};
	vkGetBufferMemoryRequirements(device, staging, &smr);
	mem_type = vk_memory_type(phys, smr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		error);
	if (mem_type == UINT32_MAX) {
		vkDestroyBuffer(device, staging, nullptr);
		return false;
	}
	VkMemoryAllocateInfo smai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = smr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!check_vk(vkAllocateMemory(device, &smai, nullptr, &staging_mem),
			"vkAllocateMemory staging", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		return false;
	}
	if (!check_vk(vkBindBufferMemory(device, staging, staging_mem, 0),
			"vkBindBufferMemory staging", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	uint8_t *mapped = nullptr;
	if (!check_vk(vkMapMemory(device, staging_mem, 0, staging_bytes, 0,
					  (void **) &mapped),
			"vkMapMemory staging", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	auto transition =
		[&](VkImageLayout from, VkImageLayout to, VkAccessFlags src_access,
			VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
			VkPipelineStageFlags dst_stage) -> bool {
		return submit_upload(
			[&] {
				VkImageMemoryBarrier barrier{
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.srcAccessMask = src_access,
					.dstAccessMask = dst_access,
					.oldLayout = from,
					.newLayout = to,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = tile_image,
					.subresourceRange =
						{
							.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
							.levelCount = 1,
							.layerCount = tile_count,
						},
				};
				vkCmdPipelineBarrier(upload_cmd, src_stage, dst_stage, 0, 0,
					nullptr, 0, nullptr, 1, &barrier);
			},
			error);
	};

	if (!transition(VK_IMAGE_LAYOUT_UNDEFINED,
			VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT)) {
		vkUnmapMemory(device, staging_mem);
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	for (uint32_t i = 0; i < tile_count; i++) {
		const TileRect &r = tile_rects[i];
		const size_t row_bytes = size_t(r.w) * kTileBytesPerPixel;
		for (int32_t y = 0; y < r.h; y++) {
			const uint8_t *src = pixels + size_t(r.oy + y) * stride +
				size_t(r.ox) * kTileBytesPerPixel;
			memcpy(mapped + size_t(y) * row_bytes, src, row_bytes);
		}
		VkBufferImageCopy copy{
			.bufferOffset = 0,
			.imageSubresource =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.mipLevel = 0,
					.baseArrayLayer = i,
					.layerCount = 1,
				},
			.imageExtent = {uint32_t(r.w), uint32_t(r.h), 1},
		};
		if (!submit_upload(
				[&] {
					vkCmdCopyBufferToImage(upload_cmd, staging, tile_image,
						VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
				},
				error)) {
			vkUnmapMemory(device, staging_mem);
			vkDestroyBuffer(device, staging, nullptr);
			vkFreeMemory(device, staging_mem, nullptr);
			return false;
		}
	}

	if (!transition(VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)) {
		vkUnmapMemory(device, staging_mem);
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	vkUnmapMemory(device, staging_mem);
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, staging_mem, nullptr);
	write_descriptors();
	return true;
}

void
ScaleEngine::Impl::write_descriptors()
{
	if (tile_view) {
		VkDescriptorImageInfo tiles_info{
			.sampler = sampler,
			.imageView = tile_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet write_tiles{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = dset_tiles,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &tiles_info,
		};
		vkUpdateDescriptorSets(device, 1, &write_tiles, 0, nullptr);
	}
	if (mid_array_view) {
		VkDescriptorImageInfo mid_info{
			.sampler = sampler,
			.imageView = mid_array_view,
			.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		};
		VkWriteDescriptorSet write_mid{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = dset_horiz,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &mid_info,
		};
		vkUpdateDescriptorSets(device, 1, &write_mid, 0, nullptr);
	}
}

bool
ScaleEngine::Impl::ensure_mid(uint32_t vp_w, uint32_t src_h, string *error)
{
	if (vp_w == 0 || src_h == 0) {
		if (error)
			*error = "invalid viewport for mid buffer";
		return false;
	}

	const uint32_t want_cols = max(1u, ceil_div(vp_w, max_image_dim_2d));
	const uint32_t want_rows = max(1u, ceil_div(src_h, max_image_dim_2d));
	const uint32_t want_pad_w = ceil_div(vp_w, want_cols);
	const uint32_t want_pad_h = ceil_div(src_h, want_rows);
	if (want_pad_w > max_image_dim_2d || want_pad_h > max_image_dim_2d) {
		if (error)
			*error = "mid tile pad exceeds maxImageDimension2D";
		return false;
	}
	const uint32_t want_layers = want_cols * want_rows;
	if (want_layers > max_image_array_layers) {
		if (error)
			*error = "mid layer count exceeds maxImageArrayLayers";
		return false;
	}

	const uint64_t mid_bytes = uint64_t(want_layers) * want_pad_w *
		want_pad_h * sizeof(uint16_t) * 4;
	if (mid_bytes > kMaxDeviceBytes) {
		if (error)
			*error = "mid buffer exceeds kMaxDeviceBytes";
		return false;
	}

	if (mid_image && mid_cols == want_cols && mid_rows == want_rows &&
		mid_pad_w == want_pad_w && mid_pad_h == want_pad_h)
		return true;

	destroy_mid();
	mid_cols = want_cols;
	mid_rows = want_rows;
	mid_pad_w = want_pad_w;
	mid_pad_h = want_pad_h;
	mid_layers = want_layers;

	VkImageCreateInfo ici{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kMidFormat,
		.extent = {mid_pad_w, mid_pad_h, 1},
		.mipLevels = 1,
		.arrayLayers = mid_layers,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_SAMPLED_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!check_vk(vkCreateImage(device, &ici, nullptr, &mid_image),
			"vkCreateImage mid", error))
		return false;

	VkMemoryRequirements mr{};
	vkGetImageMemoryRequirements(device, mid_image, &mr);
	uint32_t mem_type = vk_memory_type(phys, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error);
	if (mem_type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo mai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!check_vk(vkAllocateMemory(device, &mai, nullptr, &mid_memory),
			"vkAllocateMemory mid", error))
		return false;
	if (!check_vk(vkBindImageMemory(device, mid_image, mid_memory, 0),
			"vkBindImageMemory mid", error))
		return false;

	VkImageViewCreateInfo avi{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = mid_image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
		.format = kMidFormat,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = mid_layers,
			},
	};
	if (!check_vk(vkCreateImageView(device, &avi, nullptr, &mid_array_view),
			"vkCreateImageView mid array", error))
		return false;

	mid_layer_views.resize(mid_layers);
	mid_fbs.resize(mid_layers);
	for (uint32_t i = 0; i < mid_layers; i++) {
		VkImageViewCreateInfo lvi{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = mid_image,
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = kMidFormat,
			.subresourceRange =
				{
					.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
					.levelCount = 1,
					.baseArrayLayer = i,
					.layerCount = 1,
				},
		};
		if (!check_vk(vkCreateImageView(
						  device, &lvi, nullptr, &mid_layer_views[i]),
				"vkCreateImageView mid layer", error))
			return false;
		VkFramebufferCreateInfo fbi{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = mid_render_pass,
			.attachmentCount = 1,
			.pAttachments = &mid_layer_views[i],
			.width = mid_pad_w,
			.height = mid_pad_h,
			.layers = 1,
		};
		if (!check_vk(
				vkCreateFramebuffer(device, &fbi, nullptr, &mid_fbs[i]),
				"vkCreateFramebuffer mid", error))
			return false;
	}

	if (!submit_upload(
			[&] {
				VkImageMemoryBarrier barrier{
					.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
					.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
					.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
					.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
					.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
					.image = mid_image,
					.subresourceRange =
						{
							.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
							.levelCount = 1,
							.layerCount = mid_layers,
						},
				};
				vkCmdPipelineBarrier(upload_cmd,
					VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
					VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0,
					nullptr, 1, &barrier);
			},
			error))
		return false;

	write_descriptors();
	return true;
}

PushConstants
ScaleEngine::Impl::make_push(const ScaleView &view, uint32_t vp_w, uint32_t vp_h,
	const float clear_rgba[4]) const
{
	PushConstants pc{};
	pc.viewport_x = float(vp_w);
	pc.viewport_y = float(vp_h);
	pc.scale = view.scale;
	pc.transfer = int32_t(view.transfer) |
		(int32_t(orientation_or_0(view.orientation)) << 8) |
		(view.checkerboard ? (1 << 16) : 0) |
		(view.composite ? (1 << 17) : 0) | (image_opaque ? (1 << 18) : 0);
	// Decoded here, not per fragment: the shader composites in linear.
	pc.bg_r = transfer_decode(clear_rgba[0], view.transfer);
	pc.bg_g = transfer_decode(clear_rgba[1], view.transfer);
	pc.bg_b = transfer_decode(clear_rgba[2], view.transfer);
	pc.checker_r = transfer_decode(view.checker_r, view.transfer);
	pc.checker_g = transfer_decode(view.checker_g, view.transfer);
	pc.checker_b = transfer_decode(view.checker_b, view.transfer);
	pc.image_w = int32_t(image_w);
	pc.image_h = int32_t(image_h);
	pc.grid_cols = int32_t(grid_cols);
	pc.grid_rows = int32_t(grid_rows);
	pc.mid_cols = int32_t(max(mid_cols, 1u));
	pc.mid_rows = int32_t(max(mid_rows, 1u));
	pc.mid_pad_w = int32_t(mid_pad_w);
	pc.mid_pad_h = int32_t(mid_pad_h);
	pc.pan_x = view.pan_x;
	pc.pan_y = view.pan_y;
	pc.angle = view.angle;
	return pc;
}

YRange
ScaleEngine::Impl::visible_source_y_range(const ScaleView &view, uint32_t vp_h) const
{
	uint32_t disp_w = 0, disp_h = 0;
	orientation_display_size(
		image_w, image_h, view.orientation, &disp_w, &disp_h);
	const float kernel_scale = min(view.scale, 1.0f);
	const float radius = 1.0f / kernel_scale;
	const float centre = 0.5f * float(disp_h) + view.pan_y;
	const float half_h = 0.5f * float(vp_h);
	const float y_top = (0.0f - half_h) / view.scale + centre;
	const float y_bot = (float(vp_h) - half_h) / view.scale + centre;
	YRange r;
	r.lo = max(0, int32_t(floor(min(y_top, y_bot) - radius)));
	r.hi = min(int32_t(disp_h), int32_t(ceil(max(y_top, y_bot) + radius)));
	if (r.hi <= r.lo)
		return {};
	return r;
}

void
ScaleEngine::Impl::cmd_h_pass(
	VkCommandBuffer cmd, const PushConstants &pc_in, const MidTile &tile)
{
	PushConstants pc = pc_in;
	pc.layer_ox = tile.ox;
	pc.layer_oy = tile.oy;

	VkClearValue clear{.color = {{0.0f, 0.0f, 0.0f, 0.0f}}};
	VkRenderPassBeginInfo rp{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = mid_render_pass,
		.framebuffer = mid_fbs[tile.layer],
		.renderArea = {.extent = {mid_pad_w, mid_pad_h}},
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp{
		.x = 0.0f,
		.y = 0.0f,
		.width = float(tile.w),
		.height = float(tile.h),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	VkRect2D scissor{.extent = {uint32_t(tile.w), uint32_t(tile.h)}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_h);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout_tiles, 0, 1, &dset_tiles, 0, nullptr);
	vkCmdPushConstants(cmd, pipeline_layout_tiles,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

void
ScaleEngine::Impl::cmd_barrier_mid(
	VkCommandBuffer cmd, uint32_t first_layer, uint32_t last_layer)
{
	if (first_layer > last_layer)
		return;
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = mid_image,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.baseArrayLayer = first_layer,
				.layerCount = last_layer - first_layer + 1,
			},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&barrier);
}

void
ScaleEngine::Impl::cmd_v_pass(VkCommandBuffer cmd, const PushConstants &pc,
	VkFramebuffer dest_fb, uint32_t vp_w, uint32_t vp_h,
	const float clear_rgba[4])
{
	const VkClearValue clear{.color = {{clear_rgba[0], clear_rgba[1],
								 clear_rgba[2], clear_rgba[3]}}};
	VkRenderPassBeginInfo rp{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = dest_render_pass,
		.framebuffer = dest_fb,
		.renderArea = {.extent = {vp_w, vp_h}},
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp{
		.x = 0.0f,
		.y = 0.0f,
		.width = float(vp_w),
		.height = float(vp_h),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	VkRect2D scissor{.extent = {vp_w, vp_h}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_v);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout_horiz, 0, 1, &dset_horiz, 0, nullptr);
	vkCmdPushConstants(cmd, pipeline_layout_horiz,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

void
ScaleEngine::Impl::cmd_2d_pass(VkCommandBuffer cmd, const PushConstants &pc,
	VkFramebuffer dest_fb, uint32_t vp_w, uint32_t vp_h,
	const float clear_rgba[4])
{
	const VkClearValue clear{.color = {{clear_rgba[0], clear_rgba[1],
								 clear_rgba[2], clear_rgba[3]}}};
	VkRenderPassBeginInfo rp{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = dest_render_pass,
		.framebuffer = dest_fb,
		.renderArea = {.extent = {vp_w, vp_h}},
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

	VkViewport vp{
		.x = 0.0f,
		.y = 0.0f,
		.width = float(vp_w),
		.height = float(vp_h),
		.minDepth = 0.0f,
		.maxDepth = 1.0f,
	};
	VkRect2D scissor{.extent = {vp_w, vp_h}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe_2d);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout_tiles, 0, 1, &dset_tiles, 0, nullptr);
	vkCmdPushConstants(cmd, pipeline_layout_tiles,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

pair<uint32_t, uint32_t>
ScaleEngine::Impl::cmd_fill_visible_mid(VkCommandBuffer cmd,
	const PushConstants &pc, YRange need, uint32_t vp_w, uint32_t disp_h)
{
	uint32_t first = mid_layers;
	uint32_t last = 0;
	if (need.empty())
		return {first, last};

	for (uint32_t row = 0; row < mid_rows; row++) {
		const int32_t oy = int32_t(row * mid_pad_h);
		const int32_t h = min(int32_t(mid_pad_h), int32_t(disp_h) - oy);
		if (h <= 0 || oy + h <= need.lo || oy >= need.hi)
			continue;

		for (uint32_t col = 0; col < mid_cols; col++) {
			const int32_t ox = int32_t(col * mid_pad_w);
			const int32_t w = min(int32_t(mid_pad_w), int32_t(vp_w) - ox);
			if (w <= 0)
				continue;

			const MidTile tile{
				.layer = row * mid_cols + col,
				.ox = ox,
				.oy = oy,
				.w = w,
				.h = h,
			};
			first = min(first, tile.layer);
			last = max(last, tile.layer);
			cmd_h_pass(cmd, pc, tile);
		}
	}
	return {first, last};
}


ScaleEngine::ScaleEngine() = default;

ScaleEngine::~ScaleEngine()
{
	destroy();
}

void
ScaleEngine::destroy()
{
	if (impl_) {
		impl_->destroy_all();
		delete impl_;
		impl_ = nullptr;
	}
}

bool
ScaleEngine::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	uint32_t queue_family, VkFormat dest_format,
	VkImageLayout dest_final_layout, string *error)
{
	if (!phys || !device || !queue) {
		if (error)
			*error = "invalid Vulkan device handles";
		return false;
	}

	if (!impl_)
		impl_ = new Impl();

	Impl &e = *impl_;
	if (e.ready && e.phys == phys && e.device == device &&
		e.dest_format == dest_format &&
		e.dest_final_layout == dest_final_layout)
		return true;

	e.destroy_all();
	e.phys = phys;
	e.device = device;
	e.queue = queue;
	e.queue_family = queue_family;
	e.dest_format = dest_format;
	e.dest_final_layout = dest_final_layout;

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(phys, &props);
	e.max_image_dim_2d = props.limits.maxImageDimension2D;
	e.max_image_array_layers = props.limits.maxImageArrayLayers;
	if (sizeof(PushConstants) > props.limits.maxPushConstantsSize) {
		if (error)
			*error = "PushConstants exceed maxPushConstantsSize";
		e.destroy_all();
		return false;
	}

	VkCommandPoolCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = queue_family,
	};
	if (!check_vk(vkCreateCommandPool(device, &pci, nullptr, &e.upload_pool),
			"vkCreateCommandPool", error)) {
		e.destroy_all();
		return false;
	}
	VkCommandBufferAllocateInfo cai{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = e.upload_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!check_vk(vkAllocateCommandBuffers(device, &cai, &e.upload_cmd),
			"vkAllocateCommandBuffers", error)) {
		e.destroy_all();
		return false;
	}

	VkSamplerCreateInfo samp{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 0.0f,
	};
	if (!check_vk(vkCreateSampler(device, &samp, nullptr, &e.sampler),
			"vkCreateSampler", error)) {
		e.destroy_all();
		return false;
	}

	VkDescriptorSetLayoutBinding binding{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo dlci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	if (!check_vk(vkCreateDescriptorSetLayout(
					  device, &dlci, nullptr, &e.dset_layout_tiles),
			"vkCreateDescriptorSetLayout tiles", error) ||
		!check_vk(vkCreateDescriptorSetLayout(
					  device, &dlci, nullptr, &e.dset_layout_horiz),
			"vkCreateDescriptorSetLayout horiz", error)) {
		e.destroy_all();
		return false;
	}

	VkDescriptorPoolSize pool_size{
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 2,
	};
	VkDescriptorPoolCreateInfo dpci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	if (!check_vk(vkCreateDescriptorPool(device, &dpci, nullptr, &e.dset_pool),
			"vkCreateDescriptorPool", error)) {
		e.destroy_all();
		return false;
	}
	VkDescriptorSetAllocateInfo tiles_ai{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = e.dset_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &e.dset_layout_tiles,
	};
	if (!check_vk(vkAllocateDescriptorSets(device, &tiles_ai, &e.dset_tiles),
			"vkAllocateDescriptorSets tiles", error)) {
		e.destroy_all();
		return false;
	}
	VkDescriptorSetAllocateInfo horiz_ai{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = e.dset_pool,
		.descriptorSetCount = 1,
		.pSetLayouts = &e.dset_layout_horiz,
	};
	if (!check_vk(vkAllocateDescriptorSets(device, &horiz_ai, &e.dset_horiz),
			"vkAllocateDescriptorSets horiz", error)) {
		e.destroy_all();
		return false;
	}

	if (!e.create_pipeline_objects(error)) {
		e.destroy_all();
		return false;
	}

	e.ready = true;
	return true;
}

bool
ScaleEngine::set_image(
	uint32_t w, uint32_t h, const uint8_t *pixels, size_t stride, string *error)
{
	if (!impl_ || !impl_->ready) {
		if (error)
			*error = "ScaleEngine not initialized";
		return false;
	}
	if (w == 0 || h == 0 || !pixels) {
		if (error)
			*error = "invalid image parameters";
		return false;
	}

	Impl &e = *impl_;
	const uint32_t edge = min(kTileEdge, e.max_image_dim_2d);
	e.image_w = w;
	e.image_h = h;
	e.grid_cols = max(1u, ceil_div(w, edge));
	e.grid_rows = max(1u, ceil_div(h, edge));

	vector<TileRect> rects =
		Impl::split_grid(w, h, e.grid_cols, e.grid_rows, error);
	if (rects.empty())
		return false;

	return e.upload_tiles(pixels, stride, rects, error);
}

void
ScaleEngine::clear_image()
{
	if (!impl_)
		return;
	impl_->destroy_tiles();
	impl_->image_w = impl_->image_h = 0;
	impl_->grid_cols = impl_->grid_rows = 1;
	impl_->destroy_mid();
	impl_->viewport_w = impl_->viewport_h = 0;
}

uint32_t
ScaleEngine::image_width() const
{
	return impl_ ? impl_->image_w : 0;
}

uint32_t
ScaleEngine::image_height() const
{
	return impl_ ? impl_->image_h : 0;
}

bool
ScaleEngine::has_image() const
{
	return impl_ && impl_->tile_count > 0;
}

bool
ScaleEngine::ensure_viewport(
	uint32_t viewport_w, uint32_t viewport_h, string *error)
{
	if (!impl_ || !impl_->ready) {
		if (error)
			*error = "ScaleEngine not initialized";
		return false;
	}
	if (!has_image()) {
		if (error)
			*error = "no image loaded";
		return false;
	}
	Impl &e = *impl_;
	if (!e.ensure_mid(viewport_w, e.image_h, error))
		return false;
	e.viewport_w = viewport_w;
	e.viewport_h = viewport_h;
	return true;
}

bool
ScaleEngine::record(VkCommandBuffer cmd, VkFramebuffer dest_fb,
	uint32_t viewport_w, uint32_t viewport_h, const ScaleView &view,
	const float clear_rgba[4], string *error)
{
	if (!impl_ || !impl_->ready) {
		if (error)
			*error = "ScaleEngine not initialized";
		return false;
	}
	if (!cmd || !dest_fb) {
		if (error)
			*error = "invalid record parameters";
		return false;
	}
	if (!has_image()) {
		if (error)
			*error = "no image loaded";
		return false;
	}
	if (viewport_w != impl_->viewport_w || viewport_h != impl_->viewport_h) {
		if (error)
			*error = "viewport size mismatch; call ensure_viewport first";
		return false;
	}

	Impl &e = *impl_;
	if (view.filter == Filter::Nearest)
		e.pipe_2d = e.pipeline_2d_nearest;
	else if (view.filter == Filter::Expensive && view.scale >= 1.0f)
		e.pipe_2d = e.pipeline_2d_nohalo;
	else
		e.pipe_2d = e.pipeline_2d_bilinear;
	const PushConstants pc =
		e.make_push(view, viewport_w, viewport_h, clear_rgba);
	if (!use_separable(view)) {
		e.cmd_2d_pass(cmd, pc, dest_fb, viewport_w, viewport_h, clear_rgba);
		return true;
	}
	uint32_t disp_w = 0, disp_h = 0;
	orientation_display_size(
		e.image_w, e.image_h, view.orientation, &disp_w, &disp_h);
	if (!e.ensure_mid(viewport_w, disp_h, error))
		return false;
	const YRange need = e.visible_source_y_range(view, viewport_h);
	const auto [first_mid, last_mid] =
		e.cmd_fill_visible_mid(cmd, pc, need, viewport_w, disp_h);
	e.cmd_barrier_mid(cmd, first_mid, last_mid);
	e.cmd_v_pass(cmd, pc, dest_fb, viewport_w, viewport_h, clear_rgba);
	return true;
}

bool
ScaleEngine::record_clear(VkCommandBuffer cmd, VkFramebuffer dest_fb,
	uint32_t viewport_w, uint32_t viewport_h, const float clear_rgba[4],
	string *error)
{
	if (!impl_ || !impl_->ready) {
		if (error)
			*error = "ScaleEngine not initialized";
		return false;
	}
	if (!cmd || !dest_fb || !viewport_w || !viewport_h) {
		if (error)
			*error = "invalid record_clear parameters";
		return false;
	}

	const VkClearValue clear{.color = {{clear_rgba[0], clear_rgba[1],
								 clear_rgba[2], clear_rgba[3]}}};
	VkRenderPassBeginInfo rp{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = impl_->dest_render_pass,
		.framebuffer = dest_fb,
		.renderArea = {.extent = {viewport_w, viewport_h}},
		.clearValueCount = 1,
		.pClearValues = &clear,
	};
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	vkCmdEndRenderPass(cmd);
	return true;
}

bool
ScaleEngine::create_offscreen(uint32_t w, uint32_t h, VkImage *image,
	VkDeviceMemory *mem, VkImageView *view, VkFramebuffer *fb, string *error)
{
	if (!impl_ || !impl_->ready || !image || !mem || !view || !fb) {
		if (error)
			*error = "invalid create_offscreen parameters";
		return false;
	}
	if (w == 0 || h == 0) {
		if (error)
			*error = "invalid offscreen size";
		return false;
	}

	Impl &e = *impl_;
	*image = VK_NULL_HANDLE;
	*mem = VK_NULL_HANDLE;
	*view = VK_NULL_HANDLE;
	*fb = VK_NULL_HANDLE;

	VkImageCreateInfo ici{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = e.dest_format,
		.extent = {w, h, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
			VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	if (!check_vk(vkCreateImage(e.device, &ici, nullptr, image),
			"vkCreateImage offscreen", error))
		return false;

	VkMemoryRequirements mr{};
	vkGetImageMemoryRequirements(e.device, *image, &mr);
	uint32_t mem_type = vk_memory_type(
		e.phys, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, error);
	if (mem_type == UINT32_MAX) {
		destroy_offscreen(image, mem, view, fb);
		return false;
	}
	VkMemoryAllocateInfo mai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!check_vk(vkAllocateMemory(e.device, &mai, nullptr, mem),
			"vkAllocateMemory offscreen", error)) {
		destroy_offscreen(image, mem, view, fb);
		return false;
	}
	if (!check_vk(vkBindImageMemory(e.device, *image, *mem, 0),
			"vkBindImageMemory offscreen", error)) {
		destroy_offscreen(image, mem, view, fb);
		return false;
	}

	VkImageViewCreateInfo vi{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = *image,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = e.dest_format,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
	};
	if (!check_vk(vkCreateImageView(e.device, &vi, nullptr, view),
			"vkCreateImageView offscreen", error)) {
		destroy_offscreen(image, mem, view, fb);
		return false;
	}

	VkFramebufferCreateInfo fbi{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = e.dest_render_pass,
		.attachmentCount = 1,
		.pAttachments = view,
		.width = w,
		.height = h,
		.layers = 1,
	};
	if (!check_vk(vkCreateFramebuffer(e.device, &fbi, nullptr, fb),
			"vkCreateFramebuffer offscreen", error)) {
		destroy_offscreen(image, mem, view, fb);
		return false;
	}
	return true;
}

void
ScaleEngine::destroy_offscreen(
	VkImage *image, VkDeviceMemory *mem, VkImageView *view, VkFramebuffer *fb)
{
	if (!impl_ || !impl_->device)
		return;
	VkDevice device = impl_->device;
	if (fb && *fb) {
		vkDestroyFramebuffer(device, *fb, nullptr);
		*fb = VK_NULL_HANDLE;
	}
	if (view && *view) {
		vkDestroyImageView(device, *view, nullptr);
		*view = VK_NULL_HANDLE;
	}
	if (image && *image) {
		vkDestroyImage(device, *image, nullptr);
		*image = VK_NULL_HANDLE;
	}
	if (mem && *mem) {
		vkFreeMemory(device, *mem, nullptr);
		*mem = VK_NULL_HANDLE;
	}
}

VkRenderPass
ScaleEngine::dest_render_pass() const
{
	return impl_ ? impl_->dest_render_pass : VK_NULL_HANDLE;
}

VkFormat
ScaleEngine::dest_format() const
{
	return impl_ ? impl_->dest_format : VK_FORMAT_UNDEFINED;
}

}  // namespace dn
