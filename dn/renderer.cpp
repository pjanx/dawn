//
// renderer.cpp: Vulkan image renderer and its overlay pass
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "renderer.hpp"

#include "dn-overlay-frag-spv.h"
#include "dn-overlay-vert-spv.h"
#include "dn-present-frag-spv.h"
#include "dn-thumb-frag-spv.h"
#include "fullscreen-vert-spv.h"
#include "libdn/vk-device.hpp"

#include <QtLogging>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace std;

namespace dn
{

static void
check_vk(VkResult result, const char *what)
{
	if (result != VK_SUCCESS) {
		qCritical("%s failed: VkResult %d", what, int(result));
		exit(1);
	}
}

#define CALL_VK(name, suffix, ...)                                             \
	check_vk(vk##name(__VA_ARGS__), "vk" #name suffix)

[[noreturn]] static void
die(const char *message)
{
	qCritical("%s", message);
	exit(1);
}

static const char *
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

static const char *
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

// Bits per component, zero for formats we do not expect to present on.
static int
format_bits(VkFormat f)
{
	if (f == VK_FORMAT_R16G16B16A16_UNORM)
		return 16;
	if (f == VK_FORMAT_A2B10G10R10_UNORM_PACK32 ||
		f == VK_FORMAT_A2R10G10B10_UNORM_PACK32)
		return 10;
	if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM)
		return 8;
	return 0;
}

static int
colorspace_score(VkColorSpaceKHR cs)
{
	if (cs == VK_COLOR_SPACE_PASS_THROUGH_EXT)
		return 2;
	if (cs == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		return 1;
	return 0;
}

static int
surface_format_score(const VkSurfaceFormatKHR &sf)
{
	return colorspace_score(sf.colorSpace) * 100 + format_bits(sf.format);
}

// The dither target, which DN_BPC overrides no matter what we present on.
static int
dither_bits(VkFormat format)
{
	static const char *env = getenv("DN_BPC");
	static const int forced = env ? atoi(env) : 0;
	if (forced)
		return forced;
	if (format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB)
		return 8;
	return format_bits(format);
}

static VkSurfaceFormatKHR
pick_surface_format(const vector<VkSurfaceFormatKHR> &formats)
{
	VkSurfaceFormatKHR best = formats.front();
	int best_score = surface_format_score(best);
	for (size_t i = 1; i < formats.size(); i++) {
		const int score = surface_format_score(formats[i]);
		if (score > best_score) {
			best = formats[i];
			best_score = score;
		}
	}
	return best;
}

static VkPresentModeKHR
pick_present_mode(
	VkPhysicalDevice phys, VkSurfaceKHR surface, VkPresentModeKHR preferred)
{
	uint32_t count = 0;
	CALL_VK(GetPhysicalDeviceSurfacePresentModesKHR, " count", phys, surface,
		&count, nullptr);
	vector<VkPresentModeKHR> modes(count);
	CALL_VK(GetPhysicalDeviceSurfacePresentModesKHR, "", phys, surface, &count,
		modes.data());
	if (find(modes.begin(), modes.end(), preferred) != modes.end())
		return preferred;
	// TODO: A silent MAILBOX-to-FIFO fallback restores Mesa's legacy Wayland
	// FIFO wait in vkQueuePresentKHR, and thus the hidden-workspace stall. A
	// nonblocking presentation policy should try IMMEDIATE or report/fail
	// explicitly. Also report the selected mode once for diagnostics.
	return VK_PRESENT_MODE_FIFO_KHR;
}

static constexpr VkFormat kOverlayTexFormat = VK_FORMAT_R16G16B16A16_UNORM;
static constexpr VkDeviceSize kOverlayBpp = 8;
static constexpr uint32_t kThumbAtlasBase = 2048;
static constexpr VkDeviceSize kThumbAtlasBudgetCap = 512ull * 1024 * 1024;
static constexpr VkDeviceSize kThumbAtlasHeapFrac = 4;

namespace
{
struct PushConstant {
	float scale[2];
	float translate[2];
	Colour odd;
	Colour even;
	float origin[2];
	float checker_size;
};
static_assert(sizeof(PushConstant) == 60);
}  // namespace

static VkImageCreateInfo
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

// --- Renderer ----------------------------------------------------------------

bool
Renderer::init(const GpuContext &gpu, VkSurfaceKHR surface, Extent pixel,
	VkPresentModeKHR preferred_present_mode,
	function<void()> present_about_to_queue, function<void()> present_queued)
{
	destroy();
	this->surface_ = surface;
	this->phys_ = gpu.phys();
	this->preferred_ = dawn::preferred_filter(this->phys_);
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
	CALL_VK(CreateCommandPool, "", this->device_, &pool_info, nullptr,
		&this->cmd_pool_);
	VkCommandBufferAllocateInfo command_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = this->cmd_pool_,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	CALL_VK(
		AllocateCommandBuffers, "", this->device_, &command_info, &this->cmd_);

	VkFenceCreateInfo fence_info{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	CALL_VK(
		CreateFence, "", this->device_, &fence_info, nullptr, &this->fence_);
	VkSemaphoreCreateInfo semaphore_info{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	CALL_VK(CreateSemaphore, " image_available", this->device_, &semaphore_info,
		nullptr, &this->image_available_);

	if (!this->overlay_.init(
			this->phys_, this->device_, this->queue_, this->queue_family_))
		return false;

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
	this->overlay_.set_target(VK_NULL_HANDLE, {});
	destroy_compose();
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
	// Presentation only lets go of these once its swapchain is gone.
	for (VkSemaphore semaphore : this->render_finished_)
		if (semaphore)
			vkDestroySemaphore(this->device_, semaphore, nullptr);
	this->render_finished_.clear();
}

void
Renderer::destroy()
{
	if (this->device_) {
		vkDeviceWaitIdle(this->device_);
		this->overlay_.destroy();
		destroy_swapchain();
		destroy_presentation();
		this->engine_.destroy();
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
	this->extent_ = {};
	this->want_extent_ = {};
	this->encoding_.reset();
	this->present_about_to_queue_ = {};
	this->present_queued_ = {};
}

bool
Renderer::dithering() const
{
	const int bits = dither_bits(this->format_);
	return this->dither_enabled_ && bits > 0 && bits <= 10;
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
	if (!this->encoding_)
		set_encoding(make_shared<const dawn::ProfileEncoding>(
			dawn::profile_encoding(nullptr)));
}

void
Renderer::set_encoding(shared_ptr<const dawn::ProfileEncoding> encoding)
{
	if (this->encoding_ == encoding)
		return;
	wait_idle();
	this->encoding_ = std::move(encoding);
	string error;
	if (!this->engine_.set_encoding(*this->encoding_, &error))
		die(error.c_str());
}

void
Renderer::create_swapchain()
{
	destroy_swapchain();
	VkSurfaceCapabilitiesKHR capabilities{};
	CALL_VK(GetPhysicalDeviceSurfaceCapabilitiesKHR, "", this->phys_,
		this->surface_, &capabilities);

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
	if (this->format_ != old_format)
		destroy_presentation_pipeline();
	if (this->format_ != old_format || this->color_space_ != old_color_space) {
		qInfo("swapchain: %s + %s (dither: %d bpc)",
			vk_format_name(this->format_),
			vk_colorspace_name(this->color_space_),
			dithering() ? dither_bits(this->format_) : 0);
		if (this->color_space_ != VK_COLOR_SPACE_PASS_THROUGH_EXT)
			qWarning("swapchain: PASS_THROUGH unavailable; "
					 "using compositor-managed sRGB");
	}
	constexpr VkFormat dest_format = VK_FORMAT_R16G16B16A16_UNORM;
	constexpr VkImageLayout dest_layout =
		VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	ensure_engine(dest_format, dest_layout);

	// A window that isn't shown yet has no extent, but pages may already
	// be handing it images, and those need the engine.
	if (this->extent_.width == 0 || this->extent_.height == 0)
		return;

	uint32_t image_count = capabilities.minImageCount + 1;
	if (capabilities.maxImageCount > 0 &&
		image_count > capabilities.maxImageCount)
		image_count = capabilities.maxImageCount;
	VkCompositeAlphaFlagBitsKHR composite_alpha =
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	if (this->prefer_premultiplied_) {
		if (capabilities.supportedCompositeAlpha &
			VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR)
			composite_alpha = VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR;
		else
			qWarning("swapchain: PRE_MULTIPLIED composite alpha unavailable");
	}
	if (composite_alpha == VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR &&
		!(capabilities.supportedCompositeAlpha & composite_alpha)) {
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
	this->composite_alpha_ = composite_alpha;
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
	CALL_VK(CreateSwapchainKHR, "", this->device_, &swapchain_info, nullptr,
		&this->swapchain_);

	uint32_t count = 0;
	CALL_VK(GetSwapchainImagesKHR, " count", this->device_, this->swapchain_,
		&count, nullptr);
	this->images_.resize(count);
	CALL_VK(GetSwapchainImagesKHR, "", this->device_, this->swapchain_, &count,
		this->images_.data());
	this->views_.resize(count);
	this->framebuffers_.resize(count);
	this->render_finished_.resize(count);
	VkSemaphoreCreateInfo semaphore_info{
		.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
	};
	for (uint32_t i = 0; i < count; i++) {
		CALL_VK(CreateSemaphore, " render_finished", this->device_,
			&semaphore_info, nullptr, &this->render_finished_[i]);
		VkImageViewCreateInfo view_info{
			.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
			.image = this->images_[i],
			.viewType = VK_IMAGE_VIEW_TYPE_2D,
			.format = this->format_,
			.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1},
		};
		CALL_VK(CreateImageView, " swap", this->device_, &view_info, nullptr,
			&this->views_[i]);
	}
	if (!this->presentation_pool_)
		create_presentation();
	if (!this->presentation_pipe_)
		create_presentation_pipeline();
	create_compose();
	const VkRenderPass swap_rp = this->presentation_rp_;
	for (uint32_t i = 0; i < count; i++) {
		VkFramebufferCreateInfo framebuffer_info{
			.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
			.renderPass = swap_rp,
			.attachmentCount = 1,
			.pAttachments = &this->views_[i],
			.width = this->extent_.width,
			.height = this->extent_.height,
			.layers = 1,
		};
		CALL_VK(CreateFramebuffer, "", this->device_, &framebuffer_info,
			nullptr, &this->framebuffers_[i]);
	}
	this->overlay_.set_encoding_buffer(this->engine_.encoding_buffer());
	this->overlay_.set_target(this->compose_view_, this->extent_);
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
	CALL_VK(WaitForFences, "", this->device_, 1, &this->fence_, VK_TRUE,
		UINT64_MAX);
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
	dawn::Orientation orientation, float angle)
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
	this->well_[3] = 1.f;
}

void
Renderer::set_checker_colour(float r, float g, float b)
{
	this->checker_[0] = r;
	this->checker_[1] = g;
	this->checker_[2] = b;
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
	int dst_y, int atlas_side)
{
	return this->overlay_.upload_thumb(
		pixels, width, height, dst_x, dst_y, atlas_side);
}

bool
Renderer::rebuild_thumbs(span<const AtlasUpload> uploads, int atlas_side)
{
	return this->overlay_.rebuild_thumbs(uploads, atlas_side);
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
Renderer::draw_frame(const OverlayMesh &mesh, bool show_image)
{
	if (!this->device_ || !this->swapchain_ || !this->extent_.width ||
		!this->extent_.height)
		return true;
	CALL_VK(WaitForFences, "", this->device_, 1, &this->fence_, VK_TRUE,
		UINT64_MAX);
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

	CALL_VK(ResetFences, "", this->device_, 1, &this->fence_);
	CALL_VK(ResetCommandBuffer, "", this->cmd_, 0);
	VkCommandBufferBeginInfo begin_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
	};
	CALL_VK(BeginCommandBuffer, "", this->cmd_, &begin_info);
	const auto checker = dawn::sample_curves(this->encoding_->encode,
		{this->checker_[0], this->checker_[1], this->checker_[2]});
	dawn::ScaleView view{
		.scale = this->scale_,
		.pan_x = this->pan_x_,
		.pan_y = this->pan_y_,
		.angle = this->angle_,
		.profile_curves = true,
		.output_encoding = dawn::ScaleEncoding::Linear,
		.orientation = this->orientation_,
		.checkerboard = this->checkerboard_,
		.linear_blend = this->linear_blend_,
		.checker_r = checker[0],
		.checker_g = checker[1],
		.checker_b = checker[2],
		.checker_size = float(this->checker_px_),
		// The well is behind the image, so alpha resolves in the shader.
		.composite = true,
		.filter = this->filter_ ? this->preferred_ : dawn::Filter::Nearest,
	};
	const auto well = dawn::sample_curves(this->encoding_->encode,
		{this->well_[0], this->well_[1], this->well_[2]});
	const float clear[4] = {well[0], well[1], well[2], this->well_[3]};
	VkFramebuffer dest_fb = this->compose_fb_;
	string error;
	const uint32_t inset =
		(this->dest_inset_ > 0 && this->extent_.width > this->dest_inset_ * 2 &&
			this->extent_.height > this->dest_inset_ * 2)
		? this->dest_inset_
		: 0;
	this->engine_.set_dest_inset(inset, inset, inset, inset);
	if (show_image && this->engine_.has_image()) {
		if (!this->engine_.record(this->cmd_, dest_fb, this->extent_.width,
				this->extent_.height, view, clear, &error))
			die(error.c_str());
	} else if (!this->engine_.record_clear(this->cmd_, dest_fb,
				   this->extent_.width, this->extent_.height, this->well_,
				   &error)) {
		die(error.c_str());
	}
	this->overlay_.record(this->cmd_, mesh);
	record_presentation(this->cmd_, this->framebuffers_[index]);
	CALL_VK(EndCommandBuffer, "", this->cmd_);

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
		.pSignalSemaphores = &this->render_finished_[index],
	};
	CALL_VK(QueueSubmit, "", this->queue_, 1, &submit_info, this->fence_);
	VkPresentInfoKHR present_info{
		.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
		.waitSemaphoreCount = 1,
		.pWaitSemaphores = &this->render_finished_[index],
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

// The composition image follows the window extent; the presentation pipeline
// follows its format. Descriptors and layouts survive both kinds of change.
void
Renderer::destroy_compose()
{
	if (this->compose_fb_)
		vkDestroyFramebuffer(this->device_, this->compose_fb_, nullptr);
	if (this->compose_view_)
		vkDestroyImageView(this->device_, this->compose_view_, nullptr);
	if (this->compose_image_)
		vkDestroyImage(this->device_, this->compose_image_, nullptr);
	if (this->compose_memory_)
		vkFreeMemory(this->device_, this->compose_memory_, nullptr);
	this->compose_fb_ = VK_NULL_HANDLE;
	this->compose_view_ = VK_NULL_HANDLE;
	this->compose_image_ = VK_NULL_HANDLE;
	this->compose_memory_ = VK_NULL_HANDLE;
}

void
Renderer::create_compose()
{
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
	CALL_VK(CreateImage, " compose", this->device_, &image_info, nullptr,
		&this->compose_image_);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(
		this->device_, this->compose_image_, &requirements);
	const uint32_t type =
		dawn::vk_memory_type(this->phys_, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, nullptr);
	if (type == UINT32_MAX)
		die("linear compose: no device-local memory");
	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	CALL_VK(AllocateMemory, " compose", this->device_, &allocate, nullptr,
		&this->compose_memory_);
	CALL_VK(BindImageMemory, " compose", this->device_, this->compose_image_,
		this->compose_memory_, 0);
	VkImageViewCreateInfo view_info{
		.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
		.image = this->compose_image_,
		.viewType = VK_IMAGE_VIEW_TYPE_2D,
		.format = kCompose,
		.subresourceRange = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
			.levelCount = 1,
			.layerCount = 1},
	};
	CALL_VK(CreateImageView, " compose", this->device_, &view_info, nullptr,
		&this->compose_view_);
	VkFramebufferCreateInfo fb_info{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = this->engine_.dest_render_pass(),
		.attachmentCount = 1,
		.pAttachments = &this->compose_view_,
		.width = this->extent_.width,
		.height = this->extent_.height,
		.layers = 1,
	};
	CALL_VK(CreateFramebuffer, " compose", this->device_, &fb_info, nullptr,
		&this->compose_fb_);

	VkDescriptorImageInfo image_descriptor{
		.sampler = this->presentation_sampler_,
		.imageView = this->compose_view_,
		.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
	};
	VkWriteDescriptorSet write{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = this->presentation_set_,
		.dstBinding = 0,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
		.pImageInfo = &image_descriptor,
	};
	vkUpdateDescriptorSets(this->device_, 1, &write, 0, nullptr);
}

void
Renderer::destroy_presentation_pipeline()
{
	if (this->presentation_pipe_)
		vkDestroyPipeline(this->device_, this->presentation_pipe_, nullptr);
	if (this->presentation_rp_)
		vkDestroyRenderPass(this->device_, this->presentation_rp_, nullptr);
	this->presentation_pipe_ = VK_NULL_HANDLE;
	this->presentation_rp_ = VK_NULL_HANDLE;
}

void
Renderer::destroy_presentation()
{
	if (!this->device_)
		return;
	destroy_presentation_pipeline();
	if (this->presentation_layout_)
		vkDestroyPipelineLayout(
			this->device_, this->presentation_layout_, nullptr);
	if (this->presentation_pool_)
		vkDestroyDescriptorPool(
			this->device_, this->presentation_pool_, nullptr);
	if (this->presentation_set_layout_)
		vkDestroyDescriptorSetLayout(
			this->device_, this->presentation_set_layout_, nullptr);
	if (this->presentation_sampler_)
		vkDestroySampler(this->device_, this->presentation_sampler_, nullptr);
	this->presentation_layout_ = VK_NULL_HANDLE;
	this->presentation_pool_ = VK_NULL_HANDLE;
	this->presentation_set_ = VK_NULL_HANDLE;
	this->presentation_set_layout_ = VK_NULL_HANDLE;
	this->presentation_sampler_ = VK_NULL_HANDLE;
}

void
Renderer::create_presentation()
{
	VkSamplerCreateInfo sampler_info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
	};
	CALL_VK(CreateSampler, " presentation", this->device_, &sampler_info,
		nullptr, &this->presentation_sampler_);

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
			nullptr},
	};
	VkDescriptorSetLayoutCreateInfo set_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings,
	};
	CALL_VK(CreateDescriptorSetLayout, " presentation", this->device_,
		&set_info, nullptr, &this->presentation_set_layout_);
	const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
	};
	VkDescriptorPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 2,
		.pPoolSizes = pool_sizes,
	};
	CALL_VK(CreateDescriptorPool, " presentation", this->device_, &pool_info,
		nullptr, &this->presentation_pool_);
	VkDescriptorSetAllocateInfo set_alloc{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = this->presentation_pool_,
		.descriptorSetCount = 1,
		.pSetLayouts = &this->presentation_set_layout_,
	};
	CALL_VK(AllocateDescriptorSets, " presentation", this->device_, &set_alloc,
		&this->presentation_set_);
	const VkDescriptorBufferInfo curves = this->engine_.encoding_buffer();
	VkWriteDescriptorSet curve_write{
		.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
		.dstSet = this->presentation_set_,
		.dstBinding = 1,
		.descriptorCount = 1,
		.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
		.pBufferInfo = &curves};
	vkUpdateDescriptorSets(this->device_, 1, &curve_write, 0, nullptr);

	VkPushConstantRange push{
		.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
		.offset = 0,
		.size = sizeof(float) + 2 * sizeof(uint32_t),
	};
	VkPipelineLayoutCreateInfo layout_info{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &this->presentation_set_layout_,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push,
	};
	CALL_VK(CreatePipelineLayout, " presentation", this->device_, &layout_info,
		nullptr, &this->presentation_layout_);
}

