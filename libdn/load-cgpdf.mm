//
// load-cgpdf.mm: macOS Core Graphics PDF page loading
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// Deliberately not advertising support elsewhere.  This is not our focus.
//
// PostScript would be CGPSConverter, which on arm64 reports success while
// producing no pages at all.

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <CoreFoundation/CoreFoundation.h>
#include <CoreGraphics/CoreGraphics.h>

#include <cmath>
#include <cstring>
#include <memory>
#include <vector>

using namespace std;

namespace dawn
{

namespace
{

// All pages of a document share it, and outlive load_cgpdf().
using DocumentPtr = shared_ptr<CGPDFDocument>;

// The crop box is what a page means to show, and the drawing transform
// takes care of both its origin and the page's /Rotate.
void
cgpdf_page_size(CGPDFPageRef page, double *width, double *height)
{
	CGRect box = CGPDFPageGetBoxRect(page, kCGPDFCropBox);
	*width = box.size.width;
	*height = box.size.height;
	if (CGPDFPageGetRotationAngle(page) % 180) {
		*width = box.size.height;
		*height = box.size.width;
	}
}

class CGPDFRenderClosure : public RenderClosure
{
	DocumentPtr document_;
	size_t page_;
	double dpi_;  ///< Pixels per inch at scale == 1

public:
	CGPDFRenderClosure(DocumentPtr document, size_t page, double dpi)
		: document_(std::move(document)), page_(page), dpi_(dpi)
	{
	}

	ImagePtr render(Cmm *cmm, Profile *target, double scale) override;
	ImagePtr render_internal(
		double scale, const OpenContext &ctx, Error *error);
};

ImagePtr
CGPDFRenderClosure::render(Cmm *cmm, Profile *target, double scale)
{
	OpenContext ctx;
	if (cmm)
		ctx.cmm = cmm->shared_from_this();
	if (target)
		ctx.screen_profile = shared_ptr<Profile>(shared_ptr<Profile>(), target);

	Error ignored;
	return render_internal(scale, ctx, &ignored);
}

ImagePtr
CGPDFRenderClosure::render_internal(
	double scale, const OpenContext &ctx, Error *error)
{
	CGPDFPageRef page = CGPDFDocumentGetPage(document_.get(), page_);
	if (!page) {
		set_error(error, "no such page");
		return nullptr;
	}

	// A PDF unit is 1/72 inch, so the page has a physical size, and showing
	// it at 1:1 means resolving that against the screen, as resvg does for
	// the physical units in an SVG.
	double zoom = dpi_ / 72. * scale;

	double pw = 0, ph = 0;
	cgpdf_page_size(page, &pw, &ph);
	double w = ceil(pw * zoom), h = ceil(ph * zoom);
	if (w < 1 || h < 1 || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, "image dimensions overflow");
		return nullptr;
	}

	// Core Graphics composes each operation in its own colour space, so there
	// is no source profile to hand over as with bitmaps--let it convert to
	// sRGB, which then genuinely is one, and leave the rest to lcms2.
	CGColorSpaceRef srgb = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
	if (!srgb) {
		set_error(error, "cannot create an sRGB colour space");
		return nullptr;
	}

	auto uw = uint32_t(w), uh = uint32_t(h);
	size_t stride = size_t(uw) * 4;
	vector<uint8_t> pixels(stride * uh);
	CGContextRef context = CGBitmapContextCreate(pixels.data(), uw, uh, 8,
		stride, srgb,
		CGBitmapInfo(kCGImageAlphaPremultipliedLast) | kCGBitmapByteOrder32Big);
	CFDataRef icc = CGColorSpaceCopyICCData(srgb);
	CGColorSpaceRelease(srgb);
	if (!context) {
		if (icc)
			CFRelease(icc);
		set_error(error, "cannot create a bitmap context");
		return nullptr;
	}

	// A page is paper: it has no background of its own, and text on
	// transparency would be illegible over the viewer's own backdrop.
	CGContextSetRGBFillColor(context, 1., 1., 1., 1.);
	CGContextFillRect(context, CGRectMake(0, 0, w, h));
	CGContextScaleCTM(context, zoom, zoom);
	CGContextConcatCTM(context,
		CGPDFPageGetDrawingTransform(
			page, kCGPDFCropBox, CGRectMake(0, 0, pw, ph), 0, true));
	CGContextDrawPDFPage(context, page);
	CGContextRelease(context);

	ImagePtr image = image_new(uw, uh);
	if (!image) {
		if (icc)
			CFRelease(icc);
		set_error(error, "image allocation failure");
		return nullptr;
	}

	// Bitmap contexts store their first row at the top, as when drawing
	// images, and Core Graphics always premultiplies.
	pack_rgba8_to_bgra16(*image, pixels.data(), stride);
	if (icc) {
		const uint8_t *bytes = CFDataGetBytePtr(icc);
		image->icc.assign(bytes, bytes + CFDataGetLength(icc));
		CFRelease(icc);
	}

	ensure_working_premul(*image, ctx, nullptr, /*input_premul=*/true);
	return image;
}

}  // namespace

ImagePtr
detail::load_cgpdf(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// The document only maps what it needs, when it needs it, and its render
	// closures outlive this call, so it must own the bytes--the caller's are
	// long gone by the time a zoom change asks for a new rasterization.
	CFDataRef bytes =
		CFDataCreate(kCFAllocatorDefault, data.data(), CFIndex(data.size()));
	if (!bytes) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	CGDataProviderRef provider = CGDataProviderCreateWithCFData(bytes);
	CFRelease(bytes);
	if (!provider) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	DocumentPtr document(
		CGPDFDocumentCreateWithProvider(provider), CGPDFDocumentRelease);
	CGDataProviderRelease(provider);
	if (!document) {
		set_error(error, "not a PDF document");
		return nullptr;
	}

	// Core Graphics opens encrypted documents, then draws nothing of them.
	if (CGPDFDocumentIsEncrypted(document.get()) &&
		!CGPDFDocumentUnlockWithPassword(document.get(), "")) {
		set_error(error, "the document is password-protected");
		return nullptr;
	}

	size_t count = CGPDFDocumentGetNumberOfPages(document.get());
	if (!count) {
		set_error(error, "the document has no pages");
		return nullptr;
	}

	double dpi = ctx.screen_dpi > 0 ? ctx.screen_dpi : 96;

	ImagePtr head, tail;
	for (size_t i = 1; i <= count; i++) {
		auto closure = make_unique<CGPDFRenderClosure>(document, i, dpi);

		Error suberror;
		ImagePtr image = closure->render_internal(1., ctx, &suberror);
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
