//
// libdn-loaders.h: internal loader entry points
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

#include <chrono>

// TODO(p): Why in Hell is this called detail, and what should it be called?
namespace dawn::detail
{

inline thread_local OpenTiming *open_timing = nullptr;

struct OpenTimingGuard {
	OpenTiming *prev;
	explicit OpenTimingGuard(OpenTiming *t) : prev(open_timing)
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

LoadFn load_wuffs;
LoadFn load_icns;
LoadFn load_psd;
LoadFn load_jpeg;
LoadFn load_webp;
LoadFn load_tiff_ep;
LoadFn load_libraw;
LoadFn load_libwmf;
LoadFn load_resvg;
LoadFn load_librsvg;
LoadFn load_xcursor;
LoadFn load_heif;
LoadFn load_jxl;
LoadFn load_openjpeg;
LoadFn load_tiff;
LoadFn load_jxr;
LoadFn load_dnrs;
LoadFn load_imageio;
LoadFn load_cgpdf;

/// MIME types compiled into the in-tree Rust decoder.
std::vector<std::string> dnrs_media_types();

/// MIME types the system ImageIO can load (if its loader is built).
std::vector<std::string> imageio_media_types();

/// SOF width×height product for picking among embedded JPEG previews; 0 if
/// no SOF is found. Does not validate the rest of the bitstream.
int64_t jpeg_sof_pixel_count(std::span<const uint8_t> data);

}  // namespace dawn::detail