void
Renderer::create_presentation_pipeline()
{
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
	CALL_VK(CreateRenderPass, " presentation", this->device_, &rp_info, nullptr,
		&this->presentation_rp_);

	VkShaderModule presentation_vert = VK_NULL_HANDLE,
				   presentation_frag = VK_NULL_HANDLE;
	VkShaderModuleCreateInfo vert_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = fullscreen_vert_words * sizeof(uint32_t),
		.pCode = fullscreen_vert,
	};
	CALL_VK(CreateShaderModule, " presentation vert", this->device_, &vert_info,
		nullptr, &presentation_vert);
	VkShaderModuleCreateInfo frag_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = dn_present_frag_words * sizeof(uint32_t),
		.pCode = dn_present_frag,
	};
	CALL_VK(CreateShaderModule, " presentation frag", this->device_, &frag_info,
		nullptr, &presentation_frag);

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = presentation_vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = presentation_frag;
	stages[1].pName = "main";
	VkGraphicsPipelineCreateInfo pipeline_info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &dawn::kNoVertexInput,
		.pInputAssemblyState = &dawn::kTriangleList,
		.pViewportState = &dawn::kOneViewport,
		.pRasterizationState = &dawn::kRasterFill,
		.pMultisampleState = &dawn::kNoMultisample,
		.pColorBlendState = &dawn::kBlendReplace,
		.pDynamicState = &dawn::kDynamicViewportScissor,
		.layout = this->presentation_layout_,
		.renderPass = this->presentation_rp_,
	};
	CALL_VK(CreateGraphicsPipelines, " presentation", this->device_,
		VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &this->presentation_pipe_);
	vkDestroyShaderModule(this->device_, presentation_vert, nullptr);
	vkDestroyShaderModule(this->device_, presentation_frag, nullptr);
}

