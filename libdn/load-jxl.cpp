//
// load-jxl.cpp: JPEG XL image loader (libjxl)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#if DAWN_WITH_LIBJXL
#include <jxl/decode.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

using namespace std;

namespace dawn
{

// --- Decoding context --------------------------------------------------------

// Metadata boxes arrive in chunks we have to size ourselves; this only needs
// to be large enough that typical Exif payloads don't bounce off it.
constexpr size_t kBoxChunk = 4096;

// libjxl gives us tightly packed interleaved RGBA; the pack helper reorders to
// the BGRA working format. Requesting float output instead would only be
// truncated back to 16 bits on the way in, unless it goes to split_hdr().
constexpr JxlPixelFormat kFormat = {4, JXL_TYPE_UINT16, JXL_LITTLE_ENDIAN, 0};
constexpr JxlPixelFormat kFloatFormat = {
	4, JXL_TYPE_FLOAT, JXL_NATIVE_ENDIAN, 0};

namespace
{

struct JxlLoadContext {
	JxlDecoder *dec = nullptr;  ///< libjxl decoder
	JxlBasicInfo info = {};     ///< Codestream header
	vector<uint8_t> icc;        ///< ICC profile the output pixels are in
	uint8_t transfer = 0;       ///< H.273 transfer, when true HDR
	double primaries[6] = {};   ///< CIE 1931 xy of a true HDR encoding
	vector<uint8_t> scratch;    ///< Interleaved RGBA buffer for one frame
	int64_t duration_ms = 0;    ///< Duration of the frame being decoded

	vector<uint8_t> box;                  ///< Payload of the box in progress
	vector<uint8_t> *box_dest = nullptr;  ///< Where `box` lands, if wanted
	vector<uint8_t> meta_exif;            ///< Exif, if any was found
	vector<uint8_t> meta_xmp;             ///< XMP, if any was found
	vector<uint8_t> meta_jhgm;            ///< Gain map bundle, if any

	const OpenContext *octx = nullptr;  ///< Caller-supplied context

	ImagePtr result;       ///< The resulting image
	ImagePtr result_tail;  ///< The final animation frame

