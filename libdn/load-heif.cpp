//
// load-heif.cpp: HEIF/AVIF image loader (libheif)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#if DAWN_WITH_LIBHEIF
#include <libheif/heif.h>
// Not pulled in by heif.h, and only present since the item property API.
#if __has_include(<libheif/heif_properties.h>)
#include <libheif/heif_properties.h>
#define DAWN_HEIF_PROPERTIES
#endif
// Sequence tracks arrived in 1.20, but only 1.21 can be told to ignore the
// edit list, without which an infinite one never ends--see below.
#if LIBHEIF_HAVE_VERSION(1, 21, 0)
#include <libheif/heif_sequences.h>
#define DAWN_HEIF_SEQUENCES
#endif

#include <algorithm>
#include <cstdint>
#include <memory>
#include <vector>

using namespace std;

namespace dawn
{

// Whether libheif bakes an orientation into the item's decoded pixels.
static bool
heif_image_is_oriented(heif_context *hctx, heif_item_id id)
{
#ifdef DAWN_HEIF_PROPERTIES
	// A "clap" crop is transformative as well, but it is not an orientation,
	// so it must not suppress the Exif fallback.
	return heif_item_get_properties_of_type(hctx, id,
			   heif_item_property_type_transform_rotation, nullptr, 0) > 0 ||
		heif_item_get_properties_of_type(
			hctx, id, heif_item_property_type_transform_mirror, nullptr, 0) > 0;
#else
	// Too old to ask: assume the usual phone file, which carries a transform
	// property and a matching Exif tag, and never rotate twice.
	return true;
#endif
}

static void
force_opaque(Image &image)
{
	for (uint32_t y = 0; y < image.height; y++) {
		uint16_t *d = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++)
			d[x * 4 + 3] = 65535;
	}
}

// AVIF/HEIF more often carry coded values in an nclx `colr` box than a full
// ICC profile.  Convert that to a profile.
static vector<uint8_t>
heif_nclx_profile(const heif_color_profile_nclx *nclx, const OpenContext &ctx)
{
	// The enums are numerically H.273, so they pass through as-is.
	auto profile =
		cmm_or_default(ctx)->get_profile_cicp(uint8_t(nclx->color_primaries),
			uint8_t(nclx->transfer_characteristics));
	if (profile)
		return profile->to_bytes();

	// PQ or HLG refuse being treated as simple ICC profiles.
	add_warning(ctx, _("unrepresentable nclx colour space, assuming sRGB"));
	return {};
}

// https://loc.gov/preservation/digital/formats/fdd/fdd000526.shtml#factors
static vector<uint8_t>
heif_handle_profile(heif_image_handle *handle, const OpenContext &ctx)
{
	// heif_image_handle_get_color_profile_type() prioritises both types of ICC
	// profiles (un/restricted) over nclx.  It still won't hurt to run the logic
	// with nclx as a fallback.
	vector<uint8_t> icc(heif_image_handle_get_raw_color_profile_size(handle));
	if (!icc.empty()) {
		heif_error e =
			heif_image_handle_get_raw_color_profile(handle, icc.data());
		if (!e.code)
			return icc;

		add_warning(ctx, e.message);
	}

	heif_color_profile_nclx *nclx = nullptr;
	if (heif_image_handle_get_nclx_color_profile(handle, &nclx).code || !nclx)
		return {};

	vector<uint8_t> result = heif_nclx_profile(nclx, ctx);
	heif_nclx_color_profile_free(nclx);
	return result;
}

static vector<uint8_t>
heif_handle_exif(heif_image_handle *handle, const OpenContext &ctx)
{
	heif_item_id exif_id = 0;
	if (!heif_image_handle_get_list_of_metadata_block_IDs(
			handle, "Exif", &exif_id, 1))
		return {};

	vector<uint8_t> exif(heif_image_handle_get_metadata_size(handle, exif_id));
	heif_error e = heif_image_handle_get_metadata(handle, exif_id, exif.data());
	if (e.code) {
		add_warning(ctx, e.message);
		return {};
	}
	return iso_exif_payload(exif);
}

