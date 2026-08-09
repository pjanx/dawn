//
// load-jxr.cpp: JPEG XR image loader (jxrlib)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

// jxrlib's headers only parse as C++ in their ANSI configuration.
#define __ANSI__
#include <JXRGlue.h>

// jxrlib defines these as macros, and the standard library does not survive
// being included after that.
#undef min
#undef max

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <vector>

using namespace std;

namespace dawn
{

// --- Pixel layouts -----------------------------------------------------------

namespace
{

// How one of jxrlib's pixel formats sits in memory.  Every format we read as
// it comes is a run of unsigned samples in memory order, so the channel
// indices and the sample width say all there is to say about it.
struct Layout {
	uint8_t samples;     ///< Samples per pixel, padding included
	uint8_t bits;        ///< Bits per sample, 8 or 16
	int8_t r, g, b;      ///< Colour sample indices
	int8_t a;            ///< Alpha sample index, negative when opaque
	bool premultiplied;  ///< Whether that alpha is associated
};

struct NativeFormat {
	const PKPixelFormatGUID *format;
	Layout layout;
};

}  // namespace

// jxrlib's format converter refuses grey and premultiplied inputs outright,
// so reading these as they come is not an optimisation--it is the only way
// to load a good half of the files that exist.
static const NativeFormat kNativeFormats[] = {
	{&GUID_PKPixelFormat8bppGray, {1, 8, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat16bppGray, {1, 16, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat24bppRGB, {3, 8, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat24bppBGR, {3, 8, 2, 1, 0, -1, false}},
	{&GUID_PKPixelFormat32bppRGB, {4, 8, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat32bppBGR, {4, 8, 2, 1, 0, -1, false}},
	{&GUID_PKPixelFormat32bppRGBA, {4, 8, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat32bppBGRA, {4, 8, 2, 1, 0, 3, false}},
	{&GUID_PKPixelFormat32bppPRGBA, {4, 8, 0, 1, 2, 3, true}},
	{&GUID_PKPixelFormat32bppPBGRA, {4, 8, 2, 1, 0, 3, true}},
	{&GUID_PKPixelFormat48bppRGB, {3, 16, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBA, {4, 16, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat64bppPRGBA, {4, 16, 0, 1, 2, 3, true}},
};

// What to ask the format converter for when the file's own format is not in
// the table above.  jxrlib converts into a bare handful of formats, and only
// these three are worth having: the first keeps all 16 bits, the second keeps
// alpha, the third is the widest sink it has.  Whatever lands on the third
// loses precision--but that is the HDR family, and the working format is
// fixed-point BGRA16, so none of it could have been kept whole anyway.
static const PKPixelFormatGUID *const kConversionTargets[] = {
	&GUID_PKPixelFormat48bppRGB,
	&GUID_PKPixelFormat32bppRGBA,
	&GUID_PKPixelFormat24bppRGB,
};

// jxrlib rotates nothing: asking it to apply a container orientation with a
// quarter turn in it fails the decode outright (JxrDecApp defaults to O_NONE
// for the same reason).  So we clear the tag and hand the turn to the caller,
// like every other loader here does.  Its bits are flip-vertical,
// flip-horizontal and rotate-clockwise, applied in reverse of that order.
static constexpr Orientation kOrientations[] = {
	Orientation::Rotate0,    // O_NONE
	Orientation::Mirror180,  // O_FLIPV
	Orientation::Mirror0,    // O_FLIPH
	Orientation::Rotate180,  // O_FLIPVH
	Orientation::Rotate90,   // O_RCW
	Orientation::Mirror90,   // O_RCW_FLIPV
	Orientation::Mirror270,  // O_RCW_FLIPH
	Orientation::Rotate270,  // O_RCW_FLIPVH
};

static const Layout *
find_layout(const PKPixelFormatGUID &format)
{
	for (const NativeFormat &native : kNativeFormats)
		if (!memcmp(&format, native.format, sizeof format))
			return &native.layout;
	return nullptr;
}

// jxrlib decodes into host-endian words, and so does everything else here--
// the working format is only nominally little-endian.  Rows of 16-bit samples
// always start on an even offset, so the load stays aligned.
static uint16_t
sample(const uint8_t *pixel, const Layout &layout, int index)
{
	if (layout.bits == 8)
		return scale_nbit_to_u16(pixel[index], 8);
	return ((const uint16_t *) pixel)[index];
}

static void
write_pixels(
	const Layout &layout, const uint8_t *src, size_t stride, Image &out)
{
	size_t pixel_size = size_t(layout.samples) * (layout.bits / 8);
	for (uint32_t y = 0; y < out.height; y++) {
		const uint8_t *s = src + size_t(y) * stride;
		uint16_t *d = row_u16(out, y);
		for (uint32_t x = 0; x < out.width; x++, s += pixel_size, d += 4) {
			d[0] = sample(s, layout, layout.b);
			d[1] = sample(s, layout, layout.g);
			d[2] = sample(s, layout, layout.r);
			d[3] = layout.a < 0 ? 65535 : sample(s, layout, layout.a);
		}
	}
}

// --- Decoding context --------------------------------------------------------

namespace
{

struct JxrLoadContext {
	PKFactory *factory = nullptr;            ///< Makes streams
	PKCodecFactory *codecs = nullptr;        ///< Makes decoders and converters
	struct WMPStream *stream = nullptr;      ///< Wraps the data we were handed
	PKImageDecode *decoder = nullptr;        ///< JPEG XR decoder
	PKFormatConverter *converter = nullptr;  ///< Set for unhandled formats

	~JxrLoadContext();
};

// The decoder only closes streams it opened itself, and ours is not one.
JxrLoadContext::~JxrLoadContext()
{
	if (converter)
		converter->Release(&converter);
	if (decoder)
		decoder->Release(&decoder);
	if (stream)
		stream->Close(&stream);
	if (codecs)
		codecs->Release(&codecs);
	if (factory)
		factory->Release(&factory);
}

}  // namespace

static uint32_t
bits_per_pixel(const PKPixelFormatGUID &format)
{
	PKPixelInfo info{};
	info.pGUIDPixFmt = &format;
	if (PixelFormatLookup(&info, LOOKUP_FORWARD))
		return 0;
	return info.cbitUnit;
}

// Picks the best conversion jxrlib is willing to perform, and leaves the
// converter in `ctx` on success.  It has to be the decoder-attached spelling
// of Initialize: the format-to-format one leaves the converter without a
// source to pull from, and it reads that pointer while copying.  A converter
// that failed to initialize is not documented to be reusable, so each attempt
// gets a fresh one.
static const Layout *
open_converter(JxrLoadContext &ctx)
{
	for (const PKPixelFormatGUID *to : kConversionTargets) {
		PKFormatConverter *converter = nullptr;
		if (ctx.codecs->CreateFormatConverter(&converter))
			return nullptr;
		if (converter->Initialize(converter, ctx.decoder, nullptr, *to)) {
			converter->Release(&converter);
			continue;
		}

		ctx.converter = converter;
		return find_layout(*to);
	}
	return nullptr;
}

static ImagePtr
decode_image(JxrLoadContext &ctx, const OpenContext &octx, Error *error)
{
	PKPixelFormatGUID format{};
	I32 width = 0, height = 0;
	if (ctx.decoder->GetPixelFormat(ctx.decoder, &format) ||
		ctx.decoder->GetSize(ctx.decoder, &width, &height)) {
		set_error(error, "cannot read the JPEG XR image header");
		return nullptr;
	}
	if (width <= 0 || height <= 0 || uint32_t(width) > kMaxDimension ||
		uint32_t(height) > kMaxDimension) {
		set_error(error, "invalid image dimensions");
		return nullptr;
	}

	const Layout *layout = find_layout(format);
	if (!layout && !(layout = open_converter(ctx))) {
		set_error(error, "unsupported JPEG XR pixel format");
		return nullptr;
	}

	// The converter works in place: the buffer first receives the decoder's
	// own pixels and only then becomes the converted ones, so it has to fit
	// whichever of the two formats is the wider.
	uint32_t bits = uint32_t(layout->samples) * layout->bits;
	if (ctx.converter)
		bits = max(bits, bits_per_pixel(format));

	size_t stride = (size_t(width) * bits + 7) / 8;
	vector<uint8_t> buffer(stride * size_t(height));

	PKRect rect = {0, 0, width, height};
	ERR err = ctx.converter
		? ctx.converter->Copy(ctx.converter, &rect, buffer.data(), U32(stride))
		: ctx.decoder->Copy(ctx.decoder, &rect, buffer.data(), U32(stride));
	if (err) {
		set_error(error, "cannot decode the JPEG XR image");
		return nullptr;
	}

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	write_pixels(*layout, buffer.data(), stride, *image);

	U32 icc = 0;
	if (!ctx.decoder->GetColorContext(ctx.decoder, nullptr, &icc) && icc) {
		image->icc.resize(icc);
		if (ctx.decoder->GetColorContext(ctx.decoder, image->icc.data(), &icc))
			image->icc.clear();
	}

	ensure_working_premul(*image, octx, nullptr, layout->premultiplied);
	return image;
}

// --- Public entry point ------------------------------------------------------

ImagePtr
detail::load_jxr(
	span<const uint8_t> data, const OpenContext &octx, Error *error)
{
	// This sits ahead of the fallback loaders, so it needs to say no quickly.
	// The rest of the container is jxrlib's business.
	static const uint8_t signature[] = {'I', 'I', 0xBC};
	if (data.size() < sizeof signature ||
		memcmp(data.data(), signature, sizeof signature)) {
		set_error(error, "not a JPEG XR image");
		return nullptr;
	}

	JxrLoadContext ctx;
	if (PKCreateFactory(&ctx.factory, PK_SDK_VERSION) ||
		PKCreateCodecFactory(&ctx.codecs, WMP_SDK_VERSION)) {
		set_error(error, "failed to obtain a jxrlib decoder");
		return nullptr;
	}

	// jxrlib only ever reads from a stream it decodes, hence the cast.
	if (ctx.factory->CreateStreamFromMemory(
			&ctx.stream, (void *) data.data(), data.size())) {
		set_error(error, "failed to obtain a jxrlib stream");
		return nullptr;
	}
	if (ctx.codecs->CreateCodec(
			&IID_PKImageWmpDecode, (void **) &ctx.decoder) ||
		ctx.decoder->Initialize(ctx.decoder, ctx.stream)) {
		set_error(error, "unsupported or unrecognized JPEG XR image");
		return nullptr;
	}

	// Neither of these follows from the bitstream: the decoder starts out
	// ignoring the alpha plane, and reports rotated dimensions for an
	// orientation it then refuses to decode.
	ctx.decoder->WMP.wmiSCP.uAlphaMode = 2;
	Orientation orientation = Orientation::Unknown;
	if (U32(ctx.decoder->WMP.wmiI.oOrientation) < size(kOrientations))
		orientation = kOrientations[ctx.decoder->WMP.wmiI.oOrientation];
	ctx.decoder->WMP.wmiI.oOrientation = O_NONE;

	// The container is allowed to hold several images, and JPEG XR has no
	// notion of animation for them to be frames of.  jxrlib stubs the idea
	// out regardless: its frame count is the constant 1, and SelectFrame
	// accepts any index without doing anything.
	ImagePtr image = decode_image(ctx, octx, error);
	if (image)
		image->orientation = orientation;
	return image;
}

}  // namespace dawn
