//
// load-poppler.cpp: Poppler PDF page loading
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// Deliberately not advertising support elsewhere, as with Core Graphics PDF.

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <poppler-document.h>
#include <poppler-page-renderer.h>
#include <poppler-page.h>

#include <cmath>
#include <memory>
#include <mutex>

using namespace std;

namespace dawn
{

namespace
{

// All pages of a document share it, and outlive load_poppler().
// Poppler documents may not be used from multiple threads at once.
struct PopplerDocument {
	mutex lock;
	unique_ptr<poppler::document> document;
};

using DocumentPtr = shared_ptr<PopplerDocument>;

class PopplerRenderClosure : public RenderClosure
{
	DocumentPtr document_;
	int page_;    ///< Zero-based
	double dpi_;  ///< Pixels per inch at scale == 1

public:
	PopplerRenderClosure(DocumentPtr document, int page, double dpi)
		: document_(std::move(document)), page_(page), dpi_(dpi)
	{
	}

	ImagePtr render(
		const OpenContext &ctx, double scale, Error *error) override;
};

}  // namespace

// Poppler renders the crop box, rotated by the page's /Rotate.
static void
poppler_page_size(
	const poppler::page &page, double dpi, double *width, double *height)
{
	poppler::rectf box = page.page_rect(poppler::crop_box);
	*width = box.width() * dpi / 72;
	*height = box.height() * dpi / 72;
	if (page.orientation() == poppler::page::landscape ||
		page.orientation() == poppler::page::seascape)
		swap(*width, *height);
}

ImagePtr
PopplerRenderClosure::render(const OpenContext &ctx, double scale, Error *error)
{
	// A PDF unit is 1/72 inch, see load-cgpdf.mm.
	double dpi = dpi_ * scale;
	if (!isfinite(dpi) || dpi <= 0) {
		set_error(error, _("invalid scale"));
		return nullptr;
	}

	poppler::image raster;
	double w = 0, h = 0;
	{
		lock_guard<mutex> guard(document_->lock);
		unique_ptr<poppler::page> page(document_->document->create_page(page_));
		if (!page) {
			set_error(error, _("no such page"));
			return nullptr;
		}

		poppler_page_size(*page, dpi, &w, &h);

		// Splash rounds where we would ceil(), so this errs on the safe side
		// of image_new(), and fails before Poppler allocates gigabytes.
		uint32_t cw = 0, ch = 0;
		if (!render_dimensions(w, h, &cw, &ch, error))
			return nullptr;
		if (uint64_t(cw) * kBytesPerPixel * ch > UINT32_MAX) {
			set_error(error, _("image dimensions overflow"));
			return nullptr;
		}

		// Paper is white, see load-cgpdf.mm, which also leaves us no use
		// for alpha, nor for ARGB32's host-endian words.
		poppler::page_renderer renderer;
		renderer.set_image_format(poppler::image::format_rgb24);
		renderer.set_paper_color(0xffffffff);
		renderer.set_render_hints(poppler::page_renderer::antialiasing |
			poppler::page_renderer::text_antialiasing);
		raster = renderer.render_page(page.get(), dpi, dpi);
	}

	// Splash rounds to the nearest pixel, and should it fail to allocate,
	// it silently draws into a single pixel instead.
	int rw = raster.width(), rh = raster.height();
	if (!raster.is_valid() || raster.format() != poppler::image::format_rgb24 ||
		rw < 1 || rh < 1 || abs(rw - w) > 1 || abs(rh - h) > 1 ||
		raster.bytes_per_row() < rw * 3) {
		set_error(error, _("Poppler rendering failed"));
		return nullptr;
	}

	ImagePtr image = image_new(uint32_t(rw), uint32_t(rh), error);
	if (!image)
		return nullptr;

	pack_rgb8_to_bgra16(*image, (const uint8_t *) raster.const_data(),
		size_t(raster.bytes_per_row()));

	// Poppler also composes each operation in its own colour space,
	// and lacking a display profile, it converts to sRGB.
	image->effective_profile = cmm_or_default(ctx)->get_profile_sRGB();
	finish_image(*image, ctx, image->effective_profile.get(),
		/*input_premul=*/true);
	return image;
}

// Nothing else has the document yet, so it needs no locking.
static bool
measure_poppler_page(PopplerDocument &document, int index, double dpi,
	double *width, double *height, Error *error)
{
	unique_ptr<poppler::page> page(document.document->create_page(index));
	if (!page) {
		set_error(error, _("no such page"));
		return false;
	}

	// Splash rounds to the nearest pixel, see render().
	poppler_page_size(*page, dpi, width, height);
	*width = round(*width);
	*height = round(*height);
	return true;
}

ImagePtr
load_poppler(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	if (!poppler::page_renderer::can_render()) {
		set_error(error, _("Poppler has been built without Splash"));
		return nullptr;
	}

	// Render closures outlive the caller's bytes, see load-cgpdf.mm.
	// Poppler takes over the array, but only if it succeeds.
	poppler::byte_array bytes(data.begin(), data.end());
	auto document = make_shared<PopplerDocument>();
	document->document.reset(poppler::document::load_from_data(&bytes));
	if (!document->document) {
		set_error(error, _("not a PDF document"));
		return nullptr;
	}
	if (document->document->is_locked()) {
		set_error(error, _("the document is password-protected"));
		return nullptr;
	}

	int count = document->document->pages();
	if (count <= 0) {
		set_error(error, _("the document has no pages"));
		return nullptr;
	}

	double dpi = ctx.screen_dpi > 0 ? ctx.screen_dpi : 96;

	ImagePtr head, tail;
	for (int i = 0; i < count; i++) {
		auto closure = make_unique<PopplerRenderClosure>(document, i, dpi);

		// Only the first page is rendered right away: documents get long.
		Error suberror;
		double w = 0, h = 0;
		ImagePtr image;
		if (measure_poppler_page(*document, i, dpi, &w, &h, &suberror))
			image = head ? deferred_image(w, h,
							   cmm_or_default(ctx)->get_profile_sRGB(), ctx,
							   &suberror)
						 : render_now(*closure, w, h, ctx, &suberror);
		if (!image) {
			if (!head) {
				set_error(error, std::move(suberror.message));
				return nullptr;
			}
			add_warning(ctx, suberror.message);
			break;
		}

		image->render = std::move(closure);
		append_page(head, tail, std::move(image));
		if (ctx.first_frame_only)
			break;
	}
	return head;
}

}  // namespace dawn
