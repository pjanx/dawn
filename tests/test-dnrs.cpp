//
// test-dnrs.cpp: verify Dawn's libdnrs fallback
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn-loaders.h"
#include "libdn/libdn.h"

#include <cstdint>
#include <span>
#include <string_view>

int
main()
{
	const uint8_t pnm[] = "P6\n1 1\n255\n\x12\x34\x56";
	dawn::OpenContext context;
	context.first_frame_only = false;
	dawn::Error error;
	dawn::ImagePtr image = dawn::detail::load_dnrs(
		std::span(pnm, sizeof pnm - 1), context, &error);
	if (!image || error)
		return 1;
	if (image->width != 1 || image->height != 1 || image->frame_next ||
		image->page_next)
		return 2;
	const uint16_t *pixel = dawn::row_u16(*image, 0);
	if (pixel[0] != 0x5656 || pixel[1] != 0x3434 || pixel[2] != 0x1212 ||
		pixel[3] != 0xffff)
		return 3;

	image =
		dawn::open_from_data(std::span(pnm, sizeof pnm - 1), context, &error);
	if (!image || error || !image->loader ||
		std::string_view(image->loader) != "Wuffs")
		return 4;

	const uint8_t xbm[] = "#define dot_width 1\n"
						  "#define dot_height 1\n"
						  "static unsigned char dot_bits[] = { 0x01 };\n";
	image =
		dawn::open_from_data(std::span(xbm, sizeof xbm - 1), context, &error);
	if (!image || error || !image->loader ||
		std::string_view(image->loader) == "")
		return 5;

	const uint8_t pnm16[] = "P6\n1 1\n65535\n\x12\x34\x56\x78\x9a\xbc";
	image = dawn::detail::load_dnrs(
		std::span(pnm16, sizeof pnm16 - 1), context, &error);
	if (!image || error)
		return 6;
	pixel = dawn::row_u16(*image, 0);
	if (pixel[0] != 0x9abc || pixel[1] != 0x5678 || pixel[2] != 0x1234 ||
		pixel[3] != 0xffff)
		return 7;

	const uint8_t junk[] = "not an image";
	error = {};
	image = dawn::detail::load_dnrs(
		std::span(junk, sizeof junk - 1), context, &error);
	if (image || !error)
		return 8;
	return 0;
}
