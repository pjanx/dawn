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

/// Turn a vector backend's floating-point render size into pixel dimensions:
/// both are rounded up, and non-finite, non-positive, or over-large sizes are
/// rejected with the usual error, leaving the outputs untouched. Allocates
/// nothing--this is what runs before a backend commits to memory.
bool render_dimensions(double width, double height, uint32_t *out_width,
	uint32_t *out_height, Error *error);

/// Inflate a raw DEFLATE stream into an exactly sized buffer.  Wuffs is only
/// implemented in load-wuffs.cpp, so ZIP-based loaders borrow it from there.
bool inflate_raw(std::span<const uint8_t> src, std::span<uint8_t> dst);

/// Strip the four-byte big-endian offset to the TIFF header that ISO base
/// media containers (HEIF, JPEG XL) put in front of their Exif payloads, and
/// which the Exif parser would otherwise read as a byte order mark.
/// Returns nothing when the offset does not fit the payload.
std::vector<uint8_t> iso_exif_payload(std::span<const uint8_t> payload);

// --- Gain maps ---------------------------------------------------------------

/// Values of an XMP property named by namespace URI and local name: as an
/// attribute, then as an element holding text or rdf:li items.
/// Not an XML parser, just enough for what cameras and encoders write.
std::vector<std::string> xmp_values(
	std::string_view xmp, std::string_view ns, std::string_view name);
bool xmp_declares(std::string_view xmp, std::string_view ns);

/// ISO 21496-1 GainMapMetadata, as JPEG APP2 carries it past its namespace.
/// False when malformed, or when this is not what GainMap can represent:
/// channel-dependent values, unequal offsets.  Only the metadata is filled.
bool parse_iso_gain_map(std::span<const uint8_t> blob, GainMap *map);
/// Ultra HDR `hdrgm` XMP metadata, likewise.
bool parse_hdrgm_gain_map(std::string_view xmp, GainMap *map);
/// The payload of a HEIF `tmap` item, likewise.
bool parse_tmap_gain_map(std::span<const uint8_t> item, GainMap *map);
/// The ISO 21496-1 metadata and the map's naked codestream in a JPEG XL
/// `jhgm` box.  libjxl declares a reader, but does not export it.
bool split_jhgm_bundle(std::span<const uint8_t> box,
	std::span<const uint8_t> *metadata, std::span<const uint8_t> *codestream);

/// Whether XMP uses Apple's HDRGainMap namespace, as its maps do.
bool apple_gain_map_declared(std::string_view xmp);
/// Linear headroom of Apple's pre-ISO map, from the map's own XMP, or from
/// the maker note in the primary image's Exif.  Zero when unusable.
double apple_gain_map_headroom(
	std::string_view map_xmp, std::span<const uint8_t> exif);
/// The same, with maker note tags 33 and 48 already read out, as NaN and 0
/// when missing.
double apple_gain_map_headroom_from_maker(
	std::string_view map_xmp, double maker33, double maker48);
/// The metadata an Apple map is normalized to, see make_gain_map().
GainMap apple_gain_map(double headroom);

/// False for maps with no HDR effect, and those over an HDR base, which
/// also warn.  Metadata that fails this needs no decoding.
bool gain_map_applies(const GainMap &metadata, const OpenContext &ctx);
/// One channel of the decoded map with the metadata, or null when its
/// channels differ.  Apple's texels are rewritten to log2 gain.
std::unique_ptr<GainMap> make_gain_map(
	const Image &pixels, const GainMap &metadata, bool apple);

/// Bring a map into its base's frame, where a container transforms that.
/// Cropping fails, leaving the map alone, when the rectangle does not fit.
bool crop_gain_map(
	GainMap &map, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/// Counter-clockwise, by a multiple of 90 degrees.
void rotate_gain_map(GainMap &map, int ccw);
/// Left to right when `horizontally`, otherwise top to bottom.
void mirror_gain_map(GainMap &map, bool horizontally);

// --- True HDR ----------------------------------------------------------------

/// The headroom split_hdr() caps its HDR rendition at: a 1000 cd/m² alternate
/// at 203 cd/m² reference white (BT.2408).
inline constexpr double kSplitHeadroom = 1000. / 203.;

/// Rolls linear light in [0, peak] onto [0, target], the identity below a knee,
/// and everywhere when `peak <= target`.  This is BT.2390's EETF, knee and
/// Hermite spline, but over log2 of linear light rather than over PQ, so that
/// the peak has no upper bound.
class HdrRolloff
{
	double knee_ = 0;       ///< log2, where the roll-off starts
	double span_ = 0;       ///< log2, from the knee to the peak
	double threshold_ = 0;  ///< Linear, the knee; infinite for the identity

public:
	HdrRolloff(double peak, double target);
	double apply(double x) const;
};

/// H.273 Table 2 primaries as CIE 1931 xy, in R,G,B order, plus the
/// illuminant.  False for reserved, unspecified, or non-RGB code points.
bool cicp_primaries(uint8_t code, double primaries[6], double whitepoint[2]);

/// Splits linear light, 1.0 at SDR reference white, into an SDR base and,
/// when `ctx.gain_maps` is set and anything exceeds 1.0, a gain map to an
/// alternate capped at kSplitHeadroom.  `rgba` holds four values per pixel of
/// `image`, in CIE 1931 xy `primaries` with a D65 white, alpha associated when
/// `premultiplied`, and serves as scratch.  Visible negative values widen the
/// primaries first, see widen_negative().  Non-finite values, and negative
/// ones past that, warn, and become zero.  The base goes into `image.data`
/// straight, encoded with the sRGB EOTF in those primaries, which
/// `image.effective_profile` then describes: finish_image() with that as its
/// source completes it.
bool split_hdr(Image &image, const OpenContext &ctx, std::span<float> rgba,
	bool premultiplied, const double primaries[6], Error *error);

/// Where a visible pixel of straight `rgba` has a negative channel, converts
/// all of it from `primaries` to BT.2020's, which take in most of what goes
/// negative, and returns those.  Otherwise returns `primaries`, and changes
/// nothing.
const double *widen_negative(std::span<float> rgba, const double primaries[6]);

/// Whether H.273 code points say PQ or HLG, in primaries that split_hdr()
/// takes: those of a D65 white, which go into `primaries`.
bool cicp_hdr(uint8_t primaries_code, uint8_t transfer, double primaries[6]);

/// Splits a decoded BT.2100 signal, PQ (16) or HLG (18), in `image.data`,
/// as split_hdr() does.  PQ puts 1.0 at 203 cd/m² (BT.2408).  HLG gets the
/// OOTF of a display with a peak of `hlg_peak` cd/m², and 1.0 goes to its
/// reference white, 75 % signal, which comes to 203 cd/m² at 1000.
bool split_hdr_signal(Image &image, const OpenContext &ctx, uint8_t transfer,
	const double primaries[6], double hlg_peak, bool premultiplied,
	Error *error);

/// MIME types compiled into the in-tree Rust decoder.
std::vector<std::string> dnrs_media_types();

/// MIME types the system ImageIO can load (if its loader is built).
std::vector<std::string> imageio_media_types();

/// SOF width×height product for picking among embedded JPEG previews; 0 if
/// no SOF is found. Does not validate the rest of the bitstream.
int64_t jpeg_sof_pixel_count(std::span<const uint8_t> data);

}  // namespace dawn
