//
// test-save.cpp: the lossless WebP and Exiv2 metadata encoders
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

// A gradient with a translucent column and a zero-alpha one: the working
// format is premultiplied, so those are exactly what a round trip loses.
static dawn::ImagePtr
gradient(uint32_t w, uint32_t h)
{
	auto image = dawn::image_new(w, h);
	CHECK(image);
	if (!image)
		return image;

	for (uint32_t y = 0; y < h; y++) {
		uint16_t *row = dawn::row_u16(*image, y);
		for (uint32_t x = 0; x < w; x++, row += 4) {
			uint32_t a = 0xFFFF;
			if (x == 1)
				a = 0x8080;
			else if (x == 2)
				a = 0;
			const uint32_t b = uint32_t(x) * 0xFFFF / (w > 1 ? w - 1 : 1);
			const uint32_t g = uint32_t(y) * 0xFFFF / (h > 1 ? h - 1 : 1);
			const uint32_t r = 0xFFFF - b;
			// Premultiplied, as everything downstream expects.
			row[0] = uint16_t(b * a / 0xFFFF);
			row[1] = uint16_t(g * a / 0xFFFF);
			row[2] = uint16_t(r * a / 0xFFFF);
			row[3] = uint16_t(a);
		}
	}
	return image;
}

static dawn::ImagePtr
reopen(const vector<uint8_t> &data)
{
	dawn::Error error;
	auto image = dawn::open_from_data(data, {}, &error);
	if (!image)
		test::fail("%s", error.message.c_str());
	return image;
}

// Eight bits are all WebP keeps, so this is the tightest honest tolerance.
static bool
near16(uint16_t a, uint16_t b)
{
	const int d = int(a) - int(b);
	return (d < 0 ? -d : d) <= 0x0180;
}

static void
test_webp_still()
{
	auto source = gradient(16, 8);
	if (!source)
		return;

	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::save_webp(*source, nullptr, {}, &data, &error));
	CHECK(!data.empty());

	auto back = reopen(data);
	if (!back)
		return;

	CHECK(back->width == 16 && back->height == 8);
	CHECK(!back->frame_next);
	bool same = true;
	for (uint32_t y = 0; y < back->height; y++) {
		const uint16_t *a = dawn::row_u16(*source, y);
		const uint16_t *b = dawn::row_u16(*back, y);
		for (uint32_t x = 0; x < back->width * 4; x++) {
			if (!near16(a[x], b[x]))
				same = false;
		}
	}
	CHECK(same);

	// The zero-alpha column carries no colour either way.
	for (uint32_t y = 0; y < back->height; y++) {
		const uint16_t *b = dawn::row_u16(*back, y) + 2 * 4;
		CHECK(!b[0] && !b[1] && !b[2] && !b[3]);
	}
}

static void
test_webp_metadata()
{
	auto source = gradient(4, 4);
	if (!source)
		return;

	auto cmm = dawn::Cmm::get_default();
	auto srgb = cmm->get_profile_sRGB();
	CHECK(srgb);
	if (!srgb)
		return;

	const vector<uint8_t> icc = srgb->to_bytes();
	CHECK(!icc.empty());
	source->icc.assign(icc.begin(), icc.end());
	source->xmp = {'<', 'x', '>'};

	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::save_webp(*source, nullptr, {}, &data, &error));
	auto back = reopen(data);
	if (back) {
		CHECK(back->icc == source->icc);
		CHECK(back->xmp == source->xmp);
	}

	// The override wins outright over what the page carries.
	auto p3 = cmm->get_profile_display_p3();
	CHECK(p3);
	if (!p3)
		return;

	const vector<uint8_t> other = p3->to_bytes();
	CHECK(dawn::save_webp(*source, nullptr, other, &data, &error));
	back = reopen(data);
	if (back) {
		CHECK(back->icc == other);
		CHECK(back->icc != source->icc);
	}
}