// Decodes a single image handle (either a top-level image, or an auxiliary
// image such as a depth map) into one working-format page, extracting Exif
// and an embedded ICC profile, if present, and bringing it to final working
// premul before returning.
static ImagePtr
load_heif_image(heif_context *hctx, heif_item_id id, heif_image_handle *handle,
	const OpenContext &ctx, Error *error)
{
	int has_alpha = heif_image_handle_has_alpha_channel(handle);
	int bit_depth = heif_image_handle_get_luma_bits_per_pixel(handle);
	if (bit_depth < 0) {
		set_error(error, _("undefined bit depth"));
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
		set_error(error, _("invalid image dimensions"));
		heif_image_release(image);
		return nullptr;
	}

	ImagePtr result = image_new(uint32_t(w), uint32_t(h));
	if (!result) {
		set_error(error, _("image allocation failure"));
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
			pack_rgba16le_to_bgra16(*result,
				assume_aligned<const uint16_t>(src), size_t(src_stride), bits);
		} else {
			pack_rgb16le_to_bgra16(*result, assume_aligned<const uint16_t>(src),
				size_t(src_stride), bits);
		}
	} else {
		// Interleaved RGBA chroma even without an alpha channel;
		// force opaque alpha when the handle says there is none.
		// Empirically unnecessary because libheif fills it in.
		pack_rgba8_to_bgra16(*result, src, size_t(src_stride));
		if (!has_alpha)
			force_opaque(*result);
	}

	// TODO(p): Test real behaviour on real transparent images.
	bool bitstream_premul =
		has_alpha && heif_image_handle_is_premultiplied_alpha(handle);

	// libheif applies transformative properties (irot, imir) while decoding,
	// and ISO/IEC 23008-12 gives those the final say, so the Exif orientation
	// below must not rotate the pixels a second time.  Without them, Exif is
	// the only orientation the file has, and open_from_data() fills it in.
	if (heif_image_is_oriented(hctx, id))
		result->orientation = Orientation::Rotate0;

	result->exif = heif_handle_exif(handle, ctx);
	result->icc = heif_handle_profile(handle, ctx);
	heif_image_release(image);

	// Bring the page to final working premul: colour-manage against any
	// embedded ICC profile (derived automatically from result->icc), first
	// un-premultiplying if the bitstream declared premultiplied alpha and
	// colour management needs to happen.
	finish_frames(*result, ctx, nullptr, bitstream_premul);
	return result;
}

// Appends any auxiliary images (e.g. depth maps) hanging off `top`
// as further pages. We have no special processing for them yet,
// so they are included mainly to not lose them silently.
static void
load_heif_aux_images(const OpenContext &ctx, heif_context *hctx,
	heif_image_handle *top, ImagePtr &head, ImagePtr &tail)
{
	// Include the depth image, we have no special processing for it now.
	int filter = LIBHEIF_AUX_IMAGE_FILTER_OMIT_ALPHA;

	int n = heif_image_handle_get_number_of_auxiliary_images(top, filter);
	if (n <= 0)
		return;

	vector<heif_item_id> ids((size_t) n);
	n = heif_image_handle_get_list_of_auxiliary_image_IDs(
		top, filter, ids.data(), n);
	for (int i = 0; i < n; i++) {
		heif_image_handle *handle = nullptr;
		heif_error err = heif_image_handle_get_auxiliary_image_handle(
			top, ids[size_t(i)], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			continue;
		}

		Error suberror;
		ImagePtr aux =
			load_heif_image(hctx, ids[size_t(i)], handle, ctx, &suberror);
		if (aux)
			append_page(head, tail, std::move(aux));
		else
			add_warning(ctx, suberror.message);

		heif_image_handle_release(handle);
	}
}

// --- Sequences ---------------------------------------------------------------

#ifdef DAWN_HEIF_SEQUENCES

// How many bytes of working pixels one sequence may retain.  These are eight
// per pixel, so a 1080p thirty frames per second track exhausts this in under
// a second of playback, and Dawn is not a video player.
static constexpr uint64_t kSequenceBudget = uint64_t(256) << 20;

// The colour information of the file's primary still item, if it has one:
// libheif attaches none to sequence samples.
static vector<uint8_t>
heif_primary_profile(heif_context *hctx, const OpenContext &ctx)
{
	heif_item_id id = 0;
	heif_image_handle *handle = nullptr;
	if (heif_context_get_primary_image_ID(hctx, &id).code ||
		heif_context_get_image_handle(hctx, id, &handle).code)
		return {};

	vector<uint8_t> icc = heif_handle_profile(handle, ctx);
	heif_image_handle_release(handle);
	return icc;
}

// The sample's own colour information, which is not the track's.
// See heif_handle_profile().
static vector<uint8_t>
heif_sample_profile(const heif_image *img, const OpenContext &ctx)
{
	vector<uint8_t> icc(heif_image_get_raw_color_profile_size(img));
	if (!icc.empty()) {
		heif_error e = heif_image_get_raw_color_profile(img, icc.data());
		if (!e.code)
			return icc;

		add_warning(ctx, e.message);
	}

	heif_color_profile_nclx *nclx = nullptr;
	if (heif_image_get_nclx_color_profile(img, &nclx).code || !nclx)
		return {};

	vector<uint8_t> result = heif_nclx_profile(nclx, ctx);
	heif_nclx_color_profile_free(nclx);
	return result;
}

