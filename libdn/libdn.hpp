//
// libdn.hpp: image loading and colour management
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace dawn
{

// Thread safety (summary):
// - Concurrent open()/open_from_data() of different files is OK for most
//   codecs. TIFF loads are serialized internally (libtiff global handlers).
// - Prefer one Cmm per thread, or serialize all use of a shared Cmm (including
//   Cmm::get_default() / cmm_or_default). Profiles must stay with their Cmm.
//   Large cmsDoTransform work is parallelized by lcms2's threaded plugin when
//   that is built (one transform; the plugin slices). Otherwise libdn splits
//   full-width row bands across workers, each with its own cmsHTRANSFORM
//   created on the Cmm thread.
// - Do not share OpenContext::warnings or Error* across concurrent opens.
// - After load, Image is single-writer: read-only pixel sharing is fine;
//   do not mutate (finish/blend/render) from multiple threads.

struct Error {
	enum class Code {
		Ok = 0,
		Open,
		Io,
	};
	Code code = Code::Ok;
	std::string message;

	explicit operator bool() const { return code != Code::Ok; }
};

// --- Configuration -----------------------------------------------------------

namespace ini
{

struct Group {
	std::string name;
	std::vector<std::pair<std::string, std::string>> keys;
};

struct File {
	std::vector<std::string> preamble;
	std::vector<Group> groups;
};

File parse(std::string_view text);
std::string serialize(const File &ini);
std::string get(const Group &group, std::string_view key);
void set(Group &group, std::string_view key, std::string_view value);
std::string desktop_unescape(std::string_view value);
std::string desktop_escape(std::string_view value);

}  // namespace ini

/// Read and write an opaque application configuration value.
/// A missing value is not an error and returns std::nullopt.
std::optional<std::string> config_get(std::string_view key, Error *error);
bool config_set(std::string_view key, std::string_view value, Error *error);

// --- Colour management -------------------------------------------------------

/// Working-buffer TRC the scale shaders can apply. Unmatched ICC curves
/// fall back to Srgb (`profile_transfer`).
enum class Transfer : int32_t {
	Linear = 0,
	Srgb = 1,
	AdobeRgb = 2,
};

enum class ColorModel : uint8_t { Unknown, Rgb, Cmyk, Gray };

/// CIE 1931 xy (D65) from an ICC profile. Corners are sampled through lcms
/// to PCS XYZ, then Bradford-adapted D50→D65. CMYK uses the six ink corners.
struct Chromaticities {
	ColorModel model = ColorModel::Unknown;
	bool have_white = false;
	bool have_primaries = false;
	double wx = 0;
	double wy = 0;
	int n = 0;
	double x[6] = {};
	double y[6] = {};
};

/// Matrix/TRC RGB profiles keep their display primaries. Other profiles use
/// the legacy sRGB approximation in device RGB; this is not a colourimetric
/// working space for arbitrary LUT profiles.
struct ProfileEncoding {
	bool matrix_trc = false;
	/// Columns are the display RGB colourants in PCS (D50) XYZ.
	std::array<std::array<double, 3>, 3> rgb_to_xyz{};
	/// Uniform samples on [0, 1], indexed [sample][RGB channel].
	static constexpr size_t kSamples = 4097;
	std::array<std::array<float, 3>, kSamples> decode{};
	std::array<std::array<float, 3>, kSamples> encode{};
};

class Cmm;

class Profile
{
	friend class Cmm;
	friend Transfer profile_transfer(const Profile *profile);
	friend ProfileEncoding profile_encoding(const Profile *profile);
	friend Chromaticities profile_chromaticities(const Profile *profile);
	std::shared_ptr<Cmm> cmm_;
	void *profile_ = nullptr;  ///< cmsHPROFILE
	Profile(std::shared_ptr<Cmm> cmm, void *cms_profile);

public:
	~Profile();
	Profile(const Profile &) = delete;
	Profile &operator=(const Profile &) = delete;

	std::vector<uint8_t> to_bytes() const;
};

/// Serialized ICC equality. Null equals null; lcms has no compare API.
/// The header's creation date/time is excluded.
bool profiles_equal(const Profile *a, const Profile *b);

struct Image;
using ImagePtr = std::shared_ptr<Image>;

class Cmm : public std::enable_shared_from_this<Cmm>
{
	friend class Profile;
	void *context_ = nullptr;  ///< cmsContext
	bool broken_premul_ = false;

	// Weak, because a Profile owns its Cmm: we don't want a cycle.
	// Deduplicating profiles for as long as somebody else wants them.
	std::weak_ptr<Profile> cached_sRGB;
	std::weak_ptr<Profile> cached_display_p3;

public:
	Cmm();
	~Cmm();
	Cmm(const Cmm &) = delete;
	Cmm &operator=(const Cmm &) = delete;

	static std::shared_ptr<Cmm> get_default();

	std::shared_ptr<Profile> get_profile_data(const void *data, size_t len);
	std::shared_ptr<Profile> get_profile(std::span<const uint8_t> bytes);
	std::shared_ptr<Profile> get_profile_sRGB();
	std::shared_ptr<Profile> get_profile_display_p3();
	std::shared_ptr<Profile> get_profile_sRGB_gamma(double gamma);

	/// Builds a matrix/TRC profile from CIE 1931 xy chromaticities.
	/// `gamma` is the decoding exponent; without one, the curve is the
	/// sRGB EOTF, which is what PNG cHRM without gAMA asks for.
	std::shared_ptr<Profile> get_profile_parametric(std::optional<double> gamma,
		const double whitepoint[2], const double primaries[6]);

	/// An RGB profile from CIE 1931 xy chromaticities and tabulated tone
	/// curves, as TIFF's TransferFunction stores them--equally long runs of
	/// 16-bit samples mapping encoded values to linear intensity.
	std::shared_ptr<Profile> get_profile_tabulated(const double whitepoint[2],
		const double primaries[6], std::span<const uint16_t> curves[3]);

	/// Synthesizes a profile from ITU-T H.273 coded values (as carried by
	/// AVIF/HEIF nclx). Null for code points we do not model,
	/// including PQ (16) and HLG (18): both are HDR curves with no ICC v2
	/// parametric equivalent, and approximating them would shift tone badly.
	/// `matrix_coefficients` and range are deliberately not taken -- they
	/// describe a YCbCr encoding, already undone by the time we see RGB.
	std::shared_ptr<Profile> get_profile_cicp(
		uint8_t color_primaries, uint8_t transfer_characteristics);

	/// CMYK8 (inverted) → working-format BGRA16 (opaque premul).
	/// Both buffers are tightly packed `width * height` pixels.
	void convert_cmyk8(const uint8_t *src, uint8_t *dst, uint32_t width,
		uint32_t height, Profile *source, Profile *target);

	/// In-place colour transform on BGRA16 buffers.
	bool transform_bgra16(uint8_t *data, uint32_t width, uint32_t height,
		Profile *source, Profile *target, bool source_premul,
		bool target_premul);

	/// BGRA8 (straight) → BGRA16 working buffer. `dst` is `width*height`
	/// packed BGRA16 pixels. Colour-manages when both profiles resolve.
	bool transform_bgra8_to_bgra16(const uint8_t *src, uint8_t *dst,
		uint32_t width, uint32_t height, Profile *source, Profile *target,
		bool target_premul);

	bool broken_premul() const { return broken_premul_; }
	void *context() { return context_; }
};

/// Exact match of the profile TRC to Linear / sRGB / gamma 2.2. Null, missing
/// tags, mixed channels, or any other curve → Srgb.
Transfer profile_transfer(const Profile *profile);
ProfileEncoding profile_encoding(const Profile *profile);
/// Linearly sample at least two uniformly spaced RGB curve entries.
std::array<float, 3> sample_curves(
	std::span<const std::array<float, 3>> curves, std::array<float, 3> rgb);
float transfer_decode(float encoded, Transfer transfer);
float transfer_encode(float linear, Transfer transfer);

Chromaticities profile_chromaticities(const Profile *profile);

// --- Orientation -------------------------------------------------------------

// https://www.cipa.jp/std/documents/e/DC-008-2012_E.pdf Table 6
enum class Orientation : int {
	Unknown = 0,
	Rotate0 = 1,
	Mirror0 = 2,
	Rotate180 = 3,
	Mirror180 = 4,
	Mirror270 = 5,
	Rotate90 = 6,
	Mirror90 = 7,
	Rotate270 = 8,
};

/// 2D affine matrix (column-vector style: x' = xx*x + xy*y + x0).
struct Matrix {
	double xx = 1, yx = 0, xy = 0, yy = 1, x0 = 0, y0 = 0;
};

Matrix orientation_matrix(Orientation orientation, double width, double height);
Orientation exif_orientation(std::span<const uint8_t> exif);

[[nodiscard]] Orientation orientation_or_0(Orientation orientation);
void orientation_display_size(uint32_t src_w, uint32_t src_h,
	Orientation orientation, uint32_t *width, uint32_t *height);
[[nodiscard]] Orientation orientation_rotate_left(Orientation orientation);
[[nodiscard]] Orientation orientation_rotate_right(Orientation orientation);
[[nodiscard]] Orientation orientation_mirror(Orientation orientation);
void orientation_map_display_to_source(Orientation orientation, uint32_t src_w,
	uint32_t src_h, double dx, double dy, double *sx, double *sy);
void orientation_map_source_to_display(Orientation orientation, uint32_t src_w,
	uint32_t src_h, double sx, double sy, double *dx, double *dy);

// --- Image -------------------------------------------------------------------

/// CPU working pixmap and GPU sampling format: Wuffs BGRA_PREMUL_4X16LE —
/// little-endian uint16 channels B,G,R,A, premultiplied. On Vulkan upload as
/// `R16G16B16A16_UNORM` with a BGRA component swizzle (there is no native
/// `B16G16R16A16`); same idea on other APIs. No 8-bit quantize required.
inline constexpr uint32_t kBytesPerPixel = 8;

/// Maximum width or height of a loaded / rendered pixmap (inclusive).
inline constexpr uint32_t kMaxDimension = 65535;

struct OpenContext;

/// Parametric re-render for vector formats (attached at page level).
struct RenderClosure {
	virtual ~RenderClosure() = default;

	/// Rasterize anew at `scale`, finishing the pixels as `ctx` asks for.
	/// The context is only borrowed for the call.
	virtual ImagePtr render(
		const OpenContext &ctx, double scale, Error *error) = 0;
};

struct Image {
	/// Working pixels (see kBytesPerPixel). After successful open/finish:
	/// BGRA_PREMUL_4X16LE.
	std::vector<uint8_t> data;
	uint32_t width = 0;
	uint32_t stride = 0;  ///< Bytes per row (width * kBytesPerPixel).
	uint32_t height = 0;

	Orientation orientation = Orientation::Unknown;

	const char *loader = nullptr;
	std::vector<uint8_t> exif;
	std::vector<uint8_t> icc;
	std::vector<uint8_t> xmp;
	std::vector<uint8_t> thum;
	std::unordered_map<std::string, std::string> text;

	/// Source profile actually used (or assumed sRGB). `icc` stays the file
	/// blob. Null for CMYK with no profile.
	std::shared_ptr<Profile> effective_profile;
	/// True only when `effective_profile` is invented sRGB
	/// (no ICC / Exif / gAMA).
	bool profile_assumed = false;

	std::unique_ptr<RenderClosure> render;

	ImagePtr page_next;
	std::weak_ptr<Image> page_previous;
	ImagePtr frame_next;
	std::weak_ptr<Image> frame_previous;

	int64_t frame_duration = 0;  ///< Milliseconds.
	uint64_t loops = 0;          ///< Zero means infinite.

	/// Heuristic: this animation comes from a format web browsers play,
	/// and is thus a candidate for their short-delay adjustment.
	bool browser_animation_bump = false;
};

/// Row accessors — `stride` is always in bytes.
inline uint8_t *
row_bytes(Image &img, uint32_t y)
{
	return img.data.data() + size_t(y) * img.stride;
}

inline const uint8_t *
row_bytes(const Image &img, uint32_t y)
{
	return img.data.data() + size_t(y) * img.stride;
}

template <typename T, typename U>
inline T *
assume_aligned(U *p)
{
#if defined __GNUC__ || defined __clang__
	return reinterpret_cast<T *>(__builtin_assume_aligned(p, alignof(T)));
#else
	return reinterpret_cast<T *>(p);
#endif
}

inline uint16_t *
row_u16(Image &img, uint32_t y)
{
	return assume_aligned<uint16_t>(row_bytes(img, y));
}

inline const uint16_t *
row_u16(const Image &img, uint32_t y)
{
	return assume_aligned<const uint16_t>(row_bytes(img, y));
}

/// Allocate a zeroed working-format image. Returns null on OOM / overflow.
ImagePtr image_new(uint32_t width, uint32_t height);

// --- Opening -----------------------------------------------------------------

/// Accumulated CPU milliseconds for one `open()` / `open_from_data()`.
/// Zeroed by the caller; filled when `OpenContext::timing` is set.
struct OpenTiming {
	double file_ms = 0;
	double decode_ms = 0;
	double alloc_ms = 0;
	double cms_ms = 0;
	double widen_ms = 0;
};

struct OpenContext {
	std::string uri;
	std::shared_ptr<Cmm> cmm;
	std::shared_ptr<Profile> screen_profile;
	int screen_dpi = 96;
	bool enhance = false;
	bool first_frame_only = false;
	/// Loaders to try, by name, in this order; empty means all of them,
	/// in the default order. Names this build lacks are skipped.
	std::span<const std::string> loaders;
	std::vector<std::string> *warnings = nullptr;
	OpenTiming *timing = nullptr;
};

ImagePtr open(const OpenContext &ctx, Error *error);
ImagePtr open_from_data(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);

// --- Saving ------------------------------------------------------------------

/// Lossless WebP.  A null `frame` saves the whole page, animating when it has
/// more than one; otherwise just that frame.  `icc` overrides the page's own
/// profile, for when colour management has already transformed the pixels.
/// The working format's 16-bit samples are narrowed to the 8 bits WebP keeps.
bool save_webp(const Image &page, const Image *frame,
	std::span<const uint8_t> icc, std::vector<uint8_t> *out, Error *error);

/// Exif, ICC and XMP in an Exiv2-readable pseudo-JPEG.  Metadata that cannot
/// be expressed in JPEG marker segments is an error, never a truncation.
bool save_exv(const Image &page, std::vector<uint8_t> *out, Error *error);

// --- JPEG --------------------------------------------------------------------

/// Stored dimensions and the grid on which a lossless crop must start.
struct JpegGrid {
	uint32_t width = 0, height = 0;
	uint32_t mcu_width = 0, mcu_height = 0;
};
bool jpeg_grid(std::span<const uint8_t> data, JpegGrid *out, Error *error);

/// Turns stored pixels, then crops in the rotated image's MCU grid.  Zero width
/// or height means to the edge.  Rotations that cannot preserve every block
/// fail.  All markers are copied unchanged, including Exif orientation and
/// dimensions.  Returns empty on failure.
std::vector<uint8_t> jpeg_transform(std::span<const uint8_t> data,
	Orientation op, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
	Error *error);

// --- Loaders -----------------------------------------------------------------

using LoadFn = ImagePtr(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);

/// One image loader.  Loaders this build was configured without keep their
/// place, with a null `load`.
struct Loader {
	const char *name;  ///< As Image::loader names it.
	LoadFn *load;
	const char *formats;  ///< Human-readable, comma-separated; may be null.
	std::initializer_list<const char *> media_types;
	std::vector<std::string> (*dynamic_types)();
};

/// The loader table, in the default order it is tried in.
std::span<const Loader> loaders();

/// shared-mime-info types this build can load.  The order is stable.
std::vector<std::string> supported_media_types();

// --- Loader support ----------------------------------------------------------

inline constexpr uint32_t
fourcc(char a, char b, char c, char d)
{
	return uint32_t(uint8_t(a)) << 24 | uint32_t(uint8_t(b)) << 16 |
		uint32_t(uint8_t(c)) << 8 | uint32_t(uint8_t(d));
}

std::shared_ptr<Cmm> cmm_or_default(const OpenContext &ctx);

/// Bring decoded pixels to the final working format: premultiplied BGRA16,
/// colour-managed to `ctx.screen_profile` when there is one. Pixels are
/// expected straight (non-premultiplied), unless `input_premul`, in which
/// case they are only unpremultiplied when there is a target to convert them
/// to, and left alone when there is not. `source` describes what the pixels
/// are; when null, `image.icc` is loaded instead, and a missing or unusable
/// profile is assumed to be sRGB (`profile_assumed`).
void finish_image(
	Image &image, const OpenContext &ctx, Profile *source, bool input_premul);

/// finish_image() for a page and all its animation frames, which inherit
/// the page's source profile when they carry none of their own.
void finish_frames(
	Image &page, const OpenContext &ctx, Profile *source, bool input_premul);

void premultiply_bgra16(Image &image);
void unpremultiply_bgra16(Image &image);
/// In-place unpremultiplication of four-channel bytes with alpha last,
/// whatever the ordering of the three colour channels is. Rows are `stride`
/// bytes apart, and any padding beyond `width` pixels is left alone.
void unpremultiply_xxxa8(
	uint8_t *data, uint32_t width, uint32_t height, size_t stride);

/// Widen BGRA8 → straight-or-premul BGRA16 without changing association.
void widen_bgra8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride);

