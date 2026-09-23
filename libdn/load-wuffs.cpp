//
// load-wuffs.cpp: Wuffs BMP/GIF/JPEG/NIE/PNG/PNM/QOI/TARGA/WBMP/WebP loader
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#define WUFFS_IMPLEMENTATION
#define WUFFS_CONFIG__MODULES
#define WUFFS_CONFIG__MODULE__ADLER32
#define WUFFS_CONFIG__MODULE__BASE
#define WUFFS_CONFIG__MODULE__BMP
#define WUFFS_CONFIG__MODULE__CRC32
#define WUFFS_CONFIG__MODULE__DEFLATE
#define WUFFS_CONFIG__MODULE__GIF
#define WUFFS_CONFIG__MODULE__JPEG
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__NETPBM
#define WUFFS_CONFIG__MODULE__NIE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__QOI
#define WUFFS_CONFIG__MODULE__TARGA
#define WUFFS_CONFIG__MODULE__VP8
#define WUFFS_CONFIG__MODULE__WBMP
#define WUFFS_CONFIG__MODULE__WEBP
#define WUFFS_CONFIG__MODULE__ZLIB
#ifdef __clang__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wc99-extensions"
#endif
#include "wuffs-v0.4.c"
#ifdef __clang__
#pragma GCC diagnostic pop
#endif

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace dawn
{

// --- Metadata pulling --------------------------------------------------------

constexpr size_t kMaxMetadataSize = 64 * 1024 * 1024;

static bool
grow_metadata_buffer(vector<uint8_t> *storage, wuffs_base__io_buffer *dst,
	size_t required, Error *error)
{
	if (required > kMaxMetadataSize) {
		set_error(error, _("metadata is too large"));
		return false;
	}
	size_t size = storage->size();
	while (size < required)
		size = min(kMaxMetadataSize, max(required, size * 2));
	storage->resize(size);
	dst->data = wuffs_base__make_slice_u8(storage->data(), storage->size());
	return true;
}

static bool
pull_passthrough(const wuffs_base__more_information *minfo,
	wuffs_base__io_buffer *src, vector<uint8_t> *storage,
	wuffs_base__io_buffer *dst, Error *error)
{
	wuffs_base__range_ie_u64 r =
		wuffs_base__more_information__metadata_raw_passthrough__range(minfo);
	if (wuffs_base__range_ie_u64__is_empty(&r))
		return true;

	// This should currently be zero, because we read files all at once.
	uint64_t pos = src->meta.pos;
	if (pos > r.min_incl ||
		wuffs_base__u64__sat_sub(r.max_excl, pos) > src->meta.wi) {
		set_error(error, _("metadata is outside the read buffer"));
		return false;
	}
	const uint64_t length64 = r.max_excl - r.min_incl;
	if (length64 > kMaxMetadataSize - dst->meta.wi ||
		!grow_metadata_buffer(
			storage, dst, dst->meta.wi + size_t(length64), error))
		return false;

	const size_t offset = size_t(r.min_incl - pos);
	const size_t length = size_t(length64);
	memcpy(storage->data() + dst->meta.wi, src->data.ptr + offset, length);
	dst->meta.wi += length;
	// Seeking to the end is required by at least the GIF decoder.
	src->meta.ri = size_t(r.max_excl - pos);
	return true;
}

static bool
pull_metadata(wuffs_base__image_decoder *dec, wuffs_base__io_buffer *src,
	wuffs_base__more_information *minfo, vector<uint8_t> *out, Error *error)
{
	out->resize(8192);
	wuffs_base__io_buffer dst =
		wuffs_base__ptr_u8__writer(out->data(), out->size());
	while (true) {
		*minfo = wuffs_base__empty_more_information();
		wuffs_base__status status =
			wuffs_base__image_decoder__tell_me_more(dec, &dst, minfo, src);
		switch (minfo->flavor) {
		case 0:
			// Most likely as a result of an error, we'll handle that below.
		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_RAW_TRANSFORM:
			// Wuffs is reading it into the buffer.
		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_PARSED:
			// Use Wuffs accessor functions in the caller.
			break;
		default:
			set_error(error, _("Wuffs metadata API incompatibility"));
			return false;

		case WUFFS_BASE__MORE_INFORMATION__FLAVOR__METADATA_RAW_PASSTHROUGH:
			if (!pull_passthrough(minfo, src, out, &dst, error))
				return false;
		}

		if (wuffs_base__status__is_ok(&status)) {
			out->resize(dst.meta.wi);
			return true;
		}

		if (status.repr != wuffs_base__suspension__even_more_information &&
			status.repr != wuffs_base__suspension__short_write) {
			set_error(error, wuffs_base__status__message(&status));
			return false;
		}
		if (status.repr == wuffs_base__suspension__short_write &&
			!grow_metadata_buffer(out, &dst, dst.data.len + 1, error))
			return false;
	}
}

// --- Frame decoding and composition ------------------------------------------

namespace
{

struct WuffsLoadContext {
	wuffs_base__image_decoder *dec = nullptr;  ///< Wuffs decoder abstraction
	wuffs_base__io_buffer *src = nullptr;      ///< Wuffs source buffer
	wuffs_base__image_config cfg = {};         ///< Wuffs image configuration
	vector<uint8_t> workbuf_storage;           ///< Work buffer for Wuffs
	wuffs_base__slice_u8 workbuf = {};         ///< Slice into workbuf_storage
	wuffs_base__frame_config last_fc = {};     ///< Previous frame configuration
	uint32_t width = 0;                        ///< Copied from cfg.pixcfg
	uint32_t height = 0;                       ///< Copied from cfg.pixcfg

	vector<uint8_t> meta_exif;  ///< Exif, if any was found
	vector<uint8_t> meta_iccp;  ///< ICC profile, if any was found
	vector<uint8_t> meta_xmp;   ///< XMP, if any was found
	bool have_exif = false;
	bool have_iccp = false;
	bool have_xmp = false;
	unordered_map<string, string> texts;  ///< PNG tEXt/zTXt/iTXt key-values
	string pending_key;                   ///< KVP key awaiting a value
	bool have_pending_key = false;
	bool have_srgb = false;           ///< PNG sRGB chunk seen
	optional<double> gamma;           ///< Decoding exponent from PNG gAMA
	optional<array<double, 8>> chrm;  ///< PNG cHRM: white xy, then R, G, B xy
	uint8_t hdr_transfer = 0;         ///< PNG cICP transfer, when PQ or HLG
	double hdr_primaries[6] = {};     ///< PNG cICP primaries, likewise

	const OpenContext *octx = nullptr;  ///< Caller-supplied context
	shared_ptr<Cmm> cmm;                ///< CMM context, never null
	Profile *target = nullptr;          ///< Target device profile, if any
	shared_ptr<Profile> source;         ///< Source colour profile, if any

	ImagePtr result;            ///< The resulting image
	ImagePtr result_tail;       ///< The final animation frame
	ImagePtr restore_previous;  ///< Canvas before the previous frame
};

}  // namespace

// Crops a rectangular region out of a working-format image, so that it can be
// composited at its original position with dn::blend_image().
static ImagePtr
crop(const Image &src, wuffs_base__rect_ie_u32 r)
{
	uint32_t w = r.max_excl_x - r.min_incl_x;
	uint32_t h = r.max_excl_y - r.min_incl_y;
	ImagePtr out = image_new(w, h);
	if (!out)
		return nullptr;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *s = row_bytes(src, r.min_incl_y + y) +
			size_t(r.min_incl_x) * kBytesPerPixel;
		memcpy(row_bytes(*out, y), s, size_t(w) * kBytesPerPixel);
	}
	return out;
}

