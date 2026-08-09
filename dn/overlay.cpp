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
	if (this->cmd_.idx_count > 0 &&
		(!(this->cmd_.clip == clip) || this->cmd_.tex != this->tex_)) {
		this->mesh_.cmds.push_back(this->cmd_);
		this->cmd_.idx_count = 0;
	}
	if (this->cmd_.idx_count == 0)
		this->cmd_.idx_offset = uint32_t(this->mesh_.indices.size());
	this->cmd_.clip = clip;
	this->cmd_.tex = this->tex_;
}

void
OverlayList::begin(int width_px, int height_px, Uv white)
{
	this->mesh_.vertices.clear();
	this->mesh_.indices.clear();
	this->mesh_.cmds.clear();
	this->mesh_.display_w = float(width_px);
	this->mesh_.display_h = float(height_px);
	this->white_ = white;
	this->tex_ = kOverlayTexFont;
	this->clip_stack_.clear();
	this->clip_stack_.push_back({0, 0, width_px, height_px});
	this->cmd_ = {};
	sync_clip();
}

void
OverlayList::end()
{
	if (this->cmd_.idx_count > 0)
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
OverlayList::add_quad(
	Box b, Uv uv, Colour c00, Colour c10, Colour c11, Colour c01)
{
	sync_clip();
	const float x0 = float(b.x0), y0 = float(b.y0);
	const float x1 = float(b.x1), y1 = float(b.y1);
	const uint32_t i = uint32_t(this->mesh_.vertices.size());
	this->mesh_.vertices.push_back({x0, y0, uv.u0, uv.v0, premul(c00)});
	this->mesh_.vertices.push_back({x1, y0, uv.u1, uv.v0, premul(c10)});
	this->mesh_.vertices.push_back({x1, y1, uv.u1, uv.v1, premul(c11)});
	this->mesh_.vertices.push_back({x0, y1, uv.u0, uv.v1, premul(c01)});
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 1);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i);
	this->mesh_.indices.push_back(i + 2);
	this->mesh_.indices.push_back(i + 3);
	this->cmd_.idx_count += 6;
}

void
OverlayList::add_rect_filled(Box b, Colour col)
{
	this->tex_ = kOverlayTexFont;
	add_quad(b, this->white_, col, col, col, col);
}

void
OverlayList::add_rect_filled_vgradient(Box b, Colour top, Colour bottom)
{
	this->tex_ = kOverlayTexFont;
	add_quad(b, this->white_, top, top, bottom, bottom);
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
	add_quad(b, uv, col, col, col, col);
}

void
OverlayList::add_thumb(Box b, Uv uv, int transfer, Colour col)
{
	this->tex_ = kOverlayTexThumbs;
	add_quad(b, uv, col, col, col, col);
	const size_t first = this->mesh_.vertices.size() - 4;
	for (size_t i = first; i < this->mesh_.vertices.size(); ++i) {
		OverlayVertex &vertex = this->mesh_.vertices[i];
		vertex.atlas_x0 = uv.u0;
		vertex.atlas_y0 = uv.v0;
		vertex.atlas_x1 = uv.u1;
		vertex.atlas_y1 = uv.v1;
		vertex.dest_w = float(abs(b.x1 - b.x0));
		vertex.dest_h = float(abs(b.y1 - b.y0));
		vertex.transfer = float(transfer);
	}
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
	for (size_t i = 1; i < free.size(); ++i) {
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
	this->dirty = false;
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
			for (int y = 0; y < this->h; ++y) {
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
	this->dirty = true;
}

Sheet::Packed
Sheet::alloc(int tw, int th)
{
	if (tw <= 0 || th <= 0 || tw > this->w || th > this->h)
		return {};

	for (Shelf &shelf : this->shelves_) {
		if (shelf.h != th)
			continue;
		for (size_t i = 0; i < shelf.free.size(); ++i) {
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
	for (int y = 0; y < rows; ++y) {
		uint16_t *dst = this->pixels.data() +
			(size_t(slot.y + y) * size_t(this->w) + size_t(slot.x)) * 4;
		const auto *row = (const uint16_t *) ((const uint8_t *) src +
			size_t(y) * size_t(stride));
		memcpy(dst, row, size_t(cols) * 4 * sizeof(uint16_t));
	}
	this->dirty = true;
}

// TODO(p): These are normalised against the sheet as it is right now, and go
// straight into the draw list -- but glyphs are packed lazily, so a grow()
// during a paint leaves every quad emitted earlier in that frame sampling at
// the old scale.  Emitting texels and dividing in the shader (or at
// OverlayList::end()) would make the normalisation happen once, after the
// sheet has settled.  Reachable today through paint_tooltip(), which is the
// one emitter with no caching pass ahead of it.
Uv
Sheet::uv(const Packed &slot) const
{
	const float aw = float(max(this->w, 1));
	const float ah = float(max(this->h, 1));
	return {float(slot.x) / aw, float(slot.y) / ah, float(slot.x + slot.w) / aw,
		float(slot.y + slot.h) / ah};
}

bool
Sheet::take_dirty()
{
	const bool d = this->dirty;
	this->dirty = false;
	return d;
}

}  // namespace dn
