//
// test-channels.cpp: verify BGRA_PREMUL_4X16LE channel order
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn/libdn-loaders.hpp"
#include "libdn/libdn.hpp"
#include "test.hpp"

#include <lcms2.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <vector>

using namespace std;
namespace fs = filesystem;

#ifndef DAWN_TEST_FIXTURES_DIR
#error DAWN_TEST_FIXTURES_DIR must be defined
#endif

static bool
near_u16(uint16_t a, uint16_t b, uint16_t tol)
{
	return abs(int(a) - int(b)) <= int(tol);
}

namespace
{

struct Pixel {
	uint16_t b = 0, g = 0, r = 0, a = 0;
};

}  // namespace

static Pixel
pixel0(const dawn::Image &img)
{
	const uint16_t *p = dawn::row_u16(img, 0);
	return {p[0], p[1], p[2], p[3]};
}

static Pixel
pixel_at(const dawn::Image &img, uint32_t x, uint32_t y)
{
	const uint16_t *p = dawn::row_u16(img, y) + x * 4;
	return {p[0], p[1], p[2], p[3]};
}

static void
expect_bgra(const char *label, Pixel p, uint16_t b, uint16_t g, uint16_t r,
	uint16_t a, uint16_t tol)
{
	if (!near_u16(p.b, b, tol) || !near_u16(p.g, g, tol) ||
		!near_u16(p.r, r, tol) || !near_u16(p.a, a, tol)) {
		test::fail(
			"%s: got BGRA (%u,%u,%u,%u) expected (~%u,~%u,~%u,~%u) tol=%u",
			label, p.b, p.g, p.r, p.a, b, g, r, a, tol);
	}
}

static dawn::ImagePtr
load_fixture(const string &name)
{
	fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / name;
	dawn::OpenContext ctx;
	ctx.uri = dawn::path_to_uri(path.string());
	ctx.first_frame_only = true;
	dawn::Error error;
	dawn::ImagePtr img = dawn::open(ctx, &error);
	if (!img) {
		test::fail(
			"open(%s): %s", path.string().c_str(), error.message.c_str());
	}
	return img;
}