// Both passes cover the whole destination; the overlay sets its scissor per
// draw.
static void
begin_render_pass(VkCommandBuffer cmd, VkRenderPass pass, VkFramebuffer dest,
	VkExtent2D extent)
{
	VkRenderPassBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = pass,
		.framebuffer = dest,
		.renderArea = {.extent = extent},
	};
	vkCmdBeginRenderPass(cmd, &begin, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport viewport{
		.width = float(extent.width),
		.height = float(extent.height),
		.maxDepth = 1.f,
	};
	vkCmdSetViewport(cmd, 0, 1, &viewport);
}

void
Renderer::record_presentation(VkCommandBuffer cmd, VkFramebuffer dest) const
{
	if (!cmd || !dest || !this->compose_image_ || !this->presentation_pipe_)
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
	begin_render_pass(cmd, this->presentation_rp_, dest, this->extent_);
	VkRect2D scissor{.extent = this->extent_};
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(
		cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, this->presentation_pipe_);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		this->presentation_layout_, 0, 1, &this->presentation_set_, 0, nullptr);
	const struct {
		float levels;
		uint32_t premultiplied;
		uint32_t srgb_attachment;
	} push{
		dithering() ? float((1 << dither_bits(this->format_)) - 1) : 0.f,
		this->composite_alpha_ != VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR,
		this->format_ == VK_FORMAT_B8G8R8A8_SRGB ||
			this->format_ == VK_FORMAT_R8G8B8A8_SRGB,
	};
	vkCmdPushConstants(cmd, this->presentation_layout_,
		VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof push, &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
}

