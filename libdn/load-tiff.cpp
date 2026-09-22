//
// load-tiff.cpp: general TIFF image loading (libtiff)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// This is the fallback, general-purpose TIFF loader.  It may misprocess raw
// photos, so it should be run after better loaders.

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <tiff.h>
#include <tiffio.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#define TIFF_TABLES_CONSTANTS_ONLY
#include "tiff-tables.h"

using namespace std;

namespace dawn
{

// --- In-memory TIFF client adaptor -------------------------------------------

namespace
{

struct TiffIo {
	const OpenContext *ctx = nullptr;
	const uint8_t *data = nullptr;
	toff_t position = 0, len = 0;
	string error;  ///< First hard error encountered, if any

	/// How libtiff's diagnostics are to be treated: an optional metadata
	/// read may not cost us successfully decoded pages, and restoring the
	/// image directory only repeats what reading it first time round said.
	enum { Fatal, Demoted, Ignored } diagnostics = Fatal;
};

}  // namespace

static tsize_t
tiff_read(thandle_t h, tdata_t buf, tsize_t len)
{
	auto *io = (TiffIo *) h;
	if (len < 0 || io->position > io->len) {
		errno = EOVERFLOW;
		return -1;
	}

	toff_t n = min(io->len - io->position, toff_t(len));
	if (n > toff_t(numeric_limits<tmsize_t>::max())) {
		errno = EIO;
		return -1;
	}

	memcpy(buf, io->data + io->position, size_t(n));
	io->position += n;
	return tsize_t(n);
}

static tsize_t
tiff_write(thandle_t, tdata_t, tsize_t)
{
	errno = EBADF;
	return -1;
}

static toff_t
tiff_seek(thandle_t h, toff_t offset, int whence)
{
	auto *io = (TiffIo *) h;
	switch (whence) {
	case SEEK_SET:
		io->position = offset;
		break;
	case SEEK_CUR:
		io->position += offset;
		break;
	case SEEK_END:
		io->position = io->len + offset;
		break;
	default:
		errno = EINVAL;
		return toff_t(-1);
	}
	return io->position;
}

static int
tiff_close(thandle_t)
{
	return 0;
}

static toff_t
tiff_size(thandle_t h)
{
	return ((TiffIo *) h)->len;
}

DAWN_FORMAT(3, 0) static void
tiff_error(thandle_t h, const char *module, const char *format, va_list ap)
{
	auto *io = (TiffIo *) h;
	if (io->diagnostics == TiffIo::Ignored)
		return;

	char buf[1024] = "";
	vsnprintf(buf, sizeof buf, format, ap);
	// Note that two errors could theoretically come in a succession,
	// but only the first one is normally interesting to the caller.
	if (io->diagnostics == TiffIo::Demoted || !io->error.empty()) {
		if (io->ctx)
			add_warning(*io->ctx, string(module) + ": " + buf);
	} else {
		io->error = string(module) + ": " + buf;
	}
}

DAWN_FORMAT(3, 0) static void
tiff_warning(thandle_t h, const char *module, const char *format, va_list ap)
{
	auto *io = (TiffIo *) h;
	if (!io->ctx || io->diagnostics == TiffIo::Ignored)
		return;

	char buf[1024] = "";
	vsnprintf(buf, sizeof buf, format, ap);
	add_warning(*io->ctx, string(module) + ": " + buf);
}

// --- Source profile derivation -----------------------------------------------

// TransferFunction is one table per channel, or a single shared one, with
// as many entries as the bit depth can encode--not tificc's hardcoded 256.
static bool
tiff_transfer_function(
	TIFF *tiff, uint16_t photometric, span<const uint16_t> curves[3])
{
	// Palette images are out: libtiff keeps just the first of their three
	// tables, and repeating it would invent a curve the file never stated.
	if (photometric == PHOTOMETRIC_PALETTE)
		return false;

	uint16_t bps = 0;
	if (!TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bps) || !bps || bps > 16)
		return false;

	uint16_t *r = nullptr, *g = nullptr, *b = nullptr;
	if (!TIFFGetField(tiff, TIFFTAG_TRANSFERFUNCTION, &r, &g, &b) || !r)
		return false;

	size_t entries = size_t(1) << bps;
	curves[0] = {r, entries};
	curves[1] = {g ? g : r, entries};
	curves[2] = {b ? b : r, entries};
	return true;
}

