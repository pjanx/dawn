//
// load-jpeg.cpp: JPEG image loader (libjpeg-turbo)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "dawn-config.h"
#include "libdn.h"
#include "libdn-loaders.h"

#include <jpeglib.h>
#if DN_WITH_JPEG_QS
#include <libjpegqs.h>
#endif

#include <algorithm>
#include <bit>
#include <csetjmp>
#include <cstring>
#include <vector>
#if DN_WITH_JPEG_QS
#include <thread>
#endif

#define TIFF_TABLES_CONSTANTS_ONLY
#include "tiff-tables.h"
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "tiffer.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

using namespace std;

namespace dn
{
namespace
{

// --- Multi-Picture Format ----------------------------------------------------

uint32_t
parse_mpf_mpentry(const uint8_t *p, const tiffer *T)
{
	uint32_t attrs = T->un->u32(p);
	uint32_t offset = T->un->u32(p + 8);

	enum {
		TypeBaselineMPPrimaryImage = 0x030000,
		TypeLargeThumbnailVGA = 0x010001,
		TypeLargeThumbnailFullHD = 0x010002,
		TypeMultiFrameImagePanorama = 0x020001,
		TypeMultiFrameImageDisparity = 0x020002,
		TypeMultiFrameImageMultiAngle = 0x020003,
		TypeUndefined = 0x000000,
	};
	switch (attrs & 0xFFFFFF) {
	case TypeLargeThumbnailVGA:
	case TypeLargeThumbnailFullHD:
		// Wasted cycles.
	case TypeUndefined:
		// Apple uses this for HDR and depth maps (same and lower resolution).
		// TODO(p): It would be nice to be able to view them.
		return 0;
	}

	// Don't report non-JPEGs, even though they're unlikely.
	if (((attrs >> 24) & 0x7) != 0)
		return 0;

	return offset;
}

vector<uint32_t>
parse_mpf_index_entries(const tiffer *T, const tiffer_entry *entry)
{
	uint32_t count = entry->remaining_count / 16;
	vector<uint32_t> offsets;
	offsets.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		// 5.2.3.3.3. Individual Image Data Offset
		uint32_t offset = parse_mpf_mpentry(entry->p + i * 16, T);
		if (offset)
			offsets.push_back(offset);
	}
	return offsets;
}

vector<uint32_t>
parse_mpf_index_ifd(tiffer *T)
{
	tiffer_entry entry = {};
	while (tiffer_next_entry(T, &entry)) {
		// 5.2.3.3. MP Entry
		if (entry.tag == MPF_MPEntry && entry.type == TIFFER_UNDEFINED &&
			!(entry.remaining_count % 16)) {
			return parse_mpf_index_entries(T, &entry);
		}
	}
	return {};
}

/// Collects pointers (into `mpf`) to the individual JPEGs of an MPF.
bool
parse_mpf(vector<const uint8_t *> &individuals, const uint8_t *mpf, size_t len,
	size_t total_len)
{
	tiffer T = {};
	if (!tiffer_init(&T, mpf, len) || !tiffer_next_ifd(&T))
		return false;

	// First image: IFD0 is Index IFD, any IFD1 is Attribute IFD.
	// Other images: IFD0 is Attribute IFD, there is no Index IFD.
	for (uint32_t offset : parse_mpf_index_ifd(&T))
		if (offset && offset <= total_len)
			individuals.push_back(mpf + offset);
	return true;
}

// --- Exif-derived colour profile ---------------------------------------------

struct ExifProfileParams {
	double whitepoint[2] = {};             ///< TIFF_WhitePoint
	double primaries[6] = {};              ///< TIFF_PrimaryChromaticities
	enum Exif_ColorSpace colorspace = {};  ///< Exif_ColorSpace
	double gamma = 0;                      ///< Exif_Gamma

