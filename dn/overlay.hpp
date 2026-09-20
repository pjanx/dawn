//
// overlay.hpp: overlay draw lists and the CPU shelf atlas behind them
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdint>
#include <vector>

namespace dn
{

struct Colour {
	float r = 0;
	float g = 0;
	float b = 0;
	float a = 1;
	bool operator==(const Colour &) const = default;
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

// Texture coordinates in atlas texels.
struct Uv {
	float u0 = 0;
	float v0 = 0;
	float u1 = 0;
	float v1 = 0;
};

// One instance per rectangle; colours are linear and premultiplied.
struct OverlayQuad {
	Box box{};
	Uv uv{};
	Colour top{};
	Colour bottom{};
};
static_assert(sizeof(OverlayQuad) == 64);

constexpr uint32_t kOverlayTexFont = 0;
constexpr uint32_t kOverlayTexThumbs = 1;

// Linear, opaque checker colours; size and origin are framebuffer pixels.
struct ThumbBackground {
	Colour odd{};
	Colour even{};
	float origin_x = 0;
	float origin_y = 0;
	float size = 1;
	bool operator==(const ThumbBackground &) const = default;
};

struct OverlayCmd {
	uint32_t quad_offset = 0;
	uint32_t quad_count = 0;
	Box clip{};
	uint32_t tex = kOverlayTexFont;
	ThumbBackground background{};
};

struct OverlayMesh {
	std::vector<OverlayQuad> quads;
	std::vector<OverlayCmd> cmds;
	float display_w = 0;
	float display_h = 0;
};

class OverlayList
{
	OverlayMesh mesh_;
	OverlayCmd cmd_{};
	std::vector<Box> clip_stack_;
	Uv white_{};
	uint32_t tex_ = kOverlayTexFont;
	ThumbBackground background_{};

	void sync_clip();
	void add_quad(Box b, Uv uv, Colour top, Colour bottom);

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
	void add_thumb(Box b, Uv uv, Colour col, const ThumbBackground &background);

	[[nodiscard]] const OverlayMesh &mesh() const { return this->mesh_; }
};

// Square shelf packer. Font atlas keeps a uint16x4 CPU shadow (RGBA coverage);
// the thumb atlas is logical-only — pixels live on the GPU. Base side 2048;
// thumbs grow by doubling up to the device cap.
struct Sheet {
	struct Packed {
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
		[[nodiscard]] Uv texels() const;
		[[nodiscard]] bool empty() const
		{
			return this->w <= 0 || this->h <= 0;
		}
	};

	struct Shelf {
		int y = 0;
		int h = 0;
		int x = 0;
		std::vector<Packed> free;
	};

	static constexpr int kSize = 2048;

	int w = 0;
	int h = 0;
	std::vector<uint16_t> pixels;  // 4 channels / pixel; empty if !keep_pixels_
	bool dirty = false;
	bool keep_pixels_ = true;
	std::vector<Shelf> shelves_;

	Sheet() = default;
	explicit Sheet(int side, bool keep_pixels);

	void clear();
	void grow(int side);
	Packed alloc(int tw, int th);
	void release(Packed slot);
	// src is 4x uint16. stride is bytes/row; 0 means tightly packed
	// (src_w * 8). No-op if this sheet has no CPU shadow.
	void blit(
		Packed slot, const uint16_t *src, int src_w, int src_h, int stride);
	[[nodiscard]] bool take_dirty();
};

}  // namespace dn