static bool
take_reported_metadata(WuffsLoadContext &ctx, Error *error)
{
	wuffs_base__more_information minfo = {};
	vector<uint8_t> bytes;
	if (!pull_metadata(ctx.dec, ctx.src, &minfo, &bytes, error))
		return false;

	switch (wuffs_base__more_information__metadata__fourcc(&minfo)) {
	case WUFFS_BASE__FOURCC__EXIF:
		if (ctx.have_exif) {
			add_warning(*ctx.octx, _("ignoring repeated Exif"));
			break;
		}
		ctx.meta_exif = std::move(bytes);
		ctx.have_exif = true;
		break;
	case WUFFS_BASE__FOURCC__ICCP:
		if (ctx.have_iccp) {
			add_warning(*ctx.octx, _("ignoring repeated ICC profile"));
			break;
		}
		ctx.meta_iccp = std::move(bytes);
		ctx.have_iccp = true;
		break;
	case WUFFS_BASE__FOURCC__XMP:
		if (ctx.have_xmp) {
			add_warning(*ctx.octx, _("ignoring repeated XMP"));
			break;
		}
		ctx.meta_xmp = std::move(bytes);
		ctx.have_xmp = true;
		break;

	case WUFFS_BASE__FOURCC__SRGB:
		ctx.have_srgb = true;
		break;
	case WUFFS_BASE__FOURCC__GAMA:
		// The chunk stores the encoding exponent, scaled; we want its inverse.
		if (uint32_t gama =
				wuffs_base__more_information__metadata_parsed__gama(&minfo))
			ctx.gamma = 1e5 / gama;
		break;
	case WUFFS_BASE__FOURCC__CHRM: {
		array<double, 8> xy = {};
		for (uint32_t i = 0; i < xy.size(); i++) {
			const int32_t v =
				wuffs_base__more_information__metadata_parsed__chrm(&minfo, i);
			xy[i] = v / 1e5;
		}
		// Zero or negative coordinates would only give lcms a singular matrix.
		if (all_of(xy.begin(), xy.end(), [](double v) { return v > 0; }))
			ctx.chrm = xy;
		break;
	}

	case WUFFS_BASE__FOURCC__KVPK:
		ctx.pending_key.assign(bytes.begin(), bytes.end());
		ctx.have_pending_key = true;
		break;
	case WUFFS_BASE__FOURCC__KVPV:
		if (ctx.have_pending_key) {
			ctx.texts.emplace(
				std::move(ctx.pending_key), string(bytes.begin(), bytes.end()));
			ctx.have_pending_key = false;
		}
		break;
	}
	return true;
}

