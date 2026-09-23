//
// test-dnrs.cpp: verify Dawn's libdnrs fallback
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "exr.hpp"
#include "libdn/libdn-loaders.hpp"
#include "libdn/libdn.hpp"

#include <cmath>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

using namespace std;

// EXR and Radiance go through split_hdr(), which is tested on its own.
static int
test_linear()
{
	// Flat RGBE: 4.0 and 0.5, both grey.
	const uint8_t hdr[] = "#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 1 +X 2\n"
						  "\x80\x80\x80\x83\x80\x80\x80\x80";
	dawn::OpenContext context;
	context.gain_maps = true;
	dawn::Error error;
	dawn::ImagePtr image =
		dawn::load_dnrs(span(hdr, sizeof hdr - 1), context, &error);
	if (!image || error || !image->gain_map || !image->effective_profile)
		return 20;

	const auto half = uint16_t(
		lround(dawn::transfer_encode(0.5f, dawn::Transfer::Srgb) * 65535));
	const uint16_t *pixel = dawn::row_u16(*image, 0);
	if (pixel[0] != 65535 || pixel[3] != 65535 || pixel[4] != half ||
		pixel[6] != half)
		return 21;
	const dawn::GainMap &map = *image->gain_map;
	if (map.width != 2 || map.height != 1 || map.max != 2 ||
		map.alternate_headroom != 2 || map.data[0] != 65535 || map.data[1])
		return 22;

	context.gain_maps = false;
	image = dawn::load_dnrs(span(hdr, sizeof hdr - 1), context, &error);
	if (!image || error || image->gain_map ||
		dawn::row_u16(*image, 0)[4] != half)
		return 23;

	// A negative channel takes the pixels to BT.2020, where this one fits,
	// and an invisible one may hold anything without a warning.
	const float rgb[] = {1, -0.05f, 0, NAN, 1e30f, -1};
	const float alpha[] = {1, 0};
	const vector<uint8_t> exr = test::write_exr(2, 1, rgb, alpha);
	vector<string> warnings;
	context.warnings = &warnings;
	image = dawn::load_dnrs(exr, context, &error);
	if (!image || error || !warnings.empty())
		return 24;
	const dawn::Chromaticities chromaticities =
		dawn::profile_chromaticities(image->effective_profile.get());
	if (!chromaticities.have_primaries ||
		fabs(chromaticities.x[0] - 0.708) > 1e-3)
		return 25;
	pixel = dawn::row_u16(*image, 0);
	if (!pixel[0] || !pixel[1] || pixel[4] || pixel[5] || pixel[6] || pixel[7])
		return 26;
	return 0;
}

int
main()
{
	const uint8_t pnm[] = "P6\n1 1\n255\n\x12\x34\x56";

	dawn::OpenContext context;
	context.first_frame_only = false;
	dawn::Error error;
	dawn::ImagePtr image =
		dawn::load_dnrs(span(pnm, sizeof pnm - 1), context, &error);
	if (!image || error)
		return 1;

	if (image->width != 1 || image->height != 1 || image->frame_next ||
		image->page_next)
		return 2;

	const uint16_t *pixel = dawn::row_u16(*image, 0);
	if (pixel[0] != 0x5656 || pixel[1] != 0x3434 || pixel[2] != 0x1212 ||
		pixel[3] != 0xffff)
		return 3;

	image = dawn::open_from_data(span(pnm, sizeof pnm - 1), context, &error);
	if (!image || error || !image->loader ||
		string_view(image->loader) != "Wuffs")
		return 4;

	const uint8_t xbm[] = "#define dot_width 1\n"
						  "#define dot_height 1\n"
						  "static unsigned char dot_bits[] = { 0x01 };\n";
	image =
		dawn::open_from_data(span(xbm, sizeof xbm - 1), context, &error);
	if (!image || error || !image->loader || string_view(image->loader) == "")
		return 5;

	const uint8_t pnm16[] = "P6\n1 1\n65535\n\x12\x34\x56\x78\x9a\xbc";
	image = dawn::load_dnrs(span(pnm16, sizeof pnm16 - 1), context, &error);
	if (!image || error)
		return 6;

	pixel = dawn::row_u16(*image, 0);
	if (pixel[0] != 0x9abc || pixel[1] != 0x5678 || pixel[2] != 0x1234 ||
		pixel[3] != 0xffff)
		return 7;

	const uint8_t junk[] = "not an image";
	error = {};
	image = dawn::load_dnrs(span(junk, sizeof junk - 1), context, &error);
	if (image || !error)
		return 8;
	return test_linear();
}