static void
test_webp_animation()
{
	auto first = gradient(8, 8);
	auto second = gradient(8, 8);
	auto third = gradient(8, 8);
	if (!first || !second || !third)
		return;

	// Distinct pixels catch exporting the wrong frame, not just its timing.
	for (uint32_t y = 0; y < 8; y++) {
		uint16_t *green = dawn::row_u16(*second, y);
		uint16_t *blue = dawn::row_u16(*third, y);
		for (uint32_t x = 0; x < 8; x++) {
			green[x * 4] = green[x * 4 + 2] = 0;
			green[x * 4 + 1] = green[x * 4 + 3] = 0xFFFF;
			blue[x * 4 + 1] = blue[x * 4 + 2] = 0;
			blue[x * 4] = blue[x * 4 + 3] = 0xFFFF;
		}
	}
	first->frame_duration = 40;
	second->frame_duration = 70;
	third->frame_duration = 110;
	first->loops = 3;
	second->frame_next = third;
	first->frame_next = second;

	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::save_webp(*first, nullptr, {}, &data, &error));
	auto back = reopen(data);
	if (back) {
		int frames = 0;
		for (const dawn::Image *f = back.get(); f; f = f->frame_next.get())
			frames++;
		CHECK(frames == 3);
		CHECK(back->loops == 3);
		CHECK(back->frame_duration == 40);
		if (back->frame_next) {
			CHECK(back->frame_next->frame_duration == 70);
			CHECK(dawn::row_u16(*back->frame_next, 0)[1] == 0xFFFF);
			if (auto last = back->frame_next->frame_next) {
				CHECK(last->frame_duration == 110);
				CHECK(dawn::row_u16(*last, 0)[0] == 0xFFFF);
			}
		}
	}

	// A frame given explicitly is exported on its own, animating nothing.
	CHECK(dawn::save_webp(*first, third.get(), {}, &data, &error));
	back = reopen(data);
	if (back) {
		CHECK(!back->frame_next);
		CHECK(dawn::row_u16(*back, 0)[0] == 0xFFFF);
		CHECK(!dawn::row_u16(*back, 0)[1]);
	}
}

static void
test_webp_stride_and_limits()
{
	auto source = gradient(7, 3);
	const auto pixels = source->data;
	const uint32_t row_bytes = source->stride;
	source->stride += 24;
	source->data.assign(size_t(source->stride) * source->height, 0xAB);
	for (uint32_t y = 0; y < source->height; y++)
		memcpy(dawn::row_bytes(*source, y), pixels.data() + y * row_bytes,
			row_bytes);
	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::save_webp(*source, nullptr, {}, &data, &error));
	if (auto back = reopen(data)) {
		for (uint32_t y = 0; y < source->height; y++)
			for (uint32_t x = 0; x < source->width * 4; x++)
				CHECK(near16(
					dawn::row_u16(*source, y)[x], dawn::row_u16(*back, y)[x]));
	}

	// Rejected before reading pixels or allocating the encoder's input.
	dawn::Image invalid;
	for (uint32_t dimension : {0u, 16384u, UINT32_MAX}) {
		invalid.width = dimension;
		invalid.height = 1;
		CHECK(!dawn::save_webp(invalid, nullptr, {}, &data, &error));
		CHECK(data.empty() && !error.message.empty());
		std::swap(invalid.width, invalid.height);
		CHECK(!dawn::save_webp(invalid, nullptr, {}, &data, &error));
	}
	auto widest = gradient(16383, 1);
	CHECK(dawn::save_webp(*widest, nullptr, {}, &data, &error));
	if (auto back = reopen(data))
		CHECK(back->width == 16383);

	source->frame_next = gradient(7, 3);
	source->frame_duration = 0xFFFFFF;
	source->frame_next->frame_duration = 1;
	source->loops = 0xFFFF;
	CHECK(dawn::save_webp(*source, nullptr, {}, &data, &error));
	if (auto back = reopen(data)) {
		CHECK(back->frame_duration == 0xFFFFFF);
		CHECK(back->loops == 0xFFFF);
	}
	for (int64_t duration : {-1LL, 0x1000000LL, 0x100000028LL}) {
		source->frame_duration = duration;
		CHECK(!dawn::save_webp(*source, nullptr, {}, &data, &error));
		CHECK(data.empty());
	}
	source->frame_duration = 1;
	source->loops = 0x10000;
	CHECK(!dawn::save_webp(*source, nullptr, {}, &data, &error));
	CHECK(data.empty());
	// Animation limits are irrelevant to an explicitly exported still.
	CHECK(dawn::save_webp(*source, source.get(), {}, &data, &error));
}

