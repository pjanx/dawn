//
// load-libraw.cpp: raw camera image loader (LibRaw)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#if DAWN_WITH_LIBRAW
#include <libraw.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

using namespace std;

namespace dawn
{

// LibRaw's flip is a bit field, see flip_index(): 4 transposes,
// then 2 mirrors rows, and 1 mirrors columns.
static constexpr Orientation kFlipOrientations[] = {
	Orientation::Rotate0,    // 0
	Orientation::Mirror0,    // 1
	Orientation::Mirror180,  // 2
	Orientation::Rotate180,  // 3
	Orientation::Mirror270,  // 4
	Orientation::Rotate270,  // 5
	Orientation::Rotate90,   // 6
	Orientation::Mirror90,   // 7
};

// With output_color = 0, dcraw_process() stops at white-balanced camera
// colours.  The rest follows convert_to_rgb() and copy_mem_image(), except
// that the output is Rec. 2020 under the sRGB curve, while auto-brighten
// still measures linear sRGB, so that exposure does not follow the container.
static void
develop(const libraw_data_t *iprc, Image &image)
{
	const int colors = min(iprc->idata.colors, 4);
	float to_srgb[3][4] = {};
	if (iprc->rawdata.ioparams.raw_color || colors == 1) {
		for (int c = 0; c < 3; c++)
			to_srgb[c][colors == 1 ? 0 : c] = 1;
	} else {
		memcpy(to_srgb, iprc->color.rgb_cam, sizeof to_srgb);
	}

	const RgbMatrix m =
		primaries_to_primaries(kRec709Primaries, kRec2020Primaries);
	float to_output[3][3] = {};
	for (size_t c = 0; c < 3; c++)
		for (size_t k = 0; k < 3; k++)
			to_output[c][k] = float(m[k][c]);

	vector<uint32_t> histogram(3 << 13);
	for (uint32_t y = 0; y < image.height; y++) {
		const uint16_t (*in)[4] = iprc->image + size_t(y) * image.width;
		uint16_t *out = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++, out += 4) {
			const uint16_t *p = in[x];
			const float r = to_srgb[0][0] * p[0] + to_srgb[0][1] * p[1] +
				to_srgb[0][2] * p[2] + to_srgb[0][3] * p[3];
			const float g = to_srgb[1][0] * p[0] + to_srgb[1][1] * p[1] +
				to_srgb[1][2] * p[2] + to_srgb[1][3] * p[3];
			const float b = to_srgb[2][0] * p[0] + to_srgb[2][1] * p[1] +
				to_srgb[2][2] * p[2] + to_srgb[2][3] * p[3];
			histogram[0 << 13 | size_t(clamp(r, 0.f, 65535.f)) >> 3]++;
			histogram[1 << 13 | size_t(clamp(g, 0.f, 65535.f)) >> 3]++;
			histogram[2 << 13 | size_t(clamp(b, 0.f, 65535.f)) >> 3]++;
			for (int c = 0; c < 3; c++) {
				const float output = to_output[c][0] * r + to_output[c][1] * g +
					to_output[c][2] * b + .5f;
				out[2 - c] = uint16_t(clamp(int(output), 0, 65535));
			}
			out[3] = 65535;
		}
	}

	// White is where the brightest 1% of some channel starts.
	size_t clipped = size_t(
		double(image.width) * image.height * iprc->params.auto_bright_thr);
	size_t white = 32;
	for (size_t c = 0; c < 3; c++) {
		size_t total = 0;
		size_t value = 0x2000;
		while (--value > 32)
			if ((total += histogram[c << 13 | value]) > clipped)
				break;
		white = max(white, value);
	}

	vector<uint16_t> curve(0x10000, 65535);
	const size_t top = white << 3;
	for (size_t i = 0; i < top; i++)
		curve[i] = uint16_t(lround(
			transfer_encode(float(i) / float(top), Transfer::Srgb) * 65535));
	for (uint32_t y = 0; y < image.height; y++) {
		uint16_t *out = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++, out += 4)
			for (int c = 0; c < 3; c++)
				out[c] = curve[out[c]];
	}
}

// Unpacks, demosaics and colour-converts (to `profile`) a single shot already
// opened into `iprc`, producing one working-format page.
static ImagePtr
load_libraw_page(libraw_data_t *iprc, const OpenContext &ctx,
	const shared_ptr<Profile> &profile, Error *error)
{
	// Processing replaces sizes.flip with user_flip.
	Orientation orientation = kFlipOrientations[iprc->sizes.flip & 7];

	int err = 0;
	if ((err = libraw_unpack(iprc))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	// LibRaw's documentation expects applications to replace its dcraw
	// emulation.  Only its colour conversion needed replacing, see develop().
	if ((err = libraw_dcraw_process(iprc))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	ImagePtr result = image_new(iprc->sizes.width, iprc->sizes.height, error);
	if (!result)
		return nullptr;

	develop(iprc, *result);
	result->orientation = orientation;
	result->effective_profile = profile;
	finish_image(*result, ctx, profile.get(), /*input_premul=*/false);
	return result;
}

// LibRaw goes on decoding past corrupt data, and only fails on its end.
static void
on_data_error(void *data, const char *file, INT64 offset)
{
	add_warning(*(const OpenContext *) data,
		offset < 0 ? _("truncated raw data") : _("corrupted raw data"));
}

ImagePtr
load_libraw(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	unique_ptr<libraw_data_t, void (*)(libraw_data_t *)> iprc(
		libraw_init(0), libraw_close);
	if (!iprc) {
		set_error(error, _("failed to obtain a LibRaw handle"));
		return nullptr;
	}
	libraw_set_dataerror_handler(iprc.get(), on_data_error, (void *) &ctx);

	// Leave the orientation to the viewer.
	iprc->params.user_flip = 0;
	iprc->params.use_camera_wb = 1;
	iprc->params.output_color = 0;

	// Rec. 2020 keeps camera colours that sRGB would clip before our CMS
	// sees them.
	auto profile = cmm_or_default(ctx)->get_profile_parametric(
		nullopt, kD65White, kRec2020Primaries);
	if (!profile) {
		set_error(error, _("failed to describe the colour space"));
		return nullptr;
	}

	int err = 0;
	if ((err = libraw_open_buffer(iprc.get(), data.data(), data.size()))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	ImagePtr head, tail;
	ImagePtr page = load_libraw_page(iprc.get(), ctx, profile, error);
	if (!page)
		return nullptr;
	append_page(head, tail, std::move(page));

	if (!ctx.first_frame_only) {
		for (unsigned i = 1; i < iprc->idata.raw_count; i++) {
			iprc->rawparams.shot_select = i;

			// This library is terrible, we need to start again.
			if ((err = libraw_open_buffer(
					 iprc.get(), data.data(), data.size()))) {
				set_error(error, libraw_strerror(err));
				return nullptr;
			}

			ImagePtr shot = load_libraw_page(iprc.get(), ctx, profile, error);
			if (!shot)
				return nullptr;
			append_page(head, tail, std::move(shot));
		}
	}

	// Recycling keeps these, so they cover every shot.
	if (iprc->process_warnings & LIBRAW_WARN_BAD_CAMERA_WB)
		add_warning(ctx, _("unusable camera white balance"));
	return head;
}

}  // namespace dawn

#endif  // DAWN_WITH_LIBRAW
