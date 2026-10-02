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
#include <span>
#include <string>
#include <vector>

class QWindow;

namespace dn
{

/// How a frame is presented: encoded for the display profile, or extended
/// range in the platform's linear space, without or with luminance above
/// SDR white.
enum class Presentation : uint8_t { Encoded, Sdr, Hdr };

/// What the window finds out about extended presentation.
struct PresentationTarget {
	/// Whether the window can present extended at all.
	bool capable = false;
	/// Whether capability alone decides, rather than a drawn HDR page.
	bool always = false;
	/// From display linear RGB into the platform's, and its SDR white.
	dawn::RgbMatrix matrix = {{{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}};
	float white = 1;
	/// Settles what a frame gets, as the platform may only have a lesser
	/// presentation ready, and replaces `white` for it.  Null grants all.
	std::function<Presentation(Presentation wanted, float *white)> latch;
};

/// The swapchain format for either presentation.  Encoded never takes a
/// float format; extended falls back to encoded without its own.
VkSurfaceFormatKHR pick_surface_format(
	const std::vector<VkSurfaceFormatKHR> &formats, bool extended);

struct AtlasUpload {
	const uint16_t *pixels = nullptr;
	int width = 0;
	int height = 0;
	int x = 0;
	int y = 0;
	// Bytes per source row; zero means tightly packed. Regions must not
	// overlap.
	size_t stride = 0;
};

// The overlay half of the renderer: it owns the font and thumbnail atlases,
// and turns an OverlayMesh into draws on the linear composition image.
class OverlayVulkan
{
	void destroy_font();
	void destroy_thumbs();
	void destroy_sampled(
		VkImage *image, VkDeviceMemory *memory, VkImageView *view) const;
	// A host-visible, coherent buffer that grows by half again.
	struct HostBuffer {
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkDeviceSize size = 0;
	};
	void destroy_buffer(HostBuffer &buffer);
	void destroy_pipeline();
	void create_pipeline(VkRenderPass render_pass);
	void ensure_buffer(
		HostBuffer &buffer, VkDeviceSize bytes, VkBufferUsageFlags usage);
	struct UploadBatch {
		std::vector<uint8_t> pixels;
		std::vector<VkBufferImageCopy> copies;
	};
	struct PendingAtlas {
		std::vector<UploadBatch> batches;
		bool replace = false;
	};
	bool queue_rgba16(std::span<const AtlasUpload> uploads, int width,
		int height, PendingAtlas &pending, bool replace);
	void record_atlas(VkCommandBuffer cmd, PendingAtlas &pending, int width,
		int height, VkImage *image, VkDeviceMemory *memory, VkImageView *view,
		VkDescriptorSet set, VkComponentMapping swizzle, VkDeviceSize *offset);
	bool create_sampled(
		int width, int height, VkImage *image, VkDeviceMemory *memory) const;
	void bind_sampled(VkImage image, VkImageView *view, VkDescriptorSet set,
		VkComponentMapping swizzle) const;
	void compute_thumb_atlas_max();

	VkPhysicalDevice phys_ = VK_NULL_HANDLE;
	VkDevice device_ = VK_NULL_HANDLE;

	VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
	VkPipeline pipeline_ = VK_NULL_HANDLE;
	VkPipeline thumb_pipeline_ = VK_NULL_HANDLE;
	VkSampler sampler_ = VK_NULL_HANDLE;
	VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
	VkDescriptorSet descriptor_sets_[2]{};

	VkImage font_image_ = VK_NULL_HANDLE;
	VkDeviceMemory font_memory_ = VK_NULL_HANDLE;
	VkImageView font_view_ = VK_NULL_HANDLE;
	int font_width_ = 0;
	int font_height_ = 0;

	VkImage thumb_image_ = VK_NULL_HANDLE;
	VkDeviceMemory thumb_memory_ = VK_NULL_HANDLE;
	VkImageView thumb_view_ = VK_NULL_HANDLE;
	int thumb_side_ = 0;

	HostBuffer quad_;

	PendingAtlas pending_font_, pending_thumbs_;
	HostBuffer staging_;

public:
	/// The largest thumbnail atlas side the device takes, set by init().
	int thumb_atlas_max = 2048;

	OverlayVulkan() = default;
	~OverlayVulkan() { destroy(); }

	OverlayVulkan(const OverlayVulkan &) = delete;
	OverlayVulkan &operator=(const OverlayVulkan &) = delete;

