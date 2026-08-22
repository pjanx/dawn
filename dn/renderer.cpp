//
// renderer.cpp: Vulkan image renderer
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "renderer.hpp"

#include "dn-dither-frag-spv.h"
#include "fullscreen-vert-spv.h"
#include "vk-device.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace std;

namespace dn
{
namespace
{

void
check_vk(VkResult result, const char *what)
{
	if (result != VK_SUCCESS) {
		fprintf(
			stderr, "%s failed: VkResult %d\n", what, static_cast<int>(result));
		exit(1);
	}
}

[[noreturn]] void
die(const char *message)
{
	fprintf(stderr, "%s\n", message);
	exit(1);
}

const char *
vk_format_name(VkFormat f)
{
	switch (f) {
	case VK_FORMAT_R16G16B16A16_UNORM:
		return "R16G16B16A16_UNORM";
	case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
		return "A2B10G10R10_UNORM_PACK32";
	case VK_FORMAT_A2R10G10B10_UNORM_PACK32:
		return "A2R10G10B10_UNORM_PACK32";
	case VK_FORMAT_B8G8R8A8_UNORM:
		return "B8G8R8A8_UNORM";
	case VK_FORMAT_R8G8B8A8_UNORM:
		return "R8G8B8A8_UNORM";
	default:
		return "other";
	}
}

const char *
vk_colorspace_name(VkColorSpaceKHR cs)
{
	switch (cs) {
	case VK_COLOR_SPACE_PASS_THROUGH_EXT:
		return "PASS_THROUGH";
	case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
		return "SRGB_NONLINEAR";
	default:
		return "other";
	}
}

int
format_depth_score(VkFormat f)
{
	if (f == VK_FORMAT_R16G16B16A16_UNORM)
		return 3;
	if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
		f == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
		return 2;
	if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM)
		return 1;
	return 0;
}

int
colorspace_score(VkColorSpaceKHR cs)
{
	if (cs == VK_COLOR_SPACE_PASS_THROUGH_EXT)
		return 2;
	if (cs == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		return 1;
	return 0;
}

int
surface_format_score(const VkSurfaceFormatKHR &sf)
{
	return colorspace_score(sf.colorSpace) * 10 + format_depth_score(sf.format);
}

bool
is_unorm8(VkFormat f)
{
	return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
}

VkSurfaceFormatKHR
pick_surface_format(const vector<VkSurfaceFormatKHR> &formats)
{
	VkSurfaceFormatKHR best = formats.front();
	int best_score = surface_format_score(best);
	for (size_t i = 1; i < formats.size(); ++i) {
		const int score = surface_format_score(formats[i]);
		if (score > best_score) {
			best = formats[i];
			best_score = score;
		}
	}
	return best;
}

VkPresentModeKHR
pick_present_mode(
	VkPhysicalDevice phys, VkSurfaceKHR surface, VkPresentModeKHR preferred)
{
	uint32_t count = 0;
	check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
				 phys, surface, &count, nullptr),
		"vkGetPhysicalDeviceSurfacePresentModesKHR count");
	vector<VkPresentModeKHR> modes(count);
	check_vk(vkGetPhysicalDeviceSurfacePresentModesKHR(
				 phys, surface, &count, modes.data()),
		"vkGetPhysicalDeviceSurfacePresentModesKHR");
	if (find(modes.begin(), modes.end(), preferred) != modes.end())
		return preferred;
	// TODO: A silent MAILBOX-to-FIFO fallback restores Mesa's legacy Wayland
	// FIFO wait in vkQueuePresentKHR, and thus the hidden-workspace stall. A
	// nonblocking presentation policy should try IMMEDIATE or report/fail
	// explicitly. Also report the selected mode once for diagnostics.
	return VK_PRESENT_MODE_FIFO_KHR;
}

}  // namespace