// --- Overlay -----------------------------------------------------------------

void
OverlayVulkan::set_encoding_buffer(VkDescriptorBufferInfo info)
{
	for (VkDescriptorSet set : this->descriptor_sets_) {
		VkWriteDescriptorSet write{
			.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &info};
		vkUpdateDescriptorSets(this->device_, 1, &write, 0, nullptr);
	}
}

bool
OverlayVulkan::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	uint32_t queue_family)
{
	destroy();
	this->phys_ = phys;
	this->device_ = device;
	this->queue_ = queue;
	this->queue_family_ = queue_family;
	if (!this->phys_ || !this->device_ || !this->queue_)
		return false;

	VkCommandPoolCreateInfo pool_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT,
		.queueFamilyIndex = this->queue_family_,
	};
	CALL_VK(CreateCommandPool, " overlay upload", this->device_, &pool_info,
		nullptr, &this->upload_pool_);
	compute_thumb_atlas_max();

	VkSamplerCreateInfo sampler_info{
		.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_LINEAR,
		.minFilter = VK_FILTER_LINEAR,
		.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.maxLod = 1.f,
	};
	CALL_VK(CreateSampler, " overlay", this->device_, &sampler_info, nullptr,
		&this->sampler_);

	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
			nullptr},
	};
	VkDescriptorSetLayoutCreateInfo layout_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings,
	};
	CALL_VK(CreateDescriptorSetLayout, " overlay", this->device_, &layout_info,
		nullptr, &this->set_layout_);

	const VkDescriptorPoolSize pool_sizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
	};
	VkDescriptorPoolCreateInfo descriptor_pool_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 2,
		.poolSizeCount = 2,
		.pPoolSizes = pool_sizes,
	};
	CALL_VK(CreateDescriptorPool, " overlay", this->device_,
		&descriptor_pool_info, nullptr, &this->descriptor_pool_);
	VkDescriptorSetLayout layouts[2] = {this->set_layout_, this->set_layout_};
	VkDescriptorSetAllocateInfo allocate_info{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = this->descriptor_pool_,
		.descriptorSetCount = 2,
		.pSetLayouts = layouts,
	};
	CALL_VK(AllocateDescriptorSets, " overlay", this->device_, &allocate_info,
		this->descriptor_sets_);

	VkAttachmentDescription color{
		.format = VK_FORMAT_R16G16B16A16_UNORM,
		.samples = VK_SAMPLE_COUNT_1_BIT,
		.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
		.storeOp = VK_ATTACHMENT_STORE_OP_STORE,
		.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
		.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
		.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
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
	CALL_VK(CreateRenderPass, " overlay", this->device_, &render_pass_info,
		nullptr, &this->render_pass_);

	return create_pipeline();
}

