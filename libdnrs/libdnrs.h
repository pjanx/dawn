//
// libdnrs.h: image-rs and other decoders exposed through a small C ABI
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct dnrs_decoder;
struct dnrs_error;

struct dnrs_blob {
	const uint8_t *data;
	size_t length;
};

struct dnrs_text {
	const char *key;
	const char *value;
};

enum dnrs_pixel_format {
	DNRS_PIXEL_GRAY8,
	DNRS_PIXEL_GRAY_ALPHA8,
	DNRS_PIXEL_RGB8,
	DNRS_PIXEL_RGBA8,
	DNRS_PIXEL_GRAY16LE,
	DNRS_PIXEL_GRAY_ALPHA16LE,
	DNRS_PIXEL_RGB16LE,
	DNRS_PIXEL_RGBA16LE,
};

struct dnrs_document_info {
	const char *codec;
	uint32_t page_count;
	struct dnrs_blob icc;
	struct dnrs_blob exif;
	struct dnrs_blob xmp;
	const struct dnrs_text *text;
	size_t text_length;
};

struct dnrs_page_info {
	uint32_t width;
	uint32_t height;
	uint32_t frame_count;
	uint32_t orientation;
	uint64_t loop_count;
};

struct dnrs_frame {
	uint32_t width;
	uint32_t height;
	size_t stride;
	enum dnrs_pixel_format format;
	uint8_t *data;
	size_t length;
	uint64_t duration_ms;
};

const char *const *dnrs_mime_types(size_t *length);
struct dnrs_decoder *dnrs_decoder_new(const uint8_t *data, size_t length,
	bool first_frame_only, struct dnrs_error **error);
bool dnrs_decoder_get_info(const struct dnrs_decoder *decoder,
	struct dnrs_document_info *info, struct dnrs_error **error);
bool dnrs_decoder_next_page(struct dnrs_decoder *decoder,
	struct dnrs_page_info *page, struct dnrs_error **error);
bool dnrs_decoder_next_frame(struct dnrs_decoder *decoder,
	struct dnrs_frame *frame, struct dnrs_error **error);
const char *dnrs_error_message(const struct dnrs_error *error);
void dnrs_frame_clear(struct dnrs_frame *frame);
void dnrs_decoder_free(struct dnrs_decoder *decoder);
void dnrs_error_free(struct dnrs_error *error);

#ifdef __cplusplus
}
#endif
