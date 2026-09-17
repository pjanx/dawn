//
// load-librsvg.cpp: SVG image loading via librsvg (GLib + Cairo allowed here)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <librsvg/rsvg.h>

#include <cstdint>
#include <memory>
#include <string>

using namespace std;

namespace dawn
{

// Cairo's ARGB32 is a native-endian 0xAARRGGBB word, always premultiplied--
// exactly the layout pack_argb32_words_to_bgra16() expects, association
// (premultiplication) unchanged.
static void
cairo_argb32_to_image(Image &dst, cairo_surface_t *surface)
{
	const uint8_t *base = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	pack_argb32_words_to_bgra16(
		dst, assume_aligned<const uint32_t>(base), size_t(stride));
}

namespace
{

class LibrsvgRenderClosure : public RenderClosure
{
	RsvgHandle *handle_;
	double width_;   ///< Normal width at scale == 1
	double height_;  ///< Normal height at scale == 1

public:
	LibrsvgRenderClosure(RsvgHandle *handle, double width, double height)
		: handle_(handle), width_(width), height_(height)
	{
	}

	~LibrsvgRenderClosure() override { g_object_unref(handle_); }

	LibrsvgRenderClosure(const LibrsvgRenderClosure &) = delete;
	LibrsvgRenderClosure &operator=(const LibrsvgRenderClosure &) = delete;

	ImagePtr render(
		const OpenContext &ctx, double scale, Error *error) override;
};

}  // namespace

ImagePtr
LibrsvgRenderClosure::render(const OpenContext &ctx, double scale, Error *error)
{
	RsvgRectangle viewport = {
		.x = 0, .y = 0, .width = width_ * scale, .height = height_ * scale};
	// The viewport stays fractional: librsvg scales the document into it,
	// and only the surface is whole pixels.
	uint32_t uw = 0, uh = 0;
	if (!render_dimensions(viewport.width, viewport.height, &uw, &uh, error))
		return nullptr;

	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(uw), int(uh));
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		set_error(error, _("image allocation failure"));
		cairo_surface_destroy(surface);
		return nullptr;
	}

	cairo_t *cr = cairo_create(surface);
	GError *gerror = nullptr;
	gboolean success =
		rsvg_handle_render_document(handle_, cr, &viewport, &gerror);
	cairo_status_t status = cairo_status(cr);
	cairo_destroy(cr);
	if (!success) {
		set_error(error, gerror ? gerror->message : "librsvg rendering failed");
		g_clear_error(&gerror);
		cairo_surface_destroy(surface);
		return nullptr;
	}
	if (status != CAIRO_STATUS_SUCCESS) {
		set_error(error, cairo_status_to_string(status));
		cairo_surface_destroy(surface);
		return nullptr;
	}

	cairo_surface_flush(surface);
	ImagePtr image = image_new(uw, uh);
	if (!image) {
		set_error(error, _("image allocation failure"));
		cairo_surface_destroy(surface);
		return nullptr;
	}

	cairo_argb32_to_image(*image, surface);
	cairo_surface_destroy(surface);

	// Cairo ARGB32 is premultiplied. finish_image() with input_premul=true
	// is a no-op when there is no screen profile.
	finish_image(*image, ctx, nullptr, /*input_premul=*/true);
	return image;
}

ImagePtr
load_librsvg(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	GFile *base_file = g_file_new_for_uri(ctx.uri.c_str());
	GInputStream *is = g_memory_input_stream_new_from_data(
		data.data(), gssize(data.size()), nullptr);
	GError *gerror = nullptr;
	RsvgHandle *handle = rsvg_handle_new_from_stream_sync(
		is, base_file, RSVG_HANDLE_FLAG_KEEP_IMAGE_DATA, nullptr, &gerror);
	g_object_unref(base_file);
	g_object_unref(is);
	if (!handle) {
		set_error(error, gerror ? gerror->message : "librsvg parsing failed");
		g_clear_error(&gerror);
		return nullptr;
	}

	rsvg_handle_set_dpi(handle, ctx.screen_dpi > 0 ? ctx.screen_dpi : 96);

	double w = 0, h = 0;
	if (!rsvg_handle_get_intrinsic_size_in_pixels(handle, &w, &h)) {
		RsvgRectangle viewbox = {};
		gboolean has_viewport = FALSE;
		rsvg_handle_get_intrinsic_dimensions(handle, nullptr, nullptr, nullptr,
			nullptr, &has_viewport, &viewbox);
		if (!has_viewport) {
			set_error(error, _("cannot compute pixel dimensions"));
			g_object_unref(handle);
			return nullptr;
		}
		w = viewbox.width;
		h = viewbox.height;
	}
	if (!(w > 0) || !(h > 0)) {
		set_error(error, _("cannot compute pixel dimensions"));
		g_object_unref(handle);
		return nullptr;
	}

	// librsvg rasterizes filters, so rendering to a recording surface first
	// (to allow cheap re-rendering at other scales) is not an option--the
	// RsvgHandle itself is retained in the render closure instead.
	auto closure = make_unique<LibrsvgRenderClosure>(handle, w, h);

	ImagePtr image = closure->render(ctx, 1., error);
	if (!image)
		return nullptr;

	image->render = std::move(closure);
	return image;
}

}  // namespace dawn