bool
Renderer::init(const GpuContext &gpu, VkSurfaceKHR surface, Extent pixel,
	VkPresentModeKHR preferred_present_mode,
	function<void()> present_about_to_queue, function<void()> present_queued)
{
	destroy();
	this->surface_ = surface;
	this->phys_ = gpu.phys();
	this->preferred_ = dn::preferred_filter(this->phys_);
	this->device_ = gpu.device();
	this->queue_ = gpu.queue();
	this->queue_family_ = gpu.queue_family();
	// TODO: Validate device_ and surface_ before querying their present modes.
	// Current callers guarantee both, but Renderer::init should not rely on
	// that.
	this->present_mode_ =
		pick_present_mode(this->phys_, this->surface_, preferred_present_mode);
	this->present_about_to_queue_ = std::move(present_about_to_queue);
	this->present_queued_ = std::move(present_queued);
	if (!this->device_ || !this->surface_)
		return false;

	VkCommandPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = this->queue_family_,
	};
	check_vk(vkCreateCommandPool(
				 this->device_, &pool_info, nullptr, &this->cmd_pool_),
		"vkCreateCommandPool");
	VkCommandBufferAllocateInfo command_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = this->cmd_pool_,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	check_vk(
		vkAllocateCommandBuffers(this->device_, &command_info, &this->cmd_),
		"vkAllocateCommandBuffers");

	VkFenceCreateInfo fence_info{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	check_vk(vkCreateFence(this->device_, &fence_info, nullptr, &this->fence_),
		"vkCreateFence");
	VkSemaphoreCreateInfo semaphore_info{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	check_vk(vkCreateSemaphore(this->device_, &semaphore_info, nullptr,
				 &this->image_available_),
		"vkCreateSemaphore image_available");
	check_vk(vkCreateSemaphore(this->device_, &semaphore_info, nullptr,
				 &this->render_finished_),
		"vkCreateSemaphore render_finished");

	this->want_extent_ = {pixel.width, pixel.height};
	create_swapchain();
	return true;
}

void
Renderer::destroy_swapchain()
{
	if (!this->device_)
		return;
	vkDeviceWaitIdle(this->device_);
	this->overlay_.set_swapchain({}, {});
	destroy_dither();
	for (VkFramebuffer framebuffer : this->framebuffers_)
		if (framebuffer)
			vkDestroyFramebuffer(this->device_, framebuffer, nullptr);
	this->framebuffers_.clear();
	for (VkImageView view : this->views_)
		if (view)
			vkDestroyImageView(this->device_, view, nullptr);
	this->views_.clear();
	this->images_.clear();
	if (this->swapchain_) {
		vkDestroySwapchainKHR(this->device_, this->swapchain_, nullptr);
		this->swapchain_ = VK_NULL_HANDLE;
	}
}

void
Renderer::destroy()
{
	if (this->device_) {
		vkDeviceWaitIdle(this->device_);
		this->overlay_.destroy();
		destroy_swapchain();
		this->engine_.destroy();
		if (this->render_finished_)
			vkDestroySemaphore(this->device_, this->render_finished_, nullptr);
		if (this->image_available_)
			vkDestroySemaphore(this->device_, this->image_available_, nullptr);
		if (this->fence_)
			vkDestroyFence(this->device_, this->fence_, nullptr);
		if (this->cmd_pool_)
			vkDestroyCommandPool(this->device_, this->cmd_pool_, nullptr);
	}
	this->surface_ = VK_NULL_HANDLE;
	this->phys_ = VK_NULL_HANDLE;
	this->device_ = VK_NULL_HANDLE;
	this->queue_ = VK_NULL_HANDLE;
	this->cmd_pool_ = VK_NULL_HANDLE;
	this->cmd_ = VK_NULL_HANDLE;
	this->fence_ = VK_NULL_HANDLE;
	this->image_available_ = VK_NULL_HANDLE;
	this->render_finished_ = VK_NULL_HANDLE;
	this->extent_ = {};
	this->want_extent_ = {};
	this->overlay_format_ = VK_FORMAT_UNDEFINED;
	this->overlay_initial_ = VK_IMAGE_LAYOUT_UNDEFINED;
	this->overlay_final_ = VK_IMAGE_LAYOUT_UNDEFINED;
	this->present_about_to_queue_ = {};
	this->present_queued_ = {};
}

bool
Renderer::dithering() const
{
	// TODO(p): User should be able to disable that.
	return is_unorm8(this->format_);
}

void
Renderer::ensure_engine(VkFormat dest_format, VkImageLayout dest_layout)
{
	if (!this->device_)
		return;
	string error;
	if (!this->engine_.init(this->phys_, this->device_, this->queue_,
			this->queue_family_, dest_format, dest_layout, &error))
		die(error.c_str());
}

void
Renderer::create_swapchain()
{
	destroy_swapchain();
	VkSurfaceCapabilitiesKHR capabilities{};
	check_vk(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
				 this->phys_, this->surface_, &capabilities),
		"vkGetPhysicalDeviceSurfaceCapabilitiesKHR");

	this->extent_.width = clamp(this->want_extent_.width,
		capabilities.minImageExtent.width,
		capabilities.maxImageExtent.width ? capabilities.maxImageExtent.width
										  : this->want_extent_.width);
	this->extent_.height = clamp(this->want_extent_.height,
		capabilities.minImageExtent.height,
		capabilities.maxImageExtent.height ? capabilities.maxImageExtent.height
										   : this->want_extent_.height);
	if (capabilities.currentExtent.width != UINT32_MAX)
		this->extent_ = capabilities.currentExtent;
	if (this->extent_.width == 0 || this->extent_.height == 0)
		return;

	uint32_t format_count = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(
		this->phys_, this->surface_, &format_count, nullptr);
	vector<VkSurfaceFormatKHR> formats(format_count);
	vkGetPhysicalDeviceSurfaceFormatsKHR(
		this->phys_, this->surface_, &format_count, formats.data());
	if (formats.empty())
		die("surface exposes no formats");

	const VkFormat old_format = this->format_;
	const VkColorSpaceKHR old_color_space = this->color_space_;
	const VkSurfaceFormatKHR picked = pick_surface_format(formats);
	this->format_ = picked.format;
	this->color_space_ = picked.colorSpace;
	if (this->format_ != old_format || this->color_space_ != old_color_space) {
		printf("swapchain: %s + %s\n", vk_format_name(this->format_),
			vk_colorspace_name(this->color_space_));
		if (this->color_space_ != VK_COLOR_SPACE_PASS_THROUGH_EXT)
			fprintf(stderr,
				"swapchain: PASS_THROUGH unavailable; "
				"using compositor-managed sRGB\n");
	}
	const bool dither = is_unorm8(this->format_);
	const VkFormat dest_format =
		dither ? VK_FORMAT_R16G16B16A16_UNORM : this->format_;
	const VkImageLayout dest_layout = dither
		? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
		: VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	if (this->format_ != old_format || !this->engine_.dest_render_pass())
		this->engine_.destroy();
	ensure_engine(dest_format, dest_layout);

	uint32_t image_count = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 &&
		image_count > capabilities.maxImageCount)
		image_count = capabilities.maxImageCount;
	VkCompositeAlphaFlagBitsKHR composite_alpha =
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (!(capabilities.supportedCompositeAlpha & composite_alpha)) {
		for (VkCompositeAlphaFlagBitsKHR candidate :
			{VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
				VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
				VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR}) {
			if (capabilities.supportedCompositeAlpha & candidate) {
				composite_alpha = candidate;
				break;
			}
		}
	}
	VkSwapchainCreateInfoKHR swapchain_info{
		.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
		.surface = this->surface_,
		.minImageCount = image_count,
		.imageFormat = this->format_,
		.imageColorSpace = this->color_space_,
		.imageExtent = this->extent_,
		.imageArrayLayers = 1,
		.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
		.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
		.preTransform = capabilities.currentTransform,
		.compositeAlpha = composite_alpha,
		.presentMode = this->present_mode_,
		.clipped = VK_TRUE,
	};
	check_vk(vkCreateSwapchainKHR(
				 this->device_, &swapchain_info, nullptr, &this->swapchain_),
		"vkCreateSwapchainKHR");

	uint32_t count = 0;
	check_vk(vkGetSwapchainImagesKHR(
				 this->device_, this->swapchain_, &count, nullptr),
		"vkGetSwapchainImagesKHR count");
	this->images_.resize(count);
	check_vk(vkGetSwapchainImagesKHR(
				 this->device_, this->swapchain_, &count, this->images_.data()),
		"vkGetSwapchainImagesKHR");
	this->views_.resize(count);
	this->framebuffers_.resize(count);
	for (uint32_t i = 0; i < count; ++i) {
		VkImageViewCreateInfo view_info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = this->images_[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = this->format_,
			.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1},
		};
		check_vk(vkCreateImageView(
					 this->device_, &view_info, nullptr, &this->views_[i]),
			"vkCreateImageView swap");
	}
	if (dither)
		create_dither();
	const VkRenderPass swap_rp =
		dither ? this->dither_rp_ : this->engine_.dest_render_pass();
	for (uint32_t i = 0; i < count; ++i) {
		VkFramebufferCreateInfo framebuffer_info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = swap_rp,
			.attachmentCount = 1,
			.pAttachments = &this->views_[i],
			.width = this->extent_.width,
			.height = this->extent_.height,
			.layers = 1,
		};
		check_vk(vkCreateFramebuffer(this->device_, &framebuffer_info, nullptr,
					 &this->framebuffers_[i]),
			"vkCreateFramebuffer");
	}
	const VkImageLayout overlay_initial = dest_layout;
	const VkImageLayout overlay_final = dest_layout;
	if (this->overlay_format_ != dest_format ||
		this->overlay_initial_ != overlay_initial ||
		this->overlay_final_ != overlay_final) {
		if (!this->overlay_.init(this->phys_, this->device_, this->queue_,
				this->queue_family_, dest_format, overlay_initial,
				overlay_final))
			die("overlay vulkan init failed");
		this->overlay_format_ = dest_format;
		this->overlay_initial_ = overlay_initial;
		this->overlay_final_ = overlay_final;
	}
	if (dither)
		this->overlay_.set_swapchain({this->compose_view_}, this->extent_);
	else
		this->overlay_.set_swapchain(this->views_, this->extent_);
	if (this->engine_.has_image()) {
		string error;
		if (!this->engine_.ensure_viewport(
				this->extent_.width, this->extent_.height, &error))
			die(error.c_str());
	}
}

