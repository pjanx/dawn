//
// load-tiff-ep.cpp: TIFF/EP + DNG embedded JPEG preview loader
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn-loaders.h"
#include "libdn.h"

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

namespace dawn
{
namespace
{

// --- Minimal JPEG dimension sniffing -----------------------------------------
// We only need pixel counts to pick the largest preview among candidates--
// actual decoding, along with Exif/ICC extraction, is left to load_jpeg().

int64_t
jpeg_pixel_count(const uint8_t *data, size_t len)
{
	return detail::jpeg_sof_pixel_count(span<const uint8_t>(data, len));
}

// --- TIFF/EP + DNG -----------------------------------------------------------
// In Nikon NEF files, which claim to be TIFF/EP-compatible, IFD0 is a tiny
// uncompressed thumbnail with SubIFDs that, aside from raw sensor data,
// typically contain a nearly full-size JPEG preview.
//
// LibRaw takes too long a time to render something that will never be as
// good as that large preview--e.g., due to exposure correction or denoising.
// A little bit of custom processing to extract the JPEG directly won't hurt.
//
// Note that libtiff can only read the horrible IFD0 thumbnail.
// (TIFFSetSubDirectory() requires an ImageLength tag that's missing from
// JPEG SubIFDs, and TIFFReadCustomDirectory() takes a privately defined
// struct that may not be omitted.)

bool
tiffer_find(const tiffer *self, uint16_t tag, tiffer_entry *entry)
{
	// Note that we could employ binary search, because tags must be ordered:
	//  - TIFF 6.0: Sort Order
	//  - ISO/DIS 12234-2: 4.1.2, 5.1
	//  - CIPA DC-007-2009 (Multi-Picture Format): 5.2.3., 5.2.4.
	//  - CIPA DC-008-2019 (Exif 2.32): 4.6.2.
	// However, it doesn't seem to warrant the ugly code.
	tiffer T = *self;
	while (tiffer_next_entry(&T, entry)) {
		if (entry->tag == tag)
			return true;
	}
	*entry = {};
	return false;
}

bool
tiffer_find_integer(const tiffer *self, uint16_t tag, int64_t *i)
{
	tiffer_entry entry = {};
	return tiffer_find(self, tag, &entry) && tiffer_integer(self, &entry, i);
}

// In case of failure, an entry with a zero "remaining_count" is returned.
tiffer_entry
tiff_ep_subifds_init(const tiffer *T)
{
	tiffer_entry entry = {};
	(void) tiffer_find(T, TIFF_SubIFDs, &entry);
	return entry;
}

bool
tiff_ep_subifds_next(const tiffer *T, tiffer_entry *subifds, tiffer *subT)
{
	// XXX: Except for a zero "remaining_count", all conditions are errors,
	// and should perhaps be reported.
	int64_t offset = 0;
	if (!tiffer_integer(T, subifds, &offset) || offset < 0 ||
		offset > UINT32_MAX || !tiffer_subifd(T, uint32_t(offset), subT))
		return false;

	(void) tiffer_next_value(subifds);
	return true;
}

bool
tiff_ep_find_main(const tiffer *T, tiffer *outputT)
{
	// This is a mandatory field.
	int64_t type = 0;
	if (!tiffer_find_integer(T, TIFF_NewSubfileType, &type))
		return false;

	// This is the main image.
	// (See DNG rather than ISO/DIS 12234-2 for values.)
	if (type == 0) {
		*outputT = *T;
		return true;
	}

	tiffer_entry subifds = tiff_ep_subifds_init(T);
	tiffer subT = {};
	while (tiff_ep_subifds_next(T, &subifds, &subT))
		if (tiff_ep_find_main(&subT, outputT))
			return true;
	return false;
}

struct TiffEpJpeg {
	const uint8_t *jpeg = nullptr;  ///< JPEG data stream
	size_t jpeg_length = 0;         ///< JPEG data stream length
	int64_t pixels = 0;             ///< Number of pixels in the JPEG
};

void
tiff_ep_find_jpeg_evaluate(const tiffer *T, TiffEpJpeg *out)
{
	// This is a mandatory field.
	int64_t compression = 0;
	if (!tiffer_find_integer(T, TIFF_Compression, &compression))
		return;

	uint16_t tag_pointer = 0, tag_length = 0;
	switch (compression) {
		// This is how Exif specifies it, which doesn't follow TIFF 6.0.
	case TIFF_Compression_JPEG:
		tag_pointer = TIFF_JPEGInterchangeFormat;
		tag_length = TIFF_JPEGInterchangeFormatLength;
		break;
		// Theoretically, there may be more strips, but this is not expected.
	case TIFF_Compression_JPEGDatastream:
		tag_pointer = TIFF_StripOffsets;
		tag_length = TIFF_StripByteCounts;
		break;
	default:
		return;
	}

	int64_t ipointer = 0, ilength = 0;
	if (!tiffer_find_integer(T, tag_pointer, &ipointer) || ipointer <= 0 ||
		!tiffer_find_integer(T, tag_length, &ilength) || ilength <= 0 ||
		ipointer > T->end - T->begin || T->end - T->begin - ipointer < ilength)
		return;

	// Note that to get the largest JPEG,
	// we don't need to descend into Exif thumbnails.
	// TODO(p): Consider DNG 1.2.0.0 PreviewColorSpace.
	// But first, try to find some real-world files with it.
	const uint8_t *jpeg = T->begin + ipointer;
	size_t jpeg_length = size_t(ilength);

	int64_t pixels = jpeg_pixel_count(jpeg, jpeg_length);
	if (pixels > out->pixels) {
		out->jpeg = jpeg;
		out->jpeg_length = jpeg_length;
		out->pixels = pixels;
	}
}

bool
tiff_ep_find_jpeg(const tiffer *T, TiffEpJpeg *out)
{
	// This is a mandatory field.
	int64_t type = 0;
	if (!tiffer_find_integer(T, TIFF_NewSubfileType, &type))
		return false;

	// This is a thumbnail of the main image.
	// (See DNG rather than ISO/DIS 12234-2 for values.)
	if (type == 1)
		tiff_ep_find_jpeg_evaluate(T, out);

	tiffer_entry subifds = tiff_ep_subifds_init(T);
	tiffer subT = {};
	while (tiff_ep_subifds_next(T, &subifds, &subT))
		if (!tiff_ep_find_jpeg(&subT, out))
			return false;
	return true;
}

ImagePtr
load_tiff_ep_page(const tiffer *T, const OpenContext &ctx, Error *error)
{
	// ISO/DIS 12234-2 is a fuck-up that says this should be in "IFD0",
	// but it might have intended to say "all top-level IFDs".
	// The DNG specification shares the same problem.
	//
	// In any case, chained TIFFs are relatively rare.
	tiffer_entry entry = {};
	bool is_tiffep = tiffer_find(T, TIFF_TIFF_EPStandardID, &entry) &&
		entry.type == TIFFER_BYTE && entry.remaining_count == 4 &&
		entry.p[0] == 1 && !entry.p[1] && !entry.p[2] && !entry.p[3];

	// Apple ProRAW, e.g., does not claim TIFF/EP compatibility,
	// but we should still be able to make sense of it.
	bool is_supported_dng = tiffer_find(T, TIFF_DNGBackwardVersion, &entry) &&
		entry.type == TIFFER_BYTE && entry.remaining_count == 4 &&
		entry.p[0] == 1 && entry.p[1] <= 6 && !entry.p[2] && !entry.p[3];
	if (!is_tiffep && !is_supported_dng) {
		set_error(error, "not a supported TIFF/EP or DNG image");
		return nullptr;
	}

	tiffer fullT = {};
	if (!tiff_ep_find_main(T, &fullT)) {
		set_error(error, "could not find a main image");
		return nullptr;
	}

	int64_t width = 0, height = 0;
	if (!tiffer_find_integer(&fullT, TIFF_ImageWidth, &width) ||
		!tiffer_find_integer(&fullT, TIFF_ImageLength, &height) || width <= 0 ||
		height <= 0) {
		set_error(error, "missing or invalid main image dimensions");
		return nullptr;
	}

	TiffEpJpeg out;
	if (!tiff_ep_find_jpeg(T, &out)) {
		set_error(error, "error looking for a full-size JPEG preview");
		return nullptr;
	}

	// Nikon NEFs seem to generally have a preview above 99 percent,
	// (though some of them may not even reach 50 percent).
	// Be a bit more generous than that with our crop tolerance.
	// TODO(p): Also take into account DNG DefaultCropSize, if present.
	if (out.pixels / (double(width) * height) < 0.95) {
		set_error(error, "could not find a large enough JPEG preview");
		return nullptr;
	}

	ImagePtr image = detail::load_jpeg(
		span<const uint8_t>(out.jpeg, out.jpeg_length), ctx, error);
	if (!image)
		return nullptr;

	// Note that Exif may override this later in open_from_data().
	// TODO(p): Try to use the Orientation field nearest to the target IFD.
	// IFD0 just happens to be fine for Nikon NEF.
	int64_t orientation = 0;
	if (tiffer_find_integer(T, TIFF_Orientation, &orientation) &&
		orientation >= 1 && orientation <= 8)
		image->orientation = Orientation(orientation);

	// XXX: AdobeRGB Nikon NEFs can only be distinguished by a ColorSpace tag
	// from within their MakerNote.
	return image;
}

}  // namespace

ImagePtr
detail::load_tiff_ep(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	tiffer T = {};
	if (!tiffer_init(&T, data.data(), data.size())) {
		set_error(error, "not a TIFF file");
		return nullptr;
	}

	ImagePtr head, tail;
	while (tiffer_next_ifd(&T)) {
		ImagePtr page = load_tiff_ep_page(&T, ctx, error);
		if (!page)
			return nullptr;
		append_page(head, tail, std::move(page));

		if (ctx.first_frame_only)
			break;

		// tiffer_next_ifd() requires all fields of the current IFD to have
		// been read first; the callee may have stopped partway through.
		tiffer_entry dummy = {};
		while (tiffer_next_entry(&T, &dummy))
			;
	}

	if (!head)
		set_error(error, "not a TIFF/EP or DNG image with a usable preview");
	return head;
}

}  // namespace dawn
