//
// load-wuffs.cpp: BMP/GIF/NIE/PNG/TGA/WBMP loading via Wuffs
//
// Copyright The dawn Authors
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
#define WUFFS_CONFIG__MODULE__LZW
#define WUFFS_CONFIG__MODULE__NIE
#define WUFFS_CONFIG__MODULE__PNG
#define WUFFS_CONFIG__MODULE__TGA
#define WUFFS_CONFIG__MODULE__WBMP
#define WUFFS_CONFIG__MODULE__ZLIB
#include "wuffs-v0.3.c"

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace dawn
{

namespace
{

// --- Metadata pulling --------------------------------------------------------

constexpr size_t kMaxMetadataSize = 64 * 1024 * 1024;

bool
grow_metadata_buffer(vector<uint8_t> *storage, wuffs_base__io_buffer *dst,
	size_t required, Error *error)
{
	if (required > kMaxMetadataSize) {
		set_error(error, "metadata is too large");
		return false;
	}
	size_t size = storage->size();
	while (size < required)
		size = min(kMaxMetadataSize, max(required, size * 2));
	storage->resize(size);
	dst->data = wuffs_base__make_slice_u8(storage->data(), storage->size());
	return true;
}

bool
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
		set_error(error, "metadata is outside the read buffer");
		return false;
	}
	const uint64_t length64 = r.max_excl - r.min_incl;
	if (length64 > kMaxMetadataSize - dst->meta.wi ||
		!grow_metadata_buffer(
			storage, dst, dst->meta.wi + static_cast<size_t>(length64), error))
		return false;

	const size_t offset = static_cast<size_t>(r.min_incl - pos);
	const size_t length = static_cast<size_t>(length64);
	memcpy(storage->data() + dst->meta.wi, src->data.ptr + offset, length);
	dst->meta.wi += length;
	// Seeking to the end is required by at least the GIF decoder.
	src->meta.ri = static_cast<size_t>(r.max_excl - pos);
	return true;
}

bool
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
			set_error(error, "Wuffs metadata API incompatibility");
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
	double gamma = 0;  ///< From sRGB / gAMA, 0 if unset

	const OpenContext *octx = nullptr;  ///< Caller-supplied context
	shared_ptr<Cmm> cmm;                ///< CMM context, never null
	Profile *target = nullptr;          ///< Target device profile, if any
	shared_ptr<Profile> source;         ///< Source colour profile, if any

	ImagePtr result;            ///< The resulting image
	ImagePtr result_tail;       ///< The final animation frame
	ImagePtr restore_previous;  ///< Canvas before the previous frame
};

// Crops a rectangular region out of a working-format image, so that it can be
// composited at its original position with dn::blend_image().
ImagePtr
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

bool
take_reported_metadata(WuffsLoadContext &ctx, Error *error)
{
	wuffs_base__more_information minfo = {};
	vector<uint8_t> bytes;
	if (!pull_metadata(ctx.dec, ctx.src, &minfo, &bytes, error))
		return false;

	switch (wuffs_base__more_information__metadata__fourcc(&minfo)) {
	case WUFFS_BASE__FOURCC__EXIF:
		if (ctx.have_exif) {
			add_warning(*ctx.octx, "ignoring repeated Exif");
			break;
		}
		ctx.meta_exif = std::move(bytes);
		ctx.have_exif = true;
		break;
	case WUFFS_BASE__FOURCC__ICCP:
		if (ctx.have_iccp) {
			add_warning(*ctx.octx, "ignoring repeated ICC profile");
			break;
		}
		ctx.meta_iccp = std::move(bytes);
		ctx.have_iccp = true;
		break;
	case WUFFS_BASE__FOURCC__XMP:
		if (ctx.have_xmp) {
			add_warning(*ctx.octx, "ignoring repeated XMP");
			break;
		}
		ctx.meta_xmp = std::move(bytes);
		ctx.have_xmp = true;
		break;

	case WUFFS_BASE__FOURCC__SRGB:
		ctx.gamma = 2.2;
		break;
	case WUFFS_BASE__FOURCC__GAMA:
		ctx.gamma =
			1e5 / wuffs_base__more_information__metadata_parsed__gama(&minfo);
		break;

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

void
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

bool
load_wuffs_frame_compose(WuffsLoadContext &ctx, ImagePtr &image,
	const wuffs_base__frame_config &fc, Error *error)
{
	// Copy the previous frame to a new image.
	const ImagePtr &prev = ctx.result_tail;
	ImagePtr canvas = image_new(prev->width, prev->height);
	if (!canvas) {
		set_error(error, "image allocation failure");
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
			set_error(error, "image allocation failure");
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
			set_error(error, "image allocation failure");
			return false;
		}
		BlendOp op = wuffs_base__frame_config__overwrite_instead_of_blend(&fc)
			? BlendOp::Source
			: BlendOp::Over;
		blend_image(*canvas, *region, int(bounds.min_incl_x),
			int(bounds.min_incl_y), op);
	}

	image = std::move(canvas);
	return true;
}

// https://github.com/google/wuffs/blob/main/example/gifplayer/gifplayer.c
// is pure C, and a good reference.
bool
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
		set_error(error, "image allocation failure");
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
	if (ctx.source)
		image->effective_profile = ctx.source;
	ensure_working_premul(
		*image, *ctx.octx, ctx.source.get(), /*input_premul=*/false);

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
	image->frame_duration = int64_t(wuffs_base__frame_config__duration(&fc) /
		WUFFS_BASE__FLICKS_PER_MILLISECOND);

	bool ok = wuffs_base__status__is_ok(&status);
	append_frame(ctx.result, ctx.result_tail, std::move(image));
	ctx.last_fc = fc;
	return ok;
}

