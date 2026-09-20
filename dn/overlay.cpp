//
// overlay.cpp: overlay draw lists and the CPU shelf atlas behind them
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "overlay.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <utility>

using namespace std;

namespace dn
{

static Colour
premul(Colour c)
{
	return {c.r * c.a, c.g * c.a, c.b * c.a, c.a};
}

// --- OverlayList -------------------------------------------------------------

void
OverlayList::sync_clip()
{
	const Box &clip = this->clip_stack_.back();
	if (this->cmd_.quad_count > 0 &&
		(!(this->cmd_.clip == clip) || this->cmd_.tex != this->tex_ ||
			(this->tex_ == kOverlayTexThumbs &&
				this->cmd_.background != this->background_))) {
		this->mesh_.cmds.push_back(this->cmd_);
		this->cmd_.quad_count = 0;
	}
	if (this->cmd_.quad_count == 0)
		this->cmd_.quad_offset = uint32_t(this->mesh_.quads.size());
	this->cmd_.clip = clip;
	this->cmd_.tex = this->tex_;
	this->cmd_.background = this->background_;
}

void
OverlayList::begin(int width_px, int height_px, Uv white)
{
	this->mesh_.quads.clear();
	this->mesh_.cmds.clear();
	this->mesh_.display_w = float(width_px);
	this->mesh_.display_h = float(height_px);
	this->white_ = white;
	this->tex_ = kOverlayTexFont;
	this->background_ = {};
	this->clip_stack_.clear();
	this->clip_stack_.push_back({0, 0, width_px, height_px});
	this->cmd_ = {};
	sync_clip();
}

void
OverlayList::end()
{
	if (this->cmd_.quad_count > 0)
		this->mesh_.cmds.push_back(this->cmd_);
	this->cmd_ = {};
}

void
OverlayList::push_clip(Box b)
{
	const Box &prev = this->clip_stack_.back();
	Box next{max(prev.x0, b.x0), max(prev.y0, b.y0), min(prev.x1, b.x1),
		min(prev.y1, b.y1)};
	// An intersection that came out inverted is empty, not mirrored.
	next.x1 = max(next.x0, next.x1);
	next.y1 = max(next.y0, next.y1);
	this->clip_stack_.push_back(next);
	sync_clip();
}

void
OverlayList::pop_clip()
{
	if (this->clip_stack_.size() <= 1)
		return;
	this->clip_stack_.pop_back();
	sync_clip();
}

// The one funnel for geometry: everything else here ends up in this quad.
void
OverlayList::add_quad(Box b, Uv uv, Colour top, Colour bottom)
{
	sync_clip();
	this->mesh_.quads.push_back({b, uv, premul(top), premul(bottom)});
	this->cmd_.quad_count++;
}

void
OverlayList::add_rect_filled(Box b, Colour col)
{
	this->tex_ = kOverlayTexFont;
	add_quad(b, this->white_, col, col);
}

void
OverlayList::add_rect_filled_vgradient(Box b, Colour top, Colour bottom)
{
	this->tex_ = kOverlayTexFont;
	add_quad(b, this->white_, top, bottom);
}

void
OverlayList::add_rect_stroke(Box b, Colour col, int thickness)
{
	if (thickness <= 0)
		return;

	if (b.x1 < b.x0)
		swap(b.x0, b.x1);
	if (b.y1 < b.y0)
		swap(b.y0, b.y1);

	// The four bands share the corners, rather than meeting at butt caps
	// that would leave the bottom right notched.
	const int th = thickness;
	if (b.x1 - b.x0 <= 2 * th || b.y1 - b.y0 <= 2 * th) {
		add_rect_filled(b, col);
		return;
	}
	add_rect_filled({b.x0, b.y0, b.x1, b.y0 + th}, col);
	add_rect_filled({b.x0, b.y1 - th, b.x1, b.y1}, col);
	add_rect_filled({b.x0, b.y0 + th, b.x0 + th, b.y1 - th}, col);
	add_rect_filled({b.x1 - th, b.y0 + th, b.x1, b.y1 - th}, col);
}

void
OverlayList::add_image(Box b, Uv uv, Colour col)
{
	this->tex_ = kOverlayTexFont;
	add_quad(b, uv, col, col);
}

void
OverlayList::add_glyph(Box b, Uv uv, Colour col)
{
	add_image(b, uv, col);
#if !defined _WIN32
	// Native masks supply raw coverage. Boost dark text more than light text
	// to compensate for thin strokes in linear light without bright halos.
	// Use the straight colour so fading text does not change its weight.
	const float luminance = .2126f * col.r + .7152f * col.g + .0722f * col.b;
	this->mesh_.quads.back().contrast =
		1.f * (1.f - clamp(luminance, 0.f, 1.f));
#endif
}

void
OverlayList::add_thumb(
	Box b, Uv uv, Colour col, const ThumbBackground &background)
{
	this->tex_ = kOverlayTexThumbs;
	this->background_ = background;
	add_quad(b, uv, col, col);
}

// --- Sheet -------------------------------------------------------------------

static void
merge_free(vector<Sheet::Packed> &free)
{
	if (free.size() < 2)
		return;
	sort(free.begin(), free.end(),
		[](const Sheet::Packed &a, const Sheet::Packed &b) {
			return a.x < b.x;
		});
	vector<Sheet::Packed> out;
	out.push_back(free.front());
	for (size_t i = 1; i < free.size(); i++) {
		Sheet::Packed &last = out.back();
		if (last.x + last.w == free[i].x)
			last.w += free[i].w;
		else
			out.push_back(free[i]);
	}
	free.swap(out);
}

// --- Sheet -------------------------------------------------------------------

Sheet::Sheet(int side, bool keep_pixels) : keep_pixels_(keep_pixels)
{
	if (side > 0)
		grow(side);
}

void
Sheet::clear()
{
	this->w = 0;
	this->h = 0;
	this->pixels.clear();
	this->shelves_.clear();
	this->dirty = {};
}

void
Sheet::grow(int side)
{
	if (side < 1)
		return;
	const int nw = max(side, this->w);
	const int nh = max(side, this->h);
	if (this->keep_pixels_) {
		if (nw == this->w && nh == this->h && !this->pixels.empty())
			return;
		vector<uint16_t> next(size_t(nw) * size_t(nh) * 4, 0);
		if (this->w > 0 && this->h > 0 && !this->pixels.empty()) {
			for (int y = 0; y < this->h; y++) {
				memcpy(next.data() + size_t(y) * size_t(nw) * 4,
					this->pixels.data() + size_t(y) * size_t(this->w) * 4,
					size_t(this->w) * 8);
			}
		}
		this->pixels.swap(next);
	} else if (nw == this->w && nh == this->h) {
		return;
	}
	this->w = nw;
	this->h = nh;
	mark_dirty({0, 0, nw, nh});
}

Sheet::Packed
Sheet::alloc(int tw, int th)
{
	if (tw <= 0 || th <= 0 || tw > this->w || th > this->h)
		return {};

	for (Shelf &shelf : this->shelves_) {
		if (shelf.h != th)
			continue;
		for (size_t i = 0; i < shelf.free.size(); i++) {
			Packed &span = shelf.free[i];
			if (span.w < tw)
				continue;
			Packed slot{span.x, shelf.y, tw, th};
			if (span.w > tw) {
				span.x += tw;
				span.w -= tw;
			} else {
				shelf.free.erase(shelf.free.begin() + long(i));
			}
			return slot;
		}
		if (shelf.x + tw <= this->w) {
			Packed slot{shelf.x, shelf.y, tw, th};
			shelf.x += tw;
			return slot;
		}
	}

	int y = 0;
	if (!this->shelves_.empty()) {
		const Shelf &last = this->shelves_.back();
		y = last.y + last.h;
	}
	if (y + th > this->h)
		return {};
	Shelf shelf;
	shelf.y = y;
	shelf.h = th;
	shelf.x = tw;
	this->shelves_.push_back(std::move(shelf));
	return {0, y, tw, th};
}

void
Sheet::release(Packed slot)
{
	if (slot.w <= 0 || slot.h <= 0)
		return;
	for (Shelf &shelf : this->shelves_) {
		if (shelf.y != slot.y || shelf.h != slot.h)
			continue;
		shelf.free.push_back({slot.x, slot.y, slot.w, slot.h});
		merge_free(shelf.free);
		return;
	}
}

void
Sheet::blit(Packed slot, const uint16_t *src, int src_w, int src_h, int stride)
{
	if (!this->keep_pixels_ || this->pixels.empty())
		return;
	if (!src || slot.w <= 0 || slot.h <= 0)
		return;
	if (slot.x < 0 || slot.y < 0 || slot.x + slot.w > this->w ||
		slot.y + slot.h > this->h)
		return;
	if (stride <= 0)
		stride = src_w * int(sizeof(uint16_t) * 4);
	const int cols = min(slot.w, src_w);
	const int rows = min(slot.h, src_h);
	for (int y = 0; y < rows; y++) {
		uint16_t *dst = this->pixels.data() +
			(size_t(slot.y + y) * size_t(this->w) + size_t(slot.x)) * 4;
		const auto *row = (const uint8_t *) src + size_t(y) * size_t(stride);
		memcpy(dst, row, size_t(cols) * 4 * sizeof(uint16_t));
	}
	mark_dirty({slot.x, slot.y, cols, rows});
}

Uv
Sheet::Packed::texels() const
{
	return {float(this->x), float(this->y), float(this->x + this->w),
		float(this->y + this->h)};
}

void
Sheet::mark_dirty(Packed slot)
{
	if (slot.empty() || !this->keep_pixels_)
		return;
	if (this->dirty.empty()) {
		this->dirty = slot;
		return;
	}
	const int x = min(this->dirty.x, slot.x);
	const int y = min(this->dirty.y, slot.y);
	this->dirty = {x, y,
		max(this->dirty.x + this->dirty.w, slot.x + slot.w) - x,
		max(this->dirty.y + this->dirty.h, slot.y + slot.h) - y};
}

}  // namespace dn
