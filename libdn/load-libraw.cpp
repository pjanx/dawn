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

#include <cstdint>
#include <memory>

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

// Unpacks, demosaics and colour-converts (to sRGB) a single shot already
// opened into `iprc`, producing one working-format page. LibRaw hands back
// tightly packed, interleaved 16-bit RGB rows, which carry no alpha.
static ImagePtr
load_libraw_page(libraw_data_t *iprc, const OpenContext &ctx, Error *error)
{
	// Processing replaces sizes.flip with user_flip.
	Orientation orientation = kFlipOrientations[iprc->sizes.flip & 7];

	int err = 0;
	if ((err = libraw_unpack(iprc))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	// TODO(p): Documentation says I should look at the code and do it myself.
	if ((err = libraw_dcraw_process(iprc))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	libraw_processed_image_t *image = libraw_dcraw_make_mem_image(iprc, &err);
	if (!image) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	// This should have been transformed, and kept, respectively.
	if (image->colors != 3 || image->bits != 16) {
		set_error(error, _("unexpected number of colours, or bit depth"));
		libraw_dcraw_clear_mem(image);
		return nullptr;
	}

	ImagePtr result = image_new(image->width, image->height);
	if (!result) {
		set_error(error, _("image allocation failure"));
		libraw_dcraw_clear_mem(image);
		return nullptr;
	}

	pack_rgb16le_to_bgra16(*result, assume_aligned<const uint16_t>(image->data),
		size_t(image->width) * 3 * sizeof(uint16_t), 16);
	libraw_dcraw_clear_mem(image);
	result->orientation = orientation;

	// LibRaw was told to output sRGB directly; there is no embedded profile
	// to pass on, and the CMS falls back to sRGB by itself.
	finish_image(*result, ctx, nullptr, /*input_premul=*/false);
	return result;
}

ImagePtr
load_libraw(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// https://github.com/LibRaw/LibRaw/issues/418
	// Memory allocation failures are reported via return codes and need no
	// callback flag (unlike in older LibRaw releases fiv-io.c targeted).
	unique_ptr<libraw_data_t, void (*)(libraw_data_t *)> iprc(
		libraw_init(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK), libraw_close);
	if (!iprc) {
		set_error(error, _("failed to obtain a LibRaw handle"));
		return nullptr;
	}

	// Leave the orientation to the viewer.
	iprc->params.user_flip = 0;
	iprc->params.use_camera_wb = 1;
	iprc->params.output_color = 1;  // sRGB, TODO(p): Is this used?
	iprc->params.output_bps = 16;

	int err = 0;
	if ((err = libraw_open_buffer(iprc.get(), data.data(), data.size()))) {
		set_error(error, libraw_strerror(err));
		return nullptr;
	}

	ImagePtr head, tail;
	ImagePtr page = load_libraw_page(iprc.get(), ctx, error);
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

			ImagePtr shot = load_libraw_page(iprc.get(), ctx, error);
			if (!shot)
				return nullptr;
			append_page(head, tail, std::move(shot));
		}
	}
	return head;
}

}  // namespace dawn

#endif  // DAWN_WITH_LIBRAW
