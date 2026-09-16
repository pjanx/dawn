//
// libdn-loaders.hpp: internal loader entry points
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.hpp"

#include <chrono>

namespace dawn
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
LoadFn load_ora;
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
LoadFn load_poppler;

/// Inflate a raw DEFLATE stream into an exactly sized buffer.  Wuffs is only
/// implemented in load-wuffs.cpp, so ZIP-based loaders borrow it from there.
bool inflate_raw(std::span<const uint8_t> src, std::span<uint8_t> dst);

/// Strip the four-byte big-endian offset to the TIFF header that ISO base
/// media containers (HEIF, JPEG XL) put in front of their Exif payloads, and
/// which the Exif parser would otherwise read as a byte order mark.
/// Returns nothing when the offset does not fit the payload.
std::vector<uint8_t> iso_exif_payload(std::span<const uint8_t> payload);

/// MIME types compiled into the in-tree Rust decoder.
std::vector<std::string> dnrs_media_types();

/// MIME types the system ImageIO can load (if its loader is built).
std::vector<std::string> imageio_media_types();

/// SOF width×height product for picking among embedded JPEG previews; 0 if
/// no SOF is found. Does not validate the rest of the bitstream.
int64_t jpeg_sof_pixel_count(std::span<const uint8_t> data);

}  // namespace dawn
