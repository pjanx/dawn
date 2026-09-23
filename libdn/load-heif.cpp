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
// Gain maps need the generic item API, and the transformative properties.
#if LIBHEIF_HAVE_VERSION(1, 18, 0) && defined DAWN_HEIF_PROPERTIES
#include <libheif/heif_items.h>
#define DAWN_HEIF_GAIN_MAPS
#endif

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
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

// Decodes a single image handle into working-format pixels as they are coded,
// with no colour management, and with the bitstream's alpha association.
static ImagePtr
decode_heif_pixels(
	heif_image_handle *handle, bool ignore_transformations, Error *error)
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
	opts->ignore_transformations = ignore_transformations;
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
	heif_image_release(image);
	return result;
}

// --- True HDR ----------------------------------------------------------------

// PQ (16) or HLG (18) in an nclx box that no ICC profile overrides, with
// primaries of a D65 white, which is what split_hdr() takes.  Zero otherwise.
static uint8_t
heif_nclx_hdr_transfer(
	const heif_color_profile_nclx *nclx, double primaries[6])
{
	const auto transfer = uint8_t(nclx->transfer_characteristics);
	return cicp_hdr(uint8_t(nclx->color_primaries), transfer, primaries)
		? transfer
		: 0;
}

static uint8_t
heif_hdr_transfer(heif_image_handle *handle, double primaries[6])
{
	heif_color_profile_nclx *nclx = nullptr;
	if (heif_image_handle_get_raw_color_profile_size(handle) ||
		heif_image_handle_get_nclx_color_profile(handle, &nclx).code || !nclx)
		return 0;

	const uint8_t transfer = heif_nclx_hdr_transfer(nclx, primaries);
	heif_nclx_color_profile_free(nclx);
	return transfer;
}

// Decodes a single image handle (either a top-level image, or an auxiliary
// image such as a depth map) into one working-format page, extracting Exif
// and an embedded ICC profile, if present, and bringing it to final working
// premul before returning.
static ImagePtr
load_heif_image(heif_context *hctx, heif_item_id id, heif_image_handle *handle,
	const OpenContext &ctx, Error *error)
{
	ImagePtr result = decode_heif_pixels(handle, false, error);
	if (!result)
		return nullptr;

	// TODO(p): Test real behaviour on real transparent images.
	int has_alpha = heif_image_handle_has_alpha_channel(handle);
	bool bitstream_premul =
		has_alpha && heif_image_handle_is_premultiplied_alpha(handle);

	// libheif applies transformative properties (irot, imir) while decoding,
	// and ISO/IEC 23008-12 gives those the final say, so the Exif orientation
	// below must not rotate the pixels a second time.  Without them, Exif is
	// the only orientation the file has, and open_from_data() fills it in.
	if (heif_image_is_oriented(hctx, id))
		result->orientation = Orientation::Rotate0;

	result->exif = heif_handle_exif(handle, ctx);

	// split_hdr() leaves its base straight, in the profile it names.
	double primaries[6] = {};
	if (const uint8_t transfer = heif_hdr_transfer(handle, primaries)) {
		// HLG's nominal display, as BT.2408 has it.
		if (!split_hdr_signal(*result, ctx, transfer, primaries, 1000,
				bitstream_premul, error))
			return nullptr;
		finish_frames(*result, ctx, result->effective_profile.get(), false);
		return result;
	}
	result->icc = heif_handle_profile(handle, ctx);

	// Bring the page to final working premul: colour-manage against any
	// embedded ICC profile (derived automatically from result->icc), first
	// un-premultiplying if the bitstream declared premultiplied alpha and
	// colour management needs to happen.
	finish_frames(*result, ctx, nullptr, bitstream_premul);
	return result;
}

// --- Gain maps ---------------------------------------------------------------

#ifdef DAWN_HEIF_GAIN_MAPS

static constexpr const char *kAppleGainMapAux =
	"urn:com:apple:photo:2020:aux:hdrgainmap";

namespace
{

// libheif does not know the ISO 21496-1 `tmap` derived item, so it never
// becomes an image, but its payload and references can still be read.
struct HeifToneMap {
	heif_item_id base = 0;  ///< The SDR rendition, as a rule
	heif_item_id map = 0;   ///< Possibly a top-level image of its own
	vector<uint8_t> metadata;
};

}  // namespace

static vector<uint8_t>
heif_item_payload(heif_context *hctx, heif_item_id id)
{
	uint8_t *data = nullptr;
	size_t size = 0;
	vector<uint8_t> result;
	if (!heif_item_get_item_data(hctx, id, nullptr, &data, &size).code && data)
		result.assign(data, data + size);
	if (data)
		heif_release_item_data(hctx, &data);
	return result;
}

