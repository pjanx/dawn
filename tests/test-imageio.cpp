//
// test-imageio.cpp: check the ImageIO fallback against first-class loaders
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// ImageIO's format coverage is Apple's problem.  What is ours is that it must
// not colour-manage behind our back: every fixture here has a known-good
// first-class decoding to compare against, and display-p3-red_vs_srgb-red.png
// is the one that catches a double conversion.

#include <dawn-config.h>

#include "libdn/libdn.h"
#include "test.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace std;
namespace fs = filesystem;

#ifndef DAWN_TEST_FIXTURES_DIR
#error DAWN_TEST_FIXTURES_DIR must be defined
#endif

namespace
{

dawn::ImagePtr
load(const char *name, const vector<string> &loaders, shared_ptr<dawn::Cmm> cmm,
	shared_ptr<dawn::Profile> screen)
{
	dawn::OpenContext ctx;
	ctx.uri = (fs::path(DAWN_TEST_FIXTURES_DIR) / name).string();
	ctx.cmm = cmm;
	ctx.screen_profile = screen;
	ctx.first_frame_only = true;
	ctx.loaders = loaders;

	dawn::Error error;
	dawn::ImagePtr image = dawn::open(ctx, &error);
	if (!image)
		test::fail("open(%s): %s", name, error.message.c_str());
	return image;
}

// Compares the ImageIO decoding of a fixture against its default one,
// which is whatever loader the table would normally pick for it.
dawn::ImagePtr
compare(const char *name, int tolerance, shared_ptr<dawn::Profile> screen)
{
	auto cmm = dawn::Cmm::get_default();
	dawn::ImagePtr expected = load(name, {}, cmm, screen);
	dawn::ImagePtr actual = load(name, {"ImageIO"}, cmm, screen);
	if (!expected || !actual)
		return nullptr;

	CHECK(actual->loader && !strcmp(actual->loader, "ImageIO"));

	// ImageIO assigns sRGB to untagged files, and must not then pass that
	// off as embedded: the two loaders have to agree on whether the profile
	// was the file's or our own invention.
	if (expected->profile_assumed != actual->profile_assumed) {
		test::fail("%s: profile_assumed %d, ImageIO says %d", name,
			int(expected->profile_assumed), int(actual->profile_assumed));
	}
	if (expected->width != actual->width ||
		expected->height != actual->height) {
		test::fail("%s: %ux%u decoded as %ux%u", name, expected->width,
			expected->height, actual->width, actual->height);
		return nullptr;
	}

	int worst = 0;
	uint32_t worst_x = 0, worst_y = 0;
	for (uint32_t y = 0; y < expected->height; y++) {
		const uint16_t *a = dawn::row_u16(*expected, y);
		const uint16_t *b = dawn::row_u16(*actual, y);
		for (uint32_t x = 0; x < expected->width * 4; x++) {
			int difference = abs(int(a[x]) - int(b[x]));
			if (difference > worst) {
				worst = difference;
				worst_x = x / 4;
				worst_y = y;
			}
		}
	}
	if (worst > tolerance) {
		test::fail("%s: differs by %d at (%u, %u), tolerance %d", name, worst,
			worst_x, worst_y, tolerance);
	}
	return actual;
}

// PNG decodes losslessly either way, so the identity path must be exact
// but for premultiplication rounding.
void
test_solids()
{
	constexpr int tolerance = 2 * 257;
	compare("blue.png", tolerance, nullptr);
	compare("blue16.png", tolerance, nullptr);
	compare("red_a128.png", tolerance, nullptr);
	compare("rgbw_2x2.png", tolerance, nullptr);
#if DAWN_WITH_LIBTIFF
	compare("blue.tif", tolerance, nullptr);
#endif
}

// Both loaders hand lcms2 the same embedded Display P3 profile, so both must
// perform exactly one conversion into an AdobeRGB-like screen profile.
void
test_no_double_conversion()
{
	auto cmm = dawn::Cmm::get_default();
	double whitepoint[2] = {0.3127, 0.3290};
	double adobe_primaries[6] = {
		0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600};
	auto target = cmm->get_profile_parametric(2.2, whitepoint, adobe_primaries);
	CHECK(target != nullptr);

	dawn::ImagePtr image =
		compare("display-p3-red_vs_srgb-red.png", 4 * 257, target);
	if (!image)
		return;

	// The two halves must stay as far apart as test-channels demands of Wuffs.
	const uint16_t *p3_red = dawn::row_u16(*image, 50) + 50 * 4;
	const uint16_t *srgb_red = dawn::row_u16(*image, 50) + 150 * 4;
	CHECK(p3_red[2] > srgb_red[2] + 2000);
	CHECK(p3_red[1] + 1000 < srgb_red[1]);
	CHECK(p3_red[0] + 1000 < srgb_red[0]);
	CHECK(!image->profile_assumed);
}

}  // namespace

int
main()
{
	return test::run({
		{"solid fixtures", test_solids},
		{"no double conversion", test_no_double_conversion},
	});
}