// Reads the Exif sub-IFD's colour statements, when there is one. Its own
// failures are just missing metadata, and may not cost us the raster.
static bool
read_tiff_exif_colour(
	TIFF *tiff, bool *srgb, optional<double> *gamma, Error *error)
{
	uint64_t offset = 0;
	if (!TIFFGetField(tiff, TIFFTAG_EXIFIFD, &offset))
		return true;

	auto *io = (TiffIo *) TIFFClientdata(tiff);
	uint64_t saved = TIFFCurrentDirOffset(tiff);

	io->diagnostics = TiffIo::Demoted;
	if (TIFFReadEXIFDirectory(tiff, offset)) {
		uint16_t colorspace = 0;
		float value = 0;
		if (TIFFGetField(tiff, EXIFTAG_COLORSPACE, &colorspace))
			*srgb = colorspace == Exif_ColorSpace_sRGB;
		if (TIFFGetField(tiff, EXIFTAG_GAMMA, &value) && value > 0)
			*gamma = value;
	}

	// Rather than the directory index, which a failed custom-directory read
	// can leave behind unusable. Failing here is fatal: all its tag
	// pointers are gone, and so is any chance of decoding the raster.
	io->diagnostics = TiffIo::Ignored;
	bool restored = TIFFSetSubDirectory(tiff, saved);
	io->diagnostics = TiffIo::Fatal;
	if (restored)
		return true;

	set_error(error, _("cannot return to the TIFF image directory"));
	if (io->error.empty())
		io->error = error->message;
	return false;
}

/// Reconstructs what the directory says about its colour, for the files
/// that state it without embedding an ICC profile. Null when there is
/// nothing to go on, or when there is a profile to be read from the blob;
/// a set `error` means the directory is lost and decoding must stop.
static shared_ptr<Profile>
tiff_source_profile(TIFF *tiff, const OpenContext &ctx, Error *error)
{
	uint32_t len = 0;
	void *icc = nullptr;
	if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &len, &icc) && icc && len)
		return nullptr;

	// Reloading the directory invalidates every tag pointer, so this has
	// to happen before any of them is taken.
	bool srgb = false;
	optional<double> gamma;
	if (!read_tiff_exif_colour(tiff, &srgb, &gamma, error))
		return nullptr;

	auto cmm = cmm_or_default(ctx);
	if (srgb)
		return cmm->get_profile_sRGB();

	// Raw sensor directories carry chromaticities that describe anything
	// but their CFA samples--only trust them for rendered rasters.
	uint16_t photometric = 0;
	if (!TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric) ||
		(photometric != PHOTOMETRIC_RGB && photometric != PHOTOMETRIC_YCBCR &&
			photometric != PHOTOMETRIC_PALETTE))
		return nullptr;

	float *wp = nullptr, *prim = nullptr;
	if (!TIFFGetField(tiff, TIFFTAG_WHITEPOINT, &wp) || !wp ||
		!TIFFGetField(tiff, TIFFTAG_PRIMARYCHROMATICITIES, &prim) || !prim)
		return nullptr;

	double whitepoint[2] = {wp[0], wp[1]};
	double primaries[6] = {
		prim[0], prim[1], prim[2], prim[3], prim[4], prim[5]};

	span<const uint16_t> curves[3];
	if (tiff_transfer_function(tiff, photometric, curves)) {
		if (auto profile =
				cmm->get_profile_tabulated(whitepoint, primaries, curves))
			return profile;
	}

	// Files carrying these tags with no stated curve are camera output
	// that means sRGB, as with PNG cHRM lacking gAMA.
	return cmm->get_profile_parametric(gamma, whitepoint, primaries);
}

// --- Directory decoding ------------------------------------------------------

// TIFF orientation tags 1-8 exactly match dn::Orientation, but our request
// for ORIENTATION_LEFTTOP normalization already rotates the raster--only
// a residual mirroring can remain, and apparently only these two forms of
// it (this mirrors what fiv-io.c has empirically established works).
static void
apply_tiff_orientation(Image &image, TIFF *tiff)
{
	uint16_t orientation = 0;
	if (!TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation))
		return;
	if (orientation == 5 || orientation == 7)
		image.orientation = Orientation::Mirror270;
	if (orientation == 6 || orientation == 8)
		image.orientation = Orientation::Mirror90;
}

static void
apply_tiff_metadata(Image &image, TIFF *tiff)
{
	uint32_t len = 0;
	void *p = nullptr;
	if (TIFFGetField(tiff, TIFFTAG_ICCPROFILE, &len, &p) && p && len) {
		auto *b = (const uint8_t *) p;
		image.icc.assign(b, b + len);
	}
	if (TIFFGetField(tiff, TIFFTAG_XMLPACKET, &len, &p) && p && len) {
		auto *b = (const uint8_t *) p;
		image.xmp.assign(b, b + len);
	}
}