static void
apply_collected_metadata(WuffsLoadContext &ctx)
{
	for (Image *im = ctx.result.get(); im; im = im->frame_next.get()) {
		if (ctx.have_exif)
			im->exif = ctx.meta_exif;
		if (ctx.have_iccp)
			im->icc = ctx.meta_iccp;
		if (ctx.have_xmp)
			im->xmp = ctx.meta_xmp;
		if (!ctx.texts.empty())
			im->text = ctx.texts;
	}
}

static bool
load_wuffs_frame_compose(WuffsLoadContext &ctx, ImagePtr &image,
	const wuffs_base__frame_config &fc, Error *error)
{
	// Copy the previous frame to a new image.
	const ImagePtr &prev = ctx.result_tail;
	ImagePtr canvas = image_new(prev->width, prev->height);
	if (!canvas) {
		set_error(error, _("image allocation failure"));
		return false;
	}

	const Image &base = ctx.restore_previous ? *ctx.restore_previous : *prev;
	memcpy(canvas->data.data(), base.data.data(), base.data.size());
	ctx.restore_previous.reset();

	// Apply that frame's disposal method.
	// XXX: We do not expect opaque pictures to receive holes this way.
	wuffs_base__rect_ie_u32 bounds =
		wuffs_base__frame_config__bounds(&ctx.last_fc);
	// TODO(p): This field needs to be colour-managed.
	wuffs_base__color_u32_argb_premul bg =
		wuffs_base__frame_config__background_color(&ctx.last_fc);

	if (wuffs_base__frame_config__disposal(&ctx.last_fc) ==
			WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_BACKGROUND &&
		bounds.max_excl_x > bounds.min_incl_x &&
		bounds.max_excl_y > bounds.min_incl_y) {
		uint16_t a = uint16_t(((bg >> 24) & 0xFF) * 257u);
		uint16_t r = uint16_t(((bg >> 16) & 0xFF) * 257u);
		uint16_t g = uint16_t(((bg >> 8) & 0xFF) * 257u);
		uint16_t b = uint16_t((bg & 0xFF) * 257u);
		fill_rect(*canvas, int(bounds.min_incl_x), int(bounds.min_incl_y),
			int(bounds.max_excl_x - bounds.min_incl_x),
			int(bounds.max_excl_y - bounds.min_incl_y), b, g, r, a);
	}

	if (wuffs_base__frame_config__disposal(&fc) ==
		WUFFS_BASE__ANIMATION_DISPOSAL__RESTORE_PREVIOUS) {
		ctx.restore_previous = image_new(canvas->width, canvas->height);
		if (!ctx.restore_previous) {
			set_error(error, _("image allocation failure"));
			return false;
		}
		memcpy(ctx.restore_previous->data.data(), canvas->data.data(),
			canvas->data.size());
	}

	// Paint the current frame over that, within its bounds.
	bounds = wuffs_base__frame_config__bounds(&fc);
	if (bounds.max_excl_x > bounds.min_incl_x &&
		bounds.max_excl_y > bounds.min_incl_y) {
		ImagePtr region = crop(*image, bounds);
		if (!region) {
			set_error(error, _("image allocation failure"));
			return false;
		}
		BlendOp op = wuffs_base__frame_config__overwrite_instead_of_blend(&fc)
			? BlendOp::Source
			: BlendOp::Over;
		blend_image(*canvas, *region, int(bounds.min_incl_x),
			int(bounds.min_incl_y), op);
	}

	canvas->effective_profile = image->effective_profile;
	canvas->profile_assumed = image->profile_assumed;
	image = std::move(canvas);
	return true;
}

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference.
static bool
load_wuffs_frame(WuffsLoadContext &ctx, Error *error)
{
	wuffs_base__frame_config fc = {};
	wuffs_base__status status;
	while (true) {
		status = wuffs_base__image_decoder__decode_frame_config(
			ctx.dec, &fc, ctx.src);
		if (status.repr == wuffs_base__note__end_of_data && ctx.result)
			return false;
		if (status.repr == wuffs_base__note__metadata_reported) {
			if (!take_reported_metadata(ctx, error))
				return false;
			continue;
		}
		if (!wuffs_base__status__is_ok(&status)) {
			set_error(error, wuffs_base__status__message(&status));
			return false;
		}
		break;
	}

	ImagePtr image = image_new(ctx.width, ctx.height);
	if (!image) {
		set_error(error, _("image allocation failure"));
		ctx.result.reset();
		ctx.result_tail.reset();
		return false;
	}

	// There is no padding with our BGRA_{,NON}PREMUL_4X16LE working format.
	wuffs_base__pixel_buffer pb = {};
	status = wuffs_base__pixel_buffer__set_from_slice(&pb, &ctx.cfg.pixcfg,
		wuffs_base__make_slice_u8(image->data.data(), image->data.size()));
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));
		ctx.result.reset();
		ctx.result_tail.reset();
		return false;
	}

	status = wuffs_base__image_decoder__decode_frame(ctx.dec, &pb, ctx.src,
		WUFFS_BASE__PIXEL_BLEND__SRC, ctx.workbuf, nullptr);
	if (!wuffs_base__status__is_ok(&status)) {
		set_error(error, wuffs_base__status__message(&status));

		// The PNG decoder, at minimum, will flush any pixel data upon
		// finding out that the input is truncated, so accept whatever we get.
	}

	// We always decode into straight (non-premultiplied) pixels--Wuffs'
	// swizzler does not support every source format as a direct premultiplied
	// destination (e.g. 16-bit-per-channel truecolour PNG). Colour-manage
	// (if applicable) and premultiply now, before any compositing.
	// split_hdr() leaves its base straight, in the profile it names.
	// Maps are per page, and only still images have them.
	if (ctx.hdr_transfer) {
		OpenContext octx = *ctx.octx;
		octx.gain_maps =
			octx.gain_maps && !wuffs_base__frame_config__index(&fc);
		// HLG's nominal display, as BT.2408 has it.
		if (!split_hdr_signal(*image, octx, ctx.hdr_transfer, ctx.hdr_primaries,
				1000, false, error)) {
			ctx.result.reset();
			ctx.result_tail.reset();
			return false;
		}
	} else if (ctx.source) {
		image->effective_profile = ctx.source;
	}
	finish_image(*image, *ctx.octx, image->effective_profile.get(),
		/*input_premul=*/false);

	// Single-frame images get a fast path, animations are handled slowly.
	if (wuffs_base__frame_config__index(&fc) > 0 &&
		!load_wuffs_frame_compose(ctx, image, fc, error)) {
		ctx.result.reset();
		ctx.result_tail.reset();
		return false;
	}

	if (ctx.have_exif)
		image->exif = ctx.meta_exif;
	if (ctx.have_iccp)
		image->icc = ctx.meta_iccp;
	if (ctx.have_xmp)
		image->xmp = ctx.meta_xmp;
	if (!ctx.texts.empty())
		image->text = ctx.texts;

	image->loops = wuffs_base__image_decoder__num_animation_loops(ctx.dec);
	image->frame_duration = int64_t(wuffs_base__frame_config__duration(&fc)) /
		int64_t(WUFFS_BASE__FLICKS_PER_MILLISECOND);

	// So far, Wuffs animates GIF, APNG, NIA.
	// The latter is internal, so use a simple rule.
	image->browser_animation_bump = true;

	bool ok = wuffs_base__status__is_ok(&status);
	append_frame(ctx.result, ctx.result_tail, std::move(image));
	ctx.last_fc = fc;
	return ok;
}

