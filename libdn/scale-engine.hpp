//
// scale-engine.hpp: shared H→V tile scale engine
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.hpp"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace dawn
{

enum class Filter : uint8_t { Nearest, Bilinear, Expensive };

/// Hardcoded until it is a user setting: CPU rasterizers (lavapipe,
/// SwiftShader) get Bilinear, everything else Expensive (bilinear
/// minify, NoHalo zoom).
Filter preferred_filter(VkPhysicalDevice phys);

enum class ScaleEncoding : uint8_t { Encoded, Linear };

struct ScaleView {
	float scale = 1.f;
	float pan_x = 0.f;
	float pan_y = 0.f;
	float angle = 0.f;
	Transfer transfer = Transfer::Srgb;
	/// Use set_encoding()'s actual per-channel curves instead of transfer.
	bool profile_curves = false;
	/// Output representation, independent of the processing policy.
	/// Non-composited output remains premultiplied in this space.
	ScaleEncoding output_encoding = ScaleEncoding::Encoded;
	Orientation orientation = Orientation::Rotate0;
	bool checkerboard = false;
	/// Filter and resolve alpha in encoded values rather than linear light,
	/// matching conventional application and platform image rendering.
	bool nonlinear_processing = false;
	/// Encoded even-tile grey (toolbar_bottom). Odd tiles use `record`'s
	/// clear colour (well). Converted to the selected compositing space there.
	float checker_r = 0xF0 / 255.f;
	float checker_g = 0xF0 / 255.f;
	float checker_b = 0xF0 / 255.f;
	/// One checkerboard square, in device pixels: the caller resolves its
	/// design size against the display it draws on.
	float checker_size = 20.f;
	/// Resolve alpha against `record`'s clear colour and write opaque pixels.
	/// Clear it to keep premultiplied alpha, as offscreen readback needs.
	bool composite = false;
	Filter filter = Filter::Bilinear;
};

/// Shared H→V tile scale engine. Does not own VkInstance/VkDevice/VkQueue.
class ScaleEngine
{
	struct Impl;
	Impl *impl_ = nullptr;

public:
	ScaleEngine();
	~ScaleEngine();

	ScaleEngine(const ScaleEngine &) = delete;
	ScaleEngine &operator=(const ScaleEngine &) = delete;

	/// `dest_final_layout` is `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` for swapchain
	/// targets, or `VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL` for offscreen
	/// readback.
	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
		uint32_t queue_family, VkFormat dest_format,
		VkImageLayout dest_final_layout, std::string *error);
	void destroy();

	/// Caller must finish outstanding draws before updating these shared
	/// curves.
	bool set_encoding(const ProfileEncoding &encoding, std::string *error);
	VkDescriptorBufferInfo encoding_buffer() const;

	bool set_image(uint32_t w, uint32_t h, const uint8_t *pixels, size_t stride,
		std::string *error);
	void clear_image();

	[[nodiscard]] uint32_t image_width() const;
	[[nodiscard]] uint32_t image_height() const;
	[[nodiscard]] bool has_image() const;

	/// Record intermediate scaling outside a render pass. Call draw() next
	/// with the same view and viewport, without changing the source image.
	bool prepare(VkCommandBuffer cmd, uint32_t viewport_w, uint32_t viewport_h,
		const ScaleView &view, std::string *error);
	/// Draw inside a pass compatible with dest_render_pass(), after prepare().
	/// Background RGB is encoded; the caller owns clearing and clipping.
	void draw(VkCommandBuffer cmd, uint32_t viewport_w, uint32_t viewport_h,
		const ScaleView &view, const float background[4], VkRect2D clip);

	/// Clear/background RGB is encoded. Use transparent black for readback
	/// that preserves image alpha; the destination uses source-over.
	bool record(VkCommandBuffer cmd, VkFramebuffer dest_fb, uint32_t viewport_w,
		uint32_t viewport_h, const ScaleView &view, const float clear_rgba[4],
		std::string *error);

	bool create_offscreen(uint32_t w, uint32_t h, VkImage *image,
		VkDeviceMemory *mem, VkImageView *view, VkFramebuffer *fb,
		std::string *error);
	void destroy_offscreen(VkImage *image, VkDeviceMemory *mem,
		VkImageView *view, VkFramebuffer *fb);

	[[nodiscard]] VkRenderPass dest_render_pass() const;
	[[nodiscard]] VkFormat dest_format() const;
};

}  // namespace dawn
