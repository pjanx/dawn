//
// load-webp.cpp: WebP image loading (still and animated)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace std;

namespace dawn
{

static const char *
webp_status_string(VP8StatusCode err)
{
	switch (err) {
	case VP8_STATUS_OK:
		return "OK";
	case VP8_STATUS_OUT_OF_MEMORY:
		return "out of memory";
	case VP8_STATUS_INVALID_PARAM:
		return "invalid parameter";
	case VP8_STATUS_BITSTREAM_ERROR:
		return "bitstream error";
	case VP8_STATUS_UNSUPPORTED_FEATURE:
		return "unsupported feature";
	case VP8_STATUS_SUSPENDED:
		return "suspended";
	case VP8_STATUS_USER_ABORT:
		return "user abort";
	case VP8_STATUS_NOT_ENOUGH_DATA:
		return "not enough data";
	default:
		return "general failure";
	}
}

// Decodes a single, non-animated picture. `config->input` is expected to
// already be filled in by WebPGetFeatures(). Alpha is decoded either
// premultiplied directly (fast path, taken when no colour management needs
// to happen afterwards), or straight--in which case it is left for
// finish_image() to colour-manage and premultiply in one go.
// In either case, widen_bgra8_to_bgra16() merely widens the bytes libwebp
// produced, without touching alpha association.
static ImagePtr
load_webp_still(WebPDecoderConfig *config, const WebPData &wd, bool premultiply,
	const OpenContext &ctx, Error *error)
{
	auto width = uint32_t(config->input.width);
	auto height = uint32_t(config->input.height);
	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	config->options.use_threads = 1;
	config->output.width = config->input.width;
	config->output.height = config->input.height;
	config->output.colorspace = premultiply ? MODE_bgrA : MODE_BGRA;

	size_t stride = size_t(width) * 4;
	vector<uint8_t> buffer(stride * height);
	config->output.is_external_memory = 1;
	config->output.u.RGBA.rgba = buffer.data();
	config->output.u.RGBA.stride = int(stride);
	config->output.u.RGBA.size = buffer.size();

	WebPIDecoder *idec = WebPIDecode(nullptr, 0, config);
	if (!idec) {
		set_error(error, _("WebP decoding error"));
		return nullptr;
	}

	VP8StatusCode err = WebPIUpdate(idec, wd.bytes, wd.size);
	WebPIDelete(idec);
	if (err != VP8_STATUS_OK) {
		if (err != VP8_STATUS_SUSPENDED) {
			set_error(error,
				format_message(
					_("WebP decoding error: %s"), webp_status_string(err)));
			return nullptr;
		}

		// The undecoded remainder of the buffer is zero, i.e. transparent
		// black, which is a reasonable substitute for the missing data.
		add_warning(ctx, _("image file is truncated"));
	}

	widen_bgra8_to_bgra16(*image, buffer.data(), stride);
	return image;
}

// Fetches one already-composited frame of an animation onto its own canvas.
static ImagePtr
load_webp_frame(WebPAnimDecoder *dec, const WebPAnimInfo &info,
	int *last_timestamp, Error *error)
{
	uint8_t *buf = nullptr;
	int timestamp = 0;
	if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
		set_error(error, _("WebP decoding error"));
		return nullptr;
	}

	ImagePtr image = image_new(info.canvas_width, info.canvas_height);
	if (!image) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	size_t stride = size_t(info.canvas_width) * 4;
	widen_bgra8_to_bgra16(*image, buf, stride);

	// This API is confusing and awkward: timestamps accumulate,
	// while we want individual frame durations.
	image->frame_duration = timestamp - *last_timestamp;
	*last_timestamp = timestamp;
	return image;
}

static ImagePtr
load_webp_animated(
	const WebPData &wd, bool premultiply, const OpenContext &ctx, Error *error)
{
	WebPAnimDecoderOptions options = {};
	WebPAnimDecoderOptionsInit(&options);
	options.use_threads = 1;
	options.color_mode = premultiply ? MODE_bgrA : MODE_BGRA;

	WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &options);
	if (!dec) {
		set_error(error, _("WebP decoding error"));
		return nullptr;
	}

	WebPAnimInfo info = {};
	WebPAnimDecoderGetInfo(dec, &info);
	if (info.canvas_width > kMaxDimension ||
		info.canvas_height > kMaxDimension) {
		set_error(error, _("image dimensions overflow"));
		WebPAnimDecoderDelete(dec);
		return nullptr;
	}

	ImagePtr head, tail;
	int last_timestamp = 0;
	while (WebPAnimDecoderHasMoreFrames(dec)) {
		ImagePtr frame = load_webp_frame(dec, info, &last_timestamp, error);
		if (!frame) {
			WebPAnimDecoderDelete(dec);
			return nullptr;
		}

		append_frame(head, tail, frame);
		if (ctx.first_frame_only)
			break;
	}

	WebPAnimDecoderDelete(dec);
	if (!head)
		set_error(error, _("the animation has no frames"));
	return head;
}