// PNG cICP, which Wuffs does not parse, and which has to come before IDAT:
// its colour primaries and transfer characteristics, for RGB in full range.
static bool
png_cicp(span<const uint8_t> data, uint8_t *primaries, uint8_t *transfer)
{
	static const uint8_t signature[] = {
		0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
	if (data.size() < sizeof signature ||
		memcmp(data.data(), signature, sizeof signature))
		return false;

	for (size_t at = sizeof signature; data.size() - at >= 12;) {
		const uint8_t *chunk = data.data() + at;
		const size_t length = size_t(chunk[0]) << 24 | size_t(chunk[1]) << 16 |
			size_t(chunk[2]) << 8 | chunk[3];
		if (!memcmp(chunk + 4, "IDAT", 4) || data.size() - at - 12 < length)
			return false;
		if (!memcmp(chunk + 4, "cICP", 4)) {
			if (length != 4 || chunk[10] || !chunk[11])
				return false;
			*primaries = chunk[8];
			*transfer = chunk[9];
			return true;
		}
		at += 12 + length;
	}
	return false;
}

static ImagePtr
open_wuffs(wuffs_base__image_decoder *dec, span<const uint8_t> data,
	const OpenContext &octx, Error *error)
{
	wuffs_base__io_buffer src =
		wuffs_base__ptr_u8__reader((uint8_t *) data.data(), data.size(), true);

	WuffsLoadContext ctx;
	ctx.dec = dec;
	ctx.src = &src;
	ctx.octx = &octx;
	ctx.cmm = cmm_or_default(octx);
	ctx.target = octx.screen_profile.get();

	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__EXIF, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__ICCP, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__SRGB, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__GAMA, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__CHRM, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__XMP, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__KVP, true);

	while (true) {
		wuffs_base__status status =
			wuffs_base__image_decoder__decode_image_config(
				ctx.dec, &ctx.cfg, ctx.src);
		if (wuffs_base__status__is_ok(&status))
			break;

		if (status.repr != wuffs_base__note__metadata_reported) {
			set_error(error, wuffs_base__status__message(&status));
			return nullptr;
		}
		if (!take_reported_metadata(ctx, error))
			return nullptr;
	}

	// This, at least currently, seems excessive.
	if (!wuffs_base__image_config__is_valid(&ctx.cfg)) {
		set_error(error, _("invalid Wuffs image configuration"));
		return nullptr;
	}

	ctx.width = wuffs_base__pixel_config__width(&ctx.cfg.pixcfg);
	ctx.height = wuffs_base__pixel_config__height(&ctx.cfg.pixcfg);
	if (ctx.width == 0 || ctx.height == 0) {
		set_error(error, _("invalid image dimensions"));
		return nullptr;
	}

	// PNG (3rd edition) Table 1: iCCP outranks sRGB, which outranks cHRM and
	// gAMA; lower-priority chunks are to be ignored.  cICP comes first, but
	// only PQ and HLG are acted on (https://www.w3.org/TR/png-hdr-pq/), as
	// honouring the rest would change how existing SDR images look.
	// A missing half of the cHRM/gAMA pair is filled in from sRGB,
	// as the specification says nothing about halves.
	uint8_t cicp_code = 0, cicp_transfer = 0;
	if (png_cicp(data, &cicp_code, &cicp_transfer) &&
		cicp_hdr(cicp_code, cicp_transfer, ctx.hdr_primaries))
		ctx.hdr_transfer = cicp_transfer;
	if (ctx.have_iccp)
		ctx.source = ctx.cmm->get_profile(ctx.meta_iccp);
	if (!ctx.source && ctx.have_srgb)
		ctx.source = ctx.cmm->get_profile_sRGB();
	if (!ctx.source && ctx.chrm)
		ctx.source = ctx.cmm->get_profile_parametric(
			ctx.gamma, ctx.chrm->data(), ctx.chrm->data() + 2);
	if (!ctx.source && ctx.gamma)
		ctx.source = ctx.cmm->get_profile_sRGB_gamma(*ctx.gamma);

	// Decode into straight (non-premultiplied) 16-bit-per-channel BGRA:
	// Wuffs' pixel swizzler does not support every source pixel format as
	// a direct premultiplied destination, so premultiplication (with or
	// without colour management) always happens as a separate step.
	wuffs_base__pixel_config__set(&ctx.cfg.pixcfg,
		WUFFS_BASE__PIXEL_FORMAT__BGRA_NONPREMUL_4X16LE,
		WUFFS_BASE__PIXEL_SUBSAMPLING__NONE, ctx.width, ctx.height);

	uint64_t workbuf_len_max_incl =
		wuffs_base__image_decoder__workbuf_len(ctx.dec).max_incl;
	if (workbuf_len_max_incl) {
		ctx.workbuf_storage.resize(workbuf_len_max_incl);
		ctx.workbuf = wuffs_base__make_slice_u8(
			ctx.workbuf_storage.data(), ctx.workbuf_storage.size());
	}

	while (load_wuffs_frame(ctx, error))
		if (octx.first_frame_only)
			break;

	// The first frame only learns that it is not a still one here.
	if (ctx.result && ctx.result->frame_next)
		ctx.result->gain_map.reset();

	apply_collected_metadata(ctx);
	if (!ctx.result && error && error->message.empty())
		set_error(error, _("no frames decoded"));
	return ctx.result;
}

