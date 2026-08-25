//
// load-openjpeg.cpp: JPEG 2000 image loader (OpenJPEG)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "dawn-config.h"
#include "libdn-loaders.h"
#include "libdn.h"

#if DAWN_WITH_OPENJPEG
#include <openjpeg.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// --- Memory stream -----------------------------------------------------------

// OpenJPEG exposes no memory stream, only callbacks to drive one with.
struct MemoryStream {
	span<const uint8_t> data;
	size_t offset = 0;
};

OPJ_SIZE_T
stream_read(void *buffer, OPJ_SIZE_T count, void *user)
{
	MemoryStream *m = (MemoryStream *) user;
	size_t left = m->data.size() - m->offset;
	if (!left)
		return (OPJ_SIZE_T) -1;  // How OpenJPEG spells end of stream.
	if (count > left)
		count = left;

	memcpy(buffer, m->data.data() + m->offset, count);
	m->offset += count;
	return count;
}

OPJ_OFF_T
stream_skip(OPJ_OFF_T count, void *user)
{
	MemoryStream *m = (MemoryStream *) user;
	size_t left = m->data.size() - m->offset;
	if (count < 0 || !left)
		return -1;
	if ((OPJ_UINT64) count > left)
		count = (OPJ_OFF_T) left;

	m->offset += (size_t) count;
	return count;
}

OPJ_BOOL
stream_seek(OPJ_OFF_T offset, void *user)
{
	MemoryStream *m = (MemoryStream *) user;
	if (offset < 0 || (OPJ_UINT64) offset > m->data.size())
		return OPJ_FALSE;

	m->offset = (size_t) offset;
	return OPJ_TRUE;
}

// --- Decoding context --------------------------------------------------------

struct OpenJpegLoadContext {
	opj_codec_t *codec = nullptr;    ///< OpenJPEG decoder
	opj_stream_t *stream = nullptr;  ///< Stream wrapping `memory`
	opj_image_t *image = nullptr;    ///< Decoded planar components
	MemoryStream memory;             ///< The data we were handed

	string message;  ///< Last error OpenJPEG reported, if any

	const OpenContext *octx = nullptr;  ///< Caller-supplied context

	~OpenJpegLoadContext();
};

OpenJpegLoadContext::~OpenJpegLoadContext()
{
	if (image)
		opj_image_destroy(image);
	if (codec)
		opj_destroy_codec(codec);
	if (stream)
		opj_stream_destroy(stream);
}

// OpenJPEG terminates its messages with a newline, which we do not want.
string
trimmed(const char *message)
{
	string out = message ? message : "";
	while (!out.empty() && (out.back() == '\n' || out.back() == '\r'))
		out.pop_back();
	return out;
}

void
on_error(const char *message, void *user)
{
	OpenJpegLoadContext *ctx = (OpenJpegLoadContext *) user;
	ctx->message = trimmed(message);
}

void
on_warning(const char *message, void *user)
{
	OpenJpegLoadContext *ctx = (OpenJpegLoadContext *) user;
	add_warning(*ctx->octx, trimmed(message));
}

// Prefers whatever OpenJPEG last had to say about the failure.
ImagePtr
fail(OpenJpegLoadContext &ctx, Error *error, const char *message)
{
	set_error(error, ctx.message.empty() ? message : ctx.message.c_str());
	return nullptr;
}

// The JP2 family opens with a signature box, a bare codestream with SOC
// followed by SIZ. This loader sits in the fallback chain, so everything
// else needs rejecting before OpenJPEG gets a say.
OPJ_CODEC_FORMAT
detect_codec(span<const uint8_t> data)
{
	static const uint8_t signature[] = {
		0, 0, 0, 0x0C, 'j', 'P', ' ', ' ', 0x0D, 0x0A, 0x87, 0x0A};
	if (data.size() >= sizeof signature &&
		!memcmp(data.data(), signature, sizeof signature))
		return OPJ_CODEC_JP2;
	if (data.size() >= 4 && data[0] == 0xFF && data[1] == 0x4F &&
		data[2] == 0xFF && data[3] == 0x51)
		return OPJ_CODEC_J2K;
	return OPJ_CODEC_UNKNOWN;
}