void
Renderer::wait_idle() const
{
	if (!this->device_ || !this->fence_)
		return;
	check_vk(
		vkWaitForFences(this->device_, 1, &this->fence_, VK_TRUE, UINT64_MAX),
		"vkWaitForFences");
}

void
Renderer::set_image(
	uint32_t width, uint32_t height, const uint8_t *pixels, size_t stride)
{
	if (!this->device_ || width == 0 || height == 0 || !pixels)
		return;
	wait_idle();
	string error;
	if (!this->engine_.set_image(width, height, pixels, stride, &error))
		die(error.c_str());
	if (this->extent_.width && this->extent_.height &&
		!this->engine_.ensure_viewport(
			this->extent_.width, this->extent_.height, &error))
		die(error.c_str());
}

void
Renderer::clear_image()
{
	if (!this->device_)
		return;
	wait_idle();
	this->engine_.clear_image();
}

void
Renderer::set_view(float scale, float pan_x, float pan_y,
	dn::Orientation orientation, float angle)
{
	this->scale_ = scale;
	this->pan_x_ = pan_x;
	this->pan_y_ = pan_y;
	this->angle_ = angle;
	this->orientation_ = orientation;
}

void
Renderer::set_well_colour(float r, float g, float b)
{
	this->well_[0] = r;
	this->well_[1] = g;
	this->well_[2] = b;
	this->well_[3] = 1.0f;
}

