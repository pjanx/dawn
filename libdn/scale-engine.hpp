//
// scale-engine.hpp: shared H→V tile scale engine
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

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

struct ScaleView {
	float scale = 1.f;
	float pan_x = 0.f;
	float pan_y = 0.f;
	float angle = 0.f;
	Transfer transfer = Transfer::Srgb;
	Orientation orientation = Orientation::Rotate0;
	bool checkerboard = false;
	/// Encoded even-tile grey (toolbar_bottom). Odd tiles use `record`'s
	/// clear colour (well). Decoded on the CPU like `bg_*`.
	float checker_r = 0xF0 / 255.f;
	float checker_g = 0xF0 / 255.f;
	float checker_b = 0xF0 / 255.f;
	/// Resolve alpha against `record`'s clear colour in linear light and
	/// write opaque pixels. Correct (the fixed-function blend would
	/// composite in the dest encoding) and cheaper than re-associating.
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
		VkImageLayout dest_final_layout, std::string *error = nullptr);
	void destroy();

	bool set_image(uint32_t w, uint32_t h, const uint8_t *pixels, size_t stride,
		std::string *error = nullptr);
	void clear_image();

	[[nodiscard]] uint32_t image_width() const;
	[[nodiscard]] uint32_t image_height() const;
	[[nodiscard]] bool has_image() const;

	bool ensure_viewport(
		uint32_t viewport_w, uint32_t viewport_h, std::string *error = nullptr);
	void set_dest_inset(
		uint32_t left, uint32_t top, uint32_t right, uint32_t bottom);

	bool record(VkCommandBuffer cmd, VkFramebuffer dest_fb, uint32_t viewport_w,
		uint32_t viewport_h, const ScaleView &view, const float clear_rgba[4],
		std::string *error = nullptr);
	/// Dest-pass CLEAR only (no H/V). For presenting the well with no pixmap.
	bool record_clear(VkCommandBuffer cmd, VkFramebuffer dest_fb,
		uint32_t viewport_w, uint32_t viewport_h, const float clear_rgba[4],
		std::string *error = nullptr);

	bool create_offscreen(uint32_t w, uint32_t h, VkImage *image,
		VkDeviceMemory *mem, VkImageView *view, VkFramebuffer *fb,
		std::string *error = nullptr);
	void destroy_offscreen(VkImage *image, VkDeviceMemory *mem,
		VkImageView *view, VkFramebuffer *fb);

	[[nodiscard]] VkRenderPass dest_render_pass() const;
	[[nodiscard]] VkFormat dest_format() const;
};

}  // namespace dawn