/// Pack interleaved R,G,B,A8 → BGRA16 (association unchanged).
void pack_rgba8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride);

/// Pack interleaved R,G,B8 → opaque BGRA16.
void pack_rgb8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride);

/// Pack host-endian 0xAARRGGBB words → BGRA16 (association unchanged).
void pack_argb32_words_to_bgra16(
	Image &dst, const uint32_t *src, size_t src_stride_bytes);

/// Pack little-endian R,G,B,A uint16 (values in 0..2^bits-1) → BGRA16.
void pack_rgba16le_to_bgra16(
	Image &dst, const uint16_t *src, size_t src_stride_bytes, int bits);

/// Pack little-endian R,G,B uint16 (values in 0..2^bits-1) → opaque BGRA16.
void pack_rgb16le_to_bgra16(
	Image &dst, const uint16_t *src, size_t src_stride_bytes, int bits);

/// Scale an n-bit sample into the full uint16 working range.
uint16_t scale_nbit_to_u16(uint32_t v, int bits);

/// Software compositing on working-format buffers (animation).
enum class BlendOp { Source, Over };
void fill_rect(Image &dst, int x, int y, int w, int h, uint16_t b, uint16_t g,
	uint16_t r, uint16_t a);
void blend_image(
	Image &dst, const Image &src, int dst_x, int dst_y, BlendOp op);

