//
// test-icns.cpp: ICNS container and legacy pixel format tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn/libdn-loaders.h"
#include "libdn/libdn.h"
#include "test.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std;

// Generated once with macOS 15 ImageIO. These are the irreducible encoded
// payload tests; repetitive legacy formats are assembled below.
constexpr char kMacosGold[] =
	"icns\000\000\000\220ic11\000\000\000\210\211PNG\015\012\032\012\000\000"
	"\000\015IHDR\000\000\000 \000\000\000 \010\006\000\000\000szz\364\000"
	"\000\000GIDATX\011\355\327\261\015\0000\010\003A\234\375wN\000e\207o\036"
	"\006\260uT\244n/8g\262\247\003\325#X\362W_\001\360\002e\001\005\024P@"
	"\001\005\024P@\001\005\024P@\001\\ \373\027R\257q\207?N2\0124V\256If\000"
	"\000\000\000IEND\256B`\202";

constexpr char kJpeg2000Gold[] =
	"icns\000\000\002\354ic05\000\000\002\344\000\000\000\014jP  \015\012\207"
	"\012\000\000\000\024ftypjp2 \000\000\000\000jp2 \000\000\000Ojp2h\000"
	"\000\000\026ihdr\000\000\000 \000\000\000 \000\004\007\007\001\000\000"
	"\000\000\017colr\001\000\000\000\000\000\020\000\000\000\"cdef\000\004"
	"\000\000\000\000\000\001\000\003\000\001\000\000\000\001\000\000\000\002"
	"\000\002\000\000\000\003\000\000\000\000jp2c\377O\377Q\0002\000\000\000"
	"\000\000 \000\000\000 \000\000\000\000\000\000\000\000\000\000\000 \000"
	"\000\000 \000\000\000\000\000\000\000\000\000\004\007\001\001\007\001"
	"\001\007\001\001\007\001\001\377R\000\014\000\000\000\001\001\005\004"
	"\004\000\001\377\\\000\023 PXX`XX`XX`XXXPPX\377d\000\021\000\001Kakadu-v"
	"5.2.1\377\220\000\012\000\000\000\000\001\367\000\001\377\223\300\371"
	"\000\200\006\307\332\004\003\030\307\332\004\006\270\307\332\002\000\300"
	"|\200\240|\340`}@ \011h\011\002\300>\020`|\340\240>p@\000\0043\010\020"
	"\300>\020`|\340`>p@\011\006\001X\200\301\365\001\301\365\002@|\340\300"
	"\005\036|\007\361\223\001\007\251\221\300\371\302@\371\301\300|\340\300"
	"\007\315)\206\005On\0054D\300\371\302@\371\302@|\340\300\005\036#\223"
	"\010\012w\232\007\371\314\240\0040\003\241\000\016\013q\300\371\303\301"
	"\365\003@>@\300\025!\030&[\320\327\027S\254\024\177\304\021\341S \211)"
	"\300|\201\240|\341\240\037 `\022p\2157\204\237\025\021\267\245.\265\0258"
	"\240\305, \300|\201`|\341`\017\204@\025!\001\350\225\027S\014?8\021\341W"
	"\255\300\035\012\001\360\207\000|!\000\004\027\001U\017\004\021\301\365"
	"\004\300|\201\340>@\200\024\254\342,\205\013dq'>\177\205\265A\352M=\320"
	"\260h\300\371\304@|\201`>@`\021\374\331\211U!>$>\211K\343m=\321=\300\371"
	"\304@|\201\240>@\240\024\254\34252w\300\253=\314\362k\375?>\206\273\301o"
	"\300|\200\340\372\201 \372\200\300=\315q>\211H\255=\314\264\303\352\013"
	"\237\200\204>\320\220\024\303S\210\343\242\003\337 \332\013\030 \310ft"
	"\201\203\026W\303\346\203\377ep\353\025C{\226\177\202?\352\230\301\363"
	"\213\217\264>\017\250 \022\023K\210\343v\317/\232\267+\027t\303^X\230"
	"\235{\350`H\012\001\303\252\022\223sDc\007-\237\301\363\212\217\264:\017"
	"\250$\024\303S\210\343v\317G\326&\030 \310fw\311R\321E\3222~\035;\025C{D"
	"c\006@\357A\303\352\004\237\2004~\000\200\235\261,\350\235\233\217\277"
	"\265,\235\261\212\217\377\331";

