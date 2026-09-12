//
// load-psd.cpp: Adobe Photoshop image loading
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <cstdint>
#include <cstring>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dawn
{

// Photoshop keeps a flattened composite at the very end of the file, behind
// a layer section that may be most of it.  That composite is all we take:
// blending layers ourselves would be a different program.

enum {
	kColorBitmap = 0,
	kColorGrayscale = 1,
	kColorIndexed = 2,
	kColorRgb = 3,
	kColorCmyk = 4,
	kColorMultichannel = 7,
	kColorDuotone = 8,
	kColorLab = 9,
};

enum {
	kResourceIcc = 1039,
	kResourceVersionInfo = 1057,
	kResourceExif = 1058,
	kResourceXmp = 1060,
};

enum {
	kCompressionRaw = 0,
	kCompressionRle = 1,
};

// --- Reading -----------------------------------------------------------------

struct Reader {
	span<const uint8_t> data;
	size_t offset = 0;
	bool ok = true;
};

static const uint8_t *
take(Reader &r, size_t length)
{
	if (!r.ok || r.data.size() - r.offset < length) {
		r.ok = false;
		return nullptr;
	}
	const uint8_t *p = r.data.data() + r.offset;
	r.offset += length;
	return p;
}

static uint16_t
be16(const uint8_t *p)
{
	return uint16_t(uint32_t(p[0]) << 8 | p[1]);
}

static uint32_t
be32(const uint8_t *p)
{
	return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 |
		uint32_t(p[3]);
}

static uint16_t
read16(Reader &r)
{
	const uint8_t *p = take(r, 2);
	return p ? be16(p) : 0;
}

static uint32_t
read32(Reader &r)
{
	const uint8_t *p = take(r, 4);
	return p ? be32(p) : 0;
}

static uint64_t
read64(Reader &r)
{
	const uint8_t *p = take(r, 8);
	return p ? uint64_t(be32(p)) << 32 | be32(p + 4) : 0;
}

// --- Header ------------------------------------------------------------------

struct Header {
	bool psb = false;
	uint16_t file_channels = 0;  ///< As stored, spot channels included.
	uint32_t width = 0, height = 0;
	uint16_t depth = 0;
	uint16_t color_mode = 0;
	uint32_t color_channels = 0;  ///< Implied by `color_mode`.
	uint32_t channels = 0;        ///< What we actually decode.
};

static const char *
color_mode_name(uint16_t mode)
{
	switch (mode) {
	case kColorBitmap:
		return "Bitmap";
	case kColorGrayscale:
		return "Grayscale";
	case kColorIndexed:
		return "Indexed";
	case kColorRgb:
		return "RGB";
	case kColorCmyk:
		return "CMYK";
	case kColorMultichannel:
		return "Multichannel";
	case kColorDuotone:
		return "Duotone";
	case kColorLab:
		return "Lab";
	default:
		return "unknown";
	}
}

// --- Image resources ---------------------------------------------------------

struct Resources {
	vector<uint8_t> icc;
	vector<uint8_t> exif;
	vector<uint8_t> xmp;
	/// Resource 1057 was present, and said the composite is a white dummy.
	bool dummy_merged_data = false;
};

static void
parse_resources(
	span<const uint8_t> data, const OpenContext &ctx, Resources *out)
{
	Reader r{data};
	while (r.ok && r.offset < data.size()) {
		const uint8_t *signature = take(r, 4);
		uint16_t id = read16(r);

		// A Pascal string, padded so that it ends on an even offset.
		const uint8_t *name_length = take(r, 1);
		if (!name_length)
			break;
		take(r, size_t(*name_length) + (*name_length % 2 ? 0 : 1));

		uint32_t length = read32(r);
		const uint8_t *body = take(r, length);
		if (!body)
			break;
		if (length % 2)
			take(r, 1);
		if (memcmp(signature, "8BIM", 4))
			continue;

		switch (id) {
		case kResourceIcc:
			out->icc.assign(body, body + length);
			break;
		case kResourceExif:
			out->exif.assign(body, body + length);
			break;
		case kResourceXmp:
			out->xmp.assign(body, body + length);
			break;
		case kResourceVersionInfo:
			// A version word, then hasRealMergedData, then Unicode strings.
			if (length >= 5)
				out->dummy_merged_data = !body[4];
			break;
		}
	}
	if (!r.ok)
		add_warning(ctx, _("truncated PSD image resources"));
}

// --- Composite ---------------------------------------------------------------

/// Decode one PackBits row into exactly `length` bytes.
static bool
unpack_bits(Reader &r, uint8_t *out, size_t length)
{
	size_t done = 0;
	while (done < length) {
		const uint8_t *control = take(r, 1);
		if (!control)
			return false;

		int n = int8_t(*control);
		if (n == -128)
			continue;

		size_t count = n >= 0 ? size_t(n) + 1 : size_t(1 - n);
		if (count > length - done)
			return false;
		if (n < 0) {
			const uint8_t *value = take(r, 1);
			if (!value)
				return false;
			memset(out + done, *value, count);
		} else {
			const uint8_t *literal = take(r, count);
			if (!literal)
				return false;
			memcpy(out + done, literal, count);
		}
		done += count;
	}
	return true;
}

/// Read the leading `h.channels` planes of the composite, discarding any
/// spot channels that follow them.
static bool
read_composite(
	Reader &r, const Header &h, vector<uint8_t> *planes, Error *error)
{
	uint16_t compression = read16(r);
	if (!r.ok) {
		set_error(error, _("truncated PSD image data section"));
		return false;
	}

	const size_t sample = h.depth / 8;
	// image_new() has already bounded the pixel count; this guards the
	// remaining multiplications on 32-bit size_t.
	if (uint64_t(h.width) * h.height * sample * h.channels > SIZE_MAX) {
		set_error(error, _("PSD composite too large"));
		return false;
	}

	const size_t row = size_t(h.width) * sample;
	const size_t rows = size_t(h.channels) * h.height;
	try {
		planes->resize(row * rows);
	} catch (const bad_alloc &) {
		set_error(error, _("PSD composite too large"));
		return false;
	}

	switch (compression) {
	case kCompressionRaw:
		for (size_t i = 0; i < rows; i++) {
			const uint8_t *p = take(r, row);
			if (!p) {
				set_error(error, _("truncated PSD composite"));
				return false;
			}
			memcpy(planes->data() + row * i, p, row);
		}
		return true;
	case kCompressionRle:
		// Per-row byte counts for every stored channel precede the data.
		// PackBits rows are self-delimiting, so we only step over the table.
		take(r, size_t(h.file_channels) * h.height * (h.psb ? 4 : 2));
		for (size_t i = 0; i < rows; i++) {
			if (unpack_bits(r, planes->data() + row * i, row))
				continue;
			set_error(error, _("truncated PSD composite"));
			return false;
		}
		return true;
	default:
		// Both ZIP variants only ever show up on layers in practice.
		set_error(error, _("unsupported PSD composite compression"));
		return false;
	}
}

// Photoshop works in 0..32768 internally, but writes out the full range.
static uint16_t
sample_to_u16(const uint8_t *p, uint16_t depth)
{
	return depth == 8 ? uint16_t(*p * 257u) : be16(p);
}

static void
compose_rgb(Image &image, const vector<uint8_t> &planes, const Header &h)
{
	const size_t sample = h.depth / 8;
	const size_t plane = size_t(h.width) * h.height * sample;
	const bool grey = h.color_mode == kColorGrayscale;

	for (uint32_t y = 0; y < image.height; y++) {
		uint16_t *d = row_u16(image, y);
		const uint8_t *s = planes.data() + size_t(y) * h.width * sample;
		for (uint32_t x = 0; x < image.width; x++, s += sample, d += 4) {
			d[2] = sample_to_u16(s, h.depth);
			d[1] = grey ? d[2] : sample_to_u16(s + plane, h.depth);
			d[0] = grey ? d[2] : sample_to_u16(s + plane * 2, h.depth);
			d[3] = h.channels > h.color_channels
				? sample_to_u16(s + plane * h.color_channels, h.depth)
				: 65535;
		}
	}
}

static vector<uint8_t>
compose_cmyk8(const vector<uint8_t> &planes, const Header &h)
{
	const size_t sample = h.depth / 8;
	const size_t pixels = size_t(h.width) * h.height;
	const size_t plane = pixels * sample;

	// convert_cmyk8() wants inverted CMYK, which is how PSD stores it.
	// Any alpha channel goes: that conversion forces opaque output.
	vector<uint8_t> cmyk(pixels * 4);
	for (size_t i = 0; i < pixels; i++)
		for (size_t c = 0; c < 4; c++)
			cmyk[i * 4 + c] =
				uint8_t(sample_to_u16(
							planes.data() + plane * c + i * sample, h.depth) >>
					8);
	return cmyk;
}

/// Adobe: "Layer count.  If it is a negative number, its absolute value is
/// the number of layers and the first alpha channel contains the transparency
/// data for the merged result."  Takes its own cursor over the section.
static bool
merged_transparency(Reader r, const Header &h)
{
	uint64_t length = h.psb ? read64(r) : read32(r);
	if (!r.ok || length < 2)
		return false;

	return int16_t(read16(r)) < 0 && r.ok;
}

// --- Entry point -------------------------------------------------------------

ImagePtr
detail::load_psd(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	Reader r{data};
	const uint8_t *signature = take(r, 4);
	if (!signature || memcmp(signature, "8BPS", 4)) {
		set_error(error, _("not a PSD image"));
		return nullptr;
	}

	Header h;
	uint16_t version = read16(r);
	take(r, 6);
	h.file_channels = read16(r);
	h.height = read32(r);
	h.width = read32(r);
	h.depth = read16(r);
	h.color_mode = read16(r);
	if (!r.ok) {
		set_error(error, _("truncated PSD header"));
		return nullptr;
	}

	// Version 2 is PSB, which only widens two of the section lengths.
	if (version != 1 && version != 2) {
		set_error(
			error, format_message(_("unsupported PSD version %u"), version));
		return nullptr;
	}
	h.psb = version == 2;

	switch (h.color_mode) {
	case kColorGrayscale:
		h.color_channels = 1;
		break;
	case kColorRgb:
		h.color_channels = 3;
		break;
	case kColorCmyk:
		h.color_channels = 4;
		break;
	default:
		set_error(error,
			format_message(_("unsupported PSD colour mode: %s"),
				color_mode_name(h.color_mode)));
		return nullptr;
	}
	if (h.depth != 8 && h.depth != 16) {
		set_error(
			error, format_message(_("unsupported PSD bit depth %u"), h.depth));
		return nullptr;
	}
	if (h.file_channels < h.color_channels) {
		set_error(error, _("PSD channel count does not match its colour mode"));
		return nullptr;
	}

	// Colour mode data only says anything for the modes we have rejected.
	take(r, read32(r));

	uint32_t resources_length = read32(r);
	const uint8_t *resources = take(r, resources_length);
	if (!resources) {
		set_error(error, _("truncated PSD image resources"));
		return nullptr;
	}

	Resources res;
	parse_resources({resources, resources_length}, ctx, &res);

	// The layer and mask section is stepped over whole, save for the layer
	// count, which is the only place the composite's alpha is announced.
	uint64_t layers = h.psb ? read64(r) : read32(r);
	if (!r.ok || layers > data.size() - r.offset) {
		set_error(error, _("truncated PSD layer and mask section"));
		return nullptr;
	}

	// Anything past the colour channels is an alpha or a spot channel,
	// and only merged transparency belongs in the composite.
	h.channels = h.color_channels;
	if (h.file_channels > h.color_channels &&
		merged_transparency({data.subspan(r.offset, size_t(layers))}, h))
		h.channels++;
	r.offset += size_t(layers);

	if (res.dummy_merged_data || data.size() - r.offset < 2) {
		set_error(error,
			_("PSD carries no composite image, "
			  "it needs to be saved with Maximize Compatibility"));
		return nullptr;
	}

	ImagePtr image = image_new(h.width, h.height);
	if (!image) {
		set_error(error, _("unsupported PSD image dimensions"));
		return nullptr;
	}

	vector<uint8_t> planes;
	if (!read_composite(r, h, &planes, error))
		return nullptr;

	image->icc = std::move(res.icc);
	image->exif = std::move(res.exif);
	image->xmp = std::move(res.xmp);

	auto cmm = cmm_or_default(ctx);
	shared_ptr<Profile> source;
	if (!image->icc.empty())
		source = cmm->get_profile(image->icc);
	if (source)
		image->effective_profile = source;
	else if (h.color_mode != kColorCmyk) {
		image->effective_profile = cmm->get_profile_sRGB();
		image->profile_assumed = true;
	}

	if (h.color_mode == kColorCmyk) {
		// convert_cmyk8() colour-manages all the way to working premul.
		vector<uint8_t> cmyk = compose_cmyk8(planes, h);
		cmm->convert_cmyk8(
			*image, cmyk.data(), source.get(), ctx.screen_profile.get());
	} else {
		compose_rgb(*image, planes, h);
		ensure_working_premul(
			*image, ctx, source.get(), /*input_premul=*/false);
	}
	return image;
}

}  // namespace dawn
