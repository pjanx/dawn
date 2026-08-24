//
// libdn-loaders.h: internal loader entry points
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

#include <chrono>

namespace dn::detail
{

inline thread_local OpenTiming *open_timing = nullptr;

struct OpenTimingGuard {
	OpenTiming *prev;
	explicit OpenTimingGuard(OpenTiming *t)
		: prev(open_timing)
	{
		open_timing = t;
	}
	~OpenTimingGuard() { open_timing = prev; }
	OpenTimingGuard(const OpenTimingGuard &) = delete;
	OpenTimingGuard &operator=(const OpenTimingGuard &) = delete;
};

struct StageClock {
	double *acc;
	std::chrono::steady_clock::time_point t0{};
	explicit StageClock(double OpenTiming::*field);
	~StageClock();
	StageClock(const StageClock &) = delete;
	StageClock &operator=(const StageClock &) = delete;
};

uint32_t wuffs_guess_fourcc(std::span<const uint8_t> data);

ImagePtr load_wuffs(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_jpeg(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_webp(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_tiff_ep(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_libraw(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_resvg(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_librsvg(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_xcursor(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_heif(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_tiff(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);
ImagePtr load_gdkpixbuf(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);

/// MIME types provided by the installed gdk-pixbuf loaders; empty when
/// gdk-pixbuf support is not built.
std::vector<std::string> gdkpixbuf_media_types();

/// SOF width×height product for picking among embedded JPEG previews; 0 if
/// no SOF is found. Does not validate the rest of the bitstream.
int64_t jpeg_sof_pixel_count(std::span<const uint8_t> data);

}  // namespace dn::detail