// Attaches EXIF/ICCP/XMP/THUM metadata, as well as the loop count,
// from the container onto the head of the resulting image chain.
static void
load_webp_metadata(Image &image, const WebPData &wd, const OpenContext &ctx)
{
	WebPDemuxState state = WEBP_DEMUX_PARSE_ERROR;
	WebPDemuxer *demux = WebPDemuxPartial(&wd, &state);
	if (!demux) {
		add_warning(ctx, _("demux failure while reading metadata"));
		return;
	}

	WebPChunkIterator chunk_iter = {};
	uint32_t flags = WebPDemuxGetI(demux, WEBP_FF_FORMAT_FLAGS);
	if ((flags & EXIF_FLAG) &&
		WebPDemuxGetChunk(demux, "EXIF", 1, &chunk_iter)) {
		image.exif.assign(chunk_iter.chunk.bytes,
			chunk_iter.chunk.bytes + chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & ICCP_FLAG) &&
		WebPDemuxGetChunk(demux, "ICCP", 1, &chunk_iter)) {
		image.icc.assign(chunk_iter.chunk.bytes,
			chunk_iter.chunk.bytes + chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if ((flags & XMP_FLAG) &&
		WebPDemuxGetChunk(demux, "XMP ", 1, &chunk_iter)) {
		image.xmp.assign(chunk_iter.chunk.bytes,
			chunk_iter.chunk.bytes + chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (WebPDemuxGetChunk(demux, "THUM", 1, &chunk_iter)) {
		image.thum.assign(chunk_iter.chunk.bytes,
			chunk_iter.chunk.bytes + chunk_iter.chunk.size);
		WebPDemuxReleaseChunkIterator(&chunk_iter);
	}
	if (flags & ANIMATION_FLAG)
		image.loops = WebPDemuxGetI(demux, WEBP_FF_LOOP_COUNT);

	WebPDemuxDelete(demux);
}

ImagePtr
load_webp(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (data.size() < 12 || memcmp(data.data(), "RIFF", 4) ||
		memcmp(data.data() + 8, "WEBP", 4))
		return nullptr;

	// It is wholly zero-initialized by libwebp.
	WebPDecoderConfig config = {};
	if (!WebPInitDecoderConfig(&config)) {
		set_error(error, _("libwebp version mismatch"));
		return nullptr;
	}

	// TODO(p): Differentiate between a bad WebP, and not a WebP.
	WebPData wd{data.data(), data.size()};
	VP8StatusCode err = WebPGetFeatures(wd.bytes, wd.size, &config.input);
	if (err != VP8_STATUS_OK) {
		set_error(error,
			format_message(
				_("WebP decoding error: %s"), webp_status_string(err)));
		return nullptr;
	}

	// Decoding straight to premultiplied pixels is only correct when there
	// is no further colour management to perform; otherwise, alpha needs to
	// stay straight until finish_frames() gets a chance to colour-manage
	// and premultiply it as a single step.
	bool premultiply = !ctx.screen_profile;

	ImagePtr image = config.input.has_animation
		? load_webp_animated(wd, premultiply, ctx, error)
		: load_webp_still(&config, wd, premultiply, ctx, error);
	WebPFreeDecBuffer(&config.output);
	if (!image)
		return nullptr;

	// Of course everything has to use a different abstraction for metadata.
	load_webp_metadata(*image, wd, ctx);

	// `premultiply` tracks both whether the pixels are already premultiplied
	// and whether that was because no colour management was needed, so it
	// doubles as `input_premul` here: with no screen profile this is a no-op,
	// otherwise the (straight) frames are colour-managed and premultiplied.
	finish_frames(
		*image, ctx, /*source=*/nullptr, /*input_premul=*/premultiply);
	return image;
}

// --- Saving ------------------------------------------------------------------

// Unpremultiply into WebP's native ARGB8 input, without an import buffer.
static void
fill_picture(const Image &image, WebPPicture &picture)
{
	for (uint32_t y = 0; y < image.height; y++) {
		const uint16_t *src = row_u16(image, y);
		uint32_t *dst = picture.argb + size_t(y) * picture.argb_stride;
		for (uint32_t x = 0; x < image.width; x++, src += 4) {
			const uint32_t a = src[3];
			uint32_t pixel = ((a * 255 + 0x7FFF) / 0xFFFF) << 24;
			if (a) {
				for (int i = 0; i < 3; i++) {
					const uint32_t straight = min<uint32_t>(
						0xFFFF, (uint32_t(src[i]) * 0xFFFF + a / 2) / a);
					pixel |= ((straight * 255 + 0x7FFF) / 0xFFFF) << (i * 8);
				}
			}
			dst[x] = pixel;
		}
	}
}

static bool
mux_picture(WebPMux *mux, const Image &image, bool animated)
{
	WebPConfig config{};
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6))
		return false;
	config.thread_level = 1;

	WebPPicture picture{};
	WebPMemoryWriter writer{};
	WebPMemoryWriterInit(&writer);
	bool ok = WebPPictureInit(&picture);
	if (ok) {
		picture.use_argb = 1;
		picture.width = int(image.width);
		picture.height = int(image.height);
		ok = WebPPictureAlloc(&picture);
	}
	if (ok) {
		fill_picture(image, picture);
		picture.writer = WebPMemoryWrite;
		picture.custom_ptr = &writer;
		ok = WebPEncode(&config, &picture);
	}
	if (ok) {
		const WebPData data{writer.mem, writer.size};
		if (animated) {
			WebPMuxFrameInfo info{};
			info.bitstream = data;
			info.duration = int(image.frame_duration);
			info.id = WEBP_CHUNK_ANMF;
			info.dispose_method = WEBP_MUX_DISPOSE_NONE;
			info.blend_method = WEBP_MUX_NO_BLEND;
			ok = WebPMuxPushFrame(mux, &info, true) == WEBP_MUX_OK;
		} else {
			ok = WebPMuxSetImage(mux, &data, true) == WEBP_MUX_OK;
		}
	}
	WebPPictureFree(&picture);
	WebPMemoryWriterClear(&writer);
	return ok;
}

static bool
set_chunk(WebPMux *mux, const char *fourcc, span<const uint8_t> data)
{
	if (data.empty())
		return true;

	const WebPData chunk{data.data(), data.size()};
	return WebPMuxSetChunk(mux, fourcc, &chunk, false) == WEBP_MUX_OK;
}

bool
save_webp(const Image &page, const Image *frame, span<const uint8_t> icc,
	vector<uint8_t> *out, Error *error)
{
	if (!out) {
		set_error(error, _("no output buffer"));
		return false;
	}
	out->clear();

	const bool animated = !frame && page.frame_next;
	const Image *first = frame ? frame : &page;
	if (animated && page.loops > 0xFFFF) {
		set_error(error, _("animation loop count is too large for WebP"));
		return false;
	}
	for (const Image *f = first; f;
		f = animated ? f->frame_next.get() : nullptr) {
		if (!f->width || !f->height || f->width > WEBP_MAX_DIMENSION ||
			f->height > WEBP_MAX_DIMENSION) {
			set_error(error, _("image dimensions exceed WebP limits"));
			return false;
		}
		if (animated &&
			(f->frame_duration < 0 || f->frame_duration > 0xFFFFFF)) {
			set_error(error, _("animation frame duration exceeds WebP limits"));
			return false;
		}
	}

	WebPMux *mux = WebPMuxNew();
	if (!mux) {
		set_error(error, _("image allocation failure"));
		return false;
	}

	bool ok = true;
	for (const Image *f = first; ok && f;
		f = animated ? f->frame_next.get() : nullptr)
		ok = mux_picture(mux, *f, animated);
	if (animated) {
		WebPMuxAnimParams params{};
		params.loop_count = int(page.loops);
		ok = ok && WebPMuxSetAnimationParams(mux, &params) == WEBP_MUX_OK;
	}

	// The override wins outright: it describes what the pixels became, and
	// the page's own profile describes what they were.
	ok = ok && set_chunk(mux, "EXIF", page.exif) &&
		set_chunk(
			mux, "ICCP", icc.empty() ? span<const uint8_t>(page.icc) : icc) &&
		set_chunk(mux, "XMP ", page.xmp);

	WebPData assembled{};
	WebPDataInit(&assembled);
	ok = ok && WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK;
	if (ok)
		out->assign(assembled.bytes, assembled.bytes + assembled.size);
	else
		set_error(error, _("WebP encoding failed"));
	WebPDataClear(&assembled);
	WebPMuxDelete(mux);
	return ok;
}

// --- TO BE MOVED TO DNTHUMBD -------------------------------------------------

// The cache stores near-lossless WebP: thumbnails are re-encoded rarely and
// read often, and banding survives every later rescale.
bool
encode_thumbnail_webp(uint32_t width, uint32_t height, const uint8_t *rgba8,
	size_t stride, vector<uint8_t> *out, string *error)
{
	if (!out || !rgba8 || !width || !height) {
		if (error)
			*error = "invalid encode_thumbnail_webp arguments";
		return false;
	}
	out->clear();

	WebPConfig config{};
	WebPPicture picture{};
	WebPMemoryWriter writer{};
	WebPMemoryWriterInit(&writer);
	bool ok = WebPConfigInit(&config) && WebPConfigLosslessPreset(&config, 6);
	config.near_lossless = 95;
	// One image at a time, on a caller that is already a worker of its own.
	config.thread_level = 0;
	ok = ok && WebPValidateConfig(&config) && WebPPictureInit(&picture);
	if (ok) {
		picture.use_argb = 1;
		picture.width = int(width);
		picture.height = int(height);
		ok = WebPPictureImportRGBA(&picture, rgba8, int(stride));
	}
	if (ok) {
		picture.writer = WebPMemoryWrite;
		picture.custom_ptr = &writer;
		ok = WebPEncode(&config, &picture);
	}
	if (ok)
		out->assign(writer.mem, writer.mem + writer.size);
	else if (error)
		*error = "WebP encoding failed";
	WebPPictureFree(&picture);
	WebPMemoryWriterClear(&writer);
	return ok;
}

}  // namespace dawn