static ImagePtr
open_wuffs_using(wuffs_base__image_decoder *(*allocate)(),
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	unique_ptr<wuffs_base__image_decoder, void (*)(void *)> dec(
		allocate(), &free);
	if (!dec) {
		set_error(error, _("memory allocation failed or internal error"));
		return nullptr;
	}

	return open_wuffs(dec.get(), data, ctx, error);
}

// --- Public entry points -----------------------------------------------------

bool
inflate_raw(span<const uint8_t> src, span<uint8_t> dst)
{
	unique_ptr<wuffs_deflate__decoder, void (*)(void *)> dec(
		wuffs_deflate__decoder__alloc(), &free);
	if (!dec)
		return false;

	wuffs_base__io_buffer in =
		wuffs_base__ptr_u8__reader((uint8_t *) src.data(), src.size(), true);
	wuffs_base__io_buffer out =
		wuffs_base__ptr_u8__writer(dst.data(), dst.size());

	uint8_t workbuf[WUFFS_DEFLATE__DECODER_WORKBUF_LEN_MAX_INCL_WORST_CASE];
	wuffs_base__status status = wuffs_deflate__decoder__transform_io(dec.get(),
		&out, &in, wuffs_base__make_slice_u8(workbuf, sizeof workbuf));

	// A stream that ends early or runs long is a corrupt one, either way.
	return wuffs_base__status__is_ok(&status) && out.meta.wi == dst.size();
}

