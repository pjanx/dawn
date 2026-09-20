//
// renderer.hpp: Vulkan image renderer
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "gpu.hpp"
#include "overlay.hpp"
#include "types.hpp"

#include <libdn/libdn.hpp>
#include <libdn/scale-engine.hpp>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dn
{

struct ThumbUpload {
	const uint16_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
};

// The overlay half of the renderer: it owns the font and thumbnail atlases,
// and turns an OverlayMesh into draws on the linear composition image.
class OverlayVulkan
{
	void destroy_target();
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
		VkImageLayout layout, int dst_x, int dst_y) const;
	bool create_sampled(
		int width, int height, VkImage *image, VkDeviceMemory *memory) const;
	void bind_sampled(VkImage image, VkImageView *view, VkDescriptorSet set,
		VkComponentMapping swizzle) const;
	void compute_thumb_atlas_max();

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;
	VkQueue queue_ = VK_NULL_HANDLE;
	uint32_t queue_family_ = 0;
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

	VkFramebuffer framebuffer_ = VK_NULL_HANDLE;

public:
	OverlayVulkan() = default;
	~OverlayVulkan() { destroy(); }

	OverlayVulkan(const OverlayVulkan &) = delete;
	OverlayVulkan &operator=(const OverlayVulkan &) = delete;

	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
		uint32_t queue_family);
	void set_encoding_buffer(VkDescriptorBufferInfo info);
	// The target is linear RGBA16 UNORM in COLOR_ATTACHMENT_OPTIMAL.
	void set_target(VkImageView view, VkExtent2D extent);
	bool upload_font(const unsigned char *pixels, int width, int height);
	[[nodiscard]] int thumb_atlas_max() const { return this->thumb_atlas_max_; }
	bool upload_thumb(const uint16_t *pixels, int width, int height, int dst_x,
		int dst_y, int atlas_side, bool *recreated);
	bool rebuild_thumbs(
		const std::vector<ThumbUpload> &uploads, int atlas_side);
	void reset_thumbs();
	void record(VkCommandBuffer cmd, const OverlayMesh &mesh);
	void destroy();
};

class Renderer
{
	void destroy_swapchain();
	void create_swapchain();
	void ensure_engine(VkFormat dest_format, VkImageLayout dest_layout);
	void wait_idle() const;
	void destroy_presentation();
	void create_presentation();
	void record_presentation(VkCommandBuffer cmd, VkFramebuffer dest) const;
	[[nodiscard]] bool dithering() const;

	VkSurfaceKHR surface_ = VK_NULL_HANDLE;   // borrowed from QWindow
	VkPhysicalDevice phys_ = VK_NULL_HANDLE;  // borrowed from GpuContext
	VkDevice device_ = VK_NULL_HANDLE;        // borrowed from GpuContext
	VkQueue queue_ = VK_NULL_HANDLE;          // borrowed from GpuContext
	uint32_t queue_family_ = 0;

	VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
	VkColorSpaceKHR color_space_ = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	VkPresentModeKHR present_mode_ = VK_PRESENT_MODE_FIFO_KHR;
	VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
	VkExtent2D extent_{};
	VkExtent2D want_extent_{};
	std::vector<VkImage> images_;
	std::vector<VkImageView> views_;
	std::vector<VkFramebuffer> framebuffers_;
	// Presentation consumes these, so they must not be shared between images.
	std::vector<VkSemaphore> render_finished_;

	VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;
	VkSemaphore image_available_ = VK_NULL_HANDLE;