ImagePtr
open_wuffs(wuffs_base__image_decoder *dec, wuffs_base__io_buffer src,
	const OpenContext &octx, Error *error)
{
	WuffsLoadContext ctx;
	ctx.dec = dec;
	ctx.src = &src;
	ctx.octx = &octx;
	ctx.cmm = cmm_or_default(octx);
	ctx.target = octx.screen_profile.get();

	// TODO(p): See if something could and should be done about
	// https://www.w3.org/TR/png-hdr-pq/
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__EXIF, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__ICCP, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__SRGB, true);
	wuffs_base__image_decoder__set_report_metadata(
		ctx.dec, WUFFS_BASE__FOURCC__GAMA, true);
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
		set_error(error, "invalid Wuffs image configuration");
		return nullptr;
	}

	ctx.width = wuffs_base__pixel_config__width(&ctx.cfg.pixcfg);
	ctx.height = wuffs_base__pixel_config__height(&ctx.cfg.pixcfg);
	if (ctx.width == 0 || ctx.height == 0) {
		set_error(error, "invalid image dimensions");
		return nullptr;
	}

	// TODO(p): Improve our simplistic PNG handling of: gAMA, cHRM, sRGB.
	if (ctx.have_iccp)
		ctx.source = ctx.cmm->get_profile(ctx.meta_iccp);
	else if (isfinite(ctx.gamma) && ctx.gamma > 0)
		ctx.source = ctx.cmm->get_profile_sRGB_gamma(ctx.gamma);

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

	apply_collected_metadata(ctx);
	if (!ctx.result && error && error->message.empty())
		set_error(error, "no frames decoded");
	return ctx.result;
}

ImagePtr
open_wuffs_using(wuffs_base__image_decoder *(*allocate)(),
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	unique_ptr<wuffs_base__image_decoder, void (*)(void *)> dec(
		allocate(), &free);
	if (!dec) {
		set_error(error, "memory allocation failed or internal error");
		return nullptr;
	}

	wuffs_base__io_buffer src =
		wuffs_base__ptr_u8__reader((uint8_t *) data.data(), data.size(), true);
	return open_wuffs(dec.get(), src, ctx, error);
}

}  // namespace

// --- Public entry points -----------------------------------------------------

uint32_t
detail::wuffs_guess_fourcc(span<const uint8_t> data)
{
	wuffs_base__slice_u8 prefix =
		wuffs_base__make_slice_u8((uint8_t *) data.data(), data.size());
	int32_t fourcc = wuffs_base__magic_number_guess_fourcc(prefix, true);
	return fourcc > 0 ? uint32_t(fourcc) : 0;
}

ImagePtr
detail::load_wuffs(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	switch (wuffs_guess_fourcc(data)) {
	case WUFFS_BASE__FOURCC__BMP:
		return open_wuffs_using(
			wuffs_bmp__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__GIF:
		return open_wuffs_using(
			wuffs_gif__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__NIE:
		return open_wuffs_using(
			wuffs_nie__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__PNG:
		return open_wuffs_using(
			wuffs_png__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__TGA:
		return open_wuffs_using(
			wuffs_tga__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	case WUFFS_BASE__FOURCC__WBMP:
		return open_wuffs_using(
			wuffs_wbmp__decoder__alloc_as__wuffs_base__image_decoder, data, ctx,
			error);
	default:
		set_error(error, "unsupported or unrecognized Wuffs format");
		return nullptr;
	}
}

}  // namespace dawn