void
Renderer::set_checker_colour(float r, float g, float b)
{
	this->checker_[0] = r;
	this->checker_[1] = g;
	this->checker_[2] = b;
}

void
Renderer::set_checkerboard(bool enabled)
{
	this->checkerboard_ = enabled;
}

void
Renderer::set_filter(bool enabled)
{
	this->filter_ = enabled;
}

void
Renderer::set_transfer(dn::Transfer transfer)
{
	this->transfer_ = transfer;
}

bool
Renderer::upload_font(const unsigned char *pixels, int width, int height)
{
	return this->overlay_.upload_font(pixels, width, height);
}

int
Renderer::thumb_atlas_max() const
{
	return this->overlay_.thumb_atlas_max();
}

bool
Renderer::upload_thumb(const uint16_t *pixels, int width, int height, int dst_x,
	int dst_y, int atlas_side, bool *recreated)
{
	return this->overlay_.upload_thumb(
		pixels, width, height, dst_x, dst_y, atlas_side, recreated);
}

void
Renderer::reset_thumbs()
{
	this->overlay_.reset_thumbs();
}

void
Renderer::resize(Extent pixel)
{
	this->needs_resize_ = false;
	this->want_extent_ = {pixel.width, pixel.height};
	if (this->device_)
		create_swapchain();
}

