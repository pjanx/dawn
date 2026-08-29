//
// load-webp.cpp: WebP image loading (still and animated)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn-loaders.h"
#include "libdn.h"

#include <webp/decode.h>
#include <webp/demux.h>

#include <cstdint>

using namespace std;

namespace dawn
{

namespace
{

const char *
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
// ensure_working_premul() to colour-manage and premultiply in one go.
// In either case, widen_bgra8_to_bgra16() merely widens the bytes libwebp
// produced, without touching alpha association.
ImagePtr
load_webp_still(WebPDecoderConfig *config, const WebPData &wd, bool premultiply,
	const OpenContext &ctx, Error *error)
{
	auto width = uint32_t(config->input.width);
	auto height = uint32_t(config->input.height);
	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, "image allocation failure");
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
		set_error(error, "WebP decoding error");
		return nullptr;
	}

	VP8StatusCode err = WebPIUpdate(idec, wd.bytes, wd.size);
	WebPIDelete(idec);
	if (err != VP8_STATUS_OK) {
		if (err != VP8_STATUS_SUSPENDED) {
			set_error(error,
				string("WebP decoding error: ") + webp_status_string(err));
			return nullptr;
		}

		// The undecoded remainder of the buffer is zero, i.e. transparent
		// black, which is a reasonable substitute for the missing data.
		add_warning(ctx, "image file is truncated");
	}

	widen_bgra8_to_bgra16(*image, buffer.data(), stride);
	return image;
}

// Fetches one already-composited frame of an animation onto its own canvas.
ImagePtr
load_webp_frame(WebPAnimDecoder *dec, const WebPAnimInfo &info,
	int *last_timestamp, Error *error)
{
	uint8_t *buf = nullptr;
	int timestamp = 0;
	if (!WebPAnimDecoderGetNext(dec, &buf, &timestamp)) {
		set_error(error, "WebP decoding error");
		return nullptr;
	}

	ImagePtr image = image_new(info.canvas_width, info.canvas_height);
	if (!image) {
		set_error(error, "image allocation failure");
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

ImagePtr
load_webp_animated(
	const WebPData &wd, bool premultiply, const OpenContext &ctx, Error *error)
{
	WebPAnimDecoderOptions options = {};
	WebPAnimDecoderOptionsInit(&options);
	options.use_threads = 1;
	options.color_mode = premultiply ? MODE_bgrA : MODE_BGRA;

	WebPAnimDecoder *dec = WebPAnimDecoderNew(&wd, &options);
	if (!dec) {
		set_error(error, "WebP decoding error");
		return nullptr;
	}

	WebPAnimInfo info = {};
	WebPAnimDecoderGetInfo(dec, &info);
	if (info.canvas_width > kMaxDimension ||
		info.canvas_height > kMaxDimension) {
		set_error(error, "image dimensions overflow");
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
		set_error(error, "the animation has no frames");
	return head;
}

// Attaches EXIF/ICCP/XMP/THUM metadata, as well as the loop count,
// from the container onto the head of the resulting image chain.
void
load_webp_metadata(Image &image, const WebPData &wd, const OpenContext &ctx)
{
	WebPDemuxState state = WEBP_DEMUX_PARSE_ERROR;
	WebPDemuxer *demux = WebPDemuxPartial(&wd, &state);
	if (!demux) {
		add_warning(ctx, "demux failure while reading metadata");
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

}  // namespace

ImagePtr
detail::load_webp(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// It is wholly zero-initialized by libwebp.
	WebPDecoderConfig config = {};
	if (!WebPInitDecoderConfig(&config)) {
		set_error(error, "libwebp version mismatch");
		return nullptr;
	}

	// TODO(p): Differentiate between a bad WebP, and not a WebP.
	WebPData wd{data.data(), data.size()};
	VP8StatusCode err = WebPGetFeatures(wd.bytes, wd.size, &config.input);
	if (err != VP8_STATUS_OK) {
		set_error(
			error, string("WebP decoding error: ") + webp_status_string(err));
		return nullptr;
	}

	// Decoding straight to premultiplied pixels is only correct when there
	// is no further colour management to perform; otherwise, alpha needs to
	// stay straight until ensure_working_premul_pages() gets a chance to
	// colour-manage and premultiply it as a single step.
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
	ensure_working_premul_pages(
		*image, ctx, /*source=*/nullptr, /*input_premul=*/premultiply);
	return image;
}

}  // namespace dawn