bool
OverlayVulkan::create_pipeline()
{
	VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE,
				   thumb_frag = VK_NULL_HANDLE;
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
	VkShaderModuleCreateInfo thumb_frag_info{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = dn_thumb_frag_words * sizeof(uint32_t),
		.pCode = dn_thumb_frag,
	};
	CALL_VK(CreateShaderModule, " overlay vert", this->device_, &vert_info,
		nullptr, &vert);
	CALL_VK(CreateShaderModule, " overlay frag", this->device_, &frag_info,
		nullptr, &frag);
	CALL_VK(CreateShaderModule, " thumb frag", this->device_, &thumb_frag_info,
		nullptr, &thumb_frag);

	VkPushConstantRange push{
		.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
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
	CALL_VK(CreatePipelineLayout, " overlay", this->device_,
		&pipeline_layout_info, nullptr, &this->pipeline_layout_);

	VkPipelineShaderStageCreateInfo stages[2]{};
	stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
	stages[0].module = vert;
	stages[0].pName = "main";
	stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
	stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
	stages[1].module = frag;
	stages[1].pName = "main";

	VkVertexInputBindingDescription binding{
		.binding = 0,
		.stride = sizeof(OverlayQuad),
		.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE,
	};
	VkVertexInputAttributeDescription attributes[4]{
		{.location = 0,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SINT,
			.offset = offsetof(OverlayQuad, box)},
		{.location = 1,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = offsetof(OverlayQuad, uv)},
		{.location = 2,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = offsetof(OverlayQuad, top)},
		{.location = 3,
			.binding = 0,
			.format = VK_FORMAT_R32G32B32A32_SFLOAT,
			.offset = offsetof(OverlayQuad, bottom)},
	};
	VkPipelineVertexInputStateCreateInfo vertex_input{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
		.vertexBindingDescriptionCount = 1,
		.pVertexBindingDescriptions = &binding,
		.vertexAttributeDescriptionCount = 4,
		.pVertexAttributeDescriptions = attributes,
	};
	VkGraphicsPipelineCreateInfo pipeline_info{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &vertex_input,
		.pInputAssemblyState = &dawn::kTriangleList,
		.pViewportState = &dawn::kOneViewport,
		.pRasterizationState = &dawn::kRasterFill,
		.pMultisampleState = &dawn::kNoMultisample,
		.pColorBlendState = &dawn::kBlendPremulOver,
		.pDynamicState = &dawn::kDynamicViewportScissor,
		.layout = this->pipeline_layout_,
		.renderPass = this->render_pass_,
	};
	CALL_VK(CreateGraphicsPipelines, " overlay", this->device_, VK_NULL_HANDLE,
		1, &pipeline_info, nullptr, &this->pipeline_);
	stages[1].module = thumb_frag;
	CALL_VK(CreateGraphicsPipelines, " thumbnails", this->device_,
		VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &this->thumb_pipeline_);
	vkDestroyShaderModule(this->device_, vert, nullptr);
	vkDestroyShaderModule(this->device_, frag, nullptr);
	vkDestroyShaderModule(this->device_, thumb_frag, nullptr);
	return true;
}

void
OverlayVulkan::set_target(VkImageView view, VkExtent2D extent)
{
	destroy_target();
	this->extent_ = extent;
	if (!this->device_ || !this->render_pass_ || !view || !extent.width ||
		!extent.height)
		return;
	VkFramebufferCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
		.renderPass = this->render_pass_,
		.attachmentCount = 1,
		.pAttachments = &view,
		.width = extent.width,
		.height = extent.height,
		.layers = 1,
	};
	CALL_VK(CreateFramebuffer, " overlay", this->device_, &info, nullptr,
		&this->framebuffer_);
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
			dim = min(dim, fmt.maxExtent.width);
		if (fmt.maxExtent.height)
			dim = min(dim, fmt.maxExtent.height);
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
		if (dawn::vk_memory_type(this->phys_, requirements.memoryTypeBits,
				VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr,
				&heap) == UINT32_MAX ||
			(max_resource && requirements.size > max_resource) ||
			requirements.size >
				min(kThumbAtlasBudgetCap, heap / kThumbAtlasHeapFrac))
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
	CALL_VK(CreateImage, " overlay tex", this->device_, &image_info, nullptr,
		image);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(this->device_, *image, &requirements);
	const uint32_t image_type =
		dawn::vk_memory_type(this->phys_, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, nullptr, nullptr);
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
	CALL_VK(AllocateMemory, " overlay tex", this->device_, &allocate, nullptr,
		memory);
	CALL_VK(BindImageMemory, " overlay tex", this->device_, *image, *memory, 0);
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
	CALL_VK(CreateImageView, " overlay tex", this->device_, &view_info, nullptr,
		view);
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
OverlayVulkan::copy_rgba16(span<const AtlasUpload> uploads, int width,
	int height, VkImage image, VkImageLayout layout) const
{
	if (!this->device_ || !image || width <= 0 || height <= 0 ||
		uploads.empty())
		return false;

	VkDeviceSize size = 0;
	vector<VkBufferImageCopy> copies;
	for (const AtlasUpload &upload : uploads) {
		if (!upload.pixels || upload.width <= 0 || upload.height <= 0 ||
			upload.x < 0 || upload.y < 0 || upload.x > width ||
			upload.y > height || upload.width > width - upload.x ||
			upload.height > height - upload.y)
			return false;
		copies.push_back({
			.bufferOffset = size,
			.imageSubresource = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.layerCount = 1},
			.imageOffset = {upload.x, upload.y, 0},
			.imageExtent = {uint32_t(upload.width), uint32_t(upload.height), 1},
		});
		size += VkDeviceSize(upload.width) * upload.height * kOverlayBpp;
	}
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_memory = VK_NULL_HANDLE;
	VkBufferCreateInfo buffer_info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = size,
		.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
	};
	CALL_VK(CreateBuffer, " overlay tex staging", this->device_, &buffer_info,
		nullptr, &staging);

	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(this->device_, staging, &requirements);
	const uint32_t host_type =
		dawn::vk_memory_type(this->phys_, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			nullptr, nullptr);
	if (host_type == UINT32_MAX) {
		vkDestroyBuffer(this->device_, staging, nullptr);
		return false;
	}

	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = host_type,
	};
	CALL_VK(AllocateMemory, " overlay tex staging", this->device_, &allocate,
		nullptr, &staging_memory);
	CALL_VK(BindBufferMemory, " overlay tex staging", this->device_, staging,
		staging_memory, 0);
	void *mapped = nullptr;
	CALL_VK(MapMemory, " overlay tex staging", this->device_, staging_memory, 0,
		size, 0, &mapped);
	for (size_t i = 0; i < uploads.size(); i++) {
		const AtlasUpload &upload = uploads[i];
		memcpy((uint8_t *) mapped + copies[i].bufferOffset, upload.pixels,
			size_t(upload.width) * upload.height * kOverlayBpp);
	}
	vkUnmapMemory(this->device_, staging_memory);

	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkCommandBufferAllocateInfo cmd_info{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = this->upload_pool_,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	CALL_VK(
		AllocateCommandBuffers, " overlay tex", this->device_, &cmd_info, &cmd);
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	CALL_VK(BeginCommandBuffer, " overlay tex", cmd, &begin);
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
	vkCmdCopyBufferToImage(cmd, staging, image,
		VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(copies.size()),
		copies.data());
	VkImageMemoryBarrier to_shader = to_dst;
	to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&to_shader);
	CALL_VK(EndCommandBuffer, " overlay tex", cmd);
	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	CALL_VK(
		QueueSubmit, " overlay tex", this->queue_, 1, &submit, VK_NULL_HANDLE);
	CALL_VK(QueueWaitIdle, " overlay tex", this->queue_);
	vkFreeCommandBuffers(this->device_, this->upload_pool_, 1, &cmd);
	vkDestroyBuffer(this->device_, staging, nullptr);
	vkFreeMemory(this->device_, staging_memory, nullptr);
	return true;
}