// Packs one decoded sample into a working-format frame, charging its pixels
// against `retained`.  Null, with a warning, when the sample cannot be used,
// or when it no longer fits the budget.
static ImagePtr
load_heif_sample(const heif_image *img, bool has_alpha, uint64_t *retained,
	const OpenContext &ctx)
{
	int w = heif_image_get_width(img, heif_channel_interleaved);
	int h = heif_image_get_height(img, heif_channel_interleaved);
	int bits =
		heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
	int stride = 0;
	const uint8_t *src =
		heif_image_get_plane_readonly(img, heif_channel_interleaved, &stride);

	// The storage width is not the sample depth: libheif returns, say, 10-bit
	// values in 16-bit words, and the pack helper scales them up from there.
	if (w <= 0 || h <= 0 || bits < 1 || bits > 16 || !src ||
		int64_t(stride) < int64_t(w) * 8) {
		add_warning(ctx, _("invalid image sequence sample"));
		return nullptr;
	}

	uint64_t cost = uint64_t(w) * uint64_t(h) * kBytesPerPixel;
	if (*retained + cost > kSequenceBudget) {
		add_warning(ctx, _("the image sequence is too long to keep in memory"));
		return nullptr;
	}

	ImagePtr frame = image_new(uint32_t(w), uint32_t(h));
	if (!frame) {
		add_warning(ctx, _("image allocation failure"));
		return nullptr;
	}

	*retained += cost;
	pack_rgba16le_to_bgra16(
		*frame, assume_aligned<const uint16_t>(src), size_t(stride), bits);
	if (!has_alpha)
		force_opaque(*frame);
	return frame;
}

// Decodes the file's first image sequence track into a chain of animation
// frames, each finished on its own.  Null when there is no such track, or
// when not even its first sample could be decoded.
static ImagePtr
load_heif_sequence(heif_context *hctx, const OpenContext &ctx)
{
	if (!heif_context_has_sequence(hctx))
		return nullptr;

	// Zero asks for the first visual track; video is somebody else's job.
	heif_track *track = heif_context_get_track(hctx, 0);
	if (!track)
		return nullptr;

	uint32_t timescale = heif_track_get_timescale(track);
	if (heif_track_get_track_handler_type(track) !=
			heif_track_type_image_sequence ||
		!timescale) {
		heif_track_release(track);
		return nullptr;
	}

	// The runtime may be older than the headers were.  Version 8 brought
	// `ignore_sequence_editlist`, and without it libheif replays the edit
	// list: an infinite one never reaches the end of the sequence, and a
	// finite one duplicates frames in memory.
	heif_decoding_options *opts = heif_decoding_options_alloc();
	if (opts->version < 8) {
		add_warning(ctx, _("libheif is too old to decode image sequences"));
		heif_decoding_options_free(opts);
		heif_track_release(track);
		return nullptr;
	}
	opts->ignore_sequence_editlist = 1;

	bool has_alpha = heif_track_has_alpha_channel(track);

	// Samples carry no colour information of their own, so the primary still
	// item's profile stands in for it, and the first sample's for that.
	vector<uint8_t> inherited = heif_primary_profile(hctx, ctx);
	shared_ptr<Profile> inherited_profile;
	bool inherited_assumed = false;

	ImagePtr head, tail;
	uint64_t retained = 0;
	while (true) {
		heif_image *img = nullptr;
		heif_error err = heif_track_decode_next_image(track, &img,
			heif_colorspace_RGB, heif_chroma_interleaved_RRGGBBAA_LE, opts);
		if (err.code == heif_error_End_of_sequence)
			break;
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			break;
		}

		ImagePtr frame = load_heif_sample(img, has_alpha, &retained, ctx);
		if (!frame) {
			heif_image_release(img);
			break;
		}

		// The duration is in the track's own timescale, not the sequence's,
		// and fits int64_t because libheif reports it as a uint32_t.
		frame->frame_duration =
			int64_t(uint64_t(heif_image_get_duration(img)) * 1000 / timescale);
		frame->icc = heif_sample_profile(img, ctx);
		bool bitstream_premul =
			has_alpha && heif_image_is_premultiplied_alpha(img);
		heif_image_release(img);

		// finish_frames() would force the head's profile onto every frame,
		// overriding whatever they embed themselves.
		Profile *source = nullptr;
		if (frame->icc.empty()) {
			frame->icc = inherited;
			frame->effective_profile = inherited_profile;
			frame->profile_assumed = inherited_assumed;
			source = inherited_profile.get();
		}
		finish_image(*frame, ctx, source, bitstream_premul);
		if (!inherited_profile) {
			inherited = frame->icc;
			inherited_profile = frame->effective_profile;
			inherited_assumed = frame->profile_assumed;

			// A track carries its own `colr` box, but libheif surfaces it
			// neither on the track nor on decoded samples--asking for an
			// undecided colourspace merely invents BT.709.  So a file whose
			// only image is a track loses its colour space here, and that
			// is worth saying out loud rather than quietly guessing.
			if (inherited_assumed)
				add_warning(ctx,
					_("no colour information for the image sequence, "
					  "assuming sRGB"));
		}

		append_frame(head, tail, std::move(frame));
		if (ctx.first_frame_only)
			break;
	}

	if (head) {
		// Dawn counts playbacks, with zero standing for endless.  libheif's
		// zero means it could not make sense of the edit list, and its own
		// infinity is a sentinel; neither is a count.
		uint32_t reps = heif_track_get_number_of_repetitions(track);
		if (reps == heif_sequence_track_number_of_repetitions_infinite)
			head->loops = 0;
		else if (reps)
			head->loops = reps;
		else
			head->loops = 1;
	}

	heif_decoding_options_free(opts);
	heif_track_release(track);
	return head;
}