// The items that `from` references with the first reference of `type`,
// in file order.
static vector<heif_item_id>
heif_item_references(heif_context *hctx, heif_item_id from, uint32_t type)
{
	vector<heif_item_id> result;
	for (int index = 0; result.empty(); index++) {
		uint32_t found = 0;
		heif_item_id *to = nullptr;
		const size_t n =
			heif_context_get_item_references(hctx, from, index, &found, &to);
		if (n && found == type)
			result.assign(to, to + n);
		if (to)
			heif_release_item_references(hctx, &to);
		if (!n)
			break;
	}
	return result;
}

static vector<heif_item_id>
heif_items_of_type(heif_context *hctx, uint32_t type)
{
	int n = heif_context_get_number_of_items(hctx);
	vector<heif_item_id> ids(size_t(max(n, 0)));
	n = heif_context_get_list_of_item_IDs(hctx, ids.data(), n);
	ids.resize(size_t(max(n, 0)));
	erase_if(ids, [&](heif_item_id id) {
		return heif_item_get_item_type(hctx, id) != type;
	});
	return ids;
}

static vector<HeifToneMap>
heif_tone_maps(heif_context *hctx)
{
	vector<HeifToneMap> result;
	for (heif_item_id id :
		heif_items_of_type(hctx, heif_fourcc('t', 'm', 'a', 'p'))) {
		vector<heif_item_id> inputs =
			heif_item_references(hctx, id, heif_fourcc('d', 'i', 'm', 'g'));
		if (inputs.size() == 2)
			result.push_back(
				{inputs[0], inputs[1], heif_item_payload(hctx, id)});
	}
	return result;
}

// XMP describing `id`, from a `mime` item that references it with `cdsc`.
static string
heif_item_xmp(heif_context *hctx, heif_item_id id)
{
	for (heif_item_id mime : heif_items_of_type(hctx, heif_item_type_mime)) {
		const char *type = heif_item_get_mime_item_content_type(hctx, mime);
		if (!type || strcmp(type, "application/rdf+xml"))
			continue;

		vector<heif_item_id> described =
			heif_item_references(hctx, mime, heif_fourcc('c', 'd', 's', 'c'));
		if (find(described.begin(), described.end(), id) != described.end()) {
			vector<uint8_t> xmp = heif_item_payload(hctx, mime);
			return string(xmp.begin(), xmp.end());
		}
	}
	return {};
}

// libheif bakes the base item's own clap, irot and imir into its pixels, in
// property order, so the map gets the same, with the crop scaled to its
// resolution.  The map's own properties cannot be trusted to match: Apple
// repeats the base's irot on it, and libavif copies clap over unscaled.
static void
heif_transform_gain_map(heif_context *hctx, heif_item_id base,
	heif_image_handle *base_handle, GainMap &map)
{
	int n = heif_item_get_transformation_properties(hctx, base, nullptr, 0);
	vector<heif_property_id> properties(size_t(max(n, 0)));
	n = heif_item_get_transformation_properties(
		hctx, base, properties.data(), n);

	int width = heif_image_handle_get_ispe_width(base_handle);
	int height = heif_image_handle_get_ispe_height(base_handle);
	for (int i = 0; i < n; i++) {
		const heif_property_id property = properties[size_t(i)];
		switch (heif_item_get_property_type(hctx, base, property)) {
		case heif_item_property_type_transform_crop: {
			int left = 0, top = 0, right = 0, bottom = 0;
			heif_item_get_property_transform_crop_borders(hctx, base, property,
				width, height, &left, &top, &right, &bottom);
			if (width <= 0 || height <= 0)
				break;

			const double sx = double(map.width) / width,
						 sy = double(map.height) / height;
			const auto x0 = uint32_t(lround(left * sx)),
					   y0 = uint32_t(lround(top * sy)),
					   x1 = uint32_t(lround((width - right) * sx)),
					   y1 = uint32_t(lround((height - bottom) * sy));
			if (x1 > x0 && y1 > y0)
				crop_gain_map(map, x0, y0, x1 - x0, y1 - y0);
			width -= left + right;
			height -= top + bottom;
			break;
		}
		case heif_item_property_type_transform_rotation: {
			const int ccw = heif_item_get_property_transform_rotation_ccw(
				hctx, base, property);
			if (ccw < 0)
				break;
			rotate_gain_map(map, ccw);
			if (ccw % 180)
				swap(width, height);
			break;
		}
		case heif_item_property_type_transform_mirror:
			switch (
				heif_item_get_property_transform_mirror(hctx, base, property)) {
			case heif_transform_mirror_direction_horizontal:
				mirror_gain_map(map, true);
				break;
			case heif_transform_mirror_direction_vertical:
				mirror_gain_map(map, false);
				break;
			default:
				break;
			}
			break;
		default:
			break;
		}
	}
}

