//
// test-jpeg-crop.cpp: lossless JPEG crops and rotations
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn.hpp"
#include "test.hpp"

#include <algorithm>
#include <cstring>

using namespace std;

static vector<uint8_t>
fixture(const char *name)
{
	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::read_file(
		string(DAWN_TEST_FIXTURES_DIR) + "/" + name, &data, &error));
	return data;
}

static dawn::ImagePtr
decode(const vector<uint8_t> &data)
{
	dawn::Error error;
	auto image = dawn::open_from_data(data, {}, &error);
	CHECK(image);
	return image;
}

static void
test_crop()
{
	auto data = fixture("quads420.jpg");
	dawn::Error error;
	dawn::JpegGrid grid;
	CHECK(dawn::jpeg_grid(data, &grid, &error));
	CHECK(grid.width == 64 && grid.height == 48);
	CHECK(grid.mcu_width == 16 && grid.mcu_height == 16);
	CHECK(!grid.mpf_images);
	auto original = decode(data);
	auto cropped = decode(dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 16, 16, 32, 16, &error));
	if (cropped)
		CHECK(cropped->width == 32 && cropped->height == 16);

	// A taller crop has a nonempty interior beyond the upsampling border.
	cropped = decode(dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 16, 0, 32, 32, &error));
	if (original && cropped) {
		for (uint32_t y = 9; y < 23; y++) {
			CHECK(!memcmp(dawn::row_u16(*original, y) + 25 * 4,
				dawn::row_u16(*cropped, y) + 9 * 4, 14 * 8));
		}
	}
	cropped = decode(dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 16, 16, 1, 1, &error));
	if (cropped)
		CHECK(cropped->width == 1 && cropped->height == 1);
	CHECK(dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 8, 0, 16, 16, &error)
			.empty());
	CHECK(error);
}

static void
test_rotations()
{
	auto data = fixture("quads420.jpg");
	auto original = decode(data);
	dawn::Error error;
	for (int i = 1; i <= 8; i++) {
		auto op = dawn::Orientation(i);
		auto rotated =
			decode(dawn::jpeg_transform(data, op, 0, 0, 0, 0, &error));
		if (!original || !rotated)
			continue;

		CHECK(rotated->width == (i >= 5 ? 48 : 64));
		CHECK(rotated->height == (i >= 5 ? 64 : 48));
		for (int corner = 0; corner < 4; corner++) {
			uint32_t x = corner % 2 ? 48 : 16, y = corner / 2 ? 36 : 12;
			double dx, dy;
			dawn::orientation_map_source_to_display(op, 64, 48, x, y, &dx, &dy);
			CHECK(!memcmp(dawn::row_u16(*original, y) + x * 4,
				dawn::row_u16(*rotated, uint32_t(dy)) + uint32_t(dx) * 4, 8));
		}
	}

	// Every edge combination: only rotations that preserve all partial MCUs
	// pass.  Bit i corresponds to Orientation(i + 1), in Exif order.
	constexpr unsigned allowed[] = {0xff, 0x39, 0x93, 0x11};
	constexpr dawn::Orientation inverse[] = {dawn::Orientation::Rotate0,
		dawn::Orientation::Mirror0, dawn::Orientation::Rotate180,
		dawn::Orientation::Mirror180, dawn::Orientation::Mirror270,
		dawn::Orientation::Rotate270, dawn::Orientation::Mirror90,
		dawn::Orientation::Rotate90};
	for (unsigned edges = 0; edges < 4; edges++) {
		uint32_t w = edges & 1 ? 63 : 64, h = edges & 2 ? 47 : 48;
		auto partial = dawn::jpeg_transform(
			data, dawn::Orientation::Rotate0, 0, 0, w, h, &error);
		auto expected = decode(partial);
		for (unsigned i = 0; i < 8; i++) {
			error = {};
			auto rotated = dawn::jpeg_transform(
				partial, dawn::Orientation(i + 1), 0, 0, 0, 0, &error);
			const bool perfect = (allowed[edges] & (1u << i)) != 0;
			CHECK(!rotated.empty() == perfect);
			CHECK(bool(error) == !perfect);
			if (rotated.empty())
				continue;

			dawn::JpegGrid grid;
			CHECK(dawn::jpeg_grid(rotated, &grid, &error));
			CHECK(grid.width == (i >= 4 ? h : w));
			CHECK(grid.height == (i >= 4 ? w : h));
			auto restored = decode(
				dawn::jpeg_transform(rotated, inverse[i], 0, 0, 0, 0, &error));
			if (!expected || !restored)
				continue;

			CHECK(restored->width == w && restored->height == h);
			for (uint32_t y = 0; y < h; y++) {
				CHECK(!memcmp(dawn::row_u16(*expected, y),
					dawn::row_u16(*restored, y), w * 8));
			}
		}
	}
}

static void
test_metadata()
{
	// A little-endian Exif orientation IFD, kept byte-for-byte through
	// a rotation.
	const uint8_t exif[] = {0xff, 0xe1, 0, 34, 'E', 'x', 'i', 'f', 0, 0, 'I',
		'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0, 7, 0, 0, 0, 0,
		0, 0, 0};
	auto data = fixture("quads420.jpg");
	data.insert(data.begin() + 2, begin(exif), end(exif));
	dawn::Error error;
	auto output = dawn::jpeg_transform(
		data, dawn::Orientation::Rotate90, 0, 0, 0, 0, &error);
	CHECK(search(output.begin(), output.end(), begin(exif), end(exif)) !=
		output.end());
	auto image = decode(output);
	if (image)
		CHECK(image->orientation == dawn::Orientation::Mirror90);
	// SOF2 is the progressive frame header; the transform must not default to
	// SOF0.
	const uint8_t sof[] = {0xff, 0xc2};
	data = fixture("quads420-progressive.jpg");
	output = dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 16, 0, 32, 32, &error);
	CHECK(search(output.begin(), output.end(), begin(sof), end(sof)) !=
		output.end());
	CHECK(decode(output));
}

// The transform keeps the MPF index, but not the images past EOI.
static void
test_mpf()
{
	auto data = fixture("gainmap.jpg");
	dawn::Error error;
	dawn::JpegGrid grid;
	CHECK(dawn::jpeg_grid(data, &grid, &error));
	CHECK(grid.mpf_images == 1);

	auto output = dawn::jpeg_transform(
		data, dawn::Orientation::Rotate0, 16, 16, 32, 16, &error);
	dawn::OpenContext ctx;
	ctx.gain_maps = true;
	auto cropped = dawn::open_from_data(output, ctx, &error);
	CHECK(cropped);
	if (cropped)
		CHECK(
			cropped->width == 32 && !cropped->page_next && !cropped->gain_map);
}

static void
test_invalid()
{
	dawn::Error error;
	dawn::JpegGrid grid;
	CHECK(!dawn::jpeg_grid(fixture("red.png"), &grid, &error));
	CHECK(!dawn::jpeg_grid({}, &grid, &error));
	auto truncated = fixture("quads420.jpg");
	truncated.resize(100);
	CHECK(!dawn::jpeg_grid(truncated, &grid, &error));
	CHECK(dawn::jpeg_transform(
		fixture("red.jpg"), dawn::Orientation::Rotate90, 0, 0, 0, 0, &error)
			.empty());
	CHECK(error);
}

int
main()
{
	return test::run({{"grid and crop", test_crop},
		{"rotations", test_rotations}, {"metadata and coding", test_metadata},
		{"multi-picture format", test_mpf}, {"invalid inputs", test_invalid}});
}