void append_page(ImagePtr &head, ImagePtr &tail, ImagePtr page);
void append_frame(ImagePtr &head, ImagePtr &tail, ImagePtr frame);

void add_warning(const OpenContext &ctx, const std::string &message);
void set_error(Error *error, std::string message);

bool read_file(
	const std::string &path, std::vector<uint8_t> *out, Error *error);

/// Convert a `file://` URI (RFC 8089) to a filesystem path.  Fails on any
/// other scheme, and on escapes that cannot be part of one.
std::optional<std::string> uri_to_path(const std::string &uri);
/// The inverse, for an absolute path.
std::string path_to_uri(const std::string &path);

#ifdef _WIN32
/// Widen UTF-8 for the Windows API.  Invalid UTF-8 gives an empty string,
/// as an empty input does; callers that must tell those apart check the input.
std::wstring utf8_to_wide(std::string_view utf8);
/// Narrow what the Windows API returns, with the same convention.
std::string wide_to_utf8(std::wstring_view wide);

/// The directory a module was loaded from, with its trailing backslash, or
/// the running executable's when null.  Empty if it cannot be retrieved.
/// The handle is an `HMODULE`, left opaque to spare this header <windows.h>.
std::wstring module_directory(void *module);
#endif

// --- TO BE MOVED TO DNTHUMBD -------------------------------------------------

/// Encode straight (non-premultiplied) RGBA8 as the near-lossless WebP the
/// wide-thumbnail cache stores. `stride` is bytes per row. Callers that need
/// a cache entry still have to wrap the result in its metadata chunk.
bool encode_thumbnail_webp(uint32_t width, uint32_t height,
	const uint8_t *rgba8, size_t stride, std::vector<uint8_t> *out,
	std::string *error);

}  // namespace dawn