static void
test_pack_helpers()
{
	dawn::ImagePtr img = dawn::image_new(1, 1);
	CHECK(img != nullptr);

	// RGBA8 red → B=0 G=0 R=65535 A=65535
	{
		const uint8_t rgba[] = {255, 0, 0, 255};
		dawn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}
	{
		const uint8_t rgba[] = {0, 255, 0, 255};
		dawn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 green", pixel0(*img), 0, 65535, 0, 65535, 0);
	}
	{
		const uint8_t rgba[] = {0, 0, 255, 255};
		dawn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 blue", pixel0(*img), 65535, 0, 0, 65535, 0);
	}

	{
		const uint8_t rgb[] = {255, 0, 0};
		dawn::pack_rgb8_to_bgra16(*img, rgb, 3);
		expect_bgra("pack_rgb8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		// Host-endian 0xAARRGGBB: opaque red
		const uint32_t word = 0xFFFF0000u;
		dawn::pack_argb32_words_to_bgra16(*img, &word, sizeof word);
		expect_bgra("pack_argb32 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		const uint8_t bgra8[] = {0, 0, 255, 255};  // B,G,R,A
		dawn::widen_bgra8_to_bgra16(*img, bgra8, 4);
		expect_bgra("widen_bgra8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		// LE R,G,B,A uint16 — 10-bit max red in low 10 bits → scaled
		const uint16_t rgba16[] = {1023, 0, 0, 1023};
		dawn::pack_rgba16le_to_bgra16(*img, rgba16, 8, 10);
		expect_bgra(
			"pack_rgba16le 10-bit red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}
	{
		const uint16_t rgb16[] = {0, 0, 65535};
		dawn::pack_rgb16le_to_bgra16(*img, rgb16, 6, 16);
		expect_bgra("pack_rgb16le blue", pixel0(*img), 65535, 0, 0, 65535, 0);
	}
}

static void
test_unpremultiply_alpha_last8()
{
	// Two padded rows, so that row padding can be proven untouched.
	const uint32_t width = 4, height = 2;
	const size_t stride = size_t(width) * 4 + 3;
	const uint8_t input[height][4][4] = {
		{{0, 10, 20, 0}, {1, 1, 1, 1}, {64, 128, 200, 128}, {10, 20, 30, 255}},
		{{255, 255, 255, 255}, {5, 0, 3, 1}, {128, 128, 128, 128},
			{0, 0, 0, 0}},
	};
	const uint8_t expected[height][4][4] = {
		{{0, 0, 0, 0}, {255, 255, 255, 1}, {128, 255, 255, 128},
			{10, 20, 30, 255}},
		{{255, 255, 255, 255}, {255, 0, 255, 1}, {255, 255, 255, 128},
			{0, 0, 0, 0}},
	};

	vector<uint8_t> buffer(stride * height, 0xCD);
	for (uint32_t y = 0; y < height; y++)
		memcpy(buffer.data() + y * stride, input[y], sizeof input[y]);

	dawn::unpremultiply_xxxa8(buffer.data(), width, height, stride);
	for (uint32_t y = 0; y < height; y++) {
		const uint8_t *row = buffer.data() + y * stride;
		for (uint32_t x = 0; x < width * 4; x++) {
			const uint8_t *want = &expected[y][0][0];
			if (row[x] != want[x]) {
				test::fail("row %u byte %u: got %u expected %u", y, x, row[x],
					want[x]);
			}
		}
		for (size_t x = size_t(width) * 4; x < stride; x++)
			CHECK(row[x] == 0xCD);
	}
}

static dawn::ImagePtr
image_1x1(uint16_t b, uint16_t g, uint16_t r, uint16_t a)
{
	dawn::ImagePtr img = dawn::image_new(1, 1);
	if (!img) {
		test::fail("image_new failed");
		exit(1);
	}

	uint16_t *p = dawn::row_u16(*img, 0);
	p[0] = b;
	p[1] = g;
	p[2] = r;
	p[3] = a;
	return img;
}

static void
test_finishing()
{
	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	auto p3 = cmm->get_profile_display_p3();
	CHECK(srgb != nullptr && p3 != nullptr);
	vector<uint8_t> srgb_icc = srgb->to_bytes(), p3_icc = p3->to_bytes();
	CHECK(!srgb_icc.empty() && !p3_icc.empty());

	dawn::OpenContext plain;
	plain.cmm = cmm;
	dawn::OpenContext to_srgb = plain;
	to_srgb.screen_profile = srgb;
	dawn::OpenContext to_p3 = plain;
	to_p3.screen_profile = p3;

	// Straight pixels are premultiplied, sRGB is assumed and admitted to.
	{
		dawn::ImagePtr img = image_1x1(1000, 2000, 3000, 32768);
		dawn::finish_image(*img, plain, nullptr, false);
		expect_bgra(
			"straight without target", pixel0(*img), 500, 1000, 1500, 32768, 0);
		CHECK(img->profile_assumed);
		CHECK(dawn::profiles_equal(img->effective_profile.get(), srgb.get()));
	}

	// Premultiplied pixels with nowhere to convert them to stay untouched.
	{
		dawn::ImagePtr img = image_1x1(500, 1000, 1500, 32768);
		dawn::finish_image(*img, plain, nullptr, true);
		expect_bgra("premultiplied without target", pixel0(*img), 500, 1000,
			1500, 32768, 0);
		CHECK(img->profile_assumed);
		CHECK(img->effective_profile != nullptr);
	}

	// With a target, they make a round trip through the transform.
	{
		dawn::ImagePtr img = image_1x1(500, 1000, 1500, 32768);
		dawn::finish_image(*img, to_srgb, nullptr, true);
		expect_bgra(
			"premultiplied to sRGB", pixel0(*img), 500, 1000, 1500, 32768, 257);
		CHECK(img->profile_assumed);
	}

	// A valid embedded profile is what the pixels are then described by.
	{
		dawn::ImagePtr img = image_1x1(0, 0, 65535, 65535);
		img->icc = p3_icc;
		dawn::finish_image(*img, plain, nullptr, false);
		expect_bgra("embedded ICC", pixel0(*img), 0, 0, 65535, 65535, 0);
		CHECK(!img->profile_assumed);
		CHECK(dawn::profiles_equal(img->effective_profile.get(), p3.get()));
	}

	// An unusable one falls back to assumed sRGB, as an absent one does.
	{
		dawn::ImagePtr img = image_1x1(0, 0, 65535, 65535);
		img->icc = {0xDE, 0xAD, 0xBE, 0xEF};
		dawn::finish_image(*img, plain, nullptr, false);
		expect_bgra("invalid ICC", pixel0(*img), 0, 0, 65535, 65535, 0);
		CHECK(img->profile_assumed);
		CHECK(dawn::profiles_equal(img->effective_profile.get(), srgb.get()));
	}

	// A profile the loader already settled on survives an embedded one.
	{
		dawn::ImagePtr img = image_1x1(0, 0, 65535, 65535);
		img->icc = p3_icc;
		img->effective_profile = srgb;
		dawn::finish_image(*img, plain, nullptr, false);
		CHECK(img->effective_profile == srgb);
		CHECK(!img->profile_assumed);
	}

	// An explicit source takes precedence over the embedded profile:
	// sRGB red converted to Display P3 lands well inside its gamut, while
	// P3 red stays at the edge.
	dawn::ImagePtr from_srgb = image_1x1(0, 0, 65535, 65535);
	from_srgb->icc = p3_icc;
	dawn::finish_image(*from_srgb, to_p3, srgb.get(), false);
	// Naming a source leaves describing it to the loader that named it.
	CHECK(from_srgb->effective_profile == nullptr);

	dawn::ImagePtr from_p3 = image_1x1(0, 0, 65535, 65535);
	from_p3->icc = srgb_icc;
	dawn::finish_image(*from_p3, to_p3, p3.get(), false);

	const Pixel srgb_red = pixel0(*from_srgb), p3_red = pixel0(*from_p3);
	CHECK(srgb_red.r + 1000 < p3_red.r);
	CHECK(srgb_red.g > p3_red.g + 1000);
	CHECK(srgb_red.a == 65535 && p3_red.a == 65535);

	// Frames without a profile of their own inherit the page's.
	{
		dawn::ImagePtr page = image_1x1(1000, 2000, 3000, 32768);
		page->icc = p3_icc;
		page->frame_next = image_1x1(1000, 2000, 3000, 32768);
		page->frame_next->frame_previous = page;
		dawn::finish_frames(*page, plain, nullptr, false);
		expect_bgra("frame page", pixel0(*page), 500, 1000, 1500, 32768, 0);
		expect_bgra(
			"frame tail", pixel0(*page->frame_next), 500, 1000, 1500, 32768, 0);
		CHECK(page->frame_next->effective_profile == page->effective_profile);
		CHECK(!page->frame_next->profile_assumed);
		CHECK(dawn::profiles_equal(page->effective_profile.get(), p3.get()));
	}

	// Including the assumption made for a page without one.
	{
		dawn::ImagePtr page = image_1x1(0, 0, 65535, 65535);
		page->frame_next = image_1x1(0, 0, 65535, 65535);
		page->frame_next->frame_previous = page;
		dawn::finish_frames(*page, plain, nullptr, false);
		CHECK(page->profile_assumed && page->frame_next->profile_assumed);
		CHECK(page->frame_next->effective_profile == page->effective_profile);
	}
}

static void
test_solid(const char *path, uint16_t b, uint16_t g, uint16_t r, uint16_t tol)
{
	dawn::ImagePtr img = load_fixture(path);
	if (!img)
		return;
	CHECK(img->width == 1 && img->height == 1);
	expect_bgra(path, pixel0(*img), b, g, r, 65535, tol);
}

static void
test_loaders_solid()
{
	struct Solid {
		const char *path;
		uint16_t b, g, r, tolerance = 0;
	};
	const Solid solids[] = {
		{"red.png", 0, 0, 65535},
		{"green.png", 0, 65535, 0},
		{"blue.png", 65535, 0, 0},
		{"white.png", 65535, 65535, 65535},
		{"black.png", 0, 0, 0},
		{"red16.png", 0, 0, 65535},
		{"green16.png", 0, 65535, 0},
		{"blue16.png", 65535, 0, 0},
		{"red.bmp", 0, 0, 65535},
		{"green.bmp", 0, 65535, 0},
		{"blue.bmp", 65535, 0, 0},
		{"red.tga", 0, 0, 65535},
		{"green.tga", 0, 65535, 0},
		{"blue.tga", 65535, 0, 0},
		{"red.webp", 0, 0, 65535},
		{"green.webp", 0, 65535, 0},
		{"blue.webp", 65535, 0, 0},
		{"red.jpg", 0, 0, 65535, 514},
		{"green.jpg", 0, 65535, 0, 514},
		{"blue.jpg", 65535, 0, 0, 514},
	};
	for (const Solid &solid : solids)
		test_solid(solid.path, solid.b, solid.g, solid.r, solid.tolerance);

#if DAWN_WITH_LIBTIFF
	test_solid("red.tif", 0, 0, 65535, 0);
	test_solid("green.tif", 0, 65535, 0, 0);
	test_solid("blue.tif", 65535, 0, 0, 0);
#endif
}

static void
test_jpeg_cms_8_to_16()
{
	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);

	const uint8_t src[4] = {0, 0, 255, 255};
	uint8_t dst[8] = {};
	CHECK(cmm->transform_bgra8_to_bgra16(
		src, dst, 1, 1, srgb.get(), srgb.get(), true));
	const uint16_t *p = reinterpret_cast<const uint16_t *>(dst);
	expect_bgra("bgra8→16 premul red", {p[0], p[1], p[2], p[3]}, 0, 0, 65535,
		65535, 257 * 2);

	dawn::OpenContext ctx;
	ctx.cmm = cmm;
	ctx.screen_profile = srgb;
	ctx.first_frame_only = true;
	ctx.uri = dawn::path_to_uri(
		(fs::path(DAWN_TEST_FIXTURES_DIR) / "blue.jpg").string());
	dawn::Error error;
	dawn::ImagePtr img = dawn::open(ctx, &error);
	if (!img) {
		test::fail("jpeg cms blue.jpg: %s", error.message.c_str());
		return;
	}
	expect_bgra("jpeg cms blue.jpg", pixel0(*img), 65535, 0, 0, 65535, 257 * 2);
}

static void
test_jpeg_fatal_error()
{
	const fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / "blue.jpg";
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	CHECK(bytes.size() >= 2);
	if (bytes.size() < 2)
		return;

	// Replace the final EOI with an invalid marker. The header and scanline are
	// valid, so libjpeg reports the fatal error only while finishing the
	// decompression, after the image and temporary pixel buffer have been made.
	CHECK(bytes[bytes.size() - 2] == 0xff && bytes.back() == 0xd9);
	if (bytes[bytes.size() - 2] != 0xff || bytes.back() != 0xd9)
		return;
	bytes.back() = 0x02;

	for (bool enhance : {false, true}) {
		dawn::OpenContext ctx;
		ctx.enhance = enhance;
		ctx.first_frame_only = true;
		dawn::Error error;
		dawn::ImagePtr img = dawn::load_jpeg(bytes, ctx, &error);
		CHECK(img == nullptr);
		CHECK(error);
		CHECK(!error.message.empty());
	}
}

static void
test_cmyk_cms_opaque()
{
	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);

	const fs::path icc = fs::path(DAWN_TEST_FIXTURES_DIR) / "cmyk-lab.icc";
	ifstream input(icc, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	if (bytes.empty()) {
		test::fail("cmyk-lab.icc missing");
		return;
	}
	auto src = cmm->get_profile(bytes);
	CHECK(src != nullptr);

	dawn::ImagePtr img = dawn::image_new(1, 1);
	CHECK(img != nullptr);
	const uint8_t cmyk[4] = {0, 255, 255, 0};
	cmm->convert_cmyk8(
		cmyk, img->data.data(), img->width, img->height, src.get(), srgb.get());
	Pixel p = pixel0(*img);
	CHECK(p.a == 65535);
	CHECK(unsigned(p.b) + p.g + p.r != 0);
}

static void
test_cms_tiled()
{
	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);

	// Above the serial threshold so row bands run on more than one worker.
	constexpr uint32_t w = 600;
	constexpr uint32_t h = 128;
	vector<uint8_t> src(size_t(w) * h * 4);
	for (size_t i = 0; i < src.size(); i += 4) {
		src[i + 0] = 0;
		src[i + 1] = 0;
		src[i + 2] = 255;
		src[i + 3] = 255;
	}
	vector<uint8_t> dst(size_t(w) * h * dawn::kBytesPerPixel, uint8_t(0x5a));
	CHECK(cmm->transform_bgra8_to_bgra16(
		src.data(), dst.data(), w, h, srgb.get(), srgb.get(), true));
	const uint16_t *p = reinterpret_cast<const uint16_t *>(dst.data());
	for (uint32_t i = 0; i < w * h; i++) {
		if (!near_u16(p[i * 4 + 0], 0, 257 * 2) ||
			!near_u16(p[i * 4 + 1], 0, 257 * 2) ||
			!near_u16(p[i * 4 + 2], 65535, 257 * 2) ||
			!near_u16(p[i * 4 + 3], 65535, 0)) {
			test::fail("tiled cms: pixel %u BGRA (%u,%u,%u,%u)", i,
				p[i * 4 + 0], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
			break;
		}
	}
}

static void
test_rgbw_2x2()
{
	dawn::ImagePtr img = load_fixture("rgbw_2x2.png");
	if (!img)
		return;
	CHECK(img->width == 2 && img->height == 2);
	expect_bgra("rgbw[0,0] red", pixel_at(*img, 0, 0), 0, 0, 65535, 65535, 0);
	expect_bgra("rgbw[1,0] green", pixel_at(*img, 1, 0), 0, 65535, 0, 65535, 0);
	expect_bgra("rgbw[0,1] blue", pixel_at(*img, 0, 1), 65535, 0, 0, 65535, 0);
	expect_bgra(
		"rgbw[1,1] white", pixel_at(*img, 1, 1), 65535, 65535, 65535, 65535, 0);
}

static void
test_premul_alpha()
{
	dawn::ImagePtr img = load_fixture("red_a128.png");
	if (!img)
		return;
	// Straight (255,0,0,128) → widen → premul: R=A=128*257=32896, B=G=0
	constexpr uint16_t half = 128u * 257u;
	expect_bgra("red_a128 premul", pixel0(*img), 0, 0, half, half, 1);
}

static void
test_large_icc_and_p3_red()
{
	const fs::path path =
		fs::path(DAWN_TEST_FIXTURES_DIR) / "display-p3-red_vs_srgb-red.png";
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	CHECK(!bytes.empty());

	auto cmm = dawn::Cmm::get_default();
	double whitepoint[2] = {0.3127, 0.3290};
	double adobe_primaries[6] = {
		0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600};
	auto target = cmm->get_profile_parametric(2.2, whitepoint, adobe_primaries);
	CHECK(target != nullptr);

	dawn::OpenContext context;
	context.uri = dawn::path_to_uri(path.string());
	context.cmm = cmm;
	context.screen_profile = target;
	context.first_frame_only = true;
	dawn::Error error;
	dawn::ImagePtr image = dawn::load_wuffs(bytes, context, &error);
	if (!image) {
		test::fail("Wuffs P3 fixture: %s", error.message.c_str());
		return;
	}
	CHECK(image->width == 200 && image->height == 100);
	CHECK(image->icc.size() > 8192);

	const Pixel p3_red = pixel_at(*image, 50, 50);
	const Pixel srgb_red = pixel_at(*image, 150, 50);
	// Both source colors are encoded in the image's Display P3 profile. After
	// conversion to an AdobeRGB-like target, P3 red remains visibly wider-gamut
	// than sRGB red; they must not collapse to the same output pixel.
	CHECK(p3_red.r > srgb_red.r + 2000);
	CHECK(p3_red.g + 1000 < srgb_red.g);
	CHECK(p3_red.b + 1000 < srgb_red.b);
}

static void
test_svg_solid(const char *path, uint16_t b, uint16_t g, uint16_t r)
{
	dawn::ImagePtr img = load_fixture(path);
	if (!img)
		return;
	CHECK(img->width >= 2 && img->height >= 2);
	CHECK(img->render != nullptr);
	// Sample the interior to dodge any residual edge filtering.
	uint32_t x = img->width / 2;
	uint32_t y = img->height / 2;
	constexpr uint16_t tol = 257;
	expect_bgra(path, pixel_at(*img, x, y), b, g, r, 65535, tol);

	dawn::OpenContext ctx;
	dawn::ImagePtr scaled = img->render->render(ctx, 2.0, nullptr);
	if (!scaled) {
		test::fail("%s: render(scale=2) failed", path);
		return;
	}
	CHECK(scaled->width == img->width * 2);
	CHECK(scaled->height == img->height * 2);
	expect_bgra((string(path) + "@2x").c_str(),
		pixel_at(*scaled, scaled->width / 2, scaled->height / 2), b, g, r,
		65535, tol);
}

static void
test_svg()
{
	test_svg_solid("red.svg", 0, 0, 65535);
	test_svg_solid("green.svg", 0, 65535, 0);
	test_svg_solid("blue.svg", 65535, 0, 0);

	dawn::ImagePtr rgbw = load_fixture("rgbw_2x2.svg");
	if (rgbw) {
		CHECK(rgbw->width == 2 && rgbw->height == 2);
		constexpr uint16_t tol = 257 * 2;
		expect_bgra(
			"svg rgbw[0,0]", pixel_at(*rgbw, 0, 0), 0, 0, 65535, 65535, tol);
		expect_bgra(
			"svg rgbw[1,0]", pixel_at(*rgbw, 1, 0), 0, 65535, 0, 65535, tol);
		expect_bgra(
			"svg rgbw[0,1]", pixel_at(*rgbw, 0, 1), 65535, 0, 0, 65535, tol);
		expect_bgra("svg rgbw[1,1]", pixel_at(*rgbw, 1, 1), 65535, 65535, 65535,
			65535, tol);
	}

	// Rerendering colour-manages as the context asks: sRGB red lands well
	// inside Display P3, where it is no longer a primary.
	dawn::ImagePtr red = load_fixture("red.svg");
	if (red && red->render) {
		auto cmm = dawn::Cmm::get_default();
		dawn::OpenContext ctx;
		ctx.cmm = cmm;
		ctx.screen_profile = cmm->get_profile_display_p3();
		dawn::Error error;
		dawn::ImagePtr p3 = red->render->render(ctx, 1.0, &error);
		if (!p3) {
			test::fail("red.svg: P3 render: %s", error.message.c_str());
		} else {
			Pixel px = pixel_at(*p3, p3->width / 2, p3->height / 2);
			CHECK(px.r < 63000);
			CHECK(px.g > 2000);
			CHECK(px.a == 65535);
		}
	}

	dawn::ImagePtr half = load_fixture("red_a128.svg");
	if (half) {
		CHECK(half->render != nullptr);
		// Cairo/resvg premul ~50% red: R≈A≈32768, B=G=0
		constexpr uint16_t mid = 32768;
		constexpr uint16_t tol = 257 * 4;
		expect_bgra("svg red_a128",
			pixel_at(*half, half->width / 2, half->height / 2), 0, 0, mid, mid,
			tol);
	}
}

static vector<uint8_t>
read_fixture(const string &name)
{
	fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / name;
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	if (bytes.empty())
		test::fail("%s: cannot read", path.string().c_str());
	return bytes;
}

static void
test_render_dimensions()
{
	const double nan = numeric_limits<double>::quiet_NaN();
	const double infinity = numeric_limits<double>::infinity();
	const double limit = double(dawn::kMaxDimension);

	struct Rejected {
		const char *label;
		double width, height;
	} rejected[] = {
		{"NaN width", nan, 1},
		{"NaN height", 1, nan},
		{"infinite width", infinity, 1},
		{"infinite height", 1, infinity},
		{"negative infinity", -infinity, 1},
		{"zero", 0, 1},
		{"negative", -1, 1},
		{"over the limit", limit + 1, 1},
		{"a fraction over the limit", 1, limit + 0.5},
	};

	uint32_t width = 123, height = 456;
	for (const Rejected &entry : rejected) {
		dawn::Error error;
		if (dawn::render_dimensions(
				entry.width, entry.height, &width, &height, &error))
			test::fail("render_dimensions: %s accepted", entry.label);
		else
			CHECK(!error.message.empty());
	}

	// Nothing is written on failure, and no error is required either.
	CHECK(width == 123 && height == 456);

	CHECK(dawn::render_dimensions(0.25, limit, &width, &height, nullptr));
	CHECK(width == 1 && height == uint32_t(limit));
	CHECK(dawn::render_dimensions(64., 32.5, &width, &height, nullptr));
	CHECK(width == 64 && height == 33);
}

#if DAWN_WITH_LIBWMF
// A 64x32 placeable metafile at 72 units per inch, drawing one rectangle.
static const uint8_t kWmf[] = {0xd7, 0xcd, 0xc6, 0x9a, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x40, 0x00, 0x20, 0x00, 0x48, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x39, 0x57, 0x01, 0x00, 0x09, 0x00, 0x00, 0x03, 0x1d, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00,
	0x0b, 0x02, 0x00, 0x00, 0x00, 0x00, 0x05, 0x00, 0x00, 0x00, 0x0c, 0x02,
	0x20, 0x00, 0x40, 0x00, 0x07, 0x00, 0x00, 0x00, 0x1b, 0x04, 0x20, 0x00,
	0x40, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00};
#endif

// Loads one vector document with a named backend, then checks that its
// closure rounds fractional sizes up and refuses impossible scales.
static void
test_rerender(const char *label, dawn::LoadFn *load, span<const uint8_t> data,
	const dawn::OpenContext &ctx)
{
	dawn::Error error;
	dawn::ImagePtr image = load(data, ctx, &error);
	if (!image || !image->render) {
		test::fail("%s: %s", label, error.message.c_str());
		return;
	}

	const uint32_t w = image->width, h = image->height;
	dawn::ImagePtr scaled = image->render->render(ctx, 1.5, &error);
	if (!scaled) {
		test::fail("%s: 1.5x: %s", label, error.message.c_str());
	} else if (scaled->width != uint32_t(ceil(w * 1.5)) ||
		scaled->height != uint32_t(ceil(h * 1.5))) {
		test::fail("%s: 1.5x of %ux%u gave %ux%u", label, w, h, scaled->width,
			scaled->height);
	}

	for (double scale : {0., -1., 1e9, numeric_limits<double>::infinity(),
			 numeric_limits<double>::quiet_NaN()}) {
		dawn::Error rejected;
		if (image->render->render(ctx, scale, &rejected))
			test::fail("%s: scale %g accepted", label, scale);
		else
			CHECK(!rejected.message.empty());
	}
}

// A one-page PDF, assembled rather than spelled out, so that its
// cross-reference offsets cannot drift away from what they point at.
static string
minimal_pdf()
{
	const string objects[] = {
		"<< /Type /Catalog /Pages 2 0 R >>",
		"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
		"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 64 32]\n"
		"   /Contents 4 0 R /Resources << >> >>",
		"<< /Length 24 >>\nstream\n1 0 0 rg 0 0 64 32 re f\nendstream",
	};

	string pdf = "%PDF-1.4\n";
	vector<size_t> offsets;
	int number = 0;
	for (const string &object : objects) {
		offsets.push_back(pdf.size());
		pdf += to_string(++number) + " 0 obj\n" + object + "\nendobj\n";
	}

	const string size = to_string(offsets.size() + 1);
	const size_t xref = pdf.size();
	pdf += "xref\n0 " + size + "\n0000000000 65535 f \n";
	for (size_t offset : offsets) {
		// Entries are exactly twenty bytes, hence the trailing space.
		char entry[32] = "";
		snprintf(entry, sizeof entry, "%010zu 00000 n \n", offset);
		pdf += entry;
	}
	pdf += "trailer\n<< /Size " + size + " /Root 1 0 R >>\nstartxref\n" +
		to_string(xref) + "\n%%EOF\n";
	return pdf;
}

static void
test_vector_rerender()
{
	const vector<uint8_t> svg = read_fixture("red.svg");
	if (svg.empty())
		return;

	// Go through each backend directly: loader fallback would otherwise
	// let a broken one hide behind the next.
	dawn::OpenContext svg_ctx;
	svg_ctx.uri = dawn::path_to_uri(
		(fs::path(DAWN_TEST_FIXTURES_DIR) / "red.svg").string());
	test_rerender("resvg", &dawn::load_resvg, svg, svg_ctx);
#if DAWN_WITH_LIBRSVG
	test_rerender("librsvg", &dawn::load_librsvg, svg, svg_ctx);
#endif

#if DAWN_WITH_LIBWMF
	dawn::OpenContext wmf_ctx;
	test_rerender("libwmf", &dawn::load_libwmf, kWmf, wmf_ctx);
#endif

#if DAWN_WITH_POPPLER
	const string pdf = minimal_pdf();
	dawn::OpenContext pdf_ctx;
	// One PDF unit per pixel, so that the page's own size comes back.
	pdf_ctx.screen_dpi = 72;
	pdf_ctx.first_frame_only = true;
	test_rerender("Poppler", &dawn::load_poppler,
		{(const uint8_t *) pdf.data(), pdf.size()}, pdf_ctx);
#endif
}

// The metafile measures itself in inches, so scale 1 is as many pixels as the
// screen puts in one, rather than libwmf's own 72 units per inch.
static void
test_wmf_dpi()
{
#if DAWN_WITH_LIBWMF
	for (int dpi : {72, 144}) {
		dawn::OpenContext ctx;
		ctx.screen_dpi = dpi;
		dawn::Error error;
		dawn::ImagePtr image = dawn::load_libwmf(kWmf, ctx, &error);
		if (!image) {
			test::fail("WMF at %d DPI: %s", dpi, error.message.c_str());
			continue;
		}
		const uint32_t scale = uint32_t(dpi) / 72;
		if (image->width != 64 * scale || image->height != 32 * scale) {
			test::fail(
				"WMF at %d DPI: %ux%u", dpi, image->width, image->height);
		}
	}
#endif
}

static void
near_xy(const char *label, double x, double y, double xe, double ye, double tol)
{
	if (fabs(x - xe) > tol || fabs(y - ye) > tol) {
		test::fail(
			"%s: xy=(%.5f, %.5f) expected (~%.5f, ~%.5f)", label, x, y, xe, ye);
	}
}

static void
test_chromaticities()
{
	CHECK(dawn::profile_chromaticities(nullptr).model ==
		dawn::ColorModel::Unknown);

	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);
	dawn::Chromaticities s = dawn::profile_chromaticities(srgb.get());
	CHECK(s.model == dawn::ColorModel::Rgb);
	CHECK(s.have_primaries);
	CHECK(s.n == 3);
	near_xy("sRGB R", s.x[0], s.y[0], 0.6400, 0.3300, 0.002);
	near_xy("sRGB G", s.x[1], s.y[1], 0.3000, 0.6000, 0.002);
	near_xy("sRGB B", s.x[2], s.y[2], 0.1500, 0.0600, 0.002);
	CHECK(s.have_white);
	near_xy("sRGB W", s.wx, s.wy, 0.3127, 0.3290, 0.002);

	auto display_p3 = cmm->get_profile_display_p3();
	CHECK(display_p3 != nullptr);
	CHECK(dawn::profile_transfer(display_p3.get()) == dawn::Transfer::Srgb);
	dawn::Chromaticities d = dawn::profile_chromaticities(display_p3.get());
	CHECK(d.have_primaries && d.n == 3);
	near_xy("Display P3 R", d.x[0], d.y[0], 0.6800, 0.3200, 0.002);
	near_xy("Display P3 G", d.x[1], d.y[1], 0.2650, 0.6900, 0.002);
	near_xy("Display P3 B", d.x[2], d.y[2], 0.1500, 0.0600, 0.002);

	double wp[2] = {0.3127, 0.3290};
	double adobe[6] = {0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600};
	auto ad = cmm->get_profile_parametric(2.2, wp, adobe);
	CHECK(ad != nullptr);
	dawn::Chromaticities a = dawn::profile_chromaticities(ad.get());
	CHECK(a.have_primaries && a.n == 3);
	near_xy("Adobe G", a.x[1], a.y[1], 0.2100, 0.7100, 0.002);

	dawn::ImagePtr red = load_fixture("red.png");
	if (red) {
		CHECK(red->effective_profile != nullptr);
		CHECK(red->profile_assumed);
		dawn::Chromaticities e =
			dawn::profile_chromaticities(red->effective_profile.get());
		CHECK(e.have_primaries && e.n == 3);
		near_xy("assumed sRGB R", e.x[0], e.y[0], 0.6400, 0.3300, 0.002);
	}

	const fs::path p3 =
		fs::path(DAWN_TEST_FIXTURES_DIR) / "display-p3-red_vs_srgb-red.png";
	ifstream input(p3, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	if (!bytes.empty()) {
		dawn::OpenContext ctx;
		ctx.uri = dawn::path_to_uri(p3.string());
		ctx.cmm = cmm;
		ctx.first_frame_only = true;
		dawn::Error error;
		dawn::ImagePtr img = dawn::load_wuffs(bytes, ctx, &error);
		if (img && img->effective_profile) {
			CHECK(!img->profile_assumed);
			dawn::Chromaticities p =
				dawn::profile_chromaticities(img->effective_profile.get());
			CHECK(p.have_primaries && p.n == 3);
			near_xy("P3 R", p.x[0], p.y[0], 0.680, 0.320, 0.01);
			near_xy("P3 G", p.x[1], p.y[1], 0.265, 0.690, 0.01);
		} else {
			test::fail("P3 effective_profile missing");
		}
	}
}

// Each fixture would come out with a different transfer function if the loader
// ranked its colour chunks differently.  iCCP, which outranks all of these, is
// covered by the Display P3 file in test_chromaticities().
static void
test_png_colour_chunk(
	const char *name, dawn::Transfer transfer, double red_x, double red_y)
{
	dawn::ImagePtr image = load_fixture(name);
	if (!image)
		return;

	CHECK(!image->profile_assumed);
	if (dawn::profile_transfer(image->effective_profile.get()) != transfer)
		test::fail("%s: unexpected transfer function", name);

	dawn::Chromaticities c =
		dawn::profile_chromaticities(image->effective_profile.get());
	CHECK(c.have_primaries && c.n == 3);
	if (c.n == 3)
		near_xy(name, c.x[0], c.y[0], red_x, red_y, 0.002);
}

static void
test_png_colour_chunks()
{
	// The first file also carries a gAMA of 1.0, which the sRGB chunk beats.
	test_png_colour_chunk("srgb-chunk.png", dawn::Transfer::Srgb, 0.64, 0.33);
	test_png_colour_chunk("gama22.png", dawn::Transfer::AdobeRgb, 0.64, 0.33);
	test_png_colour_chunk("chrm-p3.png", dawn::Transfer::Srgb, 0.68, 0.32);
	test_png_colour_chunk(
		"chrm-p3-gama1.png", dawn::Transfer::Linear, 0.68, 0.32);
}

#if DAWN_WITH_LIBTIFF

static dawn::ImagePtr
load_tiff_fixture(const char *name, const shared_ptr<dawn::Cmm> &cmm,
	const shared_ptr<dawn::Profile> &screen, vector<string> *warnings)
{
	const vector<uint8_t> bytes = read_fixture(name);
	if (bytes.empty())
		return nullptr;

	// The format dispatch would give load_tiff_ep() and LibRaw a go first.
	dawn::OpenContext ctx;
	ctx.uri =
		dawn::path_to_uri((fs::path(DAWN_TEST_FIXTURES_DIR) / name).string());
	ctx.cmm = cmm;
	ctx.screen_profile = screen;
	ctx.warnings = warnings;
	dawn::Error error;
	dawn::ImagePtr image = dawn::load_tiff(bytes, ctx, &error);
	if (!image)
		test::fail("%s: %s", name, error.message.c_str());
	return image;
}

// A TIFF stating its colour numerically must not come out as invented sRGB.
static void
test_tiff_colour(
	const char *name, dawn::Transfer transfer, double green_x, double green_y)
{
	dawn::ImagePtr image =
		load_tiff_fixture(name, dawn::Cmm::get_default(), nullptr, nullptr);
	if (!image)
		return;

	CHECK(!image->profile_assumed);
	if (dawn::profile_transfer(image->effective_profile.get()) != transfer)
		test::fail("%s: unexpected transfer function", name);

	dawn::Chromaticities c =
		dawn::profile_chromaticities(image->effective_profile.get());
	CHECK(c.have_primaries && c.n == 3);
	if (c.n != 3)
		return;
	near_xy(name, c.x[0], c.y[0], 0.64, 0.33, 0.002);
	near_xy(name, c.x[1], c.y[1], green_x, green_y, 0.002);
}

static void
test_tiff_colorimetry()
{
	auto cmm = dawn::Cmm::get_default();

	// Both decoding paths: 16-bit scanlines, and TIFFRGBAImage.
	test_tiff_colour("adobergb16.tif", dawn::Transfer::Srgb, 0.21, 0.71);
	test_tiff_colour("adobergb8.tif", dawn::Transfer::Srgb, 0.21, 0.71);
	test_tiff_colour("exif-gamma.tif", dawn::Transfer::AdobeRgb, 0.21, 0.71);
	test_tiff_colour("exif-srgb.tif", dawn::Transfer::Srgb, 0.30, 0.60);
	test_tiff_colour("palette-transfer.tif", dawn::Transfer::Srgb, 0.21, 0.71);

	// Chromaticities outside an RGB raster describe nothing about it, and
	// the solid fixtures carry no colorimetry at all.
	for (const char *name : {"grey-primaries.tif", "red.tif"}) {
		dawn::ImagePtr image = load_tiff_fixture(name, cmm, nullptr, nullptr);
		if (image)
			CHECK(image->profile_assumed);
	}

	// A stated sRGB moves no pixel; a wider gamut has to move several.
	auto srgb = cmm->get_profile_sRGB();
	if (dawn::ImagePtr i =
			load_tiff_fixture("exif-srgb.tif", cmm, srgb, nullptr))
		expect_bgra("exif-srgb.tif", pixel0(*i), 96 * 257, 128 * 257, 192 * 257,
			65535, 64);
	if (dawn::ImagePtr i =
			load_tiff_fixture("adobergb16.tif", cmm, srgb, nullptr)) {
		// The red channel carries the conversion, and stays in gamut,
		// so that clipping cannot stand in for it.
		Pixel p = pixel0(*i);
		if (near_u16(p.r, 192 * 257, 2000) || p.r >= 65535)
			test::fail("adobergb16.tif: unconverted BGRA (%u,%u,%u,%u)", p.b,
				p.g, p.r, p.a);
	}

	// TransferFunction tables, over the whole range the bit depth encodes.
	const struct {
		const char *name;
		double gamma[3];
	} tabulated[] = {
		{"transfer8.tif", {1.0, 1.0, 1.0}},
		{"transfer16.tif", {1.5, 2.0, 3.0}},
	};
	for (const auto &t : tabulated) {
		dawn::ImagePtr image = load_tiff_fixture(t.name, cmm, nullptr, nullptr);
		if (!image)
			continue;
		CHECK(!image->profile_assumed);
		const auto e = dawn::profile_encoding(image->effective_profile.get());
		CHECK(e.matrix_trc);
		for (size_t c = 0; c < 3; c++)
			CHECK(abs(e.decode[2048][c] - pow(.5, t.gamma[c])) < .001);
	}

	// A palette image's three curves cannot be had from libtiff, so the
	// profile falls back to sRGB rather than repeating the one it gets.
	if (dawn::ImagePtr i =
			load_tiff_fixture("palette-transfer.tif", cmm, nullptr, nullptr)) {
		const auto e = dawn::profile_encoding(i->effective_profile.get());
		CHECK(e.matrix_trc);
		for (size_t c = 0; c < 3; c++)
			CHECK(abs(e.decode[2048][c] - .21404114) < .001);
	}

	// An unreadable Exif is missing metadata, not a lost file: every page
	// must survive it, with its pixels, and the good one in the middle has
	// to leave the page loop where it found it.
	vector<string> warnings;
	dawn::ImagePtr pages =
		load_tiff_fixture("exif-pages.tif", cmm, nullptr, &warnings);
	CHECK(!warnings.empty());
	if (!pages)
		return;

	const struct {
		const char *label;
		uint16_t b, g, r;
		bool assumed;
	} expected[] = {
		{"exif-pages.tif unreadable", 96 * 257, 128 * 257, 192 * 257, true},
		{"exif-pages.tif valid", 192 * 257, 128 * 257, 96 * 257, false},
		{"exif-pages.tif malformed", 96 * 257, 192 * 257, 128 * 257, true},
	};
	size_t page = 0;
	for (dawn::Image *p = pages.get(); p; p = p->page_next.get(), page++) {
		if (page >= size(expected)) {
			test::fail("exif-pages.tif: more than %zu pages", size(expected));
			break;
		}
		CHECK(p->profile_assumed == expected[page].assumed);
		expect_bgra(expected[page].label, pixel0(*p), expected[page].b,
			expected[page].g, expected[page].r, 65535, 0);
	}
	CHECK(page == size(expected));
}

#endif  // DAWN_WITH_LIBTIFF

static void
test_png_text_after_idat()
{
	fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / "text-after-idat.png";
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	CHECK(!bytes.empty());

	dawn::OpenContext ctx;
	ctx.uri = dawn::path_to_uri(path.string());
	ctx.first_frame_only = false;
	dawn::Error error;
	dawn::ImagePtr image = dawn::open_from_data(bytes, ctx, &error);
	if (!image) {
		test::fail("text-after-idat: %s", error.message.c_str());
		return;
	}
	CHECK(!error);
	CHECK(image->width == 1 && image->height == 1);
	auto it = image->text.find("prompt");
	CHECK(it != image->text.end());
	if (it != image->text.end())
		CHECK(it->second == "hello");
}

static void
test_profile_transfer()
{
	CHECK(dawn::profile_transfer(nullptr) == dawn::Transfer::Srgb);

	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);
	CHECK(dawn::profile_transfer(srgb.get()) == dawn::Transfer::Srgb);

	auto g22 = cmm->get_profile_sRGB_gamma(2.2);
	CHECK(g22 != nullptr);
	CHECK(dawn::profile_transfer(g22.get()) == dawn::Transfer::AdobeRgb);

	auto lin = cmm->get_profile_sRGB_gamma(1.0);
	CHECK(lin != nullptr);
	CHECK(dawn::profile_transfer(lin.get()) == dawn::Transfer::Linear);

	auto g18 = cmm->get_profile_sRGB_gamma(1.8);
	CHECK(g18 != nullptr);
	CHECK(dawn::profile_transfer(g18.get()) == dawn::Transfer::Srgb);
}

static void
test_profile_encoding()
{
	auto cmm = make_shared<dawn::Cmm>();
	for (double gamma : {1.0, 1.8, 2.2, 2.6}) {
		auto profile = cmm->get_profile_sRGB_gamma(gamma);
		const auto encoding = dawn::profile_encoding(profile.get());
		CHECK(encoding.matrix_trc);
		CHECK(abs(encoding.decode[2048][0] - pow(.5, gamma)) < .0001);
		CHECK(abs(encoding.encode[2048][0] - pow(.5, 1 / gamma)) < .0001);
	}
	const auto srgb = dawn::profile_encoding(cmm->get_profile_sRGB().get());
	const auto p3 = dawn::profile_encoding(cmm->get_profile_display_p3().get());
	CHECK(srgb.matrix_trc);
	CHECK(p3.matrix_trc);
	CHECK(abs(srgb.decode[2048][0] - .21404114) < .0001);
	CHECK(abs(srgb.encode[2048][0] - .73535698) < .0001);
	CHECK(abs(p3.decode[2048][0] - srgb.decode[2048][0]) < .0001);
	CHECK(abs(p3.encode[2048][0] - srgb.encode[2048][0]) < .0001);
	CHECK(abs(p3.rgb_to_xyz[0][0] - srgb.rgb_to_xyz[0][0]) > .05);
	CHECK(!dawn::profile_encoding(nullptr).matrix_trc);

	// Mixed TRCs must not collapse to a single guessed gamma.
	cmsHPROFILE mixed = cmsCreate_sRGBProfile();
	cmsSetProfileVersion(mixed, 4.3);
	const cmsTagSignature tags[] = {
		cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
	const double gammas[] = {1.3, 1.8, 2.6};
	for (int c = 0; c < 3; c++) {
		cmsToneCurve *curve = cmsBuildGamma(nullptr, gammas[c]);
		CHECK(cmsWriteTag(mixed, tags[c], curve));
		cmsFreeToneCurve(curve);
	}
	auto import = [&] {
		cmsUInt32Number size = 0;
		CHECK(cmsSaveProfileToMem(mixed, nullptr, &size));
		vector<uint8_t> bytes(size);
		CHECK(cmsSaveProfileToMem(mixed, bytes.data(), &size));
		return cmm->get_profile(bytes);
	};
	const auto curves = dawn::profile_encoding(import().get());
	CHECK(curves.matrix_trc);
	for (size_t c = 0; c < 3; c++) {
		CHECK(abs(curves.decode[2048][c] - pow(.5, gammas[c])) < .0001);
		CHECK(abs(curves.encode[2048][c] - pow(.5, 1 / gammas[c])) < .0001);
	}

	// Even with valid matrix/TRC tags, a LUT profile is explicitly approximate.
	cmsSetProfileVersion(mixed, 2.1);
	cmsPipeline *lut = cmsPipelineAlloc(nullptr, 3, 3);
	cmsUInt16Number table[24] = {};
	CHECK(cmsPipelineInsertStage(
		lut, cmsAT_END, cmsStageAllocCLut16bit(nullptr, 2, 3, 3, table)));
	CHECK(cmsWriteTag(mixed, cmsSigAToB0Tag, lut));
	cmsPipelineFree(lut);
	const auto fallback = dawn::profile_encoding(import().get());
	CHECK(!fallback.matrix_trc);
	CHECK(fallback.decode == dawn::profile_encoding(nullptr).decode);
	cmsCloseProfile(mixed);
}

// White lands on white, and each display colourant on its own primary.
static void
test_display_matrices()
{
	auto cmm = make_shared<dawn::Cmm>();
	const auto srgb = dawn::profile_encoding(cmm->get_profile_sRGB().get());
	const auto p3 = dawn::profile_encoding(cmm->get_profile_display_p3().get());
	const double srgb_xy[6] = {.64, .33, .30, .60, .15, .06};
	const double bt2020_xy[6] = {.708, .292, .170, .797, .131, .046};

	const dawn::RgbMatrix colourants = dawn::display_colourants_d65(srgb);
	for (int c = 0; c < 3; c++) {
		const double sum =
			colourants[c][0] + colourants[c][1] + colourants[c][2];
		CHECK(abs(colourants[c][0] / sum - srgb_xy[c * 2]) < .001);
		CHECK(abs(colourants[c][1] / sum - srgb_xy[c * 2 + 1]) < .001);
	}

	const dawn::RgbMatrix identity = dawn::display_to_primaries(srgb, srgb_xy);
	for (int c = 0; c < 3; c++)
		for (int r = 0; r < 3; r++)
			CHECK(abs(identity[c][r] - (c == r)) < .002);

	// Display P3 red leaves sRGB, as scRGB lets it, but not BT.2020, whose
	// conversion from P3 is only a hair negative, in blue from red.
	const dawn::RgbMatrix to_srgb = dawn::display_to_primaries(p3, srgb_xy);
	CHECK(to_srgb[0][0] > 1.1 && to_srgb[0][1] < 0 && to_srgb[0][2] < 0);
	const dawn::RgbMatrix to_bt2020 = dawn::display_to_primaries(p3, bt2020_xy);
	for (int r = 0; r < 3; r++) {
		double white = 0;
		for (int c = 0; c < 3; c++) {
			CHECK(to_bt2020[c][r] > -.002);
			white += to_bt2020[c][r];
		}
		CHECK(abs(white - 1) < .002);
	}
}

int
main()
{
	return test::run({
		{"packing", test_pack_helpers},
		{"unpremultiply alpha last", test_unpremultiply_alpha_last8},
		{"finishing", test_finishing},
		{"solid image loaders", test_loaders_solid},
		{"JPEG CMS", test_jpeg_cms_8_to_16},
		{"JPEG fatal error", test_jpeg_fatal_error},
		{"CMYK CMS", test_cmyk_cms_opaque},
		{"tiled CMS", test_cms_tiled},
		{"RGBW image", test_rgbw_2x2},
		{"premultiplied alpha", test_premul_alpha},
		{"large ICC profile", test_large_icc_and_p3_red},
		{"SVG rendering", test_svg},
		{"render dimensions", test_render_dimensions},
		{"vector rerendering", test_vector_rerender},
		{"WMF display size", test_wmf_dpi},
		{"chromaticities", test_chromaticities},
		{"PNG text", test_png_text_after_idat},
		{"PNG colour chunks", test_png_colour_chunks},
#if DAWN_WITH_LIBTIFF
		{"TIFF colorimetry", test_tiff_colorimetry},
#endif
		{"profile transfer", test_profile_transfer},
		{"profile encoding", test_profile_encoding},
		{"display matrices", test_display_matrices},
	});
}