// Walks the segments the way a reader would, gathering payloads by marker.
static bool
parse_exv(const vector<uint8_t> &data, vector<uint8_t> *exif,
	vector<uint8_t> *icc, vector<uint8_t> *xmp)
{
	static const char kTem[] = "\xFF\x01"
							   "Exiv2";
	if (data.size() < 7 || memcmp(data.data(), kTem, 7))
		return false;

	size_t at = 7;
	int seen = 0, total = 0;
	while (at + 2 <= data.size()) {
		if (data[at] != 0xFF)
			return false;
		const uint8_t marker = data[at + 1];
		if (marker == 0xD9) {
			CHECK(seen == total);
			return at + 2 == data.size();
		}
		if (at + 4 > data.size())
			return false;

		const size_t len = size_t(data[at + 2]) << 8 | data[at + 3];
		if (len < 2 || at + 2 + len > data.size())
			return false;

		const uint8_t *body = data.data() + at + 4;
		const size_t body_len = len - 2;
		if (marker == 0xE1 && body_len >= 6 && !memcmp(body, "Exif\0\0", 6))
			exif->insert(exif->end(), body + 6, body + body_len);
		else if (marker == 0xE2 && body_len >= 14 &&
			!memcmp(body, "ICC_PROFILE\0", 12)) {
			seen++;
			total = body[13];
			CHECK(body[12] == seen);
			icc->insert(icc->end(), body + 14, body + body_len);
		} else if (marker == 0xE1 && body_len >= 29 &&
			!memcmp(body, "http://ns.adobe.com/xap/1.0/", 29))
			xmp->insert(xmp->end(), body + 29, body + body_len);
		else
			return false;
		at += 2 + len;
	}
	return false;
}

static void
test_exv()
{
	auto source = gradient(2, 2);
	if (!source)
		return;

	source->exif = {'M', 'M', 0, 42};
	source->xmp = {'<', 'x', '/', '>'};
	// Well over one segment, so that the sequence numbering is exercised
	// right across a boundary.
	source->icc.resize(0xFFFF * 2 + 17);
	for (size_t i = 0; i < source->icc.size(); i++)
		source->icc[i] = uint8_t(i * 7 + 3);

	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::save_exv(*source, &data, &error));

	vector<uint8_t> exif, icc, xmp;
	CHECK(parse_exv(data, &exif, &icc, &xmp));
	CHECK(exif == source->exif);
	CHECK(icc == source->icc);
	CHECK(xmp == source->xmp);

	// Neither Exif nor XMP can span segments, and saying so beats a silent
	// truncation of what the caller handed over.
	auto oversized = gradient(2, 2);
	if (!oversized)
		return;

	oversized->xmp.resize(0x10000);
	CHECK(!dawn::save_exv(*oversized, &data, &error));
	CHECK(!error.message.empty());
	CHECK(data.empty());

	oversized->xmp.clear();
	oversized->exif.resize(0x10000);
	CHECK(!dawn::save_exv(*oversized, &data, &error));
}

static void
test_exv_limits()
{
	dawn::Image source;
	vector<uint8_t> data;
	dawn::Error error;
	const size_t icc_limit = 0xFFFF - 2 - 12 - 2;
	for (size_t size :
		{size_t(0), icc_limit - 1, icc_limit, icc_limit + 1, icc_limit * 255}) {
		source.icc.resize(size);
		for (size_t i = 0; i < size; i++)
			source.icc[i] = uint8_t(i);
		CHECK(dawn::save_exv(source, &data, &error));
		vector<uint8_t> exif, icc, xmp;
		CHECK(parse_exv(data, &exif, &icc, &xmp));
		CHECK(icc == source.icc);
	}
	source.icc.resize(icc_limit * 255 + 1);
	CHECK(!dawn::save_exv(source, &data, &error));
	CHECK(data.empty());
	source.icc.clear();
	for (bool exif : {false, true}) {
		auto &payload = exif ? source.exif : source.xmp;
		const size_t limit = 0xFFFF - 2 - (exif ? 6 : 29);
		payload.assign(limit, 42);
		CHECK(dawn::save_exv(source, &data, &error));
		vector<uint8_t> e, i, x;
		CHECK(parse_exv(data, &e, &i, &x));
		CHECK((exif ? e : x) == payload);
		payload.push_back(42);
		CHECK(!dawn::save_exv(source, &data, &error));
		CHECK(data.empty());
		payload.clear();
	}
}

int
main(int, char *[])
{
	return test::run({
		{"webp still", test_webp_still},
		{"webp metadata", test_webp_metadata},
		{"webp animation", test_webp_animation},
		{"webp stride and limits", test_webp_stride_and_limits},
		{"exv", test_exv},
		{"exv limits", test_exv_limits},
	});
}
