//
// sheet.hpp: CPU shelf atlas (overlay glyphs / photo thumbs)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdint>
#include <vector>

namespace dn
{

// Square shelf packer. Font atlas keeps a uint16x4 CPU shadow (RGBA coverage);
// the thumb atlas is logical-only — pixels live on the GPU. Base side 2048;
// thumbs grow by doubling up to the device cap.
struct Sheet {
	struct Packed {
		int x = 0;
		int y = 0;
		int w = 0;
		int h = 0;
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
	explicit Sheet(int side, bool keep_pixels = true);

	void clear();
	void grow(int side);
	Packed alloc(int tw, int th);
	void release(Packed slot);
	// src is 4x uint16. stride is bytes/row; 0 means tightly packed
	// (src_w * 8). No-op if this sheet has no CPU shadow.
	void blit(
		Packed slot, const uint16_t *src, int src_w, int src_h, int stride);
	void uv(
		const Packed &slot, float *u0, float *v0, float *u1, float *v1) const;
	[[nodiscard]] bool take_dirty();
};

}  // namespace dn