#else  // ! DAWN_HEIF_SEQUENCES

static ImagePtr
load_heif_sequence(heif_context *, const OpenContext &)
{
	return nullptr;
}

#endif  // ! DAWN_HEIF_SEQUENCES

// -----------------------------------------------------------------------------

ImagePtr
load_heif(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// libheif will throw C++ exceptions on allocation failures.
	// The library is generally awful through and through.
	heif_context *hctx = heif_context_alloc();
	if (!hctx) {
		set_error(error, _("failed to obtain a libheif context"));
		return nullptr;
	}

	heif_error err = heif_context_read_from_memory_without_copy(
		hctx, data.data(), data.size(), nullptr);
	if (err.code != heif_error_Ok) {
		set_error(error, err.message);
		heif_context_free(hctx);
		return nullptr;
	}

	// The track supersedes the primary still item, which is just a preview
	// of it, so decoding that item would be wasted work.  A thumbnail, on
	// the other hand, is exactly what the preview is for.
	ImagePtr sequence;
	if (!ctx.first_frame_only)
		sequence = load_heif_sequence(hctx, ctx);

	heif_item_id primary_id = 0;
	if (heif_context_get_primary_image_ID(hctx, &primary_id).code)
		primary_id = 0;

	int n = heif_context_get_number_of_top_level_images(hctx);
	vector<heif_item_id> ids(size_t(max(n, 0)));
	n = heif_context_get_list_of_top_level_image_IDs(hctx, ids.data(), n);

	ImagePtr head, tail;
	for (int i = 0; i < n; i++) {
		heif_image_handle *handle = nullptr;
		err = heif_context_get_image_handle(hctx, ids[size_t(i)], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			continue;
		}

		if (sequence && ids[size_t(i)] == primary_id) {
			// Track samples come out of libheif exactly as coded: it
			// applies no `tkhd` matrix, and exposes neither that matrix nor
			// the track's display size, so a rotated track cannot even be
			// detected here, let alone turned--that needs an ISOBMFF parser
			// this loader does not have.  Sequences are therefore always
			// left unrotated, and the still item's Exif, which describes
			// the still item, must not be allowed to rotate them either.
			sequence->exif = heif_handle_exif(handle, ctx);
			if (!sequence->exif.empty())
				sequence->orientation = Orientation::Rotate0;
			append_page(head, tail, std::move(sequence));
		} else {
			Error suberror;
			ImagePtr page =
				load_heif_image(hctx, ids[size_t(i)], handle, ctx, &suberror);
			if (page)
				append_page(head, tail, std::move(page));
			else
				add_warning(ctx, suberror.message);
		}

		// TODO(p): Possibly add thumbnail images as well.
		if (!ctx.first_frame_only)
			load_heif_aux_images(ctx, hctx, handle, head, tail);

		heif_image_handle_release(handle);
		if (ctx.first_frame_only)
			break;
	}

	// A track-only file has no still item to hang the sequence off, and a
	// thumbnail of one has nothing cheaper to fall back to.
	if (sequence)
		append_page(head, tail, std::move(sequence));
	else if (!head && ctx.first_frame_only)
		append_page(head, tail, load_heif_sequence(hctx, ctx));

	heif_context_free(hctx);
	if (!head) {
		set_error(error, _("empty or unsupported image"));
		return nullptr;
	}

	// Each page was already brought to final working premul individually,
	// in load_heif_image().
	return head;
}

}  // namespace dawn

#endif  // DAWN_WITH_LIBHEIF
