//
// load-tiff.cpp: general TIFF image loading (libtiff)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// This is the fallback, general-purpose TIFF loader--it runs after
// load_tiff_ep() and load_libraw() in the format dispatch, so that raw
// photos with a usable JPEG preview or sensor data get a chance to be
// rendered better first. This one instead trusts libtiff to make sense of
// (and composite) whatever it finds, one image per directory.
//

#include "dawn-config.h"
#include "libdn.h"
#include "libdn-loaders.h"

#include <tiff.h>
#include <tiffio.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

// --- In-memory TIFF client adaptor -------------------------------------------

struct TiffIo {
	const OpenContext *ctx = nullptr;
	const uint8_t *data = nullptr;
	toff_t position = 0, len = 0;
	string error;  ///< First hard error encountered, if any
};

tsize_t
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

tsize_t
tiff_write(thandle_t, tdata_t, tsize_t)
{
	errno = EBADF;
	return -1;
}

toff_t
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

int
tiff_close(thandle_t)
{
	return 0;
}

toff_t
tiff_size(thandle_t h)
{
	return ((TiffIo *) h)->len;
}

void
tiff_error(thandle_t h, const char *module, const char *format, va_list ap)
{
	auto *io = (TiffIo *) h;
	char buf[1024] = "";
	vsnprintf(buf, sizeof buf, format, ap);
	// Note that two errors could theoretically come in a succession,
	// but only the first one is normally interesting to the caller.
	if (io->error.empty())
		io->error = string(module) + ": " + buf;
	else if (io->ctx)
		add_warning(*io->ctx, string(module) + ": " + buf);
}

void
tiff_warning(thandle_t h, const char *module, const char *format, va_list ap)
{
	auto *io = (TiffIo *) h;
	if (!io->ctx)
		return;

	char buf[1024] = "";
	vsnprintf(buf, sizeof buf, format, ap);
	add_warning(*io->ctx, string(module) + ": " + buf);
}

// --- Directory decoding ------------------------------------------------------

// TIFF orientation tags 1-8 exactly match dn::Orientation, but our request
// for ORIENTATION_LEFTTOP normalization already rotates the raster--only
// a residual mirroring can remain, and apparently only these two forms of
// it (this mirrors what fiv-io.c has empirically established works).
void
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

void
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
ImagePtr
load_tiff_directory_u16(TIFF *tiff, const OpenContext &ctx, Error *error)
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
		set_error(error, "image dimensions too large");
		return nullptr;
	}

	tmsize_t scan_bytes = TIFFScanlineSize(tiff);
	if (scan_bytes <= 0) {
		set_error(error, "invalid TIFF scanline size");
		return nullptr;
	}

	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	vector<uint8_t> scan;
	scan.resize(size_t(scan_bytes));

	if (grey) {
		for (uint32_t y = 0; y < height; y++) {
			if (TIFFReadScanline(tiff, scan.data(), y, 0) < 0) {
				set_error(error, "TIFF decoding error");
				return nullptr;
			}
			auto *s = (const uint16_t *) scan.data();
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
				set_error(error, "TIFF decoding error");
				return nullptr;
			}
			memcpy(packed.data() + size_t(y) * width * spp, scan.data(),
				size_t(width) * spp * sizeof(uint16_t));
		}
		size_t stride = size_t(width) * spp * sizeof(uint16_t);
		if (spp == 3)
			pack_rgb16le_to_bgra16(*image, packed.data(), stride, 16);
		else
			pack_rgba16le_to_bgra16(*image, packed.data(), stride, 16);
	}

	uint16_t extras = 0;
	uint16_t *extra_types = nullptr;
	if (spp == 4 &&
		TIFFGetField(tiff, TIFFTAG_EXTRASAMPLES, &extras, &extra_types) &&
		extras > 0 && extra_types && extra_types[0] == EXTRASAMPLE_ASSOCALPHA)
		unpremultiply_bgra16(*image);

	// Full orientation tag: we do not ask libtiff to rotate the raster.
	uint16_t orientation = ORIENTATION_TOPLEFT;
	if (TIFFGetField(tiff, TIFFTAG_ORIENTATION, &orientation) &&
		orientation >= 1 && orientation <= 8)
		image->orientation = Orientation(orientation);

	apply_tiff_metadata(*image, tiff);
	ensure_working_premul(*image, ctx, nullptr, /*input_premul=*/false);
	return image;
}

ImagePtr
load_tiff_directory(TIFF *tiff, const OpenContext &ctx, Error *error)
{
	{
		Error u16err;
		ImagePtr hi = load_tiff_directory_u16(tiff, ctx, &u16err);
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
		set_error(error, "image dimensions too large");
		TIFFRGBAImageEnd(&img);
		return nullptr;
	}

	ImagePtr image = image_new(img.width, img.height);
	if (!image) {
		set_error(error, "image allocation failure");
		TIFFRGBAImageEnd(&img);
		return nullptr;
	}

	// This normalizes rotation, but not necessarily mirroring--see below.
	img.req_orientation = ORIENTATION_LEFTTOP;

	vector<uint32_t> raster(size_t(img.width) * img.height);
	bool ok = TIFFRGBAImageGet(&img, raster.data(), img.width, img.height);
	TIFFRGBAImageEnd(&img);
	if (!ok) {
		set_error(error, "TIFF decoding error");
		return nullptr;
	}

	size_t stride = size_t(img.width) * 4;
	vector<uint8_t> pixels(stride * img.height);
	uint8_t *d = pixels.data();
	for (uint32_t p : raster) {
		*d++ = TIFFGetB(p);
		*d++ = TIFFGetG(p);
		*d++ = TIFFGetR(p);
		*d++ = TIFFGetA(p);
	}

	// With associated alpha, TIFFRGBAImageGet() already premultiplies the
	// samples for us--undo that on the temporary 8-bit buffer so what gets
	// widened below is always straight (unassociated) BGRA8, exactly what
	// ensure_working_premul() expects.
	if (img.alpha == EXTRASAMPLE_ASSOCALPHA)
		unpremultiply_bgra8(pixels.data(), img.width, img.height, stride);

	widen_bgra8_to_bgra16(*image, pixels.data(), stride);

	// XXX: The whole file is essentially an Exif, any ideas?
	// TODO(p): TIFF has a number of fields that an ICC profile can be
	// constructed from--it's not a good idea to blindly default to sRGB
	// if we don't find an ICC profile.
	apply_tiff_metadata(*image, tiff);
	apply_tiff_orientation(*image, tiff);

	ensure_working_premul(*image, ctx, nullptr, /*input_premul=*/false);

	// TODO(p): It's possible to implement ClipPath easily.
	return image;
}

}  // namespace

ImagePtr
detail::load_tiff(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
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
		} while (!ctx.first_frame_only && TIFFReadDirectory(tiff));
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
		set_error(error, "empty or unsupported TIFF image");
	}
	return head;
}

}  // namespace dn
