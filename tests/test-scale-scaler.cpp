//
// test-scale-scaler.cpp: headless Vulkan scaler tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/scale-scaler.hpp"
#include "test.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;

/// One BGRA_PREMUL_4X16LE pixel, as the scaler takes them.
struct Pixel {
	uint16_t b = 0, g = 0, r = 0, a = 0;
};

static const Pixel kRed{.r = 65535, .a = 65535};
static const Pixel kGreen{.g = 65535, .a = 65535};
static const Pixel kBlue{.b = 65535, .a = 65535};
static const Pixel kWhite{65535, 65535, 65535, 65535};

static dawn::ScaleScaler scaler;

static bool
scale(const vector<Pixel> &src, uint32_t w, uint32_t h, uint32_t out_w,
	uint32_t out_h, dawn::Orientation orientation, dawn::ScaleOutput *out)
{
	string error;
	if (scaler.scale(w, h, (const uint8_t *) src.data(), w * sizeof(Pixel),
			out_w, out_h, orientation, out, &error))
		return true;

	test::fail("scale failed: %s", error.c_str());
	return false;
}

/// Output pixels are straight RGBA8, whatever the input order is.
static bool
pixel_is(const dawn::ScaleOutput &out, uint32_t x, uint32_t y, uint8_t r,
	uint8_t g, uint8_t b, uint8_t a)
{
	if (x >= out.width || y >= out.height)
		return false;

	const uint8_t *p = out.rgba8.data() + (size_t(y) * out.width + x) * 4;
	if (p[0] == r && p[1] == g && p[2] == b && p[3] == a)
		return true;

	test::fail("pixel %u,%u is %u %u %u %u, want %u %u %u %u", x, y, p[0], p[1],
		p[2], p[3], r, g, b, a);
	return false;
}

// --- Cases -------------------------------------------------------------------

static void
test_init_destroy()
{
	string error;
	CHECK(scaler.init(&error));

	scaler.destroy();
	scaler.destroy();

	// Nothing has been submitted, so there is no fence to wait on either.
	const vector<Pixel> src{kRed};
	dawn::ScaleOutput out;
	CHECK(!scaler.scale(1, 1, (const uint8_t *) src.data(), sizeof(Pixel), 1, 1,
		dawn::Orientation::Rotate0, &out, &error));
	CHECK(error == "ScaleScaler not initialized");

	CHECK(scaler.init(&error));
	CHECK(scaler.init(&error));
}

static void
test_rejected_then_valid()
{
	const vector<Pixel> src{kRed, kGreen};
	const auto *pixels = (const uint8_t *) src.data();
	const size_t stride = 2 * sizeof(Pixel);
	const auto none = dawn::Orientation::Rotate0;

	string error;
	dawn::ScaleOutput out;
	CHECK(!scaler.scale(2, 1, pixels, stride, 0, 1, none, &out, &error));
	CHECK(!scaler.scale(0, 1, pixels, stride, 2, 1, none, &out, &error));
	CHECK(!scaler.scale(2, 1, nullptr, stride, 2, 1, none, &out, &error));

	// Rejected by the device-byte limit, before any Vulkan object is made.
	CHECK(
		!scaler.scale(2, 1, pixels, stride, 65535, 65535, none, &out, &error));
	CHECK(out.width == 0 && out.height == 0 && out.rgba8.empty());

	if (scale(src, 2, 1, 4, 2, none, &out)) {
		CHECK(out.width == 4 && out.height == 2);
		CHECK(out.rgba8.size() == 4 * 2 * 4);
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 3, 1, 0, 255, 0, 255);
	}
}

static void
test_orientation()
{
	// Stored top-left to bottom-right; all four differ, so that every
	// rotation and mirror lands somewhere else.
	const vector<Pixel> src{kRed, kGreen, kBlue, kWhite};

	dawn::ScaleOutput out;
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Rotate0, &out)) {
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 1, 0, 0, 255, 0, 255);
		pixel_is(out, 0, 1, 0, 0, 255, 255);
		pixel_is(out, 1, 1, 255, 255, 255, 255);
	}
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Rotate90, &out)) {
		pixel_is(out, 0, 0, 0, 0, 255, 255);
		pixel_is(out, 1, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 255, 255, 255, 255);
		pixel_is(out, 1, 1, 0, 255, 0, 255);
	}
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Mirror0, &out)) {
		pixel_is(out, 0, 0, 0, 255, 0, 255);
		pixel_is(out, 1, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 255, 255, 255, 255);
		pixel_is(out, 1, 1, 0, 0, 255, 255);
	}

	// A quarter turn of a wide image is a tall one.
	const vector<Pixel> wide{kRed, kGreen};
	if (scale(wide, 2, 1, 1, 2, dawn::Orientation::Rotate90, &out)) {
		CHECK(out.width == 1 && out.height == 2);
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 0, 255, 0, 255);
	}
}

static void
test_partial_transparency()
{
	// Half-transparent red, premultiplied, next to nothing at all.
	const vector<Pixel> src{{.r = 32768, .a = 32768}, {}};

	dawn::ScaleOutput out;
	if (scale(src, 2, 1, 2, 1, dawn::Orientation::Rotate0, &out)) {
		pixel_is(out, 0, 0, 255, 0, 0, 127);
		pixel_is(out, 1, 0, 0, 0, 0, 0);
	}
}

int
main()
{
	string error;
	if (!scaler.init(&error)) {
		fprintf(stderr, "no usable Vulkan device: %s\n", error.c_str());
		return 77;
	}

	return test::run({
		{"init and destroy", test_init_destroy},
		{"valid after rejected", test_rejected_then_valid},
		{"orientation", test_orientation},
		{"partial transparency", test_partial_transparency},
	});
}
