//
// libdn.h: image loading and colour management
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace dawn
{

/// CPU working pixmap and GPU sampling format: Wuffs BGRA_PREMUL_4X16LE —
/// little-endian uint16 channels B,G,R,A, premultiplied. On Vulkan upload as
/// `R16G16B16A16_UNORM` with a BGRA component swizzle (there is no native
/// `B16G16R16A16`); same idea on other APIs. No 8-bit quantize required.
inline constexpr uint32_t kBytesPerPixel = 8;

/// Maximum width or height of a loaded / rendered pixmap (inclusive).
inline constexpr uint32_t kMaxDimension = 65535;

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
//   do not mutate (finish/ensure/blend/render) from multiple threads.

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

struct Error {
	enum class Code {
		Ok = 0,
		Open,
		Io,
	};
	Code code = Code::Ok;
	std::string message;

	explicit
	operator bool() const
	{
		return code != Code::Ok;
	}
};

class Cmm;
class Profile;

/// Working-buffer TRC the scale shaders can apply. Unmatched ICC curves
/// fall back to Srgb (`profile_transfer`).
enum class Transfer : int32_t {
	Linear = 0,
	Srgb = 1,
	AdobeRgb = 2,
};

/// Exact match of the profile TRC to Linear / sRGB / gamma 2.2. Null, missing
/// tags, mixed channels, or any other curve → Srgb.
Transfer profile_transfer(const Profile *profile);
float transfer_decode(float encoded, Transfer transfer);
float transfer_encode(float linear, Transfer transfer);

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

Chromaticities profile_chromaticities(const Profile *profile);

/// Accumulated CPU milliseconds for one `open()` / `open_from_data()`.
/// Zeroed by the caller; filled when `OpenContext::timing` is set.
struct OpenTiming {
	double file_ms = 0;
	double decode_ms = 0;
	double alloc_ms = 0;
	double cms_ms = 0;
	double widen_ms = 0;
};

struct Image;
using ImagePtr = std::shared_ptr<Image>;

/// Parametric re-render for vector formats (attached at page level).
struct RenderClosure {
	virtual ~RenderClosure() = default;
	virtual ImagePtr render(Cmm *cmm, Profile *target, double scale) = 0;
};

struct Image {
	/// Working pixels (see kBytesPerPixel). After successful open/finish:
	/// BGRA_PREMUL_4X16LE.
	std::vector<uint8_t> data;
	uint32_t width = 0;
	uint32_t stride = 0;  ///< Bytes per row (width * kBytesPerPixel).
	uint32_t height = 0;

	Orientation orientation = Orientation::Unknown;

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

inline uint16_t *
row_u16(Image &img, uint32_t y)
{
	return (uint16_t *) row_bytes(img, y);
}

inline const uint16_t *
row_u16(const Image &img, uint32_t y)
{
	return (const uint16_t *) row_bytes(img, y);
}

/// Allocate a zeroed working-format image. Returns null on OOM / overflow.
ImagePtr image_new(uint32_t width, uint32_t height);

class Profile
{
	friend class Cmm;
	friend Transfer profile_transfer(const Profile *profile);
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
bool profiles_equal(const Profile *a, const Profile *b);

class Cmm : public std::enable_shared_from_this<Cmm>
{
	friend class Profile;
	void *context_ = nullptr;  ///< cmsContext
	bool broken_premul_ = false;

	std::shared_ptr<Profile> cached_sRGB;
	std::shared_ptr<Profile> cached_display_p3;

public:
	Cmm();
	~Cmm();
	Cmm(const Cmm &) = delete;
	Cmm &operator=(const Cmm &) = delete;

	static std::shared_ptr<Cmm> get_default();

	std::shared_ptr<Profile> get_profile(const void *data, size_t len);
	std::shared_ptr<Profile> get_profile(std::span<const uint8_t> bytes);
	std::shared_ptr<Profile> get_profile_sRGB(bool cache = false);
	std::shared_ptr<Profile> get_profile_display_p3(bool cache = false);
	std::shared_ptr<Profile> get_profile_sRGB_gamma(double gamma);
	std::shared_ptr<Profile> get_profile_parametric(
		double gamma, double whitepoint[2], double primaries[6]);

	/// Synthesizes a profile from ITU-T H.273 coded values (as carried by
	/// AVIF/HEIF nclx, and by glycin). Null for code points we do not model,
	/// including PQ (16) and HLG (18): both are HDR curves with no ICC v2
	/// parametric equivalent, and approximating them would shift tone badly.
	/// `matrix_coefficients` and range are deliberately not taken -- they
	/// describe a YCbCr encoding, already undone by the time we see RGB.
	std::shared_ptr<Profile> get_profile_cicp(
		uint8_t color_primaries, uint8_t transfer_characteristics);

	/// CMYK8 (inverted) → working-format image (opaque premul).
	void convert_cmyk8(
		Image &dst, const uint8_t *cmyk, Profile *source, Profile *target);

	/// In-place colour transform on BGRA16 buffers.
	bool transform_bgra16(uint8_t *data, uint32_t width, uint32_t height,
		Profile *source, Profile *target, bool source_premul,
		bool target_premul);

	/// BGRA8 (straight) → BGRA16 working buffer. `dst` is `width*height`
	/// packed BGRA16 pixels. Colour-manages when both profiles resolve.
	bool transform_bgra8_to_bgra16(const uint8_t *src, uint8_t *dst,
		uint32_t width, uint32_t height, Profile *source, Profile *target,
		bool target_premul);

	/// Expects straight (non-premultiplied) BGRA16. Colour-manages when
	/// `target` is set, then guarantees premul output.
	void finish_premultiply(Image &image, Profile *source, Profile *target);

	void finish_page(Image &page, Profile *target);
	ImagePtr finish(ImagePtr image, Profile *target);

	bool
	broken_premul() const
	{
		return broken_premul_;
	}
	void *
	context()
	{
		return context_;
	}
};

struct OpenContext {
	std::string uri;
	std::shared_ptr<Cmm> cmm;
	std::shared_ptr<Profile> screen_profile;
	int screen_dpi = 96;
	bool enhance = false;
	bool first_frame_only = false;
	std::vector<std::string> *warnings = nullptr;
	OpenTiming *timing = nullptr;
};

ImagePtr open(const OpenContext &ctx, Error *error);
ImagePtr open_from_data(
	std::span<const uint8_t> data, const OpenContext &ctx, Error *error);

/// MIME types this build can load: base codecs, optional libraries, and
/// whatever gdk-pixbuf modules are installed. Order is stable.
std::vector<std::string> supported_media_types();

void orientation_dimensions(
	const Image &image, Orientation orientation, double *width, double *height);
Matrix orientation_matrix(Orientation orientation, double width, double height);
Matrix orientation_apply(
	const Image &image, Orientation orientation, double *width, double *height);
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

std::shared_ptr<Cmm> cmm_or_default(const OpenContext &ctx);

/// Bring an image to final working premul. If `source` is null and
/// `image.icc` is non-empty, loads that profile. If `input_premul` and there
/// is no screen profile, leaves pixels alone. If `input_premul` and CMS is
/// needed, un-premultiplies first. Otherwise expects straight BGRA16.
void ensure_working_premul(
	Image &image, const OpenContext &ctx, Profile *source, bool input_premul);
void ensure_working_premul_pages(
	Image &page, const OpenContext &ctx, Profile *source, bool input_premul);

void premultiply_bgra16(Image &image);
void unpremultiply_bgra16(Image &image);
void unpremultiply_bgra8(
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
std::string uri_to_path(const std::string &uri);

}  // namespace dawn
