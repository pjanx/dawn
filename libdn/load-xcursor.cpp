//
// load-xcursor.cpp: Xcursor image loading
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn.h"
#include "libdn-loaders.h"

#include <X11/Xcursor/Xcursor.h>

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstring>
#include <memory>

using namespace std;

namespace dn
{
namespace
{

// --- In-memory XcursorFile adaptor -------------------------------------------
// libXcursor checks for EOF rather than -1 on short reads, so this needs to
// be careful to only ever report exactly as many bytes as were available.

struct MemXcursorFile {
	XcursorFile parent;
	const unsigned char *data;
	long position, len;
};

int
xcursor_read(XcursorFile *file, unsigned char *buf, int len)
{
	auto *self = (MemXcursorFile *) file;
	if (self->position < 0 || self->position > self->len) {
		errno = EOVERFLOW;
		return -1;
	}

	long n = min(self->len - self->position, long(len));
	if (n > INT_MAX) {
		errno = EIO;
		return -1;
	}

	memcpy(buf, self->data + self->position, size_t(n));
	self->position += n;
	return int(n);
}

int
xcursor_write(XcursorFile *, unsigned char *, int)
{
	errno = EBADF;
	return -1;
}

int
xcursor_seek(XcursorFile *file, long offset, int whence)
{
	auto *self = (MemXcursorFile *) file;
	switch (whence) {
	case SEEK_SET:
		self->position = offset;
		break;
	case SEEK_CUR:
		self->position += offset;
		break;
	case SEEK_END:
		self->position = self->len + offset;
		break;
	default:
		errno = EINVAL;
		return -1;
	}
	// This is technically too late for fseek(), but libXcursor doesn't care.
	if (self->position < 0) {
		errno = EINVAL;
		return -1;
	}
	return self->position;
}

const XcursorFile kMemXcursorFileAdaptor = {
	.closure = nullptr,
	.read = xcursor_read,
	.write = xcursor_write,
	.seek = xcursor_seek,
};

// --- Decoding ----------------------------------------------------------------

// XcursorImage pixels are native-endian, alpha-premultiplied 0xAARRGGBB
// words (libXcursor already byte swaps into host order for us), exactly
// the layout pack_argb32_words_to_bgra16() expects.
ImagePtr
load_xcursor_image(const XcursorImage *src)
{
	if (!src->width || !src->height)
		return nullptr;

	ImagePtr image = image_new(src->width, src->height);
	if (!image)
		return nullptr;

	pack_argb32_words_to_bgra16(
		*image, src->pixels, size_t(src->width) * sizeof(XcursorPixel));
	image->frame_duration = int64_t(src->delay);
	image->loops = 0;
	return image;
}

}  // namespace

// Cursor files bundle multiple pixel sizes of essentially the same picture,
// each of which may in turn be animated--map "nominal sizes" to pages,
// and animation frames within a nominal size to a per-page frame chain.
//
// Unlike other formats, there is no meaningful colour management to apply.
ImagePtr
detail::load_xcursor(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (data.size() > size_t(LONG_MAX)) {
		set_error(error, "size overflow");
		return nullptr;
	}

	MemXcursorFile file = {
		.parent = kMemXcursorFileAdaptor,
		.data = data.data(),
		.position = 0,
		.len = long(data.size()),
	};

	unique_ptr<XcursorImages, void (*)(XcursorImages *)> images(
		XcursorXcFileLoadAllImages(&file.parent), XcursorImagesDestroy);
	if (!images) {
		set_error(error, "not an Xcursor image");
		return nullptr;
	}

	ImagePtr pages, pages_tail;
	ImagePtr frames_tail;
	bool have_size = false;
	XcursorDim last_size = 0;
	for (int i = 0; i < images->nimage; i++) {
		const XcursorImage *src = images->images[i];
		bool new_page = !have_size || src->size != last_size;
		if (!new_page && ctx.first_frame_only) {
			last_size = src->size;
			continue;
		}

		ImagePtr frame = load_xcursor_image(src);
		if (!frame) {
			add_warning(ctx, "image allocation failure");
			last_size = src->size;
			continue;
		}

		if (new_page) {
			append_page(pages, pages_tail, std::move(frame));
			frames_tail = pages_tail;
		} else {
			append_frame(pages_tail, frames_tail, std::move(frame));
		}

		have_size = true;
		last_size = src->size;
	}

	if (!pages) {
		set_error(error, "empty or unsupported Xcursor image");
		return nullptr;
	}

	// Pixels are already premultiplied; ensure_working_premul_pages() only
	// needs to step in when a screen profile is configured, treating the
	// input as (implicitly sRGB) premultiplied BGRA16.
	for (Image *page = pages.get(); page; page = page->page_next.get())
		ensure_working_premul_pages(*page, ctx, nullptr, /*input_premul=*/true);
	return pages;
}

}  // namespace dn
