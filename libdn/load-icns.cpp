//
// load-icns.cpp: Apple icon family loading
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dawn
{

static constexpr uint32_t
fourcc(char a, char b, char c, char d)
{
	return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 |
		uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

static uint32_t
be32(const uint8_t *p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
		uint32_t(p[3]);
}

static string
type_name(uint32_t type)
{
	string out(4, '?');
	for (int i = 0; i < 4; i++) {
		uint8_t c = uint8_t(type >> (24 - i * 8));
		out[i] = c >= 0x20 && c < 0x7F ? char(c) : '?';
	}
	return out;
}

struct Entry {
	uint32_t type;
	span<const uint8_t> data;
};

enum class Encoding {
	None,
	Mono,
	Palette4,
	Palette8,
	RGB24,
	ARGB,
};

struct IconInfo {
	Encoding encoding = Encoding::None;
	uint32_t width = 0, height = 0;
	uint32_t mask_type = 0;
	bool combined_mask = false;
};

static IconInfo
icon_info(uint32_t type)
{
	switch (type) {
	case fourcc('I', 'C', 'O', 'N'):
		return {Encoding::Mono, 32, 32};
	case fourcc('I', 'C', 'N', '#'):
		return {Encoding::Mono, 32, 32, 0, true};
	case fourcc('i', 'c', 'm', '#'):
		return {Encoding::Mono, 16, 12, 0, true};
	case fourcc('i', 'c', 's', '#'):
		return {Encoding::Mono, 16, 16, 0, true};
	case fourcc('i', 'c', 'h', '#'):
		return {Encoding::Mono, 48, 48, 0, true};
	case fourcc('i', 'c', 'm', '4'):
		return {Encoding::Palette4, 16, 12, fourcc('i', 'c', 'm', '#')};
	case fourcc('i', 'c', 's', '4'):
		return {Encoding::Palette4, 16, 16, fourcc('i', 'c', 's', '#')};
	case fourcc('i', 'c', 'l', '4'):
		return {Encoding::Palette4, 32, 32, fourcc('I', 'C', 'N', '#')};
	case fourcc('i', 'c', 'h', '4'):
		return {Encoding::Palette4, 48, 48, fourcc('i', 'c', 'h', '#')};
	case fourcc('i', 'c', 'm', '8'):
		return {Encoding::Palette8, 16, 12, fourcc('i', 'c', 'm', '#')};
	case fourcc('i', 'c', 's', '8'):
		return {Encoding::Palette8, 16, 16, fourcc('i', 'c', 's', '#')};
	case fourcc('i', 'c', 'l', '8'):
		return {Encoding::Palette8, 32, 32, fourcc('I', 'C', 'N', '#')};
	case fourcc('i', 'c', 'h', '8'):
		return {Encoding::Palette8, 48, 48, fourcc('i', 'c', 'h', '#')};
	case fourcc('i', 's', '3', '2'):
		return {Encoding::RGB24, 16, 16, fourcc('s', '8', 'm', 'k')};
	case fourcc('i', 'l', '3', '2'):
		return {Encoding::RGB24, 32, 32, fourcc('l', '8', 'm', 'k')};
	case fourcc('i', 'h', '3', '2'):
		return {Encoding::RGB24, 48, 48, fourcc('h', '8', 'm', 'k')};
	case fourcc('i', 't', '3', '2'):
		return {Encoding::RGB24, 128, 128, fourcc('t', '8', 'm', 'k')};
	case fourcc('i', 'c', '0', '4'):
		return {Encoding::ARGB, 16, 16};
	case fourcc('i', 'c', '0', '5'):
		return {Encoding::ARGB, 32, 32};
	case fourcc('i', 'c', 's', 'b'):
		return {Encoding::ARGB, 18, 18};
	case fourcc('i', 'c', 'p', '4'):
		return {Encoding::ARGB, 16, 16};
	case fourcc('i', 'c', 'p', '5'):
		return {Encoding::ARGB, 32, 32};
	case fourcc('i', 'c', 'p', '6'):
		return {Encoding::ARGB, 64, 64};
	case fourcc('i', 'c', '0', '7'):
		return {Encoding::ARGB, 128, 128};
	case fourcc('i', 'c', '0', '8'):
		return {Encoding::ARGB, 256, 256};
	case fourcc('i', 'c', '0', '9'):
		return {Encoding::ARGB, 512, 512};
	case fourcc('i', 'c', '1', '0'):
		return {Encoding::ARGB, 1024, 1024};
	case fourcc('i', 'c', '1', '1'):
		return {Encoding::ARGB, 32, 32};
	case fourcc('i', 'c', '1', '2'):
		return {Encoding::ARGB, 64, 64};
	case fourcc('i', 'c', '1', '3'):
		return {Encoding::ARGB, 256, 256};
	case fourcc('i', 'c', '1', '4'):
		return {Encoding::ARGB, 512, 512};
	case fourcc('i', 'c', 's', 'B'):
		return {Encoding::ARGB, 36, 36};
	case fourcc('s', 'b', '2', '4'):
		return {Encoding::ARGB, 24, 24};
	case fourcc('S', 'B', '2', '4'):
		return {Encoding::ARGB, 48, 48};
	default:
		return {};
	}
}

static bool
is_png(span<const uint8_t> data)
{
	static constexpr uint8_t signature[] = {
		0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
	return data.size() >= sizeof signature &&
		!memcmp(data.data(), signature, sizeof signature);
}

static bool
is_jpeg2000(span<const uint8_t> data)
{
	static constexpr uint8_t signature[] = {
		0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
	return (data.size() >= sizeof signature &&
			   !memcmp(data.data(), signature, sizeof signature)) ||
		(data.size() >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
			data[2] == 0xFF && data[3] == 0x51);
}

static const Entry *
find_entry(const vector<Entry> &entries, uint32_t type)
{
	auto it = find_if(entries.begin(), entries.end(),
		[type](const Entry &e) { return e.type == type; });
	return it == entries.end() ? nullptr : &*it;
}

struct Color {
	uint8_t r, g, b;
};

static Color
palette4(uint8_t index)
{
	static constexpr Color colors[] = {
		{0xFF, 0xFF, 0xFF},
		{0xFC, 0xF3, 0x05},
		{0xFF, 0x64, 0x02},
		{0xDD, 0x08, 0x06},
		{0xF2, 0x08, 0x84},
		{0x46, 0x00, 0xA5},
		{0x00, 0x00, 0xD4},
		{0x02, 0xAB, 0xEA},
		{0x1F, 0xB7, 0x14},
		{0x00, 0x64, 0x11},
		{0x56, 0x2C, 0x05},
		{0x90, 0x71, 0x3A},
		{0xC0, 0xC0, 0xC0},
		{0x80, 0x80, 0x80},
		{0x40, 0x40, 0x40},
		{0x00, 0x00, 0x00},
	};
	return colors[index & 15];
}

static Color
palette8(uint8_t index)
{
	static constexpr uint8_t cube[] = {0xFF, 0xCC, 0x99, 0x66, 0x33, 0};
	static constexpr uint8_t ramp[] = {
		0xEE, 0xDD, 0xBB, 0xAA, 0x88, 0x77, 0x55, 0x44, 0x22, 0x11};
	if (index < 215)
		return {cube[index / 36], cube[index / 6 % 6], cube[index % 6]};
	if (index < 225)
		return {ramp[index - 215], 0, 0};
	if (index < 235)
		return {0, ramp[index - 225], 0};
	if (index < 245)
		return {0, 0, ramp[index - 235]};
	uint8_t gray = index == 255 ? 0 : ramp[index - 245];
	return {gray, gray, gray};
}

static uint8_t
mask_at(span<const uint8_t> data, uint32_t width, uint32_t x, uint32_t y,
	bool one_bit)
{
	if (one_bit) {
		size_t bit = size_t(y) * width + x;
		return data[bit / 8] & (0x80 >> (bit % 8)) ? 255 : 0;
	}
	return data[size_t(y) * width + x];
}

static span<const uint8_t>
mask_data(const vector<Entry> &entries, const IconInfo &info, bool *one_bit)
{
	*one_bit = false;
	if (info.combined_mask)
		return {};
	const Entry *mask = find_entry(entries, info.mask_type);
	if (!mask)
		return {};

	size_t pixels = size_t(info.width) * info.height;
	if (info.mask_type == fourcc('I', 'C', 'N', '#') ||
		info.mask_type == fourcc('i', 'c', 'm', '#') ||
		info.mask_type == fourcc('i', 'c', 's', '#') ||
		info.mask_type == fourcc('i', 'c', 'h', '#')) {
		size_t bytes = (pixels + 7) / 8;
		if (mask->data.size() < bytes * 2)
			return {};
		*one_bit = true;
		return mask->data.subspan(bytes, bytes);
	}
	return mask->data.size() < pixels ? span<const uint8_t>()
									  : mask->data.first(pixels);
}

static void
put_pixel(Image &image, uint32_t x, uint32_t y, Color c, uint8_t alpha)
{
	uint16_t *p = row_u16(image, y) + x * 4;
	p[0] = uint16_t(c.b) * 257;
	p[1] = uint16_t(c.g) * 257;
	p[2] = uint16_t(c.r) * 257;
	p[3] = uint16_t(alpha) * 257;
}

static ImagePtr
decode_low_depth(const Entry &entry, const IconInfo &info,
	const vector<Entry> &entries, Error *error)
{
	size_t pixels = size_t(info.width) * info.height;
	size_t bytes = info.encoding == Encoding::Mono ? (pixels + 7) / 8
		: info.encoding == Encoding::Palette4      ? (pixels + 1) / 2
												   : pixels;
	if (entry.data.size() < bytes * (info.combined_mask ? 2 : 1)) {
		set_error(error, "truncated indexed icon data");
		return nullptr;
	}

	bool one_bit_mask = false;
	span<const uint8_t> mask;
	if (info.combined_mask) {
		one_bit_mask = true;
		mask = entry.data.subspan(bytes, bytes);
	} else if (info.mask_type) {
		mask = mask_data(entries, info, &one_bit_mask);
	}

	ImagePtr image = image_new(info.width, info.height);
	if (!image) {
		set_error(error, "failed to allocate indexed icon image");
		return nullptr;
	}
	for (uint32_t y = 0; y < info.height; y++) {
		for (uint32_t x = 0; x < info.width; x++) {
			size_t pixel = size_t(y) * info.width + x;
			Color color;
			if (info.encoding == Encoding::Mono) {
				uint8_t index =
					(entry.data[pixel / 8] & (0x80 >> (pixel % 8))) != 0;
				color = index ? Color{0, 0, 0} : Color{255, 255, 255};
			} else if (info.encoding == Encoding::Palette4) {
				uint8_t packed = entry.data[pixel / 2];
				color = palette4(pixel & 1 ? packed : packed >> 4);
			} else {
				color = palette8(entry.data[pixel]);
			}
			uint8_t alpha = mask.empty()
				? 255
				: mask_at(mask, info.width, x, y, one_bit_mask);
			put_pixel(*image, x, y, color, alpha);
		}
	}
	return image;
}

static bool
decode_rle_channel(
	span<const uint8_t> &data, span<uint8_t> output, Error *error)
{
	size_t in = 0, out = 0;
	while (out < output.size()) {
		if (in >= data.size()) {
			set_error(error, "truncated RLE icon data");
			return false;
		}
		uint8_t control = data[in++];
		size_t count = control < 128 ? size_t(control) + 1 : control - 125;
		if (count > output.size() - out) {
			set_error(error, "RLE icon run exceeds its channel");
			return false;
		}
		if (control < 128) {
			if (count > data.size() - in) {
				set_error(error, "truncated RLE icon literal");
				return false;
			}
			copy_n(data.data() + in, count, output.data() + out);
			in += count;
		} else {
			if (in >= data.size()) {
				set_error(error, "truncated RLE icon repeat");
				return false;
			}
			fill_n(output.data() + out, count, data[in++]);
		}
		out += count;
	}
	data = data.subspan(in);
	return true;
}

static ImagePtr
decode_argb_rle(span<const uint8_t> data, const IconInfo &info, Error *error)
{
	if (data.size() < 4 || memcmp(data.data(), "ARGB", 4)) {
		set_error(error, "invalid ARGB icon data");
		return nullptr;
	}
	data = data.subspan(4);
	size_t pixels = size_t(info.width) * info.height;
	vector<uint8_t> channels(pixels * 4);
	for (int c = 0; c < 4; c++)
		if (!decode_rle_channel(data,
				span<uint8_t>(channels).subspan(size_t(c) * pixels, pixels),
				error))
			return nullptr;

	ImagePtr image = image_new(info.width, info.height);
	if (!image) {
		set_error(error, "failed to allocate ARGB icon image");
		return nullptr;
	}
	for (uint32_t y = 0; y < info.height; y++)
		for (uint32_t x = 0; x < info.width; x++) {
			size_t p = size_t(y) * info.width + x;
			put_pixel(*image, x, y,
				{channels[pixels + p], channels[pixels * 2 + p],
					channels[pixels * 3 + p]},
				channels[p]);
		}
	return image;
}

static ImagePtr
decode_rgb24(const Entry &entry, const IconInfo &info,
	const vector<Entry> &entries, Error *error)
{
	size_t pixels = size_t(info.width) * info.height;
	vector<uint8_t> channels(pixels * 3);
	span<const uint8_t> data = entry.data;
	bool interleaved = data.size() == pixels * 4;
	if (!interleaved) {
		if (entry.type == fourcc('i', 't', '3', '2')) {
			if (data.size() < 4) {
				set_error(error, "truncated 128x128 icon prefix");
				return nullptr;
			}
			data = data.subspan(4);
		}
		for (int c = 0; c < 3; c++)
			if (!decode_rle_channel(data,
					span<uint8_t>(channels).subspan(size_t(c) * pixels, pixels),
					error))
				return nullptr;
	}

	bool one_bit_mask = false;
	span<const uint8_t> mask = mask_data(entries, info, &one_bit_mask);
	ImagePtr image = image_new(info.width, info.height);
	if (!image) {
		set_error(error, "failed to allocate RGB icon image");
		return nullptr;
	}
	for (uint32_t y = 0; y < info.height; y++)
		for (uint32_t x = 0; x < info.width; x++) {
			size_t p = size_t(y) * info.width + x;
			Color color = interleaved
				? Color{entry.data[p * 4 + 1], entry.data[p * 4 + 2],
					  entry.data[p * 4 + 3]}
				: Color{channels[p], channels[pixels + p],
					  channels[pixels * 2 + p]};
			uint8_t alpha = mask.empty()
				? 255
				: mask_at(mask, info.width, x, y, one_bit_mask);
			put_pixel(*image, x, y, color, alpha);
		}
	return image;
}

static ImagePtr
decode_argb_raw(span<const uint8_t> data, const IconInfo &info, Error *error)
{
	size_t pixels = size_t(info.width) * info.height;
	if (data.size() != pixels * 4) {
		set_error(error, "unsupported ARGB icon encoding");
		return nullptr;
	}
	ImagePtr image = image_new(info.width, info.height);
	if (!image) {
		set_error(error, "failed to allocate ARGB icon image");
		return nullptr;
	}
	for (uint32_t y = 0; y < info.height; y++)
		for (uint32_t x = 0; x < info.width; x++) {
			size_t p = size_t(y) * info.width + x;
			put_pixel(*image, x, y,
				{data[p * 4 + 1], data[p * 4 + 2], data[p * 4 + 3]},
				data[p * 4]);
		}
	return image;
}

static ImagePtr
decode_entry(const Entry &entry, const vector<Entry> &entries,
	const OpenContext &ctx, Error *error)
{
	OpenContext nested = ctx;
	nested.first_frame_only = true;
	if (is_png(entry.data))
		return detail::load_wuffs(entry.data, nested, error);
	if (is_jpeg2000(entry.data)) {
#if DAWN_WITH_OPENJPEG
		return detail::load_openjpeg(entry.data, nested, error);
#else
		set_error(error, "JPEG 2000 support is disabled");
		return nullptr;
#endif
	}

	IconInfo info = icon_info(entry.type);
	if (info.encoding == Encoding::None)
		return nullptr;
	if (entry.data.size() >= 4 && !memcmp(entry.data.data(), "ARGB", 4))
		return decode_argb_rle(entry.data, info, error);
	switch (info.encoding) {
	case Encoding::Mono:
	case Encoding::Palette4:
	case Encoding::Palette8:
		return decode_low_depth(entry, info, entries, error);
	case Encoding::RGB24:
		return decode_rgb24(entry, info, entries, error);
	case Encoding::ARGB:
		return decode_argb_raw(entry.data, info, error);
	default:
		return nullptr;
	}
}

ImagePtr
detail::load_icns(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (data.size() < 8 || memcmp(data.data(), "icns", 4)) {
		set_error(error, "not an ICNS image");
		return nullptr;
	}
	uint32_t declared = be32(data.data() + 4);
	if (declared < 8 || declared > data.size()) {
		set_error(error, "invalid ICNS file length");
		return nullptr;
	}

	vector<Entry> entries;
	size_t offset = 8;
	while (offset < declared) {
		if (declared - offset < 8) {
			add_warning(ctx, "truncated ICNS entry header");
			break;
		}
		uint32_t type = be32(data.data() + offset);
		uint32_t length = be32(data.data() + offset + 4);
		if (length < 8 || length > declared - offset) {
			add_warning(
				ctx, "invalid ICNS " + type_name(type) + " entry length");
			break;
		}
		entries.push_back({type, data.subspan(offset + 8, size_t(length) - 8)});
		offset += length;
	}

	ImagePtr head, tail;
	for (const Entry &entry : entries) {
		Error suberror;
		ImagePtr page = decode_entry(entry, entries, ctx, &suberror);
		if (page) {
			append_page(head, tail, std::move(page));
			if (ctx.first_frame_only)
				break;
		} else if (!suberror.message.empty()) {
			add_warning(
				ctx, "ICNS " + type_name(entry.type) + ": " + suberror.message);
		}
	}
	if (!head) {
		set_error(error, "empty or unsupported ICNS image");
		return nullptr;
	}
	for (Image *page = head.get(); page; page = page->page_next.get())
		if (!page->effective_profile)
			ensure_working_premul_pages(
				*page, ctx, nullptr, /*input_premul=*/false);
	return head;
}

}  // namespace dawn