bool
open_codec(OpenJpegLoadContext &ctx, OPJ_CODEC_FORMAT format, Error *error)
{
	if (!(ctx.codec = opj_create_decompress(format))) {
		set_error(error, "failed to obtain an OpenJPEG decoder");
		return false;
	}

	opj_set_error_handler(ctx.codec, on_error, &ctx);
	opj_set_warning_handler(ctx.codec, on_warning, &ctx);

	opj_dparameters_t parameters = {};
	opj_set_default_decoder_parameters(&parameters);
	if (!opj_setup_decoder(ctx.codec, &parameters)) {
		set_error(error, "failed to set up the OpenJPEG decoder");
		return false;
	}

	// A truncated file still tends to decode to something worth showing.
	opj_decoder_set_strict_mode(ctx.codec, OPJ_FALSE);

	if (!(ctx.stream = opj_stream_default_create(OPJ_TRUE))) {
		set_error(error, "failed to obtain an OpenJPEG stream");
		return false;
	}

	opj_stream_set_read_function(ctx.stream, stream_read);
	opj_stream_set_skip_function(ctx.stream, stream_skip);
	opj_stream_set_seek_function(ctx.stream, stream_seek);
	opj_stream_set_user_data(ctx.stream, &ctx.memory, nullptr);
	opj_stream_set_user_data_length(ctx.stream, ctx.memory.data.size());
	return true;
}

// --- Sample conversion -------------------------------------------------------

// One component resampled onto the output grid and normalised to the full
// uint16 range. Components may be subsampled (chroma usually is), signed,
// and of any precision the codestream cares to use.
struct Sampler {
	const OPJ_INT32 *data = nullptr;  ///< Planar samples
	uint32_t w = 0, h = 0;            ///< Extent of `data`
	uint32_t xnum = 1, xden = 1;      ///< Output x → component x
	uint32_t ynum = 1, yden = 1;      ///< Output y → component y
	int32_t offset = 0;               ///< Recentres signed samples
	uint32_t max = 0;                 ///< Largest representable sample
	int shift = 0;                    ///< Drops precision above 16 bits
	int bits = 8;                     ///< Precision after `shift`
};

Sampler
make_sampler(const opj_image_comp_t &c, uint32_t dx0, uint32_t dy0)
{
	Sampler s;
	s.data = c.data;
	s.w = c.w;
	s.h = c.h;
	s.xnum = dx0;
	s.xden = c.dx ? c.dx : 1;
	s.ynum = dy0;
	s.yden = c.dy ? c.dy : 1;
	s.offset = c.sgnd ? 1 << (c.prec - 1) : 0;
	s.max = c.prec >= 32 ? 0xFFFFFFFFu : (1u << c.prec) - 1;

	// scale_nbit_to_u16() saturates rather than scales past 16 bits.
	s.shift = c.prec > 16 ? int(c.prec) - 16 : 0;
	s.bits = int(c.prec) - s.shift;
	return s;
}

uint16_t
sample(const Sampler &s, uint32_t x, uint32_t y)
{
	uint32_t sx = x * s.xnum / s.xden, sy = y * s.ynum / s.yden;
	if (sx >= s.w)
		sx = s.w - 1;
	if (sy >= s.h)
		sy = s.h - 1;

	int64_t v = int64_t(s.data[size_t(sy) * s.w + sx]) + s.offset;
	if (v < 0)
		v = 0;
	if ((uint64_t) v > s.max)
		v = s.max;
	return scale_nbit_to_u16(uint32_t(v) >> s.shift, s.bits);
}

uint16_t
clamp_u16(double v)
{
	if (v <= 0)
		return 0;
	if (v >= 65535)
		return 65535;
	return uint16_t(v + 0.5);
}

// sYCC is full-range BT.601, its chroma planes centred on half scale. The
// library leaves this conversion to its callers.
void
ycc_to_rgb(uint16_t y, uint16_t cb, uint16_t cr, uint16_t *rgb)
{
	double b = double(cb) - 32768, r = double(cr) - 32768;
	rgb[0] = clamp_u16(double(y) + 1.402 * r);
	rgb[1] = clamp_u16(double(y) - 0.344136 * b - 0.714136 * r);
	rgb[2] = clamp_u16(double(y) + 1.772 * b);
}

enum class Colour { Grey, Rgb, Ycc };

struct Layout {
	Colour colour = Colour::Grey;  ///< How to read the colour components
	int comp[3] = {0, 1, 2};       ///< Their indices
	int alpha = -1;                ///< Index of the alpha component, if any
	bool premultiplied = false;    ///< Whether that alpha is associated
};

