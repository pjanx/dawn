//
// load-jxl.cpp: JPEG XL image loader (libjxl)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#if DAWN_WITH_LIBJXL
#include <jxl/decode.h>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// --- Decoding context --------------------------------------------------------

// Metadata boxes arrive in chunks we have to size ourselves; this only needs
// to be large enough that typical Exif payloads don't bounce off it.
constexpr size_t kBoxChunk = 4096;

// libjxl gives us tightly packed interleaved RGBA; the pack helper reorders to
// the BGRA working format. Requesting float output instead would only be
// truncated back to 16 bits on the way in.
constexpr JxlPixelFormat kFormat = {4, JXL_TYPE_UINT16, JXL_LITTLE_ENDIAN, 0};

struct JxlLoadContext {
	JxlDecoder *dec = nullptr;  ///< libjxl decoder
	JxlBasicInfo info = {};     ///< Codestream header
	vector<uint8_t> icc;        ///< ICC profile the output pixels are in
	vector<uint8_t> scratch;    ///< Interleaved RGBA16 buffer for one frame
	int64_t duration_ms = 0;    ///< Duration of the frame being decoded

	vector<uint8_t> box;                  ///< Payload of the box in progress
	vector<uint8_t> *box_dest = nullptr;  ///< Where `box` lands, if wanted
	vector<uint8_t> meta_exif;            ///< Exif, if any was found
	vector<uint8_t> meta_xmp;             ///< XMP, if any was found

	const OpenContext *octx = nullptr;  ///< Caller-supplied context

	ImagePtr result;       ///< The resulting image
	ImagePtr result_tail;  ///< The final animation frame

	~JxlLoadContext();
};

JxlLoadContext::~JxlLoadContext()
{
	if (dec)
		JxlDecoderDestroy(dec);
}

bool
setup_decoder(JxlLoadContext &ctx, span<const uint8_t> data, Error *error)
{
	if (JxlDecoderSubscribeEvents(ctx.dec,
			JXL_DEC_BASIC_INFO | JXL_DEC_COLOR_ENCODING | JXL_DEC_FRAME |
				JXL_DEC_FULL_IMAGE | JXL_DEC_BOX) != JXL_DEC_SUCCESS) {
		set_error(error, "failed to subscribe to libjxl events");
		return false;
	}

	// dawn rotates at display time, off Image::orientation, so letting libjxl
	// bake the orientation into the pixels here would apply it twice.
	if (JxlDecoderSetKeepOrientation(ctx.dec, JXL_TRUE) != JXL_DEC_SUCCESS) {
		set_error(error, "failed to retain the JPEG XL orientation");
		return false;
	}

	// Best-effort: metadata boxes may be Brotli-compressed (brob).
	JxlDecoderSetDecompressBoxes(ctx.dec, JXL_TRUE);

	if (JxlDecoderSetInput(ctx.dec, data.data(), data.size()) !=
		JXL_DEC_SUCCESS) {
		set_error(error, "failed to hand the data to libjxl");
		return false;
	}

	JxlDecoderCloseInput(ctx.dec);
	return true;
}

// --- Metadata boxes ----------------------------------------------------------

// Closes off the box in progress, trimming it to what libjxl really wrote.
void
finish_box(JxlLoadContext &ctx)
{
	if (!ctx.box_dest)
		return;

	ctx.box.resize(ctx.box.size() - JxlDecoderReleaseBoxBuffer(ctx.dec));
	*ctx.box_dest = std::move(ctx.box);
	ctx.box = {};
	ctx.box_dest = nullptr;
}

// Starts collecting a box, if it is one of the two we care about. The rest
// are left alone for libjxl to skip over.
void
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
	else
		return;

	ctx.box.resize(kBoxChunk);
	JxlDecoderSetBoxBuffer(ctx.dec, ctx.box.data(), ctx.box.size());
}

// Doubles the payload buffer of a box that outgrew it.
void
expand_box(JxlLoadContext &ctx)
{
	size_t used = ctx.box.size() - JxlDecoderReleaseBoxBuffer(ctx.dec);
	ctx.box.resize(ctx.box.size() * 2);
	JxlDecoderSetBoxBuffer(
		ctx.dec, ctx.box.data() + used, ctx.box.size() - used);
}

// Unlike the raw metadata blocks other containers hand out, an Exif box's
// payload starts with a four-byte big-endian offset to the TIFF header
// (ISO/IEC 18181-2). Left in place, the Exif parser would read that offset
// as the byte order mark and reject the whole block.
vector<uint8_t>
exif_payload(const vector<uint8_t> &box)
{
	if (box.size() < 4)
		return {};

	size_t offset = size_t(box[0]) << 24 | size_t(box[1]) << 16 |
		size_t(box[2]) << 8 | size_t(box[3]);
	if (offset > box.size() - 4)
		return {};
	return vector<uint8_t>(box.begin() + 4 + offset, box.end());
}

// --- Frame decoding ----------------------------------------------------------

