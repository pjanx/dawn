//
// overlay.hpp: tinted textured-quad overlay on dn's Vulkan device
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace dn
{

struct Colour {
	float r = 0;
	float g = 0;
	float b = 0;
	float a = 1;
};

struct OverlayVertex {
	float x = 0;
	float y = 0;
	float u = 0;
	float v = 0;
	Colour col{};
};

constexpr uint32_t kOverlayTexFont = 0;
constexpr uint32_t kOverlayTexThumbs = 1;

struct OverlayCmd {
	uint32_t idx_offset = 0;
	uint32_t idx_count = 0;
	float clip_x0 = 0;
	float clip_y0 = 0;
	float clip_x1 = 0;
	float clip_y1 = 0;
	uint32_t tex = kOverlayTexFont;
};

struct OverlayMesh {
	std::vector<OverlayVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<OverlayCmd> cmds;
	float display_w = 0;
	float display_h = 0;
};

class OverlayList
{
	struct Clip {
		float x0 = 0;
		float y0 = 0;
		float x1 = 0;
		float y1 = 0;
	};

	OverlayMesh mesh_;
	OverlayCmd cmd_{};
	std::vector<Clip> clip_stack_;
	float white_u_ = 0;
	float white_v_ = 0;
	uint32_t tex_ = kOverlayTexFont;

	void sync_clip();
	void add_quad(float x0, float y0, float x1, float y1, float u0, float v0,
		float u1, float v1, Colour c00, Colour c10, Colour c11, Colour c01);

public:
	void begin(
		float width_px, float height_px, float white_u, float white_v);
	void end();
	void push_clip(float x0, float y0, float x1, float y1);
	void pop_clip();
	void add_rect_filled(float x0, float y0, float x1, float y1, Colour col);
	void add_rect_filled_vgradient(
		float x0, float y0, float x1, float y1, Colour top, Colour bottom);
	void add_rect_stroke(float x0, float y0, float x1, float y1, Colour col,
		float thickness = 1.f);
	void add_line(float x0, float y0, float x1, float y1, Colour col,
		float thickness = 1.f);
	void add_image(float x0, float y0, float x1, float y1, float u0, float v0,
		float u1, float v1, Colour col);
	void add_thumb(float x0, float y0, float x1, float y1, float u0, float v0,
		float u1, float v1, Colour col = {1, 1, 1, 1});

	[[nodiscard]] const OverlayMesh &mesh() const { return this->mesh_; }
};

class OverlayVulkan
{
	void destroy_swapchain();
	void destroy_font();
	void destroy_thumbs();
	void destroy_sampled(
		VkImage *image, VkDeviceMemory *memory, VkImageView *view) const;
	void destroy_buffers();
	void destroy_pipeline();
	bool create_pipeline();
	bool ensure_buffers(VkDeviceSize vertex_bytes, VkDeviceSize index_bytes);
	bool upload_rgba16(const void *pixels, int width, int height,
		VkImage *image, VkDeviceMemory *memory, VkImageView *view,
		VkDescriptorSet set, VkComponentMapping swizzle) const;
	bool copy_rgba16(const void *pixels, int width, int height, VkImage image,
		VkImageLayout layout, int dst_x = 0, int dst_y = 0) const;
	bool create_sampled(
		int width, int height, VkImage *image, VkDeviceMemory *memory) const;
	void bind_sampled(VkImage image, VkImageView *view, VkDescriptorSet set,
		VkComponentMapping swizzle) const;
	void compute_thumb_atlas_max();

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
	VkFormat format_ = VK_FORMAT_UNDEFINED;
	VkExtent2D extent_{};

	VkRenderPass render_pass_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet descriptor_sets_[2]{};
	VkShaderModule vert_ = VK_NULL_HANDLE;
	VkShaderModule frag_ = VK_NULL_HANDLE;

	VkImage font_image_ = VK_NULL_HANDLE;
	VkDeviceMemory font_memory_ = VK_NULL_HANDLE;
	VkImageView font_view_ = VK_NULL_HANDLE;

	VkImage thumb_image_ = VK_NULL_HANDLE;
	VkDeviceMemory thumb_memory_ = VK_NULL_HANDLE;
	VkImageView thumb_view_ = VK_NULL_HANDLE;
	int thumb_side_ = 0;
	int thumb_atlas_max_ = 2048;

	VkBuffer vertex_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory vertex_memory_ = VK_NULL_HANDLE;
	VkDeviceSize vertex_size_ = 0;
	VkBuffer index_buffer_ = VK_NULL_HANDLE;
	VkDeviceMemory index_memory_ = VK_NULL_HANDLE;
	VkDeviceSize index_size_ = 0;

	VkCommandPool upload_pool_ = VK_NULL_HANDLE;

	std::vector<VkFramebuffer> framebuffers_;

public:
	OverlayVulkan() = default;
	~OverlayVulkan() { destroy(); }

	OverlayVulkan(const OverlayVulkan &) = delete;
	OverlayVulkan &operator=(const OverlayVulkan &) = delete;

	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
		uint32_t queue_family, VkFormat format, VkImageLayout initial_layout,
		VkImageLayout final_layout);
	void set_swapchain(
		const std::vector<VkImageView> &views, VkExtent2D extent);
	bool upload_font(const unsigned char *pixels, int width, int height);
	[[nodiscard]] int thumb_atlas_max() const { return this->thumb_atlas_max_; }
	bool upload_thumb(const uint16_t *pixels, int width, int height, int dst_x,
		int dst_y, int atlas_side, bool *recreated = nullptr);
	void reset_thumbs();
	void record(
		VkCommandBuffer cmd, uint32_t image_index, const OverlayMesh &mesh);
	void destroy();
};

}  // namespace dn