ImagePtr
load_wuffs(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data.data(), data.size());
	int32_t fourcc = wuffs_base__magic_number_guess_fourcc(prefix, true);
	switch (fourcc > 0 ? uint32_t(fourcc) : 0) {
	case WUFFS_BASE__FOURCC__BMP:
		return open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__GIF:
		return open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__JPEG:
		return open_wuffs_using(
			wuffs_jpeg__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__NIE:
		return open_wuffs_using(
			wuffs_nie__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__NPBM:
		// Wuffs only implements binary P5 (PGM) and P6 (PPM);
		// the other Netpbm variants fail with "unsupported Netpbm file".
		return open_wuffs_using(
			wuffs_netpbm__decoder__alloc_as__wuffs_base__image_decoder, data,
			ctx, error);
	case WUFFS_BASE__FOURCC__PNG:
		return open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__QOI:
		return open_wuffs_using(
			wuffs_qoi__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__TGA:
		return open_wuffs_using(
			wuffs_targa__decoder__alloc_as__wuffs_base__image_decoder, data,
			ctx, error);
	case WUFFS_BASE__FOURCC__WBMP:
		return open_wuffs_using(
			wuffs_wbmp__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__WEBP:
		return open_wuffs_using(
			wuffs_webp__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	default:
		set_error(error, _("unsupported or unrecognized Wuffs format"));
		return nullptr;
	}
}

}  // namespace dawn