bool
plan_layout(const opj_image_t &image, Layout *out, Error *error)
{
	OPJ_COLOR_SPACE space = image.color_space;
	uint32_t n = image.numcomps;
	if (!n) {
		set_error(error, "no image components");
		return false;
	}

	// A bare codestream usually leaves the colour space unstated.
	if (space == OPJ_CLRSPC_UNSPECIFIED || space == OPJ_CLRSPC_UNKNOWN)
		space = n >= 3 ? OPJ_CLRSPC_SRGB : OPJ_CLRSPC_GRAY;

	uint32_t colours = 1;
	switch (space) {
	case OPJ_CLRSPC_GRAY:
		out->colour = Colour::Grey;
		break;
	case OPJ_CLRSPC_SRGB:
		out->colour = Colour::Rgb;
		colours = 3;
		break;
	case OPJ_CLRSPC_SYCC:
	case OPJ_CLRSPC_EYCC:
		out->colour = Colour::Ycc;
		colours = 3;
		break;
	default:
		// CMYK would need an ink profile we have no way to guess at.
		set_error(error, "unsupported JPEG 2000 colour space");
		return false;
	}
	if (n < colours) {
		set_error(error, "too few image components");
		return false;
	}

	// The channel definition box may nominate any component as opacity;
	// failing that, one extra trailing component is alpha by convention.
	for (uint32_t i = 0; i < n; i++) {
		if (image.comps[i].alpha && i >= colours) {
			out->alpha = int(i);
			break;
		}
	}
	if (out->alpha < 0 && n > colours)
		out->alpha = int(colours);
	if (out->alpha >= 0)
		out->premultiplied = image.comps[out->alpha].alpha >= 2;
	return true;
}

void
write_pixels(const Layout &layout, const Sampler *samplers, Image &out)
{
	for (uint32_t y = 0; y < out.height; y++) {
		uint16_t *d = row_u16(out, y);
		for (uint32_t x = 0; x < out.width; x++, d += 4) {
			uint16_t rgb[3] = {};
			if (layout.colour == Colour::Grey) {
				rgb[0] = rgb[1] = rgb[2] = sample(samplers[0], x, y);
			} else if (layout.colour == Colour::Ycc) {
				ycc_to_rgb(sample(samplers[0], x, y), sample(samplers[1], x, y),
					sample(samplers[2], x, y), rgb);
			} else {
				rgb[0] = sample(samplers[0], x, y);
				rgb[1] = sample(samplers[1], x, y);
				rgb[2] = sample(samplers[2], x, y);
			}

			d[0] = rgb[2];
			d[1] = rgb[1];
			d[2] = rgb[0];
			d[3] = layout.alpha < 0 ? 65535 : sample(samplers[3], x, y);
		}
	}
}

ImagePtr
build_image(OpenJpegLoadContext &ctx, Error *error)
{
	const opj_image_t &image = *ctx.image;
	Layout layout;
	if (!plan_layout(image, &layout, error))
		return nullptr;

	// Everything else is resampled onto the first colour component's grid.
	const opj_image_comp_t &first = image.comps[layout.comp[0]];
	int wanted[4] = {layout.comp[0], -1, -1, layout.alpha};
	if (layout.colour != Colour::Grey) {
		wanted[1] = layout.comp[1];
		wanted[2] = layout.comp[2];
	}

	Sampler samplers[4];
	for (int i = 0; i < 4; i++) {
		if (wanted[i] < 0)
			continue;

		const opj_image_comp_t &c = image.comps[wanted[i]];
		if (!c.data || !c.w || !c.h || !c.prec || c.prec > 32) {
			set_error(error, "unsupported image component");
			return nullptr;
		}
		samplers[i] =
			make_sampler(c, first.dx ? first.dx : 1, first.dy ? first.dy : 1);
	}

	ImagePtr out = image_new(first.w, first.h);
	if (!out) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	write_pixels(layout, samplers, *out);
	if (image.icc_profile_buf && image.icc_profile_len) {
		out->icc.assign(image.icc_profile_buf,
			image.icc_profile_buf + image.icc_profile_len);
	}

	ensure_working_premul_pages(*out, *ctx.octx, nullptr, layout.premultiplied);
	return out;
}

}  // namespace

// --- Public entry point ------------------------------------------------------

ImagePtr
detail::load_openjpeg(
	span<const uint8_t> data, const OpenContext &octx, Error *error)
{
	OPJ_CODEC_FORMAT format = detect_codec(data);
	if (format == OPJ_CODEC_UNKNOWN) {
		set_error(error, "not a JPEG 2000 image");
		return nullptr;
	}

	OpenJpegLoadContext ctx;
	ctx.octx = &octx;
	ctx.memory.data = data;
	if (!open_codec(ctx, format, error))
		return nullptr;

	if (!opj_read_header(ctx.stream, ctx.codec, &ctx.image))
		return fail(ctx, error, "failed to read the JPEG 2000 header");
	if (!opj_decode(ctx.codec, ctx.stream, ctx.image) ||
		!opj_end_decompress(ctx.codec, ctx.stream))
		return fail(ctx, error, "failed to decode the image");

	return build_image(ctx, error);
}

}  // namespace dn

#endif  // DAWN_WITH_OPENJPEG
