//
// sheet.cpp: CPU shelf atlas (overlay glyphs / photo thumbs)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "sheet.hpp"

#include <algorithm>
#include <cstring>

using namespace std;

namespace dn
{
namespace
{

void
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

}  // namespace

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

void
Sheet::uv(const Packed &slot, float *u0, float *v0, float *u1, float *v1) const
{
	if (!u0 || !v0 || !u1 || !v1)
		return;
	const float aw = float(max(this->w, 1));
	const float ah = float(max(this->h, 1));
	*u0 = float(slot.x) / aw;
	*v0 = float(slot.y) / ah;
	*u1 = float(slot.x + slot.w) / aw;
	*v1 = float(slot.y + slot.h) / ah;
}

bool
Sheet::take_dirty()
{
	const bool d = this->dirty;
	this->dirty = false;
	return d;
}

}  // namespace dn