// Contiguous unsigned 16-bit grey/RGB(A) that TIFFRGBAImage would only
// quantize to 8-bit. Reads scanlines and packs to working BGRA16.
static ImagePtr
load_tiff_directory_u16(TIFF *tiff, const OpenContext &ctx,
	const shared_ptr<Profile> &source, Error *error)
{
	uint32_t width = 0, height = 0;
	uint16_t bps = 0, spp = 0, photometric = 0;
	uint16_t planar = PLANARCONFIG_CONTIG;
	uint16_t sampleformat = SAMPLEFORMAT_UINT;
	if (!TIFFGetField(tiff, TIFFTAG_IMAGEWIDTH, &width) ||
		!TIFFGetField(tiff, TIFFTAG_IMAGELENGTH, &height) ||
		!TIFFGetField(tiff, TIFFTAG_BITSPERSAMPLE, &bps) ||
		!TIFFGetField(tiff, TIFFTAG_SAMPLESPERPIXEL, &spp) ||
		!TIFFGetField(tiff, TIFFTAG_PHOTOMETRIC, &photometric))
		return nullptr;

	TIFFGetFieldDefaulted(tiff, TIFFTAG_PLANARCONFIG, &planar);
	TIFFGetFieldDefaulted(tiff, TIFFTAG_SAMPLEFORMAT, &sampleformat);

	if (bps != 16 || planar != PLANARCONFIG_CONTIG ||
		(sampleformat != SAMPLEFORMAT_UINT &&
			sampleformat != SAMPLEFORMAT_VOID))
		return nullptr;

	bool grey = photometric == PHOTOMETRIC_MINISBLACK && spp == 1;
	bool rgb = photometric == PHOTOMETRIC_RGB && (spp == 3 || spp == 4);
	if (!grey && !rgb)
		return nullptr;

	if (width > kMaxDimension || height > kMaxDimension) {
		set_error(error, _("image dimensions too large"));
		return nullptr;
	}

	tmsize_t scan_bytes = TIFFScanlineSize(tiff);
	if (scan_bytes <= 0) {
		set_error(error, _("invalid TIFF scanline size"));
		return nullptr;
	}

	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, _("image allocation failure"));
		return nullptr;
	}

	vector<uint8_t> scan;
	scan.resize(size_t(scan_bytes));

	if (grey) {
		for (uint32_t y = 0; y < height; y++) {
			if (TIFFReadScanline(tiff, scan.data(), y, 0) < 0) {
				set_error(error, _("TIFF decoding error"));
				return nullptr;
			}
			auto *s = assume_aligned<const uint16_t>(scan.data());
			auto *d = row_u16(*image, y);
			for (uint32_t x = 0; x < width; x++) {
				d[0] = d[1] = d[2] = s[x];
				d[3] = 65535;
				d += 4;
			}
		}
	} else {
		vector<uint16_t> packed(size_t(width) * spp * height);
		for (uint32_t y = 0; y < height; y++) {
			if (TIFFReadScanline(tiff, scan.data(), y, 0) < 0) {
				set_error(error, _("TIFF decoding error"));
				return nullptr;
			}
			memcpy(packed.data() + size_t(y) * width * spp, scan.data(),
				size_t(width) * spp * sizeof(uint16_t));
		}

		uint16_t extras = 0;
		uint16_t *extra_types = nullptr;
		uint16_t alpha_type = spp == 4 &&
				TIFFGetField(
					tiff, TIFFTAG_EXTRASAMPLES, &extras, &extra_types) &&
				extras > 0 && extra_types
			? extra_types[0]
			: EXTRASAMPLE_UNSPECIFIED;

		size_t stride = size_t(width) * spp * sizeof(uint16_t);
		if (spp == 3) {
			pack_rgb16le_to_bgra16(*image, packed.data(), stride, 16);
		} else {
			// We accept four samples, but the fourth sample may be useless.
			if (alpha_type != EXTRASAMPLE_ASSOCALPHA &&
				alpha_type != EXTRASAMPLE_UNASSALPHA) {
				for (size_t i = 3; i < packed.size(); i += 4)
					packed[i] = 65535;
			}
			pack_rgba16le_to_bgra16(*image, packed.data(), stride, 16);
		}

		if (alpha_type == EXTRASAMPLE_ASSOCALPHA)
			unpremultiply_bgra16(*image);
	}

	// Full orientation tag: we do not ask libtiff to rotate the raster.
	uint16_t orientation = ORIENTATION_TOPLEFT;
	if (TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation) &&
		orientation >= 1 && orientation <= 8)
		image->orientation = Orientation(orientation);

	apply_tiff_metadata(*image, tiff);
	if (source)
		image->effective_profile = source;
	finish_image(*image, ctx, source.get(), /*input_premul=*/false);
	return image;
}