	bool init(VkPhysicalDevice phys, VkDevice device, VkRenderPass render_pass);
	/// Rebuilds the pipelines for another composition pass, keeping atlases.
	void set_render_pass(VkRenderPass render_pass);
	void set_encoding_buffer(VkDescriptorBufferInfo info);
	// Upload requests copy borrowed pixels immediately; GPU work is deferred.
	bool upload_font(
		const uint16_t *pixels, int width, int height, Sheet::Packed dirty);
	bool upload_thumbs(std::span<const AtlasUpload> uploads, int atlas_side);
	bool rebuild_thumbs(std::span<const AtlasUpload> uploads, int atlas_side);
	void reset_thumbs();
	// After the previous frame fence, outside render passes. The caller must
	// submit this command buffer and finish it before recording again.
	void record_uploads(VkCommandBuffer cmd);
	// Draw inside the caller's composition pass.
	void record(
		VkCommandBuffer cmd, const OverlayMesh &mesh, VkExtent2D extent);
	void destroy();
};

class Renderer
{
	void destroy_swapchain();
	void create_swapchain();
	void ensure_engine();
	void wait_idle() const;
	void destroy_compose();
	void create_compose();
	VkRect2D begin_composition(VkCommandBuffer cmd) const;
	void destroy_presentation_pipeline();
	void create_presentation_pipeline();
	void destroy_presentation();
	void create_presentation();
	void record_presentation(
		VkCommandBuffer cmd, VkFramebuffer dest, float white) const;
	[[nodiscard]] bool dithering() const;
	[[nodiscard]] VkFormat compose_format() const;

	QWindow *window_ = nullptr;               // borrowed, for macOS tagging
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;   // borrowed from QWindow
	VkPhysicalDevice phys_ = VK_NULL_HANDLE;  // borrowed from GpuContext
	VkDevice device_ = VK_NULL_HANDLE;        // borrowed from GpuContext
	VkQueue queue_ = VK_NULL_HANDLE;          // borrowed from GpuContext
	uint32_t queue_family_ = 0;

	VkFormat format_ = VK_FORMAT_B8G8R8A8_UNORM;
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

public:
	// The kit fills its atlases; setting it up and recording stay here.
	OverlayVulkan overlay;

private:
	std::shared_ptr<const dawn::ProfileEncoding> encoding_;
	VkCompositeAlphaFlagBitsKHR composite_alpha_ =
		VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	float well_[4] = {0xE8 / 255.f, 0xE8 / 255.f, 0xE8 / 255.f, 1.f};
	float checker_[3] = {0xF0 / 255.f, 0xF0 / 255.f, 0xF0 / 255.f};
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
	bool extended_ = false;
	std::function<void()> present_about_to_queue_;
	std::function<void()> present_queued_;

public:
	// Updated by the active image view. Output encoding and background colours
	// belong to the renderer; checker_size is in device pixels.
	dawn::ScaleView view;
	dawn::Filter preferred_filter = dawn::Filter::Expensive;
	/// Its white is SDR white for extended frames, where no latch gives it:
	/// Windows's follows a slider that no notification reports.
	PresentationTarget presentation;
	bool prefer_premultiplied = false;
	bool dither_enabled = true;
	uint32_t dest_inset = 0;

	/// As the swapchain has it.
	VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
	/// What the last presented frame got.
	Presentation presented = Presentation::Encoded;
	bool needs_resize = false;

	Renderer() = default;
	~Renderer() { destroy(); }

	Renderer(const Renderer &) = delete;
	Renderer &operator=(const Renderer &) = delete;

	bool init(const GpuContext &gpu, QWindow *window, VkSurfaceKHR surface,
		Extent pixel, VkPresentModeKHR preferred_present_mode,
		std::function<void()> present_about_to_queue,
		std::function<void()> present_queued);
	void set_image(uint32_t w, uint32_t h, const uint8_t *pixels,
		size_t stride, const dawn::GainMap *map);
	void clear_image();
	void set_well_colour(float r, float g, float b);
	void set_checker_colour(float r, float g, float b);
	void set_encoding(std::shared_ptr<const dawn::ProfileEncoding> encoding);
	/// Whether the surface has the format extended presentation needs.
	/// Asked anew each time, as drivers may change it with the display mode.
	[[nodiscard]] bool offers_extended() const;
	/// Format, colour space and dithering, as the swapchain has them.
	[[nodiscard]] std::string swapchain_summary() const;
	void resize(Extent pixel);
	// False means no swapchain image was immediately available.
	bool draw_frame(const OverlayMesh &mesh, bool show_image);
	void destroy();

	[[nodiscard]] Extent extent() const
	{
		return {this->extent_.width, this->extent_.height};
	}
};

}  // namespace dn
