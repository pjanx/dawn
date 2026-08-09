//
// encode-webp.cpp: WebP thumbnail encoding
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "encode-webp.hpp"

#include <webp/encode.h>

#include <string>
#include <vector>

using namespace std;

namespace dnthumbd {

bool encode_webp_rgba8(uint32_t w, uint32_t h, const uint8_t *rgba8,
	vector<uint8_t> *out, string *error)
{
	if (!out || !rgba8 || w == 0 || h == 0) {
		if (error)
			*error = "invalid encode_webp_rgba8 arguments";
		return false;
	}
	out->clear();

	WebPConfig config;
	if (!WebPConfigInit(&config) || !WebPConfigLosslessPreset(&config, 6)) {
		if (error)
			*error = "WebPConfigInit/LosslessPreset failed";
		return false;
	}
	config.near_lossless = 95;
	config.thread_level = 1;
	if (!WebPValidateConfig(&config)) {
		if (error)
			*error = "WebPValidateConfig failed";
		return false;
	}

	WebPPicture picture;
	if (!WebPPictureInit(&picture)) {
		if (error)
			*error = "WebPPictureInit failed";
		return false;
	}
	picture.use_argb = 1;
	picture.width = int(w);
	picture.height = int(h);
	if (!WebPPictureAlloc(&picture)) {
		if (error)
			*error = "WebPPictureAlloc failed";
		return false;
	}

	for (uint32_t y = 0; y < h; y++) {
		uint32_t *dst = picture.argb + size_t(y) * picture.argb_stride;
		const uint8_t *src = rgba8 + size_t(y) * w * 4;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = src + x * 4;
			dst[x] = (uint32_t(p[3]) << 24) | (uint32_t(p[0]) << 16) |
				 (uint32_t(p[1]) << 8) | uint32_t(p[2]);
		}
	}

	WebPMemoryWriter writer;
	WebPMemoryWriterInit(&writer);
	picture.writer = WebPMemoryWrite;
	picture.custom_ptr = &writer;

	if (!WebPEncode(&config, &picture)) {
		if (error)
			*error = string("WebPEncode failed (code ") +
				 to_string(int(picture.error_code)) + ")";
		WebPPictureFree(&picture);
		WebPMemoryWriterClear(&writer);
		return false;
	}

	try {
		out->assign(writer.mem, writer.mem + writer.size);
	} catch (const bad_alloc &) {
		WebPMemoryWriterClear(&writer);
		WebPPictureFree(&picture);
		if (error)
			*error = "out of memory";
		return false;
	}

	WebPMemoryWriterClear(&writer);
	WebPPictureFree(&picture);
	return true;
}

} // namespace dnthumbd
