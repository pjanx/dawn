//
// load-heif.cpp: HEIF/AVIF image loader (libheif)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn.h"
#include "libdn-loaders.h"

#if DAWN_WITH_LIBHEIF
#include <libheif/heif.h>

#include <algorithm>
#include <cstdint>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// Decodes a single image handle (either a top-level image, or an auxiliary
// image such as a depth map) into one working-format page, extracting Exif
// and an embedded ICC profile, if present, and bringing it to final working
// premul before returning.
ImagePtr
load_heif_image(heif_image_handle *handle, const OpenContext &ctx, Error *error)
{
	int has_alpha = heif_image_handle_has_alpha_channel(handle);
	int bit_depth = heif_image_handle_get_luma_bits_per_pixel(handle);
	if (bit_depth < 0) {
		set_error(error, "undefined bit depth");
		return nullptr;
	}

	// Prefer native HDR chroma when the luma depth exceeds 8 bits
	// (typically 10 or 12). Setting `convert_hdr_to_8bit` is a no-op for
	// the interleaved RGB(A) requests below.
	heif_decoding_options *opts = heif_decoding_options_alloc();
	bool use16 = bit_depth > 8;
	heif_chroma chroma = heif_chroma_interleaved_RGBA;
	if (use16)
		chroma = has_alpha ? heif_chroma_interleaved_RRGGBBAA_LE
						   : heif_chroma_interleaved_RRGGBB_LE;

	heif_image *image = nullptr;
	heif_error err =
		heif_decode_image(handle, &image, heif_colorspace_RGB, chroma, opts);
	heif_decoding_options_free(opts);
	if (err.code != heif_error_Ok) {
		set_error(error, err.message);
		return nullptr;
	}

	int w = heif_image_get_width(image, heif_channel_interleaved);
	int h = heif_image_get_height(image, heif_channel_interleaved);
	if (w <= 0 || h <= 0) {
		set_error(error, "invalid image dimensions");
		heif_image_release(image);
		return nullptr;
	}

	ImagePtr result = image_new(uint32_t(w), uint32_t(h));
	if (!result) {
		set_error(error, "image allocation failure");
		heif_image_release(image);
		return nullptr;
	}

	// libheif uses its own row alignment; byte order is R,G,B(,A) rather
	// than B,G,R,A. The pack helpers reorder (and scale n-bit samples to
	// full uint16) while leaving the bitstream's premultiplication state.
	int src_stride = 0;
	const uint8_t *src = heif_image_get_plane_readonly(
		image, heif_channel_interleaved, &src_stride);

	if (use16) {
		int bits = min(bit_depth, 16);
		if (has_alpha) {
			pack_rgba16le_to_bgra16(
				*result, (const uint16_t *) src, size_t(src_stride), bits);
		} else {
			pack_rgb16le_to_bgra16(
				*result, (const uint16_t *) src, size_t(src_stride), bits);
		}
	} else {
		// Interleaved RGBA chroma even without an alpha channel; force
		// opaque A when the handle says there is none.
		pack_rgba8_to_bgra16(*result, src, size_t(src_stride));
		if (!has_alpha) {
			for (uint32_t y = 0; y < result->height; y++) {
				auto *d = row_u16(*result, y);
				for (uint32_t x = 0; x < result->width; x++)
					d[x * 4 + 3] = 65535;
			}
		}
	}

	// TODO(p): Test real behaviour on real transparent images.
	bool bitstream_premul =
		has_alpha && heif_image_handle_is_premultiplied_alpha(handle);

	heif_item_id exif_id = 0;
	if (heif_image_handle_get_list_of_metadata_block_IDs(
			handle, "Exif", &exif_id, 1)) {
		size_t exif_len = heif_image_handle_get_metadata_size(handle, exif_id);
		vector<uint8_t> exif(exif_len);
		heif_error e =
			heif_image_handle_get_metadata(handle, exif_id, exif.data());
		if (e.code)
			add_warning(ctx, e.message);
		else
			result->exif = std::move(exif);
	}

	// https://loc.gov/preservation/digital/formats/fdd/fdd000526.shtml#factors
	if (heif_image_handle_get_color_profile_type(handle) ==
		heif_color_profile_type_prof) {
		size_t icc_len = heif_image_handle_get_raw_color_profile_size(handle);
		vector<uint8_t> icc(icc_len);
		heif_error e =
			heif_image_handle_get_raw_color_profile(handle, icc.data());
		if (e.code)
			add_warning(ctx, e.message);
		else
			result->icc = std::move(icc);
	}

	heif_image_release(image);

	// Bring the page to final working premul: colour-manage against any
	// embedded ICC profile (derived automatically from result->icc), first
	// un-premultiplying if the bitstream declared premultiplied alpha and
	// colour management needs to happen.
	ensure_working_premul_pages(*result, ctx, nullptr, bitstream_premul);
	return result;
}

// Appends any auxiliary images (e.g. depth maps) hanging off `top`
// as further pages. We have no special processing for them yet,
// so they are included mainly to not lose them silently.
void
load_heif_aux_images(const OpenContext &ctx, heif_image_handle *top,
	ImagePtr &head, ImagePtr &tail)
{
	// Include the depth image, we have no special processing for it now.
	int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA;

	int n = heif_image_handle_get_number_of_auxiliary_images(top, filter);
	if (n <= 0)
		return;

	vector<heif_item_id> ids(n);
	n = heif_image_handle_get_list_of_auxiliary_image_IDs(
		top, filter, ids.data(), n);
	for (int i = 0; i < n; i++) {
		heif_image_handle *handle = nullptr;
		heif_error err =
			heif_image_handle_get_auxiliary_image_handle(top, ids[i], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			continue;
		}

		Error suberror;
		ImagePtr aux = load_heif_image(handle, ctx, &suberror);
		if (aux)
			append_page(head, tail, std::move(aux));
		else
			add_warning(ctx, suberror.message);

		heif_image_handle_release(handle);
	}
}

}  // namespace

ImagePtr
detail::load_heif(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// libheif will throw C++ exceptions on allocation failures.
	// The library is generally awful through and through.
	heif_context *hctx = heif_context_alloc();
	if (!hctx) {
		set_error(error, "failed to obtain a libheif context");
		return nullptr;
	}

	heif_error err = heif_context_read_from_memory_without_copy(
		hctx, data.data(), data.size(), nullptr);
	if (err.code != heif_error_Ok) {
		set_error(error, err.message);
		heif_context_free(hctx);
		return nullptr;
	}

	int n = heif_context_get_number_of_top_level_images(hctx);
	vector<heif_item_id> ids(max(n, 0));
	n = heif_context_get_list_of_top_level_image_IDs(hctx, ids.data(), n);

	ImagePtr head, tail;
	for (int i = 0; i < n; i++) {
		heif_image_handle *handle = nullptr;
		err = heif_context_get_image_handle(hctx, ids[i], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			continue;
		}

		Error suberror;
		ImagePtr page = load_heif_image(handle, ctx, &suberror);
		if (page)
			append_page(head, tail, std::move(page));
		else
			add_warning(ctx, suberror.message);

		// TODO(p): Possibly add thumbnail images as well.
		if (!ctx.first_frame_only)
			load_heif_aux_images(ctx, handle, head, tail);

		heif_image_handle_release(handle);
		if (ctx.first_frame_only)
			break;
	}

	heif_context_free(hctx);
	if (!head) {
		set_error(error, "empty or unsupported image");
		return nullptr;
	}

	// Each page was already brought to final working premul individually,
	// in load_heif_image().
	return head;
}

}  // namespace dn

#endif  // DAWN_WITH_LIBHEIF