struct Pixel {
	uint16_t b, g, r, a;
};

struct Expected {
	uint32_t width, height;
	uint8_t r, g, b, a = 255;
};

static Pixel
pixel_at(const dawn::Image &image, uint32_t x, uint32_t y)
{
	const uint16_t *p = dawn::row_u16(image, y) + x * 4;
	return {p[0], p[1], p[2], p[3]};
}

template <size_t N>
static span<const uint8_t>
bytes(const char (&data)[N])
{
	return {(const uint8_t *) data, N - 1};
}

static void
append_be32(vector<uint8_t> &out, uint32_t value)
{
	for (int shift = 24; shift >= 0; shift -= 8)
		out.push_back(uint8_t(value >> shift));
}

static void
append_entry(
	vector<uint8_t> &out, const char (&type)[5], span<const uint8_t> data)
{
	out.insert(out.end(), type, type + 4);
	append_be32(out, uint32_t(data.size() + 8));
	out.insert(out.end(), data.begin(), data.end());
}

static void
append_rle_channel(vector<uint8_t> &out, uint8_t value)
{
	// The ICNS run lengths represented by FF and FB are 130 and 126.
	out.insert(out.end(), {0xFF, value, 0xFB, value});
}

static vector<uint8_t>
legacy_gold()
{
	vector<uint8_t> entries, data(64);
	fill(data.begin() + 32, data.end(), 0xFF);
	append_entry(entries, "ics#", data);

	data.assign(128, 0x88);  // Palette 4: green in both nibbles.
	append_entry(entries, "ics4", data);
	data.assign(256, 235);  // Palette 8: blue.
	append_entry(entries, "ics8", data);

	data.clear();
	for (uint8_t channel : {0x20, 0x70, 0xD0})
		append_rle_channel(data, channel);
	append_entry(entries, "is32", data);
	data.assign(256, 128);
	append_entry(entries, "s8mk", data);

	data.assign({'A', 'R', 'G', 'B'});
	for (uint8_t channel : {0x40, 0xF0, 0x40, 0x20})
		append_rle_channel(data, channel);
	append_entry(entries, "ic04", data);

	vector<uint8_t> result{'i', 'c', 'n', 's'};
	append_be32(result, uint32_t(entries.size() + 8));
	result.insert(result.end(), entries.begin(), entries.end());
	return result;
}

static uint16_t
premul8(uint8_t value, uint8_t alpha)
{
	uint32_t v = uint32_t(value) * 257;
	uint32_t a = uint32_t(alpha) * 257;
	return uint16_t((v * a + 32768) / 65535);
}

static bool
near(uint16_t a, uint16_t b, uint16_t tolerance)
{
	return abs(int(a) - int(b)) <= tolerance;
}

static void
expect_page(
	const dawn::Image &page, const Expected &want, uint16_t tolerance = 0)
{
	CHECK(page.width == want.width);
	CHECK(page.height == want.height);
	Pixel p = pixel_at(page, page.width / 2, page.height / 2);
	if (!near(p.b, premul8(want.b, want.a), tolerance) ||
		!near(p.g, premul8(want.g, want.a), tolerance) ||
		!near(p.r, premul8(want.r, want.a), tolerance) ||
		!near(p.a, uint16_t(want.a) * 257, tolerance)) {
		test::fail("%ux%u: got BGRA (%u,%u,%u,%u)", want.width, want.height,
			p.b, p.g, p.r, p.a);
	}
}

