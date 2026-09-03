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

// Two corners in framebuffer pixels.  The overlay draws axis-aligned
// rectangles and nothing else, and it draws them on whole pixels: anything
// that wants to sit between two of them says so in its texture instead.
struct Box {
	int x0 = 0;
	int y0 = 0;
	int x1 = 0;
	int y1 = 0;
	bool operator==(const Box &) const = default;
};

// The same rectangle in normalised texture coordinates.
struct Uv {
	float u0 = 0;
	float v0 = 0;
	float u1 = 0;
	float v1 = 0;
};

struct OverlayVertex {
	float x = 0;
	float y = 0;
	float u = 0;
	float v = 0;
	Colour col{};
	float atlas_x0 = 0;
	float atlas_y0 = 0;
	float atlas_x1 = 0;
	float atlas_y1 = 0;
	float dest_w = 0;
	float dest_h = 0;
	float transfer = 0;
};

constexpr uint32_t kOverlayTexFont = 0;
constexpr uint32_t kOverlayTexThumbs = 1;

struct OverlayCmd {
	uint32_t idx_offset = 0;
	uint32_t idx_count = 0;
	Box clip{};
	uint32_t tex = kOverlayTexFont;
};

struct OverlayMesh {
	std::vector<OverlayVertex> vertices;
	std::vector<uint32_t> indices;
	std::vector<OverlayCmd> cmds;
	float display_w = 0;
	float display_h = 0;
};

struct ThumbUpload {
	const uint16_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
};

class OverlayList
{
	OverlayMesh mesh_;
	OverlayCmd cmd_{};
	std::vector<Box> clip_stack_;
	Uv white_{};
	uint32_t tex_ = kOverlayTexFont;

	void sync_clip();
	void add_quad(Box b, Uv uv, Colour c00, Colour c10, Colour c11, Colour c01);

public:
	void begin(int width_px, int height_px, Uv white);
	void end();
	void push_clip(Box b);
	void pop_clip();
	void add_rect_filled(Box b, Colour col);
	void add_rect_filled_vgradient(Box b, Colour top, Colour bottom);
	// The outline is drawn inside the box, so that a rule is just a box
	// collapsed along one axis: there is no line primitive.
	void add_rect_stroke(Box b, Colour col, int thickness);
	void add_image(Box b, Uv uv, Colour col);
	void add_thumb(Box b, Uv uv, int transfer, Colour col);

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
	VkPipeline thumb_pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet descriptor_sets_[2]{};
	VkShaderModule vert_ = VK_NULL_HANDLE;
	VkShaderModule frag_ = VK_NULL_HANDLE;
	VkShaderModule thumb_frag_ = VK_NULL_HANDLE;

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
	bool rebuild_thumbs(const std::vector<ThumbUpload> &uploads, int atlas_side);
	void reset_thumbs();
	void record(
		VkCommandBuffer cmd, uint32_t image_index, const OverlayMesh &mesh);
	void destroy();
};

}  // namespace dn