bool
Renderer::draw_frame(const OverlayMesh &mesh)
{
	if (!this->device_ || !this->swapchain_ || !this->extent_.width ||
		!this->extent_.height)
		return true;
	check_vk(
		vkWaitForFences(this->device_, 1, &this->fence_, VK_TRUE, UINT64_MAX),
		"vkWaitForFences");
	uint32_t index = 0;
	// A hidden Wayland surface has no guaranteed presentation progress, so an
	// infinite acquire timeout is invalid. Keep the latest frame dirty and let
	// Window retry later when no image is immediately available.
	VkResult acquire = vkAcquireNextImageKHR(this->device_, this->swapchain_, 0,
		this->image_available_, VK_NULL_HANDLE, &index);
	if (acquire == VK_NOT_READY || acquire == VK_TIMEOUT)
		return false;
	if (acquire == VK_ERROR_OUT_OF_DATE_KHR) {
		this->needs_resize_ = true;
		return true;
	}
	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
		check_vk(acquire, "vkAcquireNextImageKHR");

	check_vk(vkResetFences(this->device_, 1, &this->fence_), "vkResetFences");
	check_vk(vkResetCommandBuffer(this->cmd_, 0), "vkResetCommandBuffer");
	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	check_vk(
		vkBeginCommandBuffer(this->cmd_, &begin_info), "vkBeginCommandBuffer");
	dn::ScaleView view{
		.scale = this->scale_,
		.pan_x = this->pan_x_,
		.pan_y = this->pan_y_,
		.angle = this->angle_,
		.transfer = this->transfer_,
		.orientation = this->orientation_,
		.checkerboard = this->checkerboard_,
		.checker_r = this->checker_[0],
		.checker_g = this->checker_[1],
		.checker_b = this->checker_[2],
		// The well is behind the image, so alpha resolves in the shader.
		.composite = true,
		.filter = this->filter_ ? this->preferred_ : dn::Filter::Nearest,
	};
	const float clear[4] = {
		this->well_[0], this->well_[1], this->well_[2], this->well_[3]};
	const bool dither = dithering();
	VkFramebuffer dest_fb =
		dither ? this->compose_fb_ : this->framebuffers_[index];
	string error;
	if (this->engine_.has_image()) {
		if (!this->engine_.record(this->cmd_, dest_fb, this->extent_.width,
				this->extent_.height, view, clear, &error))
			die(error.c_str());
	} else if (!this->engine_.record_clear(this->cmd_, dest_fb,
				   this->extent_.width, this->extent_.height, clear, &error)) {
		die(error.c_str());
	}
	this->overlay_.record(this->cmd_, dither ? 0 : index, mesh);
	if (dither)
		record_dither(this->cmd_, this->framebuffers_[index]);
	check_vk(vkEndCommandBuffer(this->cmd_), "vkEndCommandBuffer");

	VkPipelineStageFlags wait_stage =
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
	VkSubmitInfo submit_info{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &this->image_available_,
		.pWaitDstStageMask = &wait_stage,
		.commandBufferCount = 1,
		.pCommandBuffers = &this->cmd_,
		.signalSemaphoreCount = 1,
		.pSignalSemaphores = &this->render_finished_,
	};
	check_vk(vkQueueSubmit(this->queue_, 1, &submit_info, this->fence_),
		"vkQueueSubmit");
	VkPresentInfoKHR present_info{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &this->render_finished_,
		.swapchainCount = 1,
		.pSwapchains = &this->swapchain_,
		.pImageIndices = &index,
	};
	if (this->present_about_to_queue_)
		this->present_about_to_queue_();
	VkResult present = vkQueuePresentKHR(this->queue_, &present_info);
	if (this->present_queued_)
		this->present_queued_();
	if (present == VK_ERROR_OUT_OF_DATE_KHR || present == VK_SUBOPTIMAL_KHR)
		this->needs_resize_ = true;
	else
		check_vk(present, "vkQueuePresentKHR");
	return true;
}

