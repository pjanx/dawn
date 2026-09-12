//
// load-dnrs.cpp: broad-coverage fallback through the in-tree Rust decoders
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <libdnrs.h>

#include <cstring>
#include <memory>

using namespace std;

namespace dawn
{
namespace
{

struct DecoderDeleter {
	void operator()(dnrs_decoder *decoder) const { dnrs_decoder_free(decoder); }
};

struct ErrorDeleter {
	void operator()(dnrs_error *error) const { dnrs_error_free(error); }
};

struct FrameGuard {
	dnrs_frame frame{};
	~FrameGuard() { dnrs_frame_clear(&frame); }
};

using DecoderPtr = unique_ptr<dnrs_decoder, DecoderDeleter>;
using DnrsErrorPtr = unique_ptr<dnrs_error, ErrorDeleter>;

// "Rust" names the loader, and the codec is what image-rs picked within it;
// neither is a word, so only the punctuation around them is up for grabs.
static string
dnrs_wrap(const char *codec, const char *detail)
{
	if (codec && *codec)
		return format_message(_("Rust (%s): %s"), codec, detail);
	return format_message(_("Rust: %s"), detail);
}

static string
dnrs_message(dnrs_error *error, const char *codec = nullptr)
{
	const char *detail = error ? dnrs_error_message(error) : nullptr;
	return dnrs_wrap(codec, detail && *detail ? detail : _("decoding error"));
}

static bool
valid_frame(const dnrs_frame &frame, size_t bytes_per_pixel)
{
	if (!frame.width || !frame.height || frame.width > kMaxDimension ||
		frame.height > kMaxDimension)
		return false;
	if (frame.width > SIZE_MAX / bytes_per_pixel)
		return false;
	const size_t row = size_t(frame.width) * bytes_per_pixel;
	return frame.data && frame.stride >= row &&
		frame.height - 1 <= (SIZE_MAX - row) / frame.stride &&
		frame.length >= size_t(frame.height - 1) * frame.stride + row;
}

static void
pack_gray(Image &image, const dnrs_frame &frame, bool alpha, bool wide)
{
	for (uint32_t y = 0; y < image.height; y++) {
		const uint8_t *src = frame.data + size_t(y) * frame.stride;
		uint16_t *dst = assume_aligned<uint16_t>(row_bytes(image, y));
		for (uint32_t x = 0; x < image.width; x++) {
			uint16_t g = 0, a = 65535;
			if (wide) {
				g = uint16_t(src[0] | uint16_t(src[1]) << 8);
				src += 2;
				if (alpha) {
					a = uint16_t(src[0] | uint16_t(src[1]) << 8);
					src += 2;
				}
			} else {
				g = uint16_t(*src++) * 257;
				if (alpha)
					a = uint16_t(*src++) * 257;
			}
			dst[0] = dst[1] = dst[2] = g;
			dst[3] = a;
			dst += 4;
		}
	}
}

static ImagePtr
load_frame(const dnrs_frame &frame, Error *error)
{
	size_t bpp = 0;
	switch (frame.format) {
	case DNRS_PIXEL_GRAY8:
		bpp = 1;
		break;
	case DNRS_PIXEL_GRAY_ALPHA8:
		bpp = 2;
		break;
	case DNRS_PIXEL_RGB8:
		bpp = 3;
		break;
	case DNRS_PIXEL_RGBA8:
		bpp = 4;
		break;
	case DNRS_PIXEL_GRAY16LE:
		bpp = 2;
		break;
	case DNRS_PIXEL_GRAY_ALPHA16LE:
		bpp = 4;
		break;
	case DNRS_PIXEL_RGB16LE:
		bpp = 6;
		break;
	case DNRS_PIXEL_RGBA16LE:
		bpp = 8;
		break;
	default:
		set_error(error, _("Rust: unsupported pixel format"));
		return nullptr;
	}
	if (!valid_frame(frame, bpp)) {
		set_error(error, _("Rust: invalid or truncated frame"));
		return nullptr;
	}

	ImagePtr image = image_new(frame.width, frame.height);
	if (!image) {
		set_error(error, _("Rust: image allocation failure"));
		return nullptr;
	}
	switch (frame.format) {
	case DNRS_PIXEL_GRAY8:
		pack_gray(*image, frame, false, false);
		break;
	case DNRS_PIXEL_GRAY_ALPHA8:
		pack_gray(*image, frame, true, false);
		break;
	case DNRS_PIXEL_RGB8:
		pack_rgb8_to_bgra16(*image, frame.data, frame.stride);
		break;
	case DNRS_PIXEL_RGBA8:
		pack_rgba8_to_bgra16(*image, frame.data, frame.stride);
		break;
	case DNRS_PIXEL_GRAY16LE:
		pack_gray(*image, frame, false, true);
		break;
	case DNRS_PIXEL_GRAY_ALPHA16LE:
		pack_gray(*image, frame, true, true);
		break;
	// Rust hands out plain byte buffers, but its allocator takes them from
	// malloc, and every 16-bit stride here is even, so rows are word-aligned.
	case DNRS_PIXEL_RGB16LE:
		pack_rgb16le_to_bgra16(*image,
			assume_aligned<const uint16_t>(frame.data), frame.stride, 16);
		break;
	case DNRS_PIXEL_RGBA16LE:
		pack_rgba16le_to_bgra16(*image,
			assume_aligned<const uint16_t>(frame.data), frame.stride, 16);
		break;
	}
	image->frame_duration =
		int64_t(min<uint64_t>(frame.duration_ms, uint64_t(INT64_MAX)));
	return image;
}

static void
copy_blob(vector<uint8_t> &out, dnrs_blob blob)
{
	if (blob.data && blob.length)
		out.assign(blob.data, blob.data + blob.length);
}

}  // namespace

vector<string>
detail::dnrs_media_types()
{
	size_t length = 0;
	const char *const *types = dnrs_mime_types(&length);
	vector<string> result;
	result.reserve(length);
	for (size_t i = 0; i < length; i++)
		if (types[i])
			result.emplace_back(types[i]);
	return result;
}

ImagePtr
detail::load_dnrs(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	dnrs_error *raw_error = nullptr;
	DecoderPtr decoder(dnrs_decoder_new(
		data.data(), data.size(), ctx.first_frame_only, &raw_error));
	DnrsErrorPtr dnrs_error(raw_error);
	if (!decoder) {
		set_error(error, dnrs_message(dnrs_error.get()));
		return nullptr;
	}

	dnrs_document_info document{};
	raw_error = nullptr;
	if (!dnrs_decoder_get_info(decoder.get(), &document, &raw_error)) {
		dnrs_error.reset(raw_error);
		set_error(error, dnrs_message(dnrs_error.get()));
		return nullptr;
	}
	const char *codec = document.codec;

	ImagePtr pages, pages_tail;
	dnrs_page_info page_info{};
	while (true) {
		raw_error = nullptr;
		if (!dnrs_decoder_next_page(decoder.get(), &page_info, &raw_error)) {
			dnrs_error.reset(raw_error);
			if (dnrs_error) {
				set_error(error, dnrs_message(dnrs_error.get(), codec));
				return nullptr;
			}
			break;
		}
		if (!page_info.width || !page_info.height ||
			page_info.width > kMaxDimension ||
			page_info.height > kMaxDimension) {
			set_error(error, dnrs_wrap(codec, _("invalid image dimensions")));
			return nullptr;
		}

		ImagePtr frames, frames_tail;
		while (true) {
			FrameGuard owned;
			raw_error = nullptr;
			if (!dnrs_decoder_next_frame(
					decoder.get(), &owned.frame, &raw_error)) {
				dnrs_error.reset(raw_error);
				if (dnrs_error) {
					set_error(error, dnrs_message(dnrs_error.get(), codec));
					return nullptr;
				}
				break;
			}
			ImagePtr frame = load_frame(owned.frame, error);
			if (!frame)
				return nullptr;
			append_frame(frames, frames_tail, std::move(frame));
			if (ctx.first_frame_only)
				break;
		}
		if (!frames) {
			set_error(error, dnrs_wrap(codec, _("page has no frames")));
			return nullptr;
		}

		frames->orientation = page_info.orientation <= 8
			? Orientation(page_info.orientation)
			: Orientation::Unknown;
		frames->loops = page_info.loop_count;
		copy_blob(frames->icc, document.icc);
		copy_blob(frames->exif, document.exif);
		copy_blob(frames->xmp, document.xmp);
		for (size_t i = 0; i < document.text_length; i++) {
			const dnrs_text &entry = document.text[i];
			if (entry.key && entry.value)
				frames->text.emplace(entry.key, entry.value);
		}
		ensure_working_premul_pages(
			*frames, ctx, nullptr, /*input_premul=*/false);
		append_page(pages, pages_tail, std::move(frames));
		if (ctx.first_frame_only)
			break;
	}
	if (!pages)
		set_error(error, dnrs_wrap(codec, _("image has no pages")));
	return pages;
}

}  // namespace dawn