static dawn::ImagePtr
load_gold(span<const uint8_t> data, bool first_only,
	vector<string> *warnings = nullptr)
{
	dawn::OpenContext ctx;
	ctx.first_frame_only = first_only;
	ctx.warnings = warnings;
	dawn::Error error;
	dawn::ImagePtr image = dawn::open_from_data(data, ctx, &error);
	if (!image)
		test::fail("open ICNS gold: %s", error.message.c_str());
	return image;
}

static void
expect_pages(
	dawn::ImagePtr image, span<const Expected> expected, uint16_t tolerance = 0)
{
	for (size_t i = 0; i < expected.size(); i++) {
		if (!image) {
			test::fail("page chain ended before page %zu", i + 1);
			return;
		}
		expect_page(*image, expected[i], tolerance);
		if (image->page_next)
			CHECK(image->page_next->page_previous.lock().get() == image.get());
		image = image->page_next;
	}
	CHECK(image == nullptr);
}

static void
test_macos_gold()
{
	static constexpr Expected expected[] = {
		{32, 32, 0x00, 0xFF, 0x00},
	};
	vector<string> warnings;
	dawn::ImagePtr image = load_gold(bytes(kMacosGold), false, &warnings);
	CHECK(image != nullptr);
	if (!image)
		return;
	CHECK(string(image->loader) == "ICNS");
	CHECK(warnings.empty());
	expect_pages(std::move(image), expected);
}

static void
test_first_page_only()
{
	dawn::ImagePtr image = load_gold(bytes(kMacosGold), true);
	CHECK(image != nullptr);
	if (!image)
		return;
	CHECK(image->width == 32);
	CHECK(image->height == 32);
	CHECK(image->page_next == nullptr);
}

static void
test_legacy_gold()
{
	static constexpr Expected expected[] = {
		{16, 16, 0xFF, 0xFF, 0xFF},
		{16, 16, 0x1F, 0xB7, 0x14},
		{16, 16, 0x00, 0x00, 0xEE},
		{16, 16, 0x20, 0x70, 0xD0, 128},
		{16, 16, 0xF0, 0x40, 0x20, 64},
	};
	vector<string> warnings;
	vector<uint8_t> data = legacy_gold();
	dawn::ImagePtr image = load_gold(data, false, &warnings);
	CHECK(image != nullptr);
	CHECK(warnings.empty());
	expect_pages(std::move(image), expected);
}

static void
test_jpeg2000_gold()
{
	dawn::OpenContext ctx;
	vector<string> warnings;
	ctx.warnings = &warnings;
	dawn::Error error;
	dawn::ImagePtr image =
		dawn::detail::load_icns(bytes(kJpeg2000Gold), ctx, &error);
#if DAWN_WITH_OPENJPEG
	CHECK(image != nullptr);
	if (image)
		expect_page(*image, {32, 32, 0x17, 0x6B, 0xC7}, 8 * 257);
	CHECK(warnings.empty());
#else
	CHECK(image == nullptr);
	CHECK(error.message == "empty or unsupported ICNS image");
	CHECK(warnings.size() == 1);
#endif
}

static void
test_truncated_tail()
{
	vector<uint8_t> data = legacy_gold();
	data.insert(data.end(), {'b', 'a', 'd', '!'});
	append_be32(data, 12);  // The declared entry payload is absent.
	uint32_t size = uint32_t(data.size());
	data[4] = uint8_t(size >> 24);
	data[5] = uint8_t(size >> 16);
	data[6] = uint8_t(size >> 8);
	data[7] = uint8_t(size);
	dawn::OpenContext ctx;
	vector<string> warnings;
	ctx.warnings = &warnings;
	dawn::Error error;
	dawn::ImagePtr image = dawn::detail::load_icns(data, ctx, &error);
	CHECK(image != nullptr);
	CHECK(!warnings.empty());
}

int
main()
{
	return test::run({
		{"macOS gold", test_macos_gold},
		{"first page", test_first_page_only},
		{"legacy gold", test_legacy_gold},
		{"JPEG 2000 gold", test_jpeg2000_gold},
		{"truncated tail", test_truncated_tail},
	});
}
