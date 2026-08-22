//
// renderer.hpp: Vulkan image renderer
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "gpu.hpp"
#include "libdnvk.h"
#include "overlay.hpp"
#include "types.hpp"

#include <libdn.h>

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

namespace dn
{

class Renderer
{
	void destroy_swapchain();
	void create_swapchain();
	void ensure_engine(VkFormat dest_format, VkImageLayout dest_layout);
	void wait_idle() const;
	void destroy_dither();
	void create_dither();
	void record_dither(VkCommandBuffer cmd, VkFramebuffer dest) const;
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

	VkCommandPool cmd_pool_ = VK_NULL_HANDLE;
	VkCommandBuffer cmd_ = VK_NULL_HANDLE;
	VkFence fence_ = VK_NULL_HANDLE;
	VkSemaphore image_available_ = VK_NULL_HANDLE;
	VkSemaphore render_finished_ = VK_NULL_HANDLE;

	dn::ScaleEngine engine_;
	OverlayVulkan overlay_;
	float scale_ = 1.0f;
	float pan_x_ = 0.0f;
	float pan_y_ = 0.0f;
	float angle_ = 0.0f;
	dn::Orientation orientation_ = dn::Orientation::Rotate0;
	bool checkerboard_ = false;
	bool filter_ = true;
	dn::Filter preferred_ = dn::Filter::Expensive;
	dn::Transfer transfer_ = dn::Transfer::Srgb;
	float well_[4] = {0xE8 / 255.0f, 0xE8 / 255.0f, 0xE8 / 255.0f, 1.0f};
	float checker_[3] = {0xF0 / 255.0f, 0xF0 / 255.0f, 0xF0 / 255.0f};
	VkFormat overlay_format_ = VK_FORMAT_UNDEFINED;
	VkImageLayout overlay_initial_ = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImageLayout overlay_final_ = VK_IMAGE_LAYOUT_UNDEFINED;
	VkImage compose_image_ = VK_NULL_HANDLE;
	VkDeviceMemory compose_memory_ = VK_NULL_HANDLE;
	VkImageView compose_view_ = VK_NULL_HANDLE;
	VkFramebuffer compose_fb_ = VK_NULL_HANDLE;
	VkRenderPass dither_rp_ = VK_NULL_HANDLE;
	VkDescriptorSetLayout dither_set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout dither_layout_ = VK_NULL_HANDLE;
	VkPipeline dither_pipe_ = VK_NULL_HANDLE;
	VkSampler dither_sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool dither_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet dither_set_ = VK_NULL_HANDLE;
	VkShaderModule dither_vert_ = VK_NULL_HANDLE;
	VkShaderModule dither_frag_ = VK_NULL_HANDLE;
	bool needs_resize_ = false;
	bool prefer_premultiplied_ = false;
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
		dn::Orientation orientation, float angle = 0.0f);
	void set_well_colour(float r, float g, float b);
	void set_prefer_premultiplied(bool enabled);
	void set_dest_inset(uint32_t px);
	void set_checker_colour(float r, float g, float b);
	void set_checkerboard(bool enabled);
	/// Smooth toggle: on = preferred (Bilinear on CPU, Expensive on GPU), off =
	/// Nearest.
	void set_filter(bool enabled);
	void set_transfer(dn::Transfer transfer);
	bool upload_font(const unsigned char *pixels, int width, int height);
	[[nodiscard]] int thumb_atlas_max() const;
	bool upload_thumb(const uint16_t *pixels, int width, int height, int dst_x,
		int dst_y, int atlas_side, bool *recreated = nullptr);
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