bool
OverlayVulkan::upload_rgba16(span<const AtlasUpload> uploads, int width,
	int height, VkImage *image, VkDeviceMemory *memory, VkImageView *view,
	VkDescriptorSet set, VkComponentMapping swizzle) const
{
	VkImage fresh = VK_NULL_HANDLE;
	VkDeviceMemory fresh_memory = VK_NULL_HANDLE;
	VkImageView fresh_view = VK_NULL_HANDLE;
	if (!create_sampled(width, height, &fresh, &fresh_memory))
		return false;
	if (!copy_rgba16(
			uploads, width, height, fresh, VK_IMAGE_LAYOUT_UNDEFINED)) {
		destroy_sampled(&fresh, &fresh_memory, &fresh_view);
		return false;
	}
	// The upload's queue wait also finishes draws using the old atlas.
	destroy_sampled(image, memory, view);
	*image = fresh;
	*memory = fresh_memory;
	bind_sampled(*image, view, set, swizzle);
	return true;
}

bool
OverlayVulkan::upload_thumb(const uint16_t *pixels, int width, int height,
	int dst_x, int dst_y, int atlas_side)
{
	const AtlasUpload upload{pixels, width, height, dst_x, dst_y};
	if (!this->thumb_image_)
		return rebuild_thumbs({&upload, 1}, atlas_side);
	// Changing atlas dimensions requires replacing all entries together.
	return atlas_side == this->thumb_side_ &&
		copy_rgba16({&upload, 1}, atlas_side, atlas_side, this->thumb_image_,
			VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

bool
OverlayVulkan::rebuild_thumbs(span<const AtlasUpload> uploads, int atlas_side)
{
	const VkComponentMapping bgra{VK_COMPONENT_SWIZZLE_B,
		VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_A};
	if (!upload_rgba16(uploads, atlas_side, atlas_side, &this->thumb_image_,
			&this->thumb_memory_, &this->thumb_view_,
			this->descriptor_sets_[kOverlayTexThumbs], bgra))
		return false;
	this->thumb_side_ = atlas_side;
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
	const AtlasUpload upload{(const uint16_t *) pixels, width, height, 0, 0};
	return upload_rgba16({&upload, 1}, width, height, &this->font_image_,
		&this->font_memory_, &this->font_view_,
		this->descriptor_sets_[kOverlayTexFont], {});
}

void
OverlayVulkan::destroy_thumbs()
{
	destroy_sampled(
		&this->thumb_image_, &this->thumb_memory_, &this->thumb_view_);
	this->thumb_side_ = 0;
}

bool
OverlayVulkan::ensure_buffer(VkDeviceSize bytes)
{
	if (this->quad_size_ >= bytes)
		return true;
	destroy_buffer();
	const VkDeviceSize capacity = bytes + bytes / 2;
	VkBufferCreateInfo info{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = capacity,
		.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
	};
	CALL_VK(CreateBuffer, " overlay", this->device_, &info, nullptr,
		&this->quad_buffer_);
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(
		this->device_, this->quad_buffer_, &requirements);
	const uint32_t type =
		dawn::vk_memory_type(this->phys_, requirements.memoryTypeBits,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			nullptr, nullptr);
	if (type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo allocate{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = requirements.size,
		.memoryTypeIndex = type,
	};
	CALL_VK(AllocateMemory, " overlay", this->device_, &allocate, nullptr,
		&this->quad_memory_);
	CALL_VK(BindBufferMemory, " overlay", this->device_, this->quad_buffer_,
		this->quad_memory_, 0);
	this->quad_size_ = capacity;
	return true;
}

void
OverlayVulkan::record(VkCommandBuffer cmd, const OverlayMesh &mesh)
{
	if (!cmd || !this->framebuffer_ || !this->pipeline_ || !this->font_view_)
		return;
	if (mesh.quads.empty() || mesh.cmds.empty() || this->extent_.width == 0 ||
		mesh.display_w <= 0.f || mesh.display_h <= 0.f)
		return;

	const VkDeviceSize bytes =
		VkDeviceSize(mesh.quads.size()) * sizeof(OverlayQuad);
	if (!ensure_buffer(bytes))
		return;

	void *mapped = nullptr;
	CALL_VK(MapMemory, " overlay", this->device_, this->quad_memory_, 0, bytes,
		0, &mapped);
	memcpy(mapped, mesh.quads.data(), size_t(bytes));
	vkUnmapMemory(this->device_, this->quad_memory_);

	begin_render_pass(
		cmd, this->render_pass_, this->framebuffer_, this->extent_);
	VkDeviceSize offset = 0;
	vkCmdBindVertexBuffers(cmd, 0, 1, &this->quad_buffer_, &offset);

	PushConstant push{};
	push.scale[0] = 2.f / mesh.display_w;
	push.scale[1] = 2.f / mesh.display_h;
	push.translate[0] = -1.f;
	push.translate[1] = -1.f;

	uint32_t bound_tex = ~0u;
	VkPipeline bound_pipeline = VK_NULL_HANDLE;
	for (const OverlayCmd &draw_cmd : mesh.cmds) {
		if (draw_cmd.quad_count == 0)
			continue;
		if (draw_cmd.tex == kOverlayTexThumbs && !this->thumb_view_)
			continue;
		if (draw_cmd.tex > kOverlayTexThumbs ||
			(draw_cmd.tex == kOverlayTexFont && !this->font_view_))
			continue;
		const VkPipeline pipeline = draw_cmd.tex == kOverlayTexThumbs
			? this->thumb_pipeline_
			: this->pipeline_;
		if (!pipeline)
			continue;
		if (pipeline != bound_pipeline) {
			vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
			bound_pipeline = pipeline;
		}
		if (draw_cmd.tex != bound_tex) {
			vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
				this->pipeline_layout_, 0, 1,
				&this->descriptor_sets_[draw_cmd.tex], 0, nullptr);
			bound_tex = draw_cmd.tex;
		}
		push.odd = draw_cmd.background.odd;
		push.even = draw_cmd.background.even;
		push.origin[0] = draw_cmd.background.origin_x;
		push.origin[1] = draw_cmd.background.origin_y;
		push.checker_size = max(1.f, draw_cmd.background.size);
		vkCmdPushConstants(cmd, this->pipeline_layout_,
			VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
			sizeof push, &push);
		const Box &clip = draw_cmd.clip;
		const int x0 = max(0, clip.x0), y0 = max(0, clip.y0);
		const int x1 = min(int(this->extent_.width), clip.x1);
		const int y1 = min(int(this->extent_.height), clip.y1);
		if (x1 <= x0 || y1 <= y0)
			continue;
		VkRect2D scissor{
			.offset = {x0, y0},
			.extent = {uint32_t(x1 - x0), uint32_t(y1 - y0)},
		};
		vkCmdSetScissor(cmd, 0, 1, &scissor);
		vkCmdDraw(cmd, 6, draw_cmd.quad_count, 0, draw_cmd.quad_offset);
	}
	vkCmdEndRenderPass(cmd);
}

void
OverlayVulkan::destroy_target()
{
	if (this->framebuffer_)
		vkDestroyFramebuffer(this->device_, this->framebuffer_, nullptr);
	this->framebuffer_ = VK_NULL_HANDLE;
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
OverlayVulkan::destroy_buffer()
{
	if (!this->device_)
		return;
	if (this->quad_buffer_)
		vkDestroyBuffer(this->device_, this->quad_buffer_, nullptr);
	if (this->quad_memory_)
		vkFreeMemory(this->device_, this->quad_memory_, nullptr);
	this->quad_buffer_ = VK_NULL_HANDLE;
	this->quad_memory_ = VK_NULL_HANDLE;
	this->quad_size_ = 0;
}

void
OverlayVulkan::destroy_pipeline()
{
	if (!this->device_)
		return;
	if (this->pipeline_)
		vkDestroyPipeline(this->device_, this->pipeline_, nullptr);
	if (this->thumb_pipeline_)
		vkDestroyPipeline(this->device_, this->thumb_pipeline_, nullptr);
	if (this->pipeline_layout_)
		vkDestroyPipelineLayout(this->device_, this->pipeline_layout_, nullptr);
	this->pipeline_ = VK_NULL_HANDLE;
	this->thumb_pipeline_ = VK_NULL_HANDLE;
	this->pipeline_layout_ = VK_NULL_HANDLE;
}

void
OverlayVulkan::destroy()
{
	if (!this->device_)
		return;
	vkDeviceWaitIdle(this->device_);
	destroy_target();
	destroy_buffer();
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
	this->extent_ = {};
}

}  // namespace dn