// The profile the returned pixels are actually in. We do no conversion here;
// dawn colour-manages from this, as it does for every other loader.
void
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
		add_warning(*ctx.octx, "failed to read the ICC profile");
	}
}

// Frame durations count ticks, whose length the codestream header defines as
// a fraction of a second.
bool
take_frame_header(JxlLoadContext &ctx, Error *error)
{
	JxlFrameHeader frame = {};
	if (JxlDecoderGetFrameHeader(ctx.dec, &frame) != JXL_DEC_SUCCESS) {
		set_error(error, "failed to read a frame header");
		return false;
	}

	ctx.duration_ms = 0;
	if (ctx.info.have_animation && ctx.info.animation.tps_numerator) {
		ctx.duration_ms = int64_t(frame.duration) * 1000 *
			ctx.info.animation.tps_denominator /
			ctx.info.animation.tps_numerator;
	}
	return true;
}

bool
bind_frame_buffer(JxlLoadContext &ctx, Error *error)
{
	size_t size = 0;
	if (JxlDecoderImageOutBufferSize(ctx.dec, &kFormat, &size) !=
		JXL_DEC_SUCCESS) {
		set_error(error, "failed to size the output buffer");
		return false;
	}

	ctx.scratch.resize(size);
	if (JxlDecoderSetImageOutBuffer(ctx.dec, &kFormat, ctx.scratch.data(),
			ctx.scratch.size()) != JXL_DEC_SUCCESS) {
		set_error(error, "failed to set the output buffer");
		return false;
	}
	return true;
}

bool
append_decoded_frame(JxlLoadContext &ctx, Error *error)
{
	// This also catches a frame arriving before the header we subscribed to.
	if (!ctx.info.xsize || !ctx.info.ysize) {
		set_error(error, "invalid image dimensions");
		return false;
	}

	ImagePtr image = image_new(ctx.info.xsize, ctx.info.ysize);
	if (!image) {
		set_error(error, "image allocation failure");
		return false;
	}

	// Coalescing stays on, so every frame covers the whole canvas.
	pack_rgba16le_to_bgra16(*image, (const uint16_t *) ctx.scratch.data(),
		size_t(ctx.info.xsize) * 4 * sizeof(uint16_t), 16);

	image->icc = ctx.icc;
	image->orientation = Orientation(ctx.info.orientation);
	image->frame_duration = ctx.duration_ms;
	if (ctx.info.have_animation)
		image->loops = ctx.info.animation.num_loops;

	ensure_working_premul_pages(*image, *ctx.octx, nullptr,
		ctx.info.alpha_bits && ctx.info.alpha_premultiplied);
	append_frame(ctx.result, ctx.result_tail, std::move(image));
	return true;
}

// Pumps the decoder once, dispatching whatever it has to report. Returns
// false on failure, and sets `done` once there is nothing left to decode.
bool
process_event(JxlLoadContext &ctx, bool *done, Error *error)
{
	switch (JxlDecoderProcessInput(ctx.dec)) {
	case JXL_DEC_ERROR:
		set_error(error, "invalid or unsupported JPEG XL data");
		return false;
	case JXL_DEC_NEED_MORE_INPUT:
		set_error(error, "truncated JPEG XL data");
		return false;
	case JXL_DEC_BASIC_INFO:
		if (JxlDecoderGetBasicInfo(ctx.dec, &ctx.info) != JXL_DEC_SUCCESS) {
			set_error(error, "failed to read the JPEG XL header");
			return false;
		}
		break;
	case JXL_DEC_COLOR_ENCODING:
		take_icc_profile(ctx);
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
		set_error(error, "unexpected libjxl decoder state");
		return false;
	}
	return true;
}

}  // namespace

// --- Public entry point ------------------------------------------------------

ImagePtr
detail::load_jxl(
	span<const uint8_t> data, const OpenContext &octx, Error *error)
{
	JxlLoadContext ctx;
	ctx.octx = &octx;
	ctx.dec = JxlDecoderCreate(nullptr);
	if (!ctx.dec) {
		set_error(error, "failed to obtain a libjxl decoder");
		return nullptr;
	}
	if (!setup_decoder(ctx, data, error))
		return nullptr;

	for (bool done = false; !done;)
		if (!process_event(ctx, &done, error))
			return nullptr;

	finish_box(ctx);
	if (!ctx.result) {
		set_error(error, "empty or unsupported image");
		return nullptr;
	}

	// The codestream orientation is authoritative per the specification, but
	// open_from_data() prefers Exif when it parses; encoders are expected to
	// keep the two in agreement.
	if (!ctx.meta_exif.empty())
		ctx.result->exif = exif_payload(ctx.meta_exif);
	if (!ctx.meta_xmp.empty())
		ctx.result->xmp = std::move(ctx.meta_xmp);

	// Each frame was already brought to final working premul.
	return ctx.result;
}

}  // namespace dn

#endif  // DAWN_WITH_LIBJXL