	~JxlLoadContext();
};

}  // namespace

JxlLoadContext::~JxlLoadContext()
{
	if (dec)
		JxlDecoderDestroy(dec);
}

static bool
setup_decoder(JxlLoadContext &ctx, span<const uint8_t> data, Error *error)
{
	if (JxlDecoderSubscribeEvents(ctx.dec,
			JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME |
				JXL_DEC_FULL_IMAGE | JXL_DEC_BOX) != JXL_DEC_SUCCESS) {
		set_error(error, _("failed to subscribe to libjxl events"));
		return false;
	}

	// dawn rotates at display time, off Image::orientation, so letting libjxl
	// bake the orientation into the pixels here would apply it twice.
	if (JxlDecoderSetKeepOrientation(ctx.dec, JXL_TRUE) != JXL_DEC_SUCCESS) {
		set_error(error, _("failed to retain the JPEG XL orientation"));
		return false;
	}

	// Best-effort: metadata boxes may be Brotli-compressed (brob).
	JxlDecoderSetDecompressBoxes(ctx.dec, JXL_TRUE);

	if (JxlDecoderSetInput(ctx.dec, data.data(), data.size()) !=
		JXL_DEC_SUCCESS) {
		set_error(error, _("failed to hand the data to libjxl"));
		return false;
	}

	JxlDecoderCloseInput(ctx.dec);
	return true;
}

// --- Metadata boxes ----------------------------------------------------------

// Closes off the box in progress, trimming it to what libjxl really wrote.
static void
finish_box(JxlLoadContext &ctx)
{
	if (!ctx.box_dest)
		return;

	ctx.box.resize(ctx.box.size() - JxlDecoderReleaseBoxBuffer(ctx.dec));
	*ctx.box_dest = std::move(ctx.box);
	ctx.box = {};
	ctx.box_dest = nullptr;
}

// Starts collecting a box, if it is one of those we care about. The rest
// are left alone for libjxl to skip over.
static void
start_box(JxlLoadContext &ctx)
{
	finish_box(ctx);

	JxlBoxType type = {};
	if (JxlDecoderGetBoxType(ctx.dec, type, JXL_TRUE) != JXL_DEC_SUCCESS)
		return;
	if (!memcmp(type, "Exif", sizeof type))
		ctx.box_dest = &ctx.meta_exif;
	else if (!memcmp(type, "xml ", sizeof type))
		ctx.box_dest = &ctx.meta_xmp;
	else if (!memcmp(type, "jhgm", sizeof type))
		ctx.box_dest = &ctx.meta_jhgm;
	else
		return;

	ctx.box.resize(kBoxChunk);
	JxlDecoderSetBoxBuffer(ctx.dec, ctx.box.data(), ctx.box.size());
}

// Doubles the payload buffer of a box that outgrew it.
static void
expand_box(JxlLoadContext &ctx)
{
	size_t used = ctx.box.size() - JxlDecoderReleaseBoxBuffer(ctx.dec);
	ctx.box.resize(ctx.box.size() * 2);
	JxlDecoderSetBoxBuffer(
		ctx.dec, ctx.box.data() + used, ctx.box.size() - used);
}

// --- Frame decoding ----------------------------------------------------------

// The profile the returned pixels are actually in. We do no conversion here;
// dawn colour-manages from this, as it does for every other loader.
static void
take_icc_profile(JxlLoadContext &ctx)
{
	size_t size = 0;
	if (JxlDecoderGetICCProfileSize(
			ctx.dec, JXL_COLOR_PROFILE_TARGET_DATA, &size) != JXL_DEC_SUCCESS ||
		!size)
		return;

	ctx.icc.resize(size);
	if (JxlDecoderGetColorAsICCProfile(ctx.dec, JXL_COLOR_PROFILE_TARGET_DATA,
			ctx.icc.data(), size) != JXL_DEC_SUCCESS) {
		ctx.icc.clear();
		add_warning(*ctx.octx, _("failed to read the ICC profile"));
	}
}

// PQ and HLG go to split_hdr(), and so do float codestreams in linear light,
// like EXR, whatever their peak.  All of them need to be in primaries of a D65
// white, and described by an encoding, not by an ICC profile.
static void
take_hdr_encoding(JxlLoadContext &ctx)
{
	JxlColorEncoding e = {};
	if (JxlDecoderGetColorAsEncodedProfile(
			ctx.dec, JXL_COLOR_PROFILE_TARGET_DATA, &e) != JXL_DEC_SUCCESS ||
		e.white_point != JXL_WHITE_POINT_D65 ||
		(e.color_space != JXL_COLOR_SPACE_RGB &&
			e.color_space != JXL_COLOR_SPACE_GRAY))
		return;

	// Any primaries of a D65 white leave grey alone.
	const double *primaries = nullptr;
	const double custom[6] = {e.primaries_red_xy[0], e.primaries_red_xy[1],
		e.primaries_green_xy[0], e.primaries_green_xy[1],
		e.primaries_blue_xy[0], e.primaries_blue_xy[1]};
	if (e.color_space == JXL_COLOR_SPACE_GRAY ||
		e.primaries == JXL_PRIMARIES_SRGB)
		primaries = kRec709Primaries;
	else if (e.primaries == JXL_PRIMARIES_2100)
		primaries = kRec2020Primaries;
	else if (e.primaries == JXL_PRIMARIES_P3)
		primaries = kP3Primaries;
	else if (e.primaries == JXL_PRIMARIES_CUSTOM)
		primaries = custom;
	else
		return;

	if (e.transfer_function == JXL_TRANSFER_FUNCTION_PQ)
		ctx.transfer = 16;
	else if (e.transfer_function == JXL_TRANSFER_FUNCTION_HLG)
		ctx.transfer = 18;
	else if (e.transfer_function == JXL_TRANSFER_FUNCTION_LINEAR &&
		ctx.info.exponent_bits_per_sample)
		ctx.transfer = 8;
	copy(primaries, primaries + 6, ctx.primaries);
}

// Frame durations count ticks, whose length the codestream header defines as
// a fraction of a second.
static bool
take_frame_header(JxlLoadContext &ctx, Error *error)
{
	JxlFrameHeader frame = {};
	if (JxlDecoderGetFrameHeader(ctx.dec, &frame) != JXL_DEC_SUCCESS) {
		set_error(error, _("failed to read a frame header"));
		return false;
	}

	ctx.duration_ms = 0;
	if (ctx.info.have_animation && ctx.info.animation.tps_numerator) {
		uint64_t ticks =
			uint64_t(frame.duration) * ctx.info.animation.tps_denominator;
		ctx.duration_ms = ticks > uint64_t(INT64_MAX) / 1000
			? INT64_MAX
			: int64_t(ticks * 1000 / ctx.info.animation.tps_numerator);
	}
	return true;
}

static bool
bind_frame_buffer(JxlLoadContext &ctx, Error *error)
{
	const JxlPixelFormat *format = ctx.transfer == 8 ? &kFloatFormat : &kFormat;
	size_t size = 0;
	if (JxlDecoderImageOutBufferSize(ctx.dec, format, &size) !=
		JXL_DEC_SUCCESS) {
		set_error(error, _("failed to size the output buffer"));
		return false;
	}

	ctx.scratch.resize(size);
	if (JxlDecoderSetImageOutBuffer(ctx.dec, format, ctx.scratch.data(),
			ctx.scratch.size()) != JXL_DEC_SUCCESS) {
		set_error(error, _("failed to set the output buffer"));
		return false;
	}
	return true;
}

// Linear light, 1.0 at SDR white by convention: a linear header's
// intensity_target is a loose upper bound, which encoders fill in by default.
static bool
split_float_frame(
	JxlLoadContext &ctx, const OpenContext &octx, Image &image, Error *error)
{
	span<float> rgba(assume_aligned<float>(ctx.scratch.data()),
		ctx.scratch.size() / sizeof(float));
	return split_hdr(image, octx, rgba,
		ctx.info.alpha_bits && ctx.info.alpha_premultiplied, ctx.primaries,
		error);
}

// PQ and HLG come as the signal, which split_hdr_signal() takes in 16 bits
// without loss that matters.  HLG's display peak is intensity_target.
static bool
split_signal_frame(
	JxlLoadContext &ctx, const OpenContext &octx, Image &image, Error *error)
{
	const double peak =
		ctx.info.intensity_target > 0 ? ctx.info.intensity_target : 1000;
	return split_hdr_signal(image, octx, ctx.transfer, ctx.primaries, peak,
		ctx.info.alpha_bits && ctx.info.alpha_premultiplied, error);
}

static bool
append_decoded_frame(JxlLoadContext &ctx, Error *error)
{
	// This also catches a frame arriving before the header we subscribed to.
	if (!ctx.info.xsize || !ctx.info.ysize) {
		set_error(error, _("invalid image dimensions"));
		return false;
	}

	ImagePtr image = image_new(ctx.info.xsize, ctx.info.ysize);
	if (!image) {
		set_error(error, _("image allocation failure"));
		return false;
	}

	// Coalescing stays on, so every frame covers the whole canvas.
	if (ctx.transfer != 8)
		pack_rgba16le_to_bgra16(*image,
			assume_aligned<const uint16_t>(ctx.scratch.data()),
			size_t(ctx.info.xsize) * 4 * sizeof(uint16_t), 16);

	image->icc = ctx.icc;
	image->orientation = Orientation(ctx.info.orientation);
	image->frame_duration = ctx.duration_ms;
	if (ctx.info.have_animation)
		image->loops = ctx.info.animation.num_loops;

	// split_hdr() leaves its base straight, in the profile it names.
	// Maps are per page, and only still images have them.
	if (ctx.transfer) {
		OpenContext octx = *ctx.octx;
		octx.gain_maps = octx.gain_maps && !ctx.info.have_animation;
		if (!(ctx.transfer == 8 ? split_float_frame(ctx, octx, *image, error)
								: split_signal_frame(ctx, octx, *image, error)))
			return false;
		finish_frames(
			*image, *ctx.octx, image->effective_profile.get(), false);
	} else {
		finish_frames(*image, *ctx.octx, nullptr,
			ctx.info.alpha_bits && ctx.info.alpha_premultiplied);
	}
	append_frame(ctx.result, ctx.result_tail, std::move(image));
	return true;
}

// Pumps the decoder once, dispatching whatever it has to report. Returns
// false on failure, and sets `done` once there is nothing left to decode.
static bool
process_event(JxlLoadContext &ctx, bool *done, Error *error)
{
	switch (JxlDecoderProcessInput(ctx.dec)) {
	case JXL_DEC_ERROR:
		set_error(error, _("invalid or unsupported JPEG XL data"));
		return false;
	case JXL_DEC_NEED_MORE_INPUT:
		set_error(error, _("truncated JPEG XL data"));
		return false;
	case JXL_DEC_BASIC_INFO:
		if (JxlDecoderGetBasicInfo(ctx.dec, &ctx.info) != JXL_DEC_SUCCESS) {
			set_error(error, _("failed to read the JPEG XL header"));
			return false;
		}
		break;
	case JXL_DEC_COLOR_ENCODING:
		take_icc_profile(ctx);
		take_hdr_encoding(ctx);
		break;
	case JXL_DEC_FRAME:
		return take_frame_header(ctx, error);
	case JXL_DEC_NEED_IMAGE_OUT_BUFFER:
		return bind_frame_buffer(ctx, error);
	case JXL_DEC_FULL_IMAGE:
		if (!append_decoded_frame(ctx, error))
			return false;
		// Any trailing metadata boxes are given up on here, which only
		// costs us Exif we would not have shown on a thumbnail anyway.
		*done = ctx.octx->first_frame_only;
		break;
	case JXL_DEC_BOX:
		start_box(ctx);
		break;
	case JXL_DEC_BOX_NEED_MORE_OUTPUT:
		expand_box(ctx);
		break;
	case JXL_DEC_SUCCESS:
		*done = true;
		break;
	default:
		set_error(error, _("unexpected libjxl decoder state"));
		return false;
	}
	return true;
}

// --- Gain maps ---------------------------------------------------------------

// Only an SDR base takes a map.  A true HDR one went through split_hdr(),
// and any other HDR one is left alone, with a warning.
static void
attach_jxl_gain_map(JxlLoadContext &ctx)
{
	const OpenContext &octx = *ctx.octx;
	if (!octx.gain_maps || ctx.meta_jhgm.empty() || !ctx.result ||
		ctx.info.have_animation || ctx.transfer)
		return;

	span<const uint8_t> blob, codestream;
	GainMap metadata;
	if (!split_jhgm_bundle(ctx.meta_jhgm, &blob, &codestream) ||
		!parse_iso_gain_map(blob, &metadata) ||
		!gain_map_applies(metadata, octx))
		return;

	// The map is a naked codestream of its own.  Nothing but the pixels:
	// no conversion, no recursion.
	OpenContext map_ctx;
	map_ctx.cmm = octx.cmm;
	map_ctx.first_frame_only = true;
	Error error;
	ImagePtr pixels = load_jxl(codestream, map_ctx, &error);
	if (pixels)
		ctx.result->gain_map = make_gain_map(*pixels, metadata, false);
	else
		add_warning(
			octx, format_message(_("gain map: %s"), error.message.c_str()));
}

// --- Public entry point ------------------------------------------------------

ImagePtr
load_jxl(span<const uint8_t> data, const OpenContext &octx, Error *error)
{
	JxlLoadContext ctx;
	ctx.octx = &octx;
	ctx.dec = JxlDecoderCreate(nullptr);
	if (!ctx.dec) {
		set_error(error, _("failed to obtain a libjxl decoder"));
		return nullptr;
	}
	if (!setup_decoder(ctx, data, error))
		return nullptr;

	for (bool done = false; !done;)
		if (!process_event(ctx, &done, error))
			return nullptr;

	finish_box(ctx);
	attach_jxl_gain_map(ctx);
	if (!ctx.result) {
		set_error(error, _("empty or unsupported image"));
		return nullptr;
	}

	// The codestream orientation is authoritative per the specification
	// (ISO/IEC 18181-2), and it has already been set on the image; Exif is
	// expected to agree, and open_from_data() no longer consults it.
	if (!ctx.meta_exif.empty())
		ctx.result->exif = iso_exif_payload(ctx.meta_exif);
	if (!ctx.meta_xmp.empty())
		ctx.result->xmp = std::move(ctx.meta_xmp);

	// Each frame was already brought to final working premul.
	return ctx.result;
}

}  // namespace dawn

#endif  // DAWN_WITH_LIBJXL