	bool have_whitepoint = false;
	bool have_primaries = false;
	bool have_colorspace = false;
	bool have_gamma = false;
};

bool
parse_exif_profile_reals(const tiffer *T, tiffer_entry *entry, double *out)
{
	while (tiffer_real(T, entry, out++))
		if (!tiffer_next_value(entry))
			return false;
	return true;
}

void
parse_exif_profile_subifd(
	ExifProfileParams *params, const tiffer *T, uint32_t offset)
{
	tiffer subT = {};
	if (!tiffer_subifd(T, offset, &subT))
		return;

	tiffer_entry entry = {};
	while (tiffer_next_entry(&subT, &entry)) {
		int64_t value = 0;
		if (entry.tag == Exif_ColorSpace && entry.type == TIFFER_SHORT &&
			entry.remaining_count == 1 &&
			tiffer_integer(&subT, &entry, &value)) {
			params->have_colorspace = true;
			params->colorspace = (enum Exif_ColorSpace) value;
		} else if (entry.tag == Exif_Gamma && entry.type == TIFFER_RATIONAL &&
			entry.remaining_count == 1 &&
			tiffer_real(&subT, &entry, &params->gamma)) {
			params->have_gamma = true;
		}
	}
}

/// Derives an ICC-like colour profile from Exif tags, mirroring fiv's
/// handling of sRGB/AdobeRGB Nikon JPEGs that carry no embedded ICC profile.
shared_ptr<Profile>
parse_exif_profile(Cmm &cmm, span<const uint8_t> exif)
{
	tiffer T = {};
	if (!tiffer_init(&T, exif.data(), exif.size()) || !tiffer_next_ifd(&T))
		return nullptr;

	ExifProfileParams params;
	tiffer_entry entry = {};
	while (tiffer_next_entry(&T, &entry)) {
		int64_t offset = 0;
		if (entry.tag == TIFF_ExifIFDPointer && entry.type == TIFFER_LONG &&
			entry.remaining_count == 1 && tiffer_integer(&T, &entry, &offset) &&
			offset >= 0 && offset <= UINT32_MAX) {
			parse_exif_profile_subifd(&params, &T, uint32_t(offset));
		} else if (entry.tag == TIFF_WhitePoint &&
			entry.type == TIFFER_RATIONAL && entry.remaining_count == 2) {
			params.have_whitepoint =
				parse_exif_profile_reals(&T, &entry, params.whitepoint);
		} else if (entry.tag == TIFF_PrimaryChromaticities &&
			entry.type == TIFFER_RATIONAL && entry.remaining_count == 6) {
			params.have_primaries =
				parse_exif_profile_reals(&T, &entry, params.primaries);
		}
	}
	if (!params.have_colorspace)
		return nullptr;

	// If sRGB is claimed, assume all parameters are standard.
	if (params.colorspace == Exif_ColorSpace_sRGB)
		return cmm.get_profile_sRGB();

	// AdobeRGB Nikon JPEGs provide all of these.
	if (params.colorspace != Exif_ColorSpace_Uncalibrated ||
		!params.have_gamma || !params.have_whitepoint || !params.have_primaries)
		return nullptr;

	return cmm.get_profile_parametric(
		params.gamma, params.whitepoint, params.primaries);
}

// --- JPEG segment scanning ---------------------------------------------------

struct JpegMetadata {
	vector<uint8_t> exif;         ///< Exif buffer, may be empty
	vector<uint8_t> icc;          ///< ICC profile buffer, may be empty
	vector<const uint8_t *> mpf;  ///< Multi-Picture Format entries
};

void
parse_jpeg_metadata(span<const uint8_t> data, JpegMetadata *meta)
{
	// Because the JPEG file format is simple, just do it manually.
	// See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
	enum {
		TEM = 0x01,
		SOF0 = 0xC0,
		SOF1,
		SOF2,
		SOF3,
		DHT,
		SOF5,
		SOF6,
		SOF7,
		JPG,
		SOF9,
		SOF10,
		SOF11,
		DAC,
		SOF13,
		SOF14,
		SOF15,
		RST0,
		RST1,
		RST2,
		RST3,
		RST4,
		RST5,
		RST6,
		RST7,
		SOI,
		EOI,
		SOS,
		DQT,
		DNL,
		DRI,
		DHP,
		EXP,
		APP0,
		APP1,
		APP2,
		APP3,
		APP4,
		APP5,
		APP6,
		APP7,
	};

	int icc_sequence = 0;
	bool icc_done = false;
	const uint8_t *p = data.data(), *end = p + data.size();
	while (p + 3 < end && *p++ == 0xFF && *p != SOS && *p != EOI) {
		// The previous byte is a fill byte, restart.
		if (*p == 0xFF)
			continue;

		// These markers stand alone, not starting a marker segment.
		uint8_t marker = *p++;
		switch (marker) {
		case RST0:
		case RST1:
		case RST2:
		case RST3:
		case RST4:
		case RST5:
		case RST6:
		case RST7:
		case SOI:
		case TEM:
			continue;
		}

		// Do not bother validating the structure.
		uint16_t length = uint16_t(p[0] << 8 | p[1]);
		const uint8_t *payload = p + 2;
		if ((p += length) > end)
			break;

		// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf 4.7.2
		// Adobe XMP Specification Part 3: Storage in Files, 2020/1, 1.1.3
		// Not checking the padding byte is intentional.
		// XXX: Thumbnails may in practice overflow into follow-up segments.
		if (marker == APP1 && p - payload >= 6 &&
			!memcmp(payload, "Exif\0", 5) && meta->exif.empty()) {
			payload += 6;
			meta->exif.assign(payload, p);
		}

		// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
		if (marker == APP2 && p - payload >= 14 &&
			!memcmp(payload, "ICC_PROFILE\0", 12) && !icc_done &&
			payload[12] == ++icc_sequence && payload[13] >= payload[12]) {
			payload += 14;
			meta->icc.insert(meta->icc.end(), payload, p);
			icc_done = payload[-1] == icc_sequence;
		}

		// CIPA DC-007-2021 (Multi-Picture Format) 5.2
		// https://www.cipa.jp/e/std/std-sec.html
		if (marker == APP2 && p - payload >= 8 &&
			!memcmp(payload, "MPF\0", 4) && meta->mpf.empty()) {
			payload += 4;
			parse_mpf(
				meta->mpf, payload, size_t(p - payload), size_t(end - payload));
		}

		// TODO(p): Extract the main XMP segment.
	}

	if (!icc_done)
		meta->icc.clear();
}

// --- libjpeg error handling --------------------------------------------------

struct LibjpegErrorMgr {
	jpeg_error_mgr pub;
	jmp_buf buf;
	Error *error = nullptr;
	const OpenContext *ctx = nullptr;
};

void
libjpeg_error_exit(j_common_ptr cinfo)
{
	auto *err = (LibjpegErrorMgr *) cinfo->err;
	char buf[JMSG_LENGTH_MAX] = "";
	(*cinfo->err->format_message)(cinfo, buf);
	set_error(err->error, buf);
	longjmp(err->buf, 1);
}

void
libjpeg_output_message(j_common_ptr cinfo)
{
	auto *err = (LibjpegErrorMgr *) cinfo->err;
	char buf[JMSG_LENGTH_MAX] = "";
	(*cinfo->err->format_message)(cinfo, buf);
	add_warning(*err->ctx, buf);
}

// --- Decoding loops ----------------------------------------------------------

void
load_libjpeg_simple(jpeg_decompress_struct *cinfo, JSAMPARRAY lines)
{
	(void) jpeg_start_decompress(cinfo);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpeg_finish_decompress(cinfo);
}

#if DN_WITH_JPEG_QS

void
load_libjpeg_enhanced(jpeg_decompress_struct *cinfo, JSAMPARRAY lines)
{
	// Go for the maximum quality setting.
	jpegqs_control_t opts = {};
	opts.flags = JPEGQS_DIAGONALS | JPEGQS_JOINT_YUV;
	opts.threads = int(thread::hardware_concurrency());
	opts.niter = 3;

	// Waiting for https://github.com/ilyakurdyukov/jpeg-quantsmooth/issues/28
#if LIBJPEG_TURBO_VERSION_NUMBER < 2001090
	opts.flags |= JPEGQS_UPSAMPLE_UV;
#endif

	(void) jpegqs_start_decompress(cinfo, &opts);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpegqs_finish_decompress(cinfo);
}

#else

inline void
load_libjpeg_enhanced(jpeg_decompress_struct *cinfo, JSAMPARRAY lines)
{
	load_libjpeg_simple(cinfo, lines);
}

#endif

void
load_libjpeg12_simple(jpeg_decompress_struct *cinfo, J12SAMPARRAY lines)
{
	(void) jpeg_start_decompress(cinfo);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg12_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpeg_finish_decompress(cinfo);
}

void
load_libjpeg16_simple(jpeg_decompress_struct *cinfo, J16SAMPARRAY lines)
{
	(void) jpeg_start_decompress(cinfo);
	while (cinfo->output_scanline < cinfo->output_height)
		(void) jpeg16_read_scanlines(cinfo, lines + cinfo->output_scanline,
			cinfo->output_height - cinfo->output_scanline);
	(void) jpeg_finish_decompress(cinfo);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

ImagePtr open_libjpeg_turbo(
	span<const uint8_t> data, const OpenContext &ctx, Error *error);

/// Packs opaque interleaved EXT_BGRA / EXT_ARGB uint16 samples into working
/// BGRA16, scaling from `bits` (12 or 16) to the full uint16 range.
void
pack_jpeg_ext_to_bgra16(
	Image &dst, const uint16_t *src, size_t src_stride, int bits, bool argb)
{
	detail::StageClock clk(&OpenTiming::widen_ms);
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint16_t *s =
			(const uint16_t *) ((const uint8_t *) src + y * src_stride);
		for (uint32_t x = 0; x < dst.width; x++) {
			if (argb) {
				d[0] = scale_nbit_to_u16(s[3], bits);
				d[1] = scale_nbit_to_u16(s[2], bits);
				d[2] = scale_nbit_to_u16(s[1], bits);
				d[3] = 65535;
			} else {
				d[0] = scale_nbit_to_u16(s[0], bits);
				d[1] = scale_nbit_to_u16(s[1], bits);
				d[2] = scale_nbit_to_u16(s[2], bits);
				d[3] = 65535;
			}
			d += 4;
			s += 4;
		}
	}
}

/// Finishes a decoded JPEG page: metadata, optional MPF follow-ups, then
/// colour-manage. `bits` is 8 for JSAMPLE output, or 12/16 for high precision.
/// When bits==8, `pixels8` is BGRA/ARGB/CMYK8; otherwise `pixels16` is used.
void
load_jpeg_finalize(ImagePtr &image, bool cmyk, bool argb, int bits,
	const OpenContext &ctx, span<const uint8_t> data, const uint8_t *pixels8,
	const uint16_t *pixels16)
{
	JpegMetadata meta;
	parse_jpeg_metadata(data, &meta);

	if (!ctx.first_frame_only) {
		// XXX: This is ugly, as it relies on just the first individual image
		// having any follow-up entries (as it should be).
		ImagePtr tail = image;
		for (size_t i = 0; i < meta.mpf.size(); i++) {
			const uint8_t *jpeg = meta.mpf[i];
			size_t sub_len = size_t((data.data() + data.size()) - jpeg);

			Error suberror;
			ImagePtr sub = open_libjpeg_turbo(
				span<const uint8_t>(jpeg, sub_len), ctx, &suberror);
			if (sub)
				append_page(image, tail, std::move(sub));
			else
				add_warning(ctx,
					"MPF image " + to_string(i + 2) + ": " + suberror.message);
		}
	}

	if (!meta.exif.empty())
		image->exif = std::move(meta.exif);
	if (!meta.icc.empty())
		image->icc = std::move(meta.icc);

	auto cmm = cmm_or_default(ctx);
	shared_ptr<Profile> source;
	if (!image->icc.empty())
		source = cmm->get_profile(image->icc);
	else if (!image->exif.empty())
		source = parse_exif_profile(*cmm, image->exif);
	if (source)
		image->effective_profile = source;
	else if (!cmyk) {
		image->effective_profile = cmm->get_profile_sRGB();
		image->profile_assumed = true;
	}

	if (cmyk) {
		// convert_cmyk8() already colour-manages to working premul; do not
		// call ensure_working_premul afterwards.
		const uint8_t *cmyk8 = pixels8;
		vector<uint8_t> quantized;
		if (bits > 8) {
			quantized.resize(size_t(image->width) * 4 * image->height);
			for (uint32_t y = 0; y < image->height; y++) {
				const uint16_t *s = pixels16 + size_t(y) * image->width * 4;
				uint8_t *d = quantized.data() + size_t(y) * image->width * 4;
				for (uint32_t x = 0; x < image->width * 4; x++)
					d[x] = uint8_t(scale_nbit_to_u16(s[x], bits) >> 8);
			}
			cmyk8 = quantized.data();
		}
		cmm->convert_cmyk8(
			*image, cmyk8, source.get(), ctx.screen_profile.get());
	} else if (bits == 8) {
		Profile *target = ctx.screen_profile.get();
		bool converted = false;
		if (target) {
			const bool premul = !cmm->broken_premul();
			converted = cmm->transform_bgra8_to_bgra16(pixels8,
				image->data.data(), image->width, image->height,
				source.get(), target, premul);
			if (converted && !premul)
				premultiply_bgra16(*image);
		}
		if (!converted) {
			widen_bgra8_to_bgra16(
				*image, pixels8, size_t(image->width) * 4);
			ensure_working_premul(*image, ctx, source.get(),
				/*input_premul=*/false);
		}
	} else {
		pack_jpeg_ext_to_bgra16(*image, pixels16,
			size_t(image->width) * 4 * sizeof(uint16_t), bits, argb);
		ensure_working_premul(*image, ctx,
			image->icc.empty() ? source.get() : nullptr,
			/*input_premul=*/false);
	}
}

ImagePtr
load_libjpeg_turbo(span<const uint8_t> data, const OpenContext &ctx,
	void (*loop)(jpeg_decompress_struct *, JSAMPARRAY), Error *error)
{
	LibjpegErrorMgr jerr = {};
	jerr.error = error;
	jerr.ctx = &ctx;

	jpeg_decompress_struct cinfo = {};
	cinfo.err = jpeg_std_error(&jerr.pub);
	jerr.pub.error_exit = libjpeg_error_exit;
	jerr.pub.output_message = libjpeg_output_message;
	if (setjmp(jerr.buf)) {
		jpeg_destroy_decompress(&cinfo);
		return nullptr;
	}

	{
		detail::StageClock clk(&OpenTiming::decode_ms);
		jpeg_create_decompress(&cinfo);
		jpeg_mem_src(&cinfo, data.data(), (unsigned long) data.size());
		(void) jpeg_read_header(&cinfo, TRUE);
	}

	int precision = cinfo.data_precision;
	bool high = precision == 12 || precision == 16;

	bool use_cmyk = cinfo.jpeg_color_space == JCS_CMYK ||
		cinfo.jpeg_color_space == JCS_YCCK;
	bool use_argb = false;
	if (use_cmyk) {
		cinfo.out_color_space = JCS_CMYK;
	} else if constexpr (endian::native == endian::big) {
		// Unlike JCS_EXT_XRGB/JCS_EXT_BGRX (as used by fiv, which hands the
		// result to Cairo and does not care about the 4th byte), the "A"
		// variants guarantee an opaque 0xFF alpha byte, which
		// widen_bgra8_to_bgra16() below trusts.
		cinfo.out_color_space = JCS_EXT_ARGB;
		use_argb = true;
	} else {
		cinfo.out_color_space = JCS_EXT_BGRA;
	}

	{
		detail::StageClock clk(&OpenTiming::decode_ms);
		jpeg_calc_output_dimensions(&cinfo);
	}
	int width = int(cinfo.output_width);
	int height = int(cinfo.output_height);

	ImagePtr image = image_new(uint32_t(width), uint32_t(height));
	if (!image) {
		set_error(error, "image allocation failure");
		longjmp(jerr.buf, 1);
	}

	if (high) {
		// jpegqs / enhance only applies to the 8-bit path.
		vector<uint16_t> samples;
		{
			detail::StageClock clk(&OpenTiming::alloc_ms);
			samples.resize(size_t(width) * 4 * size_t(height));
		}
		if (precision == 12) {
			auto lines =
				J12SAMPARRAY((*cinfo.mem->alloc_small)((j_common_ptr) &cinfo,
					JPOOL_IMAGE, sizeof(J12SAMPROW) * height));
			for (int i = 0; i < height; i++)
				lines[i] =
					(J12SAMPROW) (samples.data() + size_t(i) * width * 4);
			detail::StageClock clk(&OpenTiming::decode_ms);
			load_libjpeg12_simple(&cinfo, lines);
		} else {
			auto lines =
				J16SAMPARRAY((*cinfo.mem->alloc_small)((j_common_ptr) &cinfo,
					JPOOL_IMAGE, sizeof(J16SAMPROW) * height));
			for (int i = 0; i < height; i++)
				lines[i] =
					(J16SAMPROW) (samples.data() + size_t(i) * width * 4);
			detail::StageClock clk(&OpenTiming::decode_ms);
			load_libjpeg16_simple(&cinfo, lines);
		}
		load_jpeg_finalize(image, use_cmyk, use_argb, precision, ctx, data,
			nullptr, samples.data());
	} else {
		vector<uint8_t> pixels;
		{
			detail::StageClock clk(&OpenTiming::alloc_ms);
			pixels.resize(size_t(width) * 4 * size_t(height));
		}
		auto lines = JSAMPARRAY((*cinfo.mem->alloc_small)(
			(j_common_ptr) &cinfo, JPOOL_IMAGE, sizeof(JSAMPROW) * height));
		for (int i = 0; i < height; i++)
			lines[i] = pixels.data() + size_t(i) * width * 4;

		{
			detail::StageClock clk(&OpenTiming::decode_ms);
			loop(&cinfo, lines);
		}
		load_jpeg_finalize(
			image, use_cmyk, use_argb, 8, ctx, data, pixels.data(), nullptr);
	}

	jpeg_destroy_decompress(&cinfo);
	return image;
}

ImagePtr
open_libjpeg_turbo(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	return load_libjpeg_turbo(data, ctx,
		ctx.enhance ? load_libjpeg_enhanced : load_libjpeg_simple, error);
}

}  // namespace

