//
// load-libraw.cpp: raw camera image loader (LibRaw)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "dn-config.h"
#include "libdn.h"
#include "libdn-loaders.h"

#if DN_WITH_LIBRAW
#include <libraw.h>

#include <cstdint>

using namespace std;

namespace dn
{
namespace
{

// Unpacks, demosaics and colour-converts (to sRGB) a single shot already
// opened into `iprc`, producing one working-format page. LibRaw hands back
// tightly packed, interleaved 16-bit RGB rows, which carry no alpha.
ImagePtr
load_libraw_page(libraw_data_t *iprc, const OpenContext &ctx, Error *error)
{
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
		set_error(error, "unexpected number of colours, or bit depth");
		libraw_dcraw_clear_mem(image);
		return nullptr;
	}

	ImagePtr result = image_new(image->width, image->height);
	if (!result) {
		set_error(error, "image allocation failure");
		libraw_dcraw_clear_mem(image);
		return nullptr;
	}

	pack_rgb16le_to_bgra16(*result, (const uint16_t *) image->data,
		size_t(image->width) * 3 * sizeof(uint16_t), 16);
	libraw_dcraw_clear_mem(image);

	// LibRaw was told to output sRGB directly; there is no embedded profile
	// to pass on, and the CMS falls back to sRGB by itself.
	ensure_working_premul(*result, ctx, nullptr, /*input_premul=*/false);
	return result;
}

}  // namespace

ImagePtr
detail::load_libraw(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// https://github.com/LibRaw/LibRaw/issues/418
	// Memory allocation failures are reported via return codes and need no
	// callback flag (unlike in older LibRaw releases fiv-io.c targeted).
	libraw_data_t *iprc = libraw_init(LIBRAW_OPTIONS_NO_DATAERR_CALLBACK);
	if (!iprc) {
		set_error(error, "failed to obtain a LibRaw handle");
		return nullptr;
	}

	// TODO(p): Check if we need to set anything for autorotation (sizes.flip).
	iprc->params.use_camera_wb = 1;
	iprc->params.output_color = 1;  // sRGB, TODO(p): Is this used?
	iprc->params.output_bps = 16;

	int err = 0;
	if ((err = libraw_open_buffer(iprc, data.data(), data.size()))) {
		set_error(error, libraw_strerror(err));
		libraw_close(iprc);
		return nullptr;
	}

	ImagePtr head, tail;
	ImagePtr page = load_libraw_page(iprc, ctx, error);
	if (!page) {
		libraw_close(iprc);
		return nullptr;
	}
	append_page(head, tail, std::move(page));

	if (!ctx.first_frame_only) {
		for (unsigned i = 1; i < iprc->idata.raw_count; i++) {
			iprc->rawparams.shot_select = i;

			// This library is terrible, we need to start again.
			if ((err = libraw_open_buffer(iprc, data.data(), data.size()))) {
				set_error(error, libraw_strerror(err));
				libraw_close(iprc);
				return nullptr;
			}

			ImagePtr shot = load_libraw_page(iprc, ctx, error);
			if (!shot) {
				libraw_close(iprc);
				return nullptr;
			}
			append_page(head, tail, std::move(shot));
		}
	}

	libraw_close(iprc);
	return head;
}

}  // namespace dn

#endif  // DN_WITH_LIBRAW
