//
// load-librsvg.cpp: SVG image loading via librsvg (GLib + Cairo allowed here)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn-loaders.h"
#include "libdn.h"

#include <cairo.h>
#include <gio/gio.h>
#include <glib.h>
#include <librsvg/rsvg.h>

#include <cmath>
#include <cstdint>
#include <memory>
#include <string>

using namespace std;

namespace dawn
{

namespace
{

// librsvg/Cairo rendering is capped at the project pixmap limit.
constexpr double kMaxDimension = double(dawn::kMaxDimension);

// Cairo's ARGB32 is a native-endian 0xAARRGGBB word, always premultiplied--
// exactly the layout pack_argb32_words_to_bgra16() expects, association
// (premultiplication) unchanged.
void
cairo_argb32_to_image(Image &dst, cairo_surface_t *surface)
{
	const uint8_t *base = cairo_image_surface_get_data(surface);
	int stride = cairo_image_surface_get_stride(surface);
	pack_argb32_words_to_bgra16(dst, (const uint32_t *) base, size_t(stride));
}

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

	ImagePtr render(Cmm *cmm, Profile *target, double scale) override;
	ImagePtr render_internal(
		double scale, Cmm *cmm, Profile *target, Error *error);
};

ImagePtr
LibrsvgRenderClosure::render(Cmm *cmm, Profile *target, double scale)
{
	Error ignored;
	return render_internal(scale, cmm, target, &ignored);
}

ImagePtr
LibrsvgRenderClosure::render_internal(
	double scale, Cmm *cmm, Profile *target, Error *error)
{
	RsvgRectangle viewport = {
		.x = 0, .y = 0, .width = width_ * scale, .height = height_ * scale};
	double w = ceil(viewport.width), h = ceil(viewport.height);
	if (w < 1 || h < 1 || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, "image dimensions overflow");
		return nullptr;
	}

	auto uw = uint32_t(w), uh = uint32_t(h);
	cairo_surface_t *surface =
		cairo_image_surface_create(CAIRO_FORMAT_ARGB32, int(uw), int(uh));
	if (cairo_surface_status(surface) != CAIRO_STATUS_SUCCESS) {
		set_error(error, "image allocation failure");
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
		set_error(error, "image allocation failure");
		cairo_surface_destroy(surface);
		return nullptr;
	}

	cairo_argb32_to_image(*image, surface);
	cairo_surface_destroy(surface);

	// Cairo ARGB32 is premultiplied. ensure_working_premul with
	// input_premul=true is a no-op when there is no screen profile.
	OpenContext finish_ctx;
	if (cmm)
		finish_ctx.cmm = cmm->shared_from_this();
	if (target)
		finish_ctx.screen_profile =
			shared_ptr<Profile>(shared_ptr<Profile>(), target);
	ensure_working_premul(*image, finish_ctx, nullptr, /*input_premul=*/true);
	return image;
}

}  // namespace

ImagePtr
detail::load_librsvg(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
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
			set_error(error, "cannot compute pixel dimensions");
			g_object_unref(handle);
			return nullptr;
		}
		w = viewbox.width;
		h = viewbox.height;
	}
	if (!(w > 0) || !(h > 0)) {
		set_error(error, "cannot compute pixel dimensions");
		g_object_unref(handle);
		return nullptr;
	}

	// librsvg rasterizes filters, so rendering to a recording surface first
	// (to allow cheap re-rendering at other scales) is not an option--the
	// RsvgHandle itself is retained in the render closure instead.
	auto closure = make_unique<LibrsvgRenderClosure>(handle, w, h);

	ImagePtr image = closure->render_internal(
		1., ctx.cmm.get(), ctx.screen_profile.get(), error);
	if (!image)
		return nullptr;

	image->render = std::move(closure);
	return image;
}

}  // namespace dawn