static void
attach_heif_gain_map(heif_context *hctx, heif_item_id base,
	heif_image_handle *base_handle, heif_image_handle *map_handle,
	const GainMap &metadata, bool apple, Image &page, const OpenContext &ctx)
{
	// Nothing but the pixels, in the map's stored frame.
	Error error;
	ImagePtr pixels = decode_heif_pixels(map_handle, true, &error);
	if (!pixels) {
		add_warning(
			ctx, format_message(_("gain map: %s"), error.message.c_str()));
		return;
	}
	page.gain_map = make_gain_map(*pixels, metadata, apple);
	if (page.gain_map)
		heif_transform_gain_map(hctx, base, base_handle, *page.gain_map);
}

static void
load_heif_tone_map(heif_context *hctx, heif_item_id id,
	heif_image_handle *handle, const vector<HeifToneMap> &tone_maps,
	Image &page, const OpenContext &ctx)
{
	auto tmap = find_if(tone_maps.begin(), tone_maps.end(),
		[&](const HeifToneMap &candidate) { return candidate.base == id; });
	GainMap metadata;
	if (tmap == tone_maps.end() || !ctx.gain_maps ||
		!parse_tmap_gain_map(tmap->metadata, &metadata) ||
		!gain_map_applies(metadata, ctx))
		return;

	heif_image_handle *map = nullptr;
	heif_error err = heif_context_get_image_handle(hctx, tmap->map, &map);
	if (err.code != heif_error_Ok) {
		add_warning(ctx, err.message);
		return;
	}
	attach_heif_gain_map(hctx, id, handle, map, metadata, false, page, ctx);
	heif_image_handle_release(map);
}

static bool
heif_is_apple_gain_map(const heif_image_handle *aux)
{
	const char *type = nullptr;
	if (heif_image_handle_get_auxiliary_type(aux, &type).code || !type)
		return false;

	const bool result = !strcmp(type, kAppleGainMapAux);
	heif_image_handle_release_auxiliary_type(aux, &type);
	return result;
}

// Apple's pre-ISO map, which only counts where no ISO map has been attached.
static void
load_heif_apple_gain_map(heif_context *hctx, heif_item_id base,
	heif_image_handle *base_handle, heif_item_id map_id,
	heif_image_handle *map_handle, Image &page, const OpenContext &ctx)
{
	if (!ctx.gain_maps || page.gain_map)
		return;

	const double headroom =
		apple_gain_map_headroom(heif_item_xmp(hctx, map_id), page.exif);
	if (!(headroom > 0))
		return;

	const GainMap metadata = apple_gain_map(headroom);
	if (gain_map_applies(metadata, ctx))
		attach_heif_gain_map(
			hctx, base, base_handle, map_handle, metadata, true, page, ctx);
}

#else  // ! DAWN_HEIF_GAIN_MAPS

namespace
{

struct HeifToneMap {
	heif_item_id base = 0;
	heif_item_id map = 0;
};

}  // namespace

static vector<HeifToneMap>
heif_tone_maps(heif_context *)
{
	return {};
}

static void
load_heif_tone_map(heif_context *, heif_item_id, heif_image_handle *,
	const vector<HeifToneMap> &, Image &, const OpenContext &)
{
}

static bool
heif_is_apple_gain_map(const heif_image_handle *)
{
	return false;
}

static void
load_heif_apple_gain_map(heif_context *, heif_item_id, heif_image_handle *,
	heif_item_id, heif_image_handle *, Image &, const OpenContext &)
{
}

#endif  // ! DAWN_HEIF_GAIN_MAPS

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