void
Renderer::destroy_dither()
{
	if (!this->device_)
		return;
	if (this->compose_fb_)
		vkDestroyFramebuffer(this->device_, this->compose_fb_, nullptr);
	if (this->compose_view_)
		vkDestroyImageView(this->device_, this->compose_view_, nullptr);
	if (this->compose_image_)
		vkDestroyImage(this->device_, this->compose_image_, nullptr);
	if (this->compose_memory_)
		vkFreeMemory(this->device_, this->compose_memory_, nullptr);
	if (this->dither_pipe_)
		vkDestroyPipeline(this->device_, this->dither_pipe_, nullptr);
	if (this->dither_layout_)
		vkDestroyPipelineLayout(this->device_, this->dither_layout_, nullptr);
	if (this->dither_vert_)
		vkDestroyShaderModule(this->device_, this->dither_vert_, nullptr);
	if (this->dither_frag_)
		vkDestroyShaderModule(this->device_, this->dither_frag_, nullptr);
	if (this->dither_pool_)
		vkDestroyDescriptorPool(this->device_, this->dither_pool_, nullptr);
	if (this->dither_set_layout_)
		vkDestroyDescriptorSetLayout(
			this->device_, this->dither_set_layout_, nullptr);
	if (this->dither_sampler_)
		vkDestroySampler(this->device_, this->dither_sampler_, nullptr);
	if (this->dither_rp_)
		vkDestroyRenderPass(this->device_, this->dither_rp_, nullptr);
	this->compose_fb_ = VK_NULL_HANDLE;
	this->compose_view_ = VK_NULL_HANDLE;
	this->compose_image_ = VK_NULL_HANDLE;
	this->compose_memory_ = VK_NULL_HANDLE;
	this->dither_pipe_ = VK_NULL_HANDLE;
	this->dither_layout_ = VK_NULL_HANDLE;
	this->dither_vert_ = VK_NULL_HANDLE;
	this->dither_frag_ = VK_NULL_HANDLE;
	this->dither_pool_ = VK_NULL_HANDLE;
	this->dither_set_ = VK_NULL_HANDLE;
	this->dither_set_layout_ = VK_NULL_HANDLE;
	this->dither_sampler_ = VK_NULL_HANDLE;
	this->dither_rp_ = VK_NULL_HANDLE;
}