ImagePtr
detail::load_jpeg(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	return open_libjpeg_turbo(data, ctx, error);
}

int64_t
detail::jpeg_sof_pixel_count(span<const uint8_t> data)
{
	// See: https://www.w3.org/Graphics/JPEG/itu-t81.pdf
	enum {
		TEM = 0x01,
		SOF0 = 0xC0,
		SOF1,
		SOF2,
		SOF3,
		DHT,
		SOF5,
		SOF6,
		SOF7,
		JPG,
		SOF9,
		SOF10,
		SOF11,
		DAC,
		SOF13,
		SOF14,
		SOF15,
		RST0,
		RST1,
		RST2,
		RST3,
		RST4,
		RST5,
		RST6,
		RST7,
		SOI,
		EOI,
		SOS,
	};

	int64_t width = 0, height = 0;
	const uint8_t *p = data.data(), *end = p + data.size();
	while (p + 3 < end && *p++ == 0xFF && *p != SOS && *p != EOI) {
		if (*p == 0xFF)
			continue;

		uint8_t marker = *p++;
		switch (marker) {
		case RST0:
		case RST1:
		case RST2:
		case RST3:
		case RST4:
		case RST5:
		case RST6:
		case RST7:
		case SOI:
		case TEM:
			continue;
		}

		uint16_t length = uint16_t(p[0] << 8 | p[1]);
		const uint8_t *payload = p + 2;
		if ((p += length) > end)
			break;

		switch (marker) {
		case SOF0:
		case SOF1:
		case SOF2:
		case SOF3:
		case SOF5:
		case SOF6:
		case SOF7:
		case SOF9:
		case SOF10:
		case SOF11:
		case SOF13:
		case SOF14:
		case SOF15:
			if (length >= 5) {
				width = (payload[3] << 8) + payload[4];
				height = (payload[1] << 8) + payload[2];
			}
		}
	}
	return width * height;
}

}  // namespace dn
