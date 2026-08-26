//
// test-channels.cpp: verify BGRA_PREMUL_4X16LE channel order
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace std;
namespace fs = filesystem;

#ifndef DAWN_TEST_FIXTURES_DIR
#error DAWN_TEST_FIXTURES_DIR must be defined
#endif

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
				__LINE__);                                                     \
			++g_failures;                                                      \
		}                                                                      \
	} while (0)

bool
near_u16(uint16_t a, uint16_t b, uint16_t tol)
{
	return abs(int(a) - int(b)) <= int(tol);
}

struct Pixel {
	uint16_t b = 0, g = 0, r = 0, a = 0;
};

Pixel
pixel0(const dn::Image &img)
{
	const uint16_t *p = dn::row_u16(img, 0);
	return {p[0], p[1], p[2], p[3]};
}

Pixel
pixel_at(const dn::Image &img, uint32_t x, uint32_t y)
{
	const uint16_t *p = dn::row_u16(img, y) + x * 4;
	return {p[0], p[1], p[2], p[3]};
}

void
expect_bgra(const char *label, Pixel p, uint16_t b, uint16_t g, uint16_t r,
	uint16_t a, uint16_t tol)
{
	if (!near_u16(p.b, b, tol) || !near_u16(p.g, g, tol) ||
		!near_u16(p.r, r, tol) || !near_u16(p.a, a, tol)) {
		fprintf(stderr,
			"%s: got BGRA (%u,%u,%u,%u) expected (~%u,~%u,~%u,~%u) tol=%u\n",
			label, p.b, p.g, p.r, p.a, b, g, r, a, tol);
		++g_failures;
	}
}

dn::ImagePtr
load_fixture(const string &name)
{
	fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / name;
	dn::OpenContext ctx;
	ctx.uri = path.string();
	ctx.first_frame_only = true;
	dn::Error error;
	dn::ImagePtr img = dn::open(ctx, &error);
	if (!img) {
		fprintf(stderr, "open(%s): %s\n", path.string().c_str(),
			error.message.c_str());
		++g_failures;
	}
	return img;
}