static ImagePtr
load_tiff_directory(TIFF *tiff, const OpenContext &ctx, Error *error)
{
	// This reloads the directory, so it must precede both any tag pointer
	// being taken and any pixel being decoded.
	Error derivation;
	shared_ptr<Profile> source = tiff_source_profile(tiff, ctx, &derivation);
	if (!derivation.message.empty()) {
		set_error(error, derivation.message);
		return nullptr;
	}

	{
		Error u16err;
		ImagePtr hi = load_tiff_directory_u16(tiff, ctx, source, &u16err);
		if (hi)
			return hi;
		if (!u16err.message.empty()) {
			set_error(error, u16err.message);
			return nullptr;
		}
	}

	char emsg[1024] = "";
	if (!TIFFRGBAImageOK(tiff, emsg)) {
		set_error(error, emsg);
		return nullptr;
	}

	TIFFRGBAImage img;
	if (!TIFFRGBAImageBegin(&img, tiff, /*stop_on_error=*/1, emsg)) {
		set_error(error, emsg);
		return nullptr;
	}

	if (img.width > kMaxDimension || img.height > kMaxDimension) {
		set_error(error, _("image dimensions too large"));
		TIFFRGBAImageEnd(&img);
		return nullptr;
	}

	ImagePtr image = image_new(img.width, img.height);
	if (!image) {
		set_error(error, _("image allocation failure"));
		TIFFRGBAImageEnd(&img);
		return nullptr;
	}

	// This normalizes rotation, but not necessarily mirroring--see below.
	img.req_orientation = ORIENTATION_LEFTTOP;

	vector<uint32_t> raster(size_t(img.width) * img.height);
	bool ok = TIFFRGBAImageGet(&img, raster.data(), img.width, img.height);
	TIFFRGBAImageEnd(&img);
	if (!ok) {
		set_error(error, _("TIFF decoding error"));
		return nullptr;
	}

	size_t stride = size_t(img.width) * 4;
	vector<uint8_t> pixels(stride * img.height);
	uint8_t *d = pixels.data();
	for (uint32_t p : raster) {
		*d++ = uint8_t(TIFFGetB(p));
		*d++ = uint8_t(TIFFGetG(p));
		*d++ = uint8_t(TIFFGetR(p));
		*d++ = uint8_t(TIFFGetA(p));
	}

	// With associated alpha, TIFFRGBAImageGet() already premultiplies the
	// samples for us--undo that on the temporary 8-bit buffer so what gets
	// widened below is always straight (unassociated) BGRA8, exactly what
	// finish_image() expects.
	if (img.alpha == EXTRASAMPLE_ASSOCALPHA)
		unpremultiply_xxxa8(pixels.data(), img.width, img.height, stride);

	widen_bgra8_to_bgra16(*image, pixels.data(), stride);

	// XXX: The whole file is essentially an Exif, any ideas?
	apply_tiff_metadata(*image, tiff);
	apply_tiff_orientation(*image, tiff);

	if (source)
		image->effective_profile = source;
	finish_image(*image, ctx, source.get(), /*input_premul=*/false);

	// TODO(p): It's possible to implement ClipPath easily.
	return image;
}

ImagePtr
load_tiff(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// libtiff error handlers are process-global; serialize installs.
	static mutex tiff_handler_mutex;
	lock_guard lock(tiff_handler_mutex);

	// Both kinds of handlers are called, redirect everything to our own,
	// which report through `error`/add_warning() instead of stderr.
	TIFFErrorHandler eh = TIFFSetErrorHandler(nullptr);
	TIFFErrorHandler wh = TIFFSetWarningHandler(nullptr);
	TIFFErrorHandlerExt ehe = TIFFSetErrorHandlerExt(tiff_error);
	TIFFErrorHandlerExt whe = TIFFSetWarningHandlerExt(tiff_warning);

	TiffIo io;
	io.ctx = &ctx;
	io.data = data.data();
	io.position = 0;
	io.len = toff_t(data.size());

	ImagePtr head, tail;
	const char *name = ctx.uri.empty() ? "(memory)" : ctx.uri.c_str();
	TIFF *tiff = TIFFClientOpen(name, "rm" /* Avoid mmap. */, &io, tiff_read,
		tiff_write, tiff_seek, tiff_close, tiff_size, nullptr, nullptr);
	if (tiff) {
		do {
			Error suberror;
			ImagePtr page = load_tiff_directory(tiff, ctx, &suberror);
			if (page)
				append_page(head, tail, std::move(page));
			else if (!suberror.message.empty())
				add_warning(ctx, suberror.message);
		} while (!ctx.first_frame_only && io.error.empty() &&
			TIFFReadDirectory(tiff));
		TIFFClose(tiff);
	}

	TIFFSetErrorHandlerExt(ehe);
	TIFFSetWarningHandlerExt(whe);
	TIFFSetErrorHandler(eh);
	TIFFSetWarningHandler(wh);

	if (!io.error.empty()) {
		head.reset();
		set_error(error, io.error);
	} else if (!head) {
		set_error(error, _("empty or unsupported TIFF image"));
	}
	return head;
}

}  // namespace dawn
