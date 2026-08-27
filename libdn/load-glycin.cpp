//
// load-glycin.cpp: sandboxed image loading via glycin
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

// Like gdk-pixbuf, glycin is a broad-coverage fallback built on out-of-process
// loaders, so it only gets a chance after everything more specific has failed.
// It goes first of the two: the loaders are memory-safe and sandboxed, and it
// gives us real 16-bit output rather than gdk-pixbuf's forced 8-bit RGB(A).
//
// glycin's C API exposes no ICC blob and no Exif, and its orientation getter
// stays at 1 unless transformations are applied, so we let it apply them and
// take the pixels as already oriented.  Colour comes from the CICP instead,
// synthesized into a profile where we can represent it (see get_profile_cicp);
// key-value metadata comes through Image::text.

#include <dawn-config.h>

#include "libdn.h"
#include "libdn-loaders.h"

#include <glycin.h>

using namespace std;

namespace dn
{
namespace
{

// The formats we ask glycin for.  This is a whitelist, not a filter: glycin
// converts anything else into the nearest listed format, so a short list
// costs conversions, never coverage.
//
// Four is the minimum that avoids all lossy conversions.  glycin's other
// formats reduce to these without losing anything:
//
//   - channel order (A8R8G8B8, B8G8R8A8, ...) is just a permutation, and
//     our packers only read R,G,B[,A] order;
//   - premultiplied variants would have to be un-premultiplied for
//     ensure_working_premul() anyway, so we take the straight ones;
//   - grayscale (G8, G16, G8A8, ...) widens to RGB losslessly;
//   - 8-bit widens to our 16-bit working format either way.
//
// The gap is float: R16G16B16_FLOAT and the R32 formats get squashed into
// 16-bit unsigned, which clips values outside [0,1] and loses precision on
// HDR sources (EXR most of all).  The working format is fixed-point BGRA16,
// so nothing here could preserve that anyway--this is a libdn-wide limit,
// not one this list imposes.  Should the working format ever grow a float
// variant, add those formats here and give them a packer.
constexpr GlyMemoryFormatSelection kAcceptedFormats =
	GlyMemoryFormatSelection(GLY_MEMORY_SELECTION_R8G8B8 |
		GLY_MEMORY_SELECTION_R8G8B8A8 | GLY_MEMORY_SELECTION_R16G16B16 |
		GLY_MEMORY_SELECTION_R16G16B16A16);

ImagePtr
load_glycin_frame(GlyFrame *frame, const OpenContext &ctx, Error *error)
{
	uint32_t width = gly_frame_get_width(frame);
	uint32_t height = gly_frame_get_height(frame);
	if (!width || !height || width > kMaxDimension || height > kMaxDimension) {
		set_error(error, "invalid image dimensions");
		return nullptr;
	}

	GBytes *bytes = gly_frame_get_buf_bytes(frame);
	if (!bytes) {
		set_error(error, "missing frame buffer");
		return nullptr;
	}

	gsize length = 0;
	const uint8_t *src = (const uint8_t *) g_bytes_get_data(bytes, &length);
	size_t stride = gly_frame_get_stride(frame);
	GlyMemoryFormat format = gly_frame_get_memory_format(frame);

	// Guard against a loader reporting a buffer smaller than it describes,
	// which would otherwise have us read past the end of the mapping.
	size_t bpp = 0;
	switch (format) {
	case GLY_MEMORY_R8G8B8:
		bpp = 3;
		break;
	case GLY_MEMORY_R8G8B8A8:
		bpp = 4;
		break;
	case GLY_MEMORY_R16G16B16:
		bpp = 6;
		break;
	case GLY_MEMORY_R16G16B16A16:
		bpp = 8;
		break;
	default:
		set_error(error, "unsupported glycin pixel format");
		return nullptr;
	}
	if (stride < size_t(width) * bpp ||
		length < size_t(height - 1) * stride + size_t(width) * bpp) {
		set_error(error, "truncated glycin frame buffer");
		return nullptr;
	}

	ImagePtr image = image_new(width, height);
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	// None of the accepted formats is premultiplied, which is what
	// ensure_working_premul() below expects.
	switch (format) {
	case GLY_MEMORY_R8G8B8:
		pack_rgb8_to_bgra16(*image, src, stride);
		break;
	case GLY_MEMORY_R8G8B8A8:
		pack_rgba8_to_bgra16(*image, src, stride);
		break;
	case GLY_MEMORY_R16G16B16:
		pack_rgb16le_to_bgra16(*image, (const uint16_t *) src, stride, 16);
		break;
	case GLY_MEMORY_R16G16B16A16:
		pack_rgba16le_to_bgra16(*image, (const uint16_t *) src, stride, 16);
		break;
	default:
		break;
	}

	int64_t delay = gly_frame_get_delay(frame);
	if (delay > 0)
		image->frame_duration = delay / 1000;

	// The only colour information glycin exposes is the CICP; when we can
	// turn it into a profile, record the blob too, so the viewer reports a
	// source profile like it does for every other loader.
	shared_ptr<Profile> source;
	if (GlyCicp *cicp = gly_frame_get_color_cicp(frame)) {
		source = cmm_or_default(ctx)->get_profile_cicp(
			cicp->color_primaries, cicp->transfer_characteristics);
		if (source)
			image->icc = source->to_bytes();
		else
			add_warning(ctx,
				"glycin: unrepresentable CICP colour space, assuming sRGB");
		gly_cicp_free(cicp);
	}

	ensure_working_premul(*image, ctx, source.get(), /*input_premul=*/false);
	return image;
}

// Copies glycin's key-value metadata (PNG tEXt/zTXt/iTXt and friends) onto
// the image. glycin already strips the chunk-type prefixes gdk-pixbuf keeps,
// which matches what load-wuffs.cpp puts in Image::text.
void
load_glycin_metadata(Image &image, GlyImage *img)
{
	GStrv keys = gly_image_get_metadata_keys(img);
	for (gchar **k = keys; k && *k; k++) {
		gchar *value = gly_image_get_metadata_key_value(img, *k);
		if (!value)
			continue;
		image.text.emplace(*k, value);
		g_free(value);
	}
	g_strfreev(keys);
}

}  // namespace

vector<string>
detail::glycin_media_types()
{
	vector<string> types;
	GStrv mime_types = gly_loader_get_mime_types();
	for (gchar **p = mime_types; p && *p; p++)
		types.push_back(*p);
	g_strfreev(mime_types);
	return types;
}

ImagePtr
detail::load_glycin(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// glycin takes ownership of the GBytes, and we must not hand it a view
	// of a span whose lifetime it does not control.
	GBytes *bytes = g_bytes_new(data.data(), data.size());
	GlyLoader *loader = gly_loader_new_for_bytes(bytes);
	g_bytes_unref(bytes);

	// We have no way to read the untransformed orientation back out, so let
	// glycin bake it into the pixels rather than lose it entirely.
	gly_loader_set_apply_transformations(loader, TRUE);
	gly_loader_set_accepted_memory_formats(loader, kAcceptedFormats);

	GError *gerror = nullptr;
	GlyImage *img = gly_loader_load(loader, &gerror);
	g_object_unref(loader);
	if (!img) {
		set_error(error, gerror ? gerror->message : "glycin decoding error");
		if (gerror)
			g_error_free(gerror);
		return nullptr;
	}

	ImagePtr head, tail;
	while (true) {
		// Without this, glycin restarts animations from the first frame
		// instead of telling us that it has run out.
		GlyFrameRequest *request = gly_frame_request_new();
		gly_frame_request_set_loop_animation(request, FALSE);

		GError *ferror = nullptr;
		GlyFrame *frame =
			gly_image_get_specific_frame(img, request, &ferror);
		g_object_unref(request);
		if (!frame) {
			// Running out of frames is normal, and not always reported as
			// NO_MORE_FRAMES--some loaders raise a generic error instead.
			// Only a failure on the very first frame means the image itself
			// is undecodable.
			if (!head)
				set_error(error,
					ferror ? ferror->message : "glycin decoding error");
			if (ferror)
				g_error_free(ferror);
			break;
		}

		ImagePtr image = load_glycin_frame(frame, ctx, error);
		int64_t delay = gly_frame_get_delay(frame);
		g_object_unref(frame);
		if (!image) {
			if (!head) {
				g_object_unref(img);
				return nullptr;
			}
			add_warning(ctx, "glycin: dropping undecodable trailing frame");
			break;
		}

		append_frame(head, tail, image);
		if (ctx.first_frame_only)
			break;

		// Stop unless this really is an animation.  Asking a still image for
		// another frame is how we would learn that it has none, but glycin's
		// ISOBMFF loaders (AVIF, HEIF) never answer that question: they block
		// forever instead of raising NO_MORE_FRAMES.  A zero delay marks a
		// still frame in every loader tested, so key off that and never make
		// the request.  Animations are still probed, and there the
		// end-of-frames reply does arrive.
		if (delay <= 0)
			break;
	}

	// Metadata belongs to the image, not any one frame: attach it to the head.
	if (head)
		load_glycin_metadata(*head, img);

	g_object_unref(img);
	return head;
}

}  // namespace dn
