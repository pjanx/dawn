//
// load-jxr.cpp: JPEG XR image loader (jxrlib)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

// jxrlib's headers only parse as C++ in their ANSI configuration.
#define __ANSI__
#include <JXRGlue.h>

// jxrlib defines these as macros, and the standard library does not survive
// being included after that.
#undef min
#undef max

#include <algorithm>
#include <bit>
#include <cmath>
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

// How samples are stored.  Past the unsigned ones comes the float family:
// linear light in scRGB's primaries, 1.0 at SDR white, and allowed to leave
// both behind.
enum class Sample : uint8_t {
	Unorm8,
	Unorm16,
	Half,     ///< IEEE 754 binary16
	Float,    ///< IEEE 754 binary32
	Fixed16,  ///< Signed, with 13 fractional bits
	Fixed32,  ///< Signed, with 24 fractional bits
	Rgbe,     ///< Bytes sharing an exponent in the fourth one, as Radiance's
};

// How one of jxrlib's pixel formats sits in memory.  Every format we read as
// it comes is a run of samples in memory order, so the channel indices and
// the sample type say all there is to say about it.
struct Layout {
	uint8_t samples;     ///< Samples per pixel, padding included
	Sample type;         ///< How each of them is stored
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
// The float family has to be read as it comes too, as jxrlib has no float
// target to convert it into.
static const NativeFormat kNativeFormats[] = {
	{&GUID_PKPixelFormat8bppGray, {1, Sample::Unorm8, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat16bppGray, {1, Sample::Unorm16, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat24bppRGB, {3, Sample::Unorm8, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat24bppBGR, {3, Sample::Unorm8, 2, 1, 0, -1, false}},
	{&GUID_PKPixelFormat32bppRGB, {4, Sample::Unorm8, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat32bppBGR, {4, Sample::Unorm8, 2, 1, 0, -1, false}},
	{&GUID_PKPixelFormat32bppRGBA, {4, Sample::Unorm8, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat32bppBGRA, {4, Sample::Unorm8, 2, 1, 0, 3, false}},
	{&GUID_PKPixelFormat32bppPRGBA, {4, Sample::Unorm8, 0, 1, 2, 3, true}},
	{&GUID_PKPixelFormat32bppPBGRA, {4, Sample::Unorm8, 2, 1, 0, 3, true}},
	{&GUID_PKPixelFormat48bppRGB, {3, Sample::Unorm16, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBA, {4, Sample::Unorm16, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat64bppPRGBA, {4, Sample::Unorm16, 0, 1, 2, 3, true}},

	{&GUID_PKPixelFormat16bppGrayHalf, {1, Sample::Half, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat48bppRGBHalf, {3, Sample::Half, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBHalf, {4, Sample::Half, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBAHalf, {4, Sample::Half, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat32bppGrayFloat, {1, Sample::Float, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat96bppRGBFloat, {3, Sample::Float, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat128bppRGBFloat, {4, Sample::Float, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat128bppRGBAFloat, {4, Sample::Float, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat128bppPRGBAFloat, {4, Sample::Float, 0, 1, 2, 3, true}},
	{&GUID_PKPixelFormat16bppGrayFixedPoint,
		{1, Sample::Fixed16, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat48bppRGBFixedPoint,
		{3, Sample::Fixed16, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBFixedPoint,
		{4, Sample::Fixed16, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat64bppRGBAFixedPoint,
		{4, Sample::Fixed16, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat32bppGrayFixedPoint,
		{1, Sample::Fixed32, 0, 0, 0, -1, false}},
	{&GUID_PKPixelFormat96bppRGBFixedPoint,
		{3, Sample::Fixed32, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat128bppRGBFixedPoint,
		{4, Sample::Fixed32, 0, 1, 2, -1, false}},
	{&GUID_PKPixelFormat128bppRGBAFixedPoint,
		{4, Sample::Fixed32, 0, 1, 2, 3, false}},
	{&GUID_PKPixelFormat32bppRGBE, {4, Sample::Rgbe, 0, 1, 2, -1, false}},
};

// What to ask the format converter for when the file's own format is not in
// the table above.  jxrlib converts into a bare handful of formats, and only
// these three are worth having: the first keeps all 16 bits, the second keeps
// alpha, the third is the widest sink it has.
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

static size_t
sample_size(Sample type)
{
	switch (type) {
	case Sample::Unorm16:
	case Sample::Half:
	case Sample::Fixed16:
		return 2;
	case Sample::Float:
	case Sample::Fixed32:
		return 4;
	default:
		return 1;
	}
}

// jxrlib decodes into host-endian words, and so does everything else here--
// the working format is only nominally little-endian.
static uint16_t
sample(const uint8_t *pixel, const Layout &layout, int index)
{
	if (layout.type == Sample::Unorm8)
		return scale_nbit_to_u16(pixel[index], 8);
	uint16_t value = 0;
	memcpy(&value, pixel + size_t(index) * sizeof value, sizeof value);
	return value;
}

static float
half_to_float(uint16_t half)
{
	const int exponent = half >> 10 & 0x1f, mantissa = half & 0x3ff;
	float value = INFINITY;
	if (!exponent)
		value = ldexp(float(mantissa), -24);
	else if (exponent < 31)
		value = ldexp(float(mantissa | 0x400), exponent - 25);
	else if (mantissa)
		value = NAN;
	return half & 0x8000 ? -value : value;
}

// The float family, as the jxrlib converters that it has would read it.
static float
float_sample(const uint8_t *pixel, const Layout &layout, int index)
{
	const uint8_t *p = pixel + size_t(index) * sample_size(layout.type);
	uint16_t u16 = 0;
	uint32_t u32 = 0;
	switch (layout.type) {
	case Sample::Half:
		memcpy(&u16, p, sizeof u16);
		return half_to_float(u16);
	case Sample::Fixed16:
		memcpy(&u16, p, sizeof u16);
		return float(int16_t(u16)) / (1 << 13);
	case Sample::Float:
		memcpy(&u32, p, sizeof u32);
		return bit_cast<float>(u32);
	case Sample::Fixed32:
		memcpy(&u32, p, sizeof u32);
		return float(double(int32_t(u32)) / (1 << 24));
	case Sample::Rgbe:
		return pixel[3] ? ldexp(float(pixel[index]), pixel[3] - 128 - 8) : 0;
	default:
		return 0;
	}
}

// scRGB puts 1.0 at SDR white by convention, and its colours may step out of
// its primaries, which are BT.709's.
static bool
split_pixels(const Layout &layout, const uint8_t *src, size_t stride,
	Image &out, const OpenContext &ctx, Error *error)
{
	const size_t pixel_size = size_t(layout.samples) * sample_size(layout.type);
	vector<float> rgba(size_t(out.width) * out.height * 4);
	for (uint32_t y = 0; y < out.height; y++) {
		const uint8_t *s = src + size_t(y) * stride;
		float *d = &rgba[size_t(y) * out.width * 4];
		for (uint32_t x = 0; x < out.width; x++, s += pixel_size, d += 4) {
			d[0] = float_sample(s, layout, layout.r);
			d[1] = float_sample(s, layout, layout.g);
			d[2] = float_sample(s, layout, layout.b);
			d[3] = layout.a < 0 ? 1 : float_sample(s, layout, layout.a);
		}
	}
	return split_hdr(
		out, ctx, rgba, layout.premultiplied, kRec709Primaries, error);
}

static void
write_pixels(
	const Layout &layout, const uint8_t *src, size_t stride, Image &out)
{
	size_t pixel_size = size_t(layout.samples) * sample_size(layout.type);
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
		set_error(error, _("cannot read the JPEG XR image header"));
		return nullptr;
	}
	if (width <= 0 || height <= 0 || uint32_t(width) > kMaxDimension ||
		uint32_t(height) > kMaxDimension) {
		set_error(error, _("invalid image dimensions"));
		return nullptr;
	}

	const Layout *layout = find_layout(format);
	if (!layout && !(layout = open_converter(ctx))) {
		set_error(error, _("unsupported JPEG XR pixel format"));
		return nullptr;
	}

	// The converter works in place: the buffer first receives the decoder's
	// own pixels and only then becomes the converted ones, so it has to fit
	// whichever of the two formats is the wider.
	uint32_t bits = uint32_t(layout->samples * sample_size(layout->type) * 8);
	if (ctx.converter)
		bits = max(bits, bits_per_pixel(format));

	size_t stride = (size_t(width) * bits + 7) / 8;
	vector<uint8_t> buffer(stride * size_t(height));

	PKRect rect = {0, 0, width, height};
	ERR err = ctx.converter
		? ctx.converter->Copy(ctx.converter, &rect, buffer.data(), U32(stride))
		: ctx.decoder->Copy(ctx.decoder, &rect, buffer.data(), U32(stride));
	if (err) {
		set_error(error, _("cannot decode the JPEG XR image"));
		return nullptr;
	}

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	U32 icc = 0;
	if (!ctx.decoder->GetColorContext(ctx.decoder, nullptr, &icc) && icc) {
		image->icc.resize(icc);
		if (ctx.decoder->GetColorContext(ctx.decoder, image->icc.data(), &icc))
			image->icc.clear();
	}

	// split_hdr() leaves its base straight, in the profile it names.
	if (layout->type < Sample::Half) {
		write_pixels(*layout, buffer.data(), stride, *image);
		finish_image(*image, octx, nullptr, layout->premultiplied);
	} else if (split_pixels(
				   *layout, buffer.data(), stride, *image, octx, error)) {
		finish_image(*image, octx, image->effective_profile.get(), false);
	} else {
		return nullptr;
	}
	return image;
}

// --- Public entry point ------------------------------------------------------

ImagePtr
load_jxr(span<const uint8_t> data, const OpenContext &octx, Error *error)
{
	// This sits ahead of the fallback loaders, so it needs to say no quickly.
	// The rest of the container is jxrlib's business.
	static const uint8_t signature[] = {'I', 'I', 0xBC};
	if (data.size() < sizeof signature ||
		memcmp(data.data(), signature, sizeof signature)) {
		set_error(error, _("not a JPEG XR image"));
		return nullptr;
	}

	JxrLoadContext ctx;
	if (PKCreateFactory(&ctx.factory, PK_SDK_VERSION) ||
		PKCreateCodecFactory(&ctx.codecs, WMP_SDK_VERSION)) {
		set_error(error, _("failed to obtain a jxrlib decoder"));
		return nullptr;
	}

	// jxrlib only ever reads from a stream it decodes, hence the cast.
	if (ctx.factory->CreateStreamFromMemory(
			&ctx.stream, (void *) data.data(), data.size())) {
		set_error(error, _("failed to obtain a jxrlib stream"));
		return nullptr;
	}
	if (ctx.codecs->CreateCodec(
			&IID_PKImageWmpDecode, (void **) &ctx.decoder) ||
		ctx.decoder->Initialize(ctx.decoder, ctx.stream)) {
		set_error(error, _("unsupported or unrecognized JPEG XR image"));
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