void
test_pack_helpers()
{
	dn::ImagePtr img = dn::image_new(1, 1);
	CHECK(img != nullptr);

	// RGBA8 red → B=0 G=0 R=65535 A=65535
	{
		const uint8_t rgba[] = {255, 0, 0, 255};
		dn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}
	{
		const uint8_t rgba[] = {0, 255, 0, 255};
		dn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 green", pixel0(*img), 0, 65535, 0, 65535, 0);
	}
	{
		const uint8_t rgba[] = {0, 0, 255, 255};
		dn::pack_rgba8_to_bgra16(*img, rgba, 4);
		expect_bgra("pack_rgba8 blue", pixel0(*img), 65535, 0, 0, 65535, 0);
	}

	{
		const uint8_t rgb[] = {255, 0, 0};
		dn::pack_rgb8_to_bgra16(*img, rgb, 3);
		expect_bgra("pack_rgb8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		// Host-endian 0xAARRGGBB: opaque red
		const uint32_t word = 0xFFFF0000u;
		dn::pack_argb32_words_to_bgra16(*img, &word, sizeof(word));
		expect_bgra("pack_argb32 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		const uint8_t bgra8[] = {0, 0, 255, 255};  // B,G,R,A
		dn::widen_bgra8_to_bgra16(*img, bgra8, 4);
		expect_bgra("widen_bgra8 red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}

	{
		// LE R,G,B,A uint16 — 10-bit max red in low 10 bits → scaled
		const uint16_t rgba16[] = {1023, 0, 0, 1023};
		dn::pack_rgba16le_to_bgra16(*img, rgba16, 8, 10);
		expect_bgra(
			"pack_rgba16le 10-bit red", pixel0(*img), 0, 0, 65535, 65535, 0);
	}
	{
		const uint16_t rgb16[] = {0, 0, 65535};
		dn::pack_rgb16le_to_bgra16(*img, rgb16, 6, 16);
		expect_bgra("pack_rgb16le blue", pixel0(*img), 65535, 0, 0, 65535, 0);
	}
}

void
test_solid(const char *path, uint16_t b, uint16_t g, uint16_t r, uint16_t tol)
{
	dn::ImagePtr img = load_fixture(path);
	if (!img)
		return;
	CHECK(img->width == 1 && img->height == 1);
	expect_bgra(path, pixel0(*img), b, g, r, 65535, tol);
}

void
test_loaders_solid()
{
	// Lossless / exact formats
	constexpr uint16_t exact = 0;
	test_solid("red.png", 0, 0, 65535, exact);
	test_solid("green.png", 0, 65535, 0, exact);
	test_solid("blue.png", 65535, 0, 0, exact);
	test_solid("white.png", 65535, 65535, 65535, exact);
	test_solid("black.png", 0, 0, 0, exact);

	test_solid("red16.png", 0, 0, 65535, exact);
	test_solid("green16.png", 0, 65535, 0, exact);
	test_solid("blue16.png", 65535, 0, 0, exact);

	test_solid("red.bmp", 0, 0, 65535, exact);
	test_solid("green.bmp", 0, 65535, 0, exact);
	test_solid("blue.bmp", 65535, 0, 0, exact);

	test_solid("red.tga", 0, 0, 65535, exact);
	test_solid("green.tga", 0, 65535, 0, exact);
	test_solid("blue.tga", 65535, 0, 0, exact);

	test_solid("red.webp", 0, 0, 65535, exact);
	test_solid("green.webp", 0, 65535, 0, exact);
	test_solid("blue.webp", 65535, 0, 0, exact);

#if DAWN_WITH_LIBTIFF
	test_solid("red.tif", 0, 0, 65535, exact);
	test_solid("green.tif", 0, 65535, 0, exact);
	test_solid("blue.tif", 65535, 0, 0, exact);
#endif

	// JPEG may round-trip with tiny error even at q=100 / 4:4:4.
	constexpr uint16_t jpeg_tol = 257 * 2;
	test_solid("red.jpg", 0, 0, 65535, jpeg_tol);
	test_solid("green.jpg", 0, 65535, 0, jpeg_tol);
	test_solid("blue.jpg", 65535, 0, 0, jpeg_tol);
}

void
test_jpeg_cms_8_to_16()
{
	auto cmm = dn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);

	const uint8_t src[4] = {0, 0, 255, 255};
	uint8_t dst[8] = {};
	CHECK(cmm->transform_bgra8_to_bgra16(
		src, dst, 1, 1, srgb.get(), srgb.get(), true));
	const uint16_t *p = reinterpret_cast<const uint16_t *>(dst);
	expect_bgra("bgra8→16 premul red", {p[0], p[1], p[2], p[3]}, 0, 0, 65535,
		65535, 257 * 2);

	dn::OpenContext ctx;
	ctx.cmm = cmm;
	ctx.screen_profile = srgb;
	ctx.first_frame_only = true;
	ctx.uri = (fs::path(DAWN_TEST_FIXTURES_DIR) / "blue.jpg").string();
	dn::Error error;
	dn::ImagePtr img = dn::open(ctx, &error);
	if (!img) {
		fprintf(stderr, "jpeg cms blue.jpg: %s\n", error.message.c_str());
		++g_failures;
		return;
	}
	expect_bgra("jpeg cms blue.jpg", pixel0(*img), 65535, 0, 0, 65535, 257 * 2);
}

void
test_cmyk_cms_opaque()
{
	auto cmm = dn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);

	const fs::path icc = fs::path(DAWN_TEST_FIXTURES_DIR) / "cmyk-lab.icc";
	ifstream input(icc, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	if (bytes.empty()) {
		fprintf(stderr, "cmyk-lab.icc missing\n");
		++g_failures;
		return;
	}
	auto src = cmm->get_profile(bytes);
	CHECK(src != nullptr);

	dn::ImagePtr img = dn::image_new(1, 1);
	CHECK(img != nullptr);
	const uint8_t cmyk[4] = {0, 255, 255, 0};
	cmm->convert_cmyk8(*img, cmyk, src.get(), srgb.get());
	Pixel p = pixel0(*img);
	if (p.a != 65535) {
		fprintf(stderr, "cmyk cms: alpha %u (premul-zero trap)\n", p.a);
		++g_failures;
	}
	if (unsigned(p.b) + p.g + p.r == 0) {
		fprintf(stderr, "cmyk cms: RGB all zero\n");
		++g_failures;
	}
}

void
test_cms_tiled()
{
	auto cmm = dn::Cmm::get_default();
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
	vector<uint8_t> dst(size_t(w) * h * dn::kBytesPerPixel, uint8_t(0x5a));
	CHECK(cmm->transform_bgra8_to_bgra16(
		src.data(), dst.data(), w, h, srgb.get(), srgb.get(), true));
	const uint16_t *p = reinterpret_cast<const uint16_t *>(dst.data());
	for (uint32_t i = 0; i < w * h; i++) {
		if (!near_u16(p[i * 4 + 0], 0, 257 * 2) ||
			!near_u16(p[i * 4 + 1], 0, 257 * 2) ||
			!near_u16(p[i * 4 + 2], 65535, 257 * 2) ||
			!near_u16(p[i * 4 + 3], 65535, 0)) {
			fprintf(stderr, "tiled cms: pixel %u BGRA (%u,%u,%u,%u)\n", i,
				p[i * 4 + 0], p[i * 4 + 1], p[i * 4 + 2], p[i * 4 + 3]);
			++g_failures;
			break;
		}
	}
}

void
test_rgbw_2x2()
{
	dn::ImagePtr img = load_fixture("rgbw_2x2.png");
	if (!img)
		return;
	CHECK(img->width == 2 && img->height == 2);
	expect_bgra("rgbw[0,0] red", pixel_at(*img, 0, 0), 0, 0, 65535, 65535, 0);
	expect_bgra("rgbw[1,0] green", pixel_at(*img, 1, 0), 0, 65535, 0, 65535, 0);
	expect_bgra("rgbw[0,1] blue", pixel_at(*img, 0, 1), 65535, 0, 0, 65535, 0);
	expect_bgra(
		"rgbw[1,1] white", pixel_at(*img, 1, 1), 65535, 65535, 65535, 65535, 0);
}

void
test_premul_alpha()
{
	dn::ImagePtr img = load_fixture("red_a128.png");
	if (!img)
		return;
	// Straight (255,0,0,128) → widen → premul: R=A=128*257=32896, B=G=0
	constexpr uint16_t half = 128u * 257u;
	expect_bgra("red_a128 premul", pixel0(*img), 0, 0, half, half, 1);
}

void
test_large_icc_and_p3_red()
{
	const fs::path path =
		fs::path(DAWN_TEST_FIXTURES_DIR) / "display-p3-red_vs_srgb-red.png";
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	CHECK(!bytes.empty());

	auto cmm = dn::Cmm::get_default();
	double whitepoint[2] = {0.3127, 0.3290};
	double adobe_primaries[6] = {
		0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600};
	auto target = cmm->get_profile_parametric(2.2, whitepoint, adobe_primaries);
	CHECK(target != nullptr);

	dn::OpenContext context;
	context.uri = path.string();
	context.cmm = cmm;
	context.screen_profile = target;
	context.first_frame_only = true;
	dn::Error error;
	dn::ImagePtr image = dn::detail::load_wuffs(bytes, context, &error);
	if (!image) {
		fprintf(stderr, "Wuffs P3 fixture: %s\n", error.message.c_str());
		++g_failures;
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

void
test_svg_solid(const char *path, uint16_t b, uint16_t g, uint16_t r)
{
	dn::ImagePtr img = load_fixture(path);
	if (!img)
		return;
	CHECK(img->width >= 2 && img->height >= 2);
	CHECK(img->render != nullptr);
	// Sample the interior to dodge any residual edge filtering.
	uint32_t x = img->width / 2;
	uint32_t y = img->height / 2;
	constexpr uint16_t tol = 257;
	expect_bgra(path, pixel_at(*img, x, y), b, g, r, 65535, tol);

	dn::ImagePtr scaled = img->render->render(nullptr, nullptr, 2.0);
	if (!scaled) {
		fprintf(stderr, "%s: render(scale=2) failed\n", path);
		++g_failures;
		return;
	}
	CHECK(scaled->width == img->width * 2);
	CHECK(scaled->height == img->height * 2);
	expect_bgra((string(path) + "@2x").c_str(),
		pixel_at(*scaled, scaled->width / 2, scaled->height / 2), b, g, r,
		65535, tol);
}

void
test_svg()
{
	test_svg_solid("red.svg", 0, 0, 65535);
	test_svg_solid("green.svg", 0, 65535, 0);
	test_svg_solid("blue.svg", 65535, 0, 0);

	dn::ImagePtr rgbw = load_fixture("rgbw_2x2.svg");
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

	dn::ImagePtr half = load_fixture("red_a128.svg");
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

void
near_xy(const char *label, double x, double y, double xe, double ye, double tol)
{
	if (fabs(x - xe) > tol || fabs(y - ye) > tol) {
		fprintf(stderr, "%s: xy=(%.5f, %.5f) expected (~%.5f, ~%.5f)\n", label,
			x, y, xe, ye);
		++g_failures;
	}
}

void
test_chromaticities()
{
	CHECK(dn::profile_chromaticities(nullptr).model == dn::ColorModel::Unknown);

	auto cmm = dn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);
	dn::Chromaticities s = dn::profile_chromaticities(srgb.get());
	CHECK(s.model == dn::ColorModel::Rgb);
	CHECK(s.have_primaries);
	CHECK(s.n == 3);
	near_xy("sRGB R", s.x[0], s.y[0], 0.6400, 0.3300, 0.002);
	near_xy("sRGB G", s.x[1], s.y[1], 0.3000, 0.6000, 0.002);
	near_xy("sRGB B", s.x[2], s.y[2], 0.1500, 0.0600, 0.002);
	CHECK(s.have_white);
	near_xy("sRGB W", s.wx, s.wy, 0.3127, 0.3290, 0.002);

	auto display_p3 = cmm->get_profile_display_p3();
	CHECK(display_p3 != nullptr);
	CHECK(dn::profile_transfer(display_p3.get()) == dn::Transfer::Srgb);
	dn::Chromaticities d = dn::profile_chromaticities(display_p3.get());
	CHECK(d.have_primaries && d.n == 3);
	near_xy("Display P3 R", d.x[0], d.y[0], 0.6800, 0.3200, 0.002);
	near_xy("Display P3 G", d.x[1], d.y[1], 0.2650, 0.6900, 0.002);
	near_xy("Display P3 B", d.x[2], d.y[2], 0.1500, 0.0600, 0.002);

	double wp[2] = {0.3127, 0.3290};
	double adobe[6] = {0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600};
	auto ad = cmm->get_profile_parametric(2.2, wp, adobe);
	CHECK(ad != nullptr);
	dn::Chromaticities a = dn::profile_chromaticities(ad.get());
	CHECK(a.have_primaries && a.n == 3);
	near_xy("Adobe G", a.x[1], a.y[1], 0.2100, 0.7100, 0.002);

	dn::ImagePtr red = load_fixture("red.png");
	if (red) {
		CHECK(red->effective_profile != nullptr);
		CHECK(red->profile_assumed);
		dn::Chromaticities e =
			dn::profile_chromaticities(red->effective_profile.get());
		CHECK(e.have_primaries && e.n == 3);
		near_xy("assumed sRGB R", e.x[0], e.y[0], 0.6400, 0.3300, 0.002);
	}

	const fs::path p3 =
		fs::path(DAWN_TEST_FIXTURES_DIR) / "display-p3-red_vs_srgb-red.png";
	ifstream input(p3, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	if (!bytes.empty()) {
		dn::OpenContext ctx;
		ctx.uri = p3.string();
		ctx.cmm = cmm;
		ctx.first_frame_only = true;
		dn::Error error;
		dn::ImagePtr img = dn::detail::load_wuffs(bytes, ctx, &error);
		if (img && img->effective_profile) {
			CHECK(!img->profile_assumed);
			dn::Chromaticities p =
				dn::profile_chromaticities(img->effective_profile.get());
			CHECK(p.have_primaries && p.n == 3);
			near_xy("P3 R", p.x[0], p.y[0], 0.680, 0.320, 0.01);
			near_xy("P3 G", p.x[1], p.y[1], 0.265, 0.690, 0.01);
		} else {
			fprintf(stderr, "P3 effective_profile missing\n");
			++g_failures;
		}
	}
}

void
test_png_text_after_idat()
{
	fs::path path = fs::path(DAWN_TEST_FIXTURES_DIR) / "text-after-idat.png";
	ifstream input(path, ios::binary);
	vector<uint8_t> bytes(
		(istreambuf_iterator<char>(input)), istreambuf_iterator<char>{});
	CHECK(!bytes.empty());

	dn::OpenContext ctx;
	ctx.uri = path.string();
	ctx.first_frame_only = false;
	dn::Error error;
	dn::ImagePtr image = dn::open_from_data(bytes, ctx, &error);
	if (!image) {
		fprintf(stderr, "text-after-idat: %s\n", error.message.c_str());
		++g_failures;
		return;
	}
	CHECK(!error);
	CHECK(image->width == 1 && image->height == 1);
	auto it = image->text.find("prompt");
	CHECK(it != image->text.end());
	if (it != image->text.end())
		CHECK(it->second == "hello");
}

void
test_profile_transfer()
{
	CHECK(dn::profile_transfer(nullptr) == dn::Transfer::Srgb);

	auto cmm = dn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb != nullptr);
	CHECK(dn::profile_transfer(srgb.get()) == dn::Transfer::Srgb);

	auto g22 = cmm->get_profile_sRGB_gamma(2.2);
	CHECK(g22 != nullptr);
	CHECK(dn::profile_transfer(g22.get()) == dn::Transfer::AdobeRgb);

	auto lin = cmm->get_profile_sRGB_gamma(1.0);
	CHECK(lin != nullptr);
	CHECK(dn::profile_transfer(lin.get()) == dn::Transfer::Linear);

	auto g18 = cmm->get_profile_sRGB_gamma(1.8);
	CHECK(g18 != nullptr);
	CHECK(dn::profile_transfer(g18.get()) == dn::Transfer::Srgb);
}

}  // namespace

int
main()
{
	test_pack_helpers();
	test_loaders_solid();
	test_jpeg_cms_8_to_16();
	test_cmyk_cms_opaque();
	test_cms_tiled();
	test_rgbw_2x2();
	test_premul_alpha();
	test_large_icc_and_p3_red();
	test_svg();
	test_chromaticities();
	test_png_text_after_idat();
	test_profile_transfer();

	if (g_failures) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	puts("ok");
	return 0;
}