	dawn::ScaleEngine engine_;
	OverlayVulkan overlay_;
	float scale_ = 1.f;
	float pan_x_ = 0.f;
	float pan_y_ = 0.f;
	float angle_ = 0.f;
	dawn::Orientation orientation_ = dawn::Orientation::Rotate0;
	bool checkerboard_ = false;
	bool linear_blend_ = false;
	bool filter_ = true;
	dawn::Filter preferred_ = dawn::Filter::Expensive;
	std::shared_ptr<const dawn::ProfileEncoding> encoding_;
	VkCompositeAlphaFlagBitsKHR composite_alpha_ =
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	float well_[4] = {0xE8 / 255.f, 0xE8 / 255.f, 0xE8 / 255.f, 1.f};
	float checker_[3] = {0xF0 / 255.f, 0xF0 / 255.f, 0xF0 / 255.f};
	int checker_px_ = 20;
	VkImage compose_image_ = VK_NULL_HANDLE;
	VkDeviceMemory compose_memory_ = VK_NULL_HANDLE;
	VkImageView compose_view_ = VK_NULL_HANDLE;
	VkFramebuffer compose_fb_ = VK_NULL_HANDLE;
	VkRenderPass presentation_rp_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout presentation_set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout presentation_layout_ = VK_NULL_HANDLE;
	VkPipeline presentation_pipe_ = VK_NULL_HANDLE;
	VkSampler presentation_sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool presentation_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet presentation_set_ = VK_NULL_HANDLE;
	VkShaderModule presentation_vert_ = VK_NULL_HANDLE;
	VkShaderModule presentation_frag_ = VK_NULL_HANDLE;
	bool needs_resize_ = false;
	bool prefer_premultiplied_ = false;
	bool dither_enabled_ = true;
	uint32_t dest_inset_ = 0;
	std::function<void()> present_about_to_queue_;
	std::function<void()> present_queued_;

public:
	Renderer() = default;
	~Renderer() { destroy(); }

	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	bool init(const GpuContext &gpu, VkSurfaceKHR surface, Extent pixel,
		VkPresentModeKHR preferred_present_mode,
		std::function<void()> present_about_to_queue,
		std::function<void()> present_queued);
	void set_image(
		uint32_t w, uint32_t h, const uint8_t *pixels, size_t stride);
	void clear_image();
	void set_view(float scale, float pan_x, float pan_y,
		dawn::Orientation orientation, float angle);
	void set_well_colour(float r, float g, float b);
	void set_prefer_premultiplied(bool enabled)
	{
		this->prefer_premultiplied_ = enabled;
	}
	void set_dither_enabled(bool enabled) { this->dither_enabled_ = enabled; }
	void set_dest_inset(uint32_t px) { this->dest_inset_ = px; }
	void set_checker_colour(float r, float g, float b);
	/// `size` is one checkerboard square in device pixels: kCheckPts run
	/// through Kit::px(), like every other design size.
	void set_checkerboard(bool enabled, int size)
	{
		this->checkerboard_ = enabled;
		this->checker_px_ = size;
	}
	void set_blend_linear_light(bool enabled) { this->linear_blend_ = enabled; }
	/// Smooth toggle: on = preferred (Bilinear on CPU, Expensive on GPU), off =
	/// Nearest.
	void set_filter(bool enabled) { this->filter_ = enabled; }
	void set_encoding(std::shared_ptr<const dawn::ProfileEncoding> encoding);
	bool upload_font(const unsigned char *pixels, int width, int height);
	[[nodiscard]] int thumb_atlas_max() const;
	bool upload_thumb(const uint16_t *pixels, int width, int height, int dst_x,
		int dst_y, int atlas_side, bool *recreated);
	bool rebuild_thumbs(
		const std::vector<ThumbUpload> &uploads, int atlas_side);
	void reset_thumbs();
	void resize(Extent pixel);
	// False means no swapchain image was immediately available.
	bool draw_frame(const OverlayMesh &mesh);
	void destroy();

	[[nodiscard]] Extent extent() const
	{
		return {this->extent_.width, this->extent_.height};
	}
	[[nodiscard]] bool needs_resize() const { return this->needs_resize_; }
	[[nodiscard]] VkColorSpaceKHR color_space() const
	{
		return this->color_space_;
	}
};

}  // namespace dn