void
Renderer::create_dither()
{
	destroy_dither();
	if (!this->device_ || !this->engine_.dest_render_pass() ||
		!this->extent_.width || !this->extent_.height)
		die("dither compose: missing dest pass or extent");

	constexpr VkFormat kCompose = VK_FORMAT_R16G16B16A16_UNORM;
	VkImageCreateInfo image_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
		.imageType = VK_IMAGE_TYPE_2D,
		.format = kCompose,
		.extent = {this->extent_.width, this->extent_.height, 1},
		.mipLevels = 1,
		.arrayLayers = 1,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.tiling = VK_IMAGE_TILING_OPTIMAL,
		.usage =
			VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
	};
	check_vk(vkCreateImage(
				 this->device_, &image_info, nullptr, &this->compose_image_),
		"vkCreateImage compose");
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(
		this->device_, this->compose_image_, &requirements);
	const uint32_t type = vk_memory_type(this->phys_,
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	if (type == UINT32_MAX)
		die("dither compose: no device-local memory");
	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	check_vk(vkAllocateMemory(
				 this->device_, &allocate, nullptr, &this->compose_memory_),
		"vkAllocateMemory compose");
	check_vk(vkBindImageMemory(
				 this->device_, this->compose_image_, this->compose_memory_, 0),
		"vkBindImageMemory compose");
	VkImageViewCreateInfo view_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = this->compose_image_,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = kCompose,
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1},
	};
	check_vk(vkCreateImageView(
				 this->device_, &view_info, nullptr, &this->compose_view_),
		"vkCreateImageView compose");
	VkFramebufferCreateInfo fb_info{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = this->engine_.dest_render_pass(),
		.attachmentCount = 1,
		.pAttachments = &this->compose_view_,
		.width = this->extent_.width,
		.height = this->extent_.height,
		.layers = 1,
	};
	check_vk(vkCreateFramebuffer(
				 this->device_, &fb_info, nullptr, &this->compose_fb_),
		"vkCreateFramebuffer compose");

	VkAttachmentDescription color{
		.format = this->format_,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
		.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
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
		.srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
		.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		.srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
	};
	VkRenderPassCreateInfo rp_info{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
		.attachmentCount = 1,
		.pAttachments = &color,
		.subpassCount = 1,
		.pSubpasses = &subpass,
		.dependencyCount = 1,
		.pDependencies = &dependency,
	};
	check_vk(
		vkCreateRenderPass(this->device_, &rp_info, nullptr, &this->dither_rp_),
		"vkCreateRenderPass dither");

	VkSamplerCreateInfo sampler_info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	check_vk(vkCreateSampler(
				 this->device_, &sampler_info, nullptr, &this->dither_sampler_),
		"vkCreateSampler dither");

	VkDescriptorSetLayoutBinding binding{
		.binding = 0,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
	};
	VkDescriptorSetLayoutCreateInfo set_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 1,
		.pBindings = &binding,
	};
	check_vk(vkCreateDescriptorSetLayout(
				 this->device_, &set_info, nullptr, &this->dither_set_layout_),
		"vkCreateDescriptorSetLayout dither");
	VkDescriptorPoolSize pool_size{
		.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.descriptorCount = 1,
	};
	VkDescriptorPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 1,
		.pPoolSizes = &pool_size,
	};
	check_vk(vkCreateDescriptorPool(
				 this->device_, &pool_info, nullptr, &this->dither_pool_),
		"vkCreateDescriptorPool dither");
	VkDescriptorSetAllocateInfo set_alloc{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = this->dither_pool_,
		.descriptorSetCount = 1,
		.pSetLayouts = &this->dither_set_layout_,
	};
	check_vk(
		vkAllocateDescriptorSets(this->device_, &set_alloc, &this->dither_set_),
		"vkAllocateDescriptorSets dither");
	VkDescriptorImageInfo image_descriptor{
		.sampler = this->dither_sampler_,
		.imageView = this->compose_view_,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = this->dither_set_,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_descriptor,
	};
	vkUpdateDescriptorSets(this->device_, 1, &write, 0, nullptr);

	VkShaderModuleCreateInfo vert_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fullscreen_vert_words * sizeof(uint32_t),
		.pCode = fullscreen_vert,
	};
	check_vk(vkCreateShaderModule(
				 this->device_, &vert_info, nullptr, &this->dither_vert_),
		"vkCreateShaderModule dither vert");
	VkShaderModuleCreateInfo frag_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = dn_dither_frag_words * sizeof(uint32_t),
		.pCode = dn_dither_frag,
	};
	check_vk(vkCreateShaderModule(
				 this->device_, &frag_info, nullptr, &this->dither_frag_),
		"vkCreateShaderModule dither frag");

	VkPipelineLayoutCreateInfo layout_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &this->dither_set_layout_,
	};
	check_vk(vkCreatePipelineLayout(
				 this->device_, &layout_info, nullptr, &this->dither_layout_),
		"vkCreatePipelineLayout dither");

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = this->dither_vert_;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = this->dither_frag_;
	stages[1].pName = "main";
	VkPipelineVertexInputStateCreateInfo vertex_input{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
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
		.layout = this->dither_layout_,
		.renderPass = this->dither_rp_,
	};
	check_vk(vkCreateGraphicsPipelines(this->device_, VK_NULL_HANDLE, 1,
				 &pipeline_info, nullptr, &this->dither_pipe_),
		"vkCreateGraphicsPipelines dither");
}

void
Renderer::record_dither(VkCommandBuffer cmd, VkFramebuffer dest) const
{
	if (!cmd || !dest || !this->compose_image_ || !this->dither_pipe_)
		return;
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = this->compose_image_,
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1},
	};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&barrier);
	VkRenderPassBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = this->dither_rp_,
		.framebuffer = dest,
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
	VkRect2D scissor{.extent = this->extent_};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->dither_pipe_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		this->dither_layout_, 0, 1, &this->dither_set_, 0, nullptr);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

}  // namespace dn
