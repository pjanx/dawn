//
// load-gdkpixbuf.cpp: last-resort image loading via gdk-pixbuf
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// This is the last-resort loader: gdk-pixbuf has a wide range of loadable
// formats via its module system, of varying quality and trustworthiness,
// so it only gets a chance after everything more specific has already failed.

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <gdk-pixbuf/gdk-pixbuf.h>

#include <cstring>

using namespace std;

namespace dawn
{
namespace
{

// GdkPixbuf currently only ever produces 8-bit-per-sample RGB(A) buffers,
// with 3 or 4 channels--this isn't expected to ever legitimately fail.
ImagePtr
load_gdkpixbuf_pixels(GdkPixbuf *pixbuf, Error *error)
{
	if (gdk_pixbuf_get_colorspace(pixbuf) != GDK_COLORSPACE_RGB ||
		gdk_pixbuf_get_bits_per_sample(pixbuf) != 8) {
		set_error(error, "unsupported gdk-pixbuf pixel format");
		return nullptr;
	}

	auto width = uint32_t(gdk_pixbuf_get_width(pixbuf));
	auto height = uint32_t(gdk_pixbuf_get_height(pixbuf));
	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	bool has_alpha = gdk_pixbuf_get_has_alpha(pixbuf);
	int src_stride = gdk_pixbuf_get_rowstride(pixbuf);
	guint length = 0;
	const guchar *src = gdk_pixbuf_get_pixels_with_length(pixbuf, &length);

	// gdk-pixbuf's in-memory representation always carries straight
	// (unassociated) alpha, exactly what ensure_working_premul() below
	// expects.
	if (has_alpha)
		pack_rgba8_to_bgra16(*image, src, size_t(src_stride));
	else
		pack_rgb8_to_bgra16(*image, src, size_t(src_stride));
	return image;
}

void
load_gdkpixbuf_metadata(Image &image, GdkPixbuf *pixbuf)
{
	const char *orientation = gdk_pixbuf_get_option(pixbuf, "orientation");
	if (orientation && strlen(orientation) == 1) {
		int n = *orientation - '0';
		if (n >= 1 && n <= 8)
			image.orientation = Orientation(n);
	}

	const char *icc = gdk_pixbuf_get_option(pixbuf, "icc-profile");
	if (icc) {
		gsize len = 0;
		guchar *raw = g_base64_decode(icc, &len);
		if (raw) {
			image.icc.assign(raw, raw + len);
			g_free(raw);
		}
	}
}

}  // namespace

vector<string>
detail::gdkpixbuf_media_types()
{
	vector<string> types;
	GSList *formats = gdk_pixbuf_get_formats();
	for (GSList *iter = formats; iter; iter = iter->next) {
		gchar **mime_types =
			gdk_pixbuf_format_get_mime_types((GdkPixbufFormat *) iter->data);
		for (gchar **p = mime_types; *p; p++)
			types.push_back(*p);
		g_strfreev(mime_types);
	}
	g_slist_free(formats);
	return types;
}

ImagePtr
detail::load_gdkpixbuf(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// gdk-pixbuf controls the playback of animations itself, and there is
	// no reliable method of extracting individual frames from it--treat
	// everything gdk-pixbuf can load as a single-frame, single-page image.
	GdkPixbufLoader *loader = gdk_pixbuf_loader_new();
	GError *gerror = nullptr;
	bool ok = gdk_pixbuf_loader_write(
		loader, data.data(), gsize(data.size()), &gerror);
	if (ok)
		ok = gdk_pixbuf_loader_close(loader, &gerror);
	else
		(void) gdk_pixbuf_loader_close(loader, nullptr);

	GdkPixbuf *pixbuf = ok ? gdk_pixbuf_loader_get_pixbuf(loader) : nullptr;
	if (!pixbuf) {
		set_error(
			error, gerror ? gerror->message : "gdk-pixbuf decoding error");
		if (gerror)
			g_error_free(gerror);
		g_object_unref(loader);
		return nullptr;
	}

	g_object_ref(pixbuf);
	g_object_unref(loader);

	ImagePtr image = load_gdkpixbuf_pixels(pixbuf, error);
	if (image) {
		load_gdkpixbuf_metadata(*image, pixbuf);
		ensure_working_premul(*image, ctx, nullptr, /*input_premul=*/false);
	}

	g_object_unref(pixbuf);
	return image;
}

}  // namespace dawn
