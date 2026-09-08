//
// png-io.cpp: PNG output for daemon command-line clients
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "png-io.hpp"

#include <png.h>

#include <bit>
#include <cstdio>
#include <vector>

using namespace std;

static void
png_error_fn(png_structp png, png_const_charp message)
{
	*(string *) png_get_error_ptr(png) = message;
	longjmp(png_jmpbuf(png), 1);
}

static bool
begin(FILE *file, uint32_t width, uint32_t height, int depth,
	span<const uint8_t> icc, string *error, png_structp *png_out,
	png_infop *info_out)
{
	png_structp png = png_create_write_struct(
		PNG_LIBPNG_VER_STRING, error, png_error_fn, nullptr);
	if (!png)
		return false;
	png_infop info = png_create_info_struct(png);
	if (!info) {
		png_destroy_write_struct(&png, nullptr);
		return false;
	}
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		return false;
	}
	png_init_io(png, file);
	png_set_IHDR(png, info, width, height, depth, PNG_COLOR_TYPE_RGBA,
		PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_BASE, PNG_FILTER_TYPE_BASE);
	if (!icc.empty())
		png_set_iCCP(png, info, "sRGB", PNG_COMPRESSION_TYPE_BASE,
			const_cast<png_bytep>(icc.data()), png_uint_32(icc.size()));
	*png_out = png;
	*info_out = info;
	return true;
}

bool
write_png16(FILE *file, uint32_t width, uint32_t height, uint32_t stride,
	const uint8_t *pixels, span<const uint8_t> icc, int32_t orientation,
	string *error)
{
	png_structp png;
	png_infop info;
	if (!begin(file, width, height, 16, icc, error, &png, &info))
		return false;
	string text = to_string(orientation);
	vector<uint16_t> row(size_t(width) * 4);
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		return false;
	}
	png_text entry{};
	entry.compression = PNG_TEXT_COMPRESSION_NONE;
	entry.key = const_cast<char *>("dawn:orientation");
	entry.text = text.data();
	png_set_text(png, info, &entry, 1);
	if constexpr (endian::native == endian::little)
		png_set_swap(png);
	png_write_info(png, info);
	for (uint32_t y = 0; y < height; y++) {
		const auto *src =
			reinterpret_cast<const uint16_t *>(pixels + size_t(y) * stride);
		for (uint32_t x = 0; x < width; x++) {
			const uint32_t a = src[x * 4 + 3];
			auto straight = [a](uint16_t v) {
				return uint16_t(
					a ? min(65535u, (uint32_t(v) * 65535u + a / 2) / a) : 0);
			};
			row[x * 4] = straight(src[x * 4 + 2]);
			row[x * 4 + 1] = straight(src[x * 4 + 1]);
			row[x * 4 + 2] = straight(src[x * 4]);
			row[x * 4 + 3] = uint16_t(a);
		}
		png_write_row(png, reinterpret_cast<png_bytep>(row.data()));
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	return true;
}

bool
write_png8(FILE *file, uint32_t width, uint32_t height,
	span<const uint8_t> rgba, span<const uint8_t> icc, string *error)
{
	if (rgba.size() < uint64_t(width) * height * 4)
		return false;
	png_structp png;
	png_infop info;
	if (!begin(file, width, height, 8, icc, error, &png, &info))
		return false;
	if (setjmp(png_jmpbuf(png))) {
		png_destroy_write_struct(&png, &info);
		return false;
	}
	png_write_info(png, info);
	for (uint32_t y = 0; y < height; y++)
		png_write_row(
			png, const_cast<png_bytep>(rgba.data() + size_t(y) * width * 4));
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);
	return true;
}