// Appends any auxiliary images (e.g. depth maps) hanging off `top`
// as further pages. We have no special processing for them yet,
// so they are included mainly to not lose them silently.
// Apple's gain map is not a page, but goes to `page`, if any.
static void
load_heif_aux_images(const OpenContext &ctx, heif_context *hctx,
	heif_item_id top_id, heif_image_handle *top, Image *page, ImagePtr &head,
	ImagePtr &tail)
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
		if (heif_is_apple_gain_map(handle)) {
			if (page)
				load_heif_apple_gain_map(
					hctx, top_id, top, ids[size_t(i)], handle, *page, ctx);
			heif_image_handle_release(handle);
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
// libheif attaches none to sequence samples.  PQ and HLG go to `transfer`
// and `primaries` instead, as heif_hdr_transfer() has it.
static vector<uint8_t>
heif_primary_profile(heif_context *hctx, const OpenContext &ctx,
	uint8_t *transfer, double primaries[6])
{
	heif_item_id id = 0;
	heif_image_handle *handle = nullptr;
	if (heif_context_get_primary_image_ID(hctx, &id).code ||
		heif_context_get_image_handle(hctx, id, &handle).code)
		return {};

	vector<uint8_t> icc;
	if (!(*transfer = heif_hdr_transfer(handle, primaries)))
		icc = heif_handle_profile(handle, ctx);
	heif_image_handle_release(handle);
	return icc;
}

// The sample's own colour information, which is not the track's.
// See heif_handle_profile() and heif_primary_profile().
static vector<uint8_t>
heif_sample_profile(const heif_image *img, const OpenContext &ctx,
	uint8_t *transfer, double primaries[6])
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

	vector<uint8_t> result;
	if (!(*transfer = heif_nclx_hdr_transfer(nclx, primaries)))
		result = heif_nclx_profile(nclx, ctx);
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
	uint8_t inherited_transfer = 0;
	double inherited_primaries[6] = {};
	vector<uint8_t> inherited = heif_primary_profile(
		hctx, ctx, &inherited_transfer, inherited_primaries);
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
		uint8_t transfer = 0;
		double primaries[6] = {};
		frame->icc = heif_sample_profile(img, ctx, &transfer, primaries);
		bool bitstream_premul =
			has_alpha && heif_image_is_premultiplied_alpha(img);
		heif_image_release(img);
		if (!transfer && frame->icc.empty() && inherited_transfer) {
			transfer = inherited_transfer;
			copy(begin(inherited_primaries), end(inherited_primaries),
				primaries);
		}

		// finish_frames() would force the head's profile onto every frame,
		// overriding whatever they embed themselves.
		Profile *source = nullptr;
		if (transfer) {
			// Maps are per page, and only still images have them.
			OpenContext frame_ctx = ctx;
			frame_ctx.gain_maps = false;
			Error suberror;
			if (!split_hdr_signal(*frame, frame_ctx, transfer, primaries, 1000,
					bitstream_premul, &suberror)) {
				add_warning(ctx, suberror.message);
				break;
			}
			source = frame->effective_profile.get();
			bitstream_premul = false;
		} else if (frame->icc.empty()) {
			frame->icc = inherited;
			frame->effective_profile = inherited_profile;
			frame->profile_assumed = inherited_assumed;
			source = inherited_profile.get();
		}
		finish_image(*frame, ctx, source, bitstream_premul);
		if (!inherited_profile && !transfer) {
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

	// A gain map item that is not hidden is a top-level image, but no page,
	// whether it is going to be used or not.
	const vector<HeifToneMap> tone_maps = heif_tone_maps(hctx);
	ids.resize(size_t(max(n, 0)));
	erase_if(ids, [&](heif_item_id id) {
		return any_of(tone_maps.begin(), tone_maps.end(),
			[&](const HeifToneMap &tmap) { return tmap.map == id; });
	});
	n = int(ids.size());

	ImagePtr head, tail;
	for (int i = 0; i < n; i++) {
		heif_image_handle *handle = nullptr;
		err = heif_context_get_image_handle(hctx, ids[size_t(i)], &handle);
		if (err.code != heif_error_Ok) {
			add_warning(ctx, err.message);
			continue;
		}

		Image *page = nullptr;  // What takes the file's own gain maps
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
			ImagePtr image =
				load_heif_image(hctx, ids[size_t(i)], handle, ctx, &suberror);
			if (!image) {
				add_warning(ctx, suberror.message);
			} else {
				// Where the base is true HDR, split_hdr() makes the map,
				// and the file's own are ignored.
				double primaries[6] = {};
				if (!heif_hdr_transfer(handle, primaries)) {
					page = image.get();
					load_heif_tone_map(
						hctx, ids[size_t(i)], handle, tone_maps, *page, ctx);
				}
				append_page(head, tail, std::move(image));
			}
		}

		// TODO(p): Possibly add thumbnail images as well.
		if (!ctx.first_frame_only)
			load_heif_aux_images(
				ctx, hctx, ids[size_t(i)], handle, page, head, tail);

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
