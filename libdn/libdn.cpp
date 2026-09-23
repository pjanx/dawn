//
// libdn.cpp: image loading and processing
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <locale>
#include <new>
#include <numbers>
#include <sstream>
#include <string_view>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define TIFF_TABLES_CONSTANTS_ONLY
#include "tiff-tables.h"
#if defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "tiffer.h"
#if defined __GNUC__
#pragma GCC diagnostic pop
#endif

using namespace std;

namespace dawn
{

StageClock::StageClock(double OpenTiming::*field)
	: acc(open_timing ? &(open_timing->*field) : nullptr)
{
	if (acc)
		t0 = chrono::steady_clock::now();
}

StageClock::~StageClock()
{
	if (acc) {
		*acc +=
			chrono::duration<double, milli>(chrono::steady_clock::now() - t0)
				.count();
	}
}

/// Scale an n-bit sample (1..16) into the full uint16 working range.
uint16_t
scale_nbit_to_u16(uint32_t v, int bits)
{
	if (bits >= 16)
		return uint16_t(min(v, 65535u));
	if (bits <= 0)
		return 0;
	uint32_t maxv = (1u << bits) - 1u;
	return uint16_t((v * 65535u + maxv / 2u) / maxv);
}

// Matches [x * a / 65535] with rounding.
static uint16_t
premultiply16(uint16_t a, uint16_t x)
{
	return uint16_t((uint32_t(x) * a + 32768u) / 65535u);
}

static uint16_t
unpremultiply16(uint16_t a, uint16_t x)
{
	if (a == 0)
		return 0;
	return uint16_t(min(65535u, (uint32_t(x) * 65535u + a / 2) / a));
}

static uint8_t
unpremultiply8(uint8_t a, uint8_t x)
{
	if (a == 0)
		return 0;
	return uint8_t(min(255u, (uint32_t(x) * 255u + a / 2) / a));
}

// --- Image -------------------------------------------------------------------

bool
render_dimensions(double width, double height, uint32_t *out_width,
	uint32_t *out_height, Error *error)
{
	// NaN fails the first comparison, infinity the second.
	double w = ceil(width), h = ceil(height);
	if (!(w >= 1) || !(h >= 1) || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, _("image dimensions overflow"));
		return false;
	}

	*out_width = uint32_t(w);
	*out_height = uint32_t(h);
	return true;
}

ImagePtr
image_new(uint32_t width, uint32_t height)
{
	if (width == 0 || height == 0)
		return nullptr;
	if (width > kMaxDimension || height > kMaxDimension)
		return nullptr;
	if (width > UINT32_MAX / kBytesPerPixel)
		return nullptr;

	uint32_t stride = width * kBytesPerPixel;
	if (height > SIZE_MAX / stride)
		return nullptr;

	auto image = make_shared<Image>();
	try {
		StageClock clk(&OpenTiming::alloc_ms);
		image->data.assign(size_t(stride) * height, 0);
	} catch (const bad_alloc &) {
		return nullptr;
	}
	image->width = width;
	image->stride = stride;
	image->height = height;
	return image;
}

void
append_page(ImagePtr &head, ImagePtr &tail, ImagePtr page)
{
	if (!page)
		return;

	if (head) {
		tail->page_next = page;
		page->page_previous = tail;
		tail = std::move(page);
	} else {
		head = tail = std::move(page);
	}
}

void
append_frame(ImagePtr &head, ImagePtr &tail, ImagePtr frame)
{
	if (!frame)
		return;

	if (head) {
		tail->frame_next = frame;
		frame->frame_previous = tail;
		tail = std::move(frame);
	} else {
		head = tail = std::move(frame);
	}
}

void
add_warning(const OpenContext &ctx, const string &message)
{
	if (ctx.warnings)
		ctx.warnings->push_back(message);
}

void
set_error(Error *error, string message)
{
	if (!error)
		return;

	error->code = Error::Code::Open;
	error->message = std::move(message);
}

shared_ptr<Cmm>
cmm_or_default(const OpenContext &ctx)
{
	return ctx.cmm ? ctx.cmm : Cmm::get_default();
}

void
widen_bgra8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride)
{
	StageClock clk(&OpenTiming::widen_ms);
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint8_t *s = src + y * src_stride;
		for (uint32_t x = 0; x < dst.width; x++) {
			d[0] = uint16_t(s[0] * 257u);
			d[1] = uint16_t(s[1] * 257u);
			d[2] = uint16_t(s[2] * 257u);
			d[3] = uint16_t(s[3] * 257u);
			d += 4;
			s += 4;
		}
	}
}

void
pack_rgba8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride)
{
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint8_t *s = src + y * src_stride;
		for (uint32_t x = 0; x < dst.width; x++) {
			d[0] = uint16_t(s[2] * 257u);
			d[1] = uint16_t(s[1] * 257u);
			d[2] = uint16_t(s[0] * 257u);
			d[3] = uint16_t(s[3] * 257u);
			d += 4;
			s += 4;
		}
	}
}

void
pack_rgb8_to_bgra16(Image &dst, const uint8_t *src, size_t src_stride)
{
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint8_t *s = src + y * src_stride;
		for (uint32_t x = 0; x < dst.width; x++) {
			d[0] = uint16_t(s[2] * 257u);
			d[1] = uint16_t(s[1] * 257u);
			d[2] = uint16_t(s[0] * 257u);
			d[3] = 65535;
			d += 4;
			s += 3;
		}
	}
}

void
pack_argb32_words_to_bgra16(
	Image &dst, const uint32_t *src, size_t src_stride_bytes)
{
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint32_t *s = assume_aligned<const uint32_t>(
			(const uint8_t *) src + y * src_stride_bytes);
		for (uint32_t x = 0; x < dst.width; x++) {
			uint32_t p = s[x];
			d[0] = uint16_t((p & 0xFFu) * 257u);
			d[1] = uint16_t(((p >> 8) & 0xFFu) * 257u);
			d[2] = uint16_t(((p >> 16) & 0xFFu) * 257u);
			d[3] = uint16_t(((p >> 24) & 0xFFu) * 257u);
			d += 4;
		}
	}
}

void
pack_rgba16le_to_bgra16(
	Image &dst, const uint16_t *src, size_t src_stride_bytes, int bits)
{
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint16_t *s = assume_aligned<const uint16_t>(
			(const uint8_t *) src + y * src_stride_bytes);

		// Full-range samples pass through.  Deciding that once per row
		// rather than per sample is what lets the copy vectorise, and it
		// is worth several times the run time of the general case.
		if (bits >= 16) {
			for (uint32_t x = 0; x < dst.width; x++) {
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				d[3] = s[3];
				d += 4;
				s += 4;
			}
			continue;
		}
		for (uint32_t x = 0; x < dst.width; x++) {
			d[0] = scale_nbit_to_u16(s[2], bits);
			d[1] = scale_nbit_to_u16(s[1], bits);
			d[2] = scale_nbit_to_u16(s[0], bits);
			d[3] = scale_nbit_to_u16(s[3], bits);
			d += 4;
			s += 4;
		}
	}
}

void
pack_rgb16le_to_bgra16(
	Image &dst, const uint16_t *src, size_t src_stride_bytes, int bits)
{
	for (uint32_t y = 0; y < dst.height; y++) {
		auto *d = row_u16(dst, y);
		const uint16_t *s = assume_aligned<const uint16_t>(
			(const uint8_t *) src + y * src_stride_bytes);

		// As above.
		if (bits >= 16) {
			for (uint32_t x = 0; x < dst.width; x++) {
				d[0] = s[2];
				d[1] = s[1];
				d[2] = s[0];
				d[3] = 65535;
				d += 4;
				s += 3;
			}
			continue;
		}
		for (uint32_t x = 0; x < dst.width; x++) {
			d[0] = scale_nbit_to_u16(s[2], bits);
			d[1] = scale_nbit_to_u16(s[1], bits);
			d[2] = scale_nbit_to_u16(s[0], bits);
			d[3] = 65535;
			d += 4;
			s += 3;
		}
	}
}

static bool
open_file(const string &path, ifstream &in, size_t *size, Error *error)
{
#ifdef _WIN32
	// The manifest sets no activeCodePage, so a narrow path would be opened
	// through the ANSI code page, which most filenames do not survive.
	in.open(filesystem::path(utf8_to_wide(path)), ios::binary);
#else
	in.open(path, ios::binary);
#endif
	if (!in) {
		if (error) {
			error->code = Error::Code::Io;
			error->message =
				format_message(_("failed to open: %s"), path.c_str());
		}
		return false;
	}
	in.seekg(0, ios::end);
	auto sz = in.tellg();
	if (sz < 0) {
		if (error) {
			error->code = Error::Code::Io;
			error->message =
				format_message(_("failed to size: %s"), path.c_str());
		}
		return false;
	}
	in.seekg(0, ios::beg);
	*size = size_t(sz);
	return true;
}

// Reads the `size` bytes that `open_file()` has measured.
static bool
read_opened(
	ifstream &in, uint8_t *data, size_t size, const string &path, Error *error)
{
	if (size && !in.read((char *) data, streamsize(size))) {
		if (error) {
			error->code = Error::Code::Io;
			error->message =
				format_message(_("failed to read: %s"), path.c_str());
		}
		return false;
	}
	return true;
}

bool
read_file(const string &path, vector<uint8_t> *out, Error *error)
{
	ifstream in;
	size_t size = 0;
	if (!open_file(path, in, &size, error))
		return false;

	out->resize(size);
	return read_opened(in, out->data(), size, path, error);
}

// --- Saving ------------------------------------------------------------------

static void
append_u16(vector<uint8_t> &out, size_t value)
{
	out.push_back(uint8_t(value >> 8));
	out.push_back(uint8_t(value));
}

// One or more APP segments carrying `data` behind `id`.  ICC is the only
// payload that may span several, and it numbers them; nothing reassembles
// the others, so those have to fit or say that they do not.  Note that the
// two sequence bytes count against the length as well.
static bool
append_app(vector<uint8_t> &out, uint8_t marker, string_view id,
	span<const uint8_t> data, bool sequenced, const char *what, Error *error)
{
	if (data.empty())
		return true;

	const size_t extra = sequenced ? 2 : 0;
	const size_t limit = 0xFFFF - 2 - id.size() - extra;
	const size_t total = (data.size() + limit - 1) / limit;
	if (total > (sequenced ? 255u : 1u)) {
		set_error(
			error, format_message(_("%s metadata is too large to save"), what));
		return false;
	}

	for (size_t i = 0; i < total; i++) {
		const size_t chunk = min(limit, data.size() - i * limit);
		out.push_back(0xFF);
		out.push_back(marker);
		append_u16(out, chunk + 2 + id.size() + extra);
		out.insert(out.end(), id.begin(), id.end());
		if (sequenced) {
			out.push_back(uint8_t(i + 1));
			out.push_back(uint8_t(total));
		}
		const uint8_t *from = data.data() + i * limit;
		out.insert(out.end(), from, from + chunk);
	}
	return true;
}

bool
save_exv(const Image &page, vector<uint8_t> *out, Error *error)
{
	if (!out) {
		set_error(error, _("no output buffer"));
		return false;
	}
	out->clear();

	// This does not constitute a valid JPEG codestream--it is a standalone
	// TEM marker with trailing nonsense, which is what Exiv2 looks for.
	static constexpr string_view kTem = "\xFF\x01"
										"Exiv2";
	static constexpr string_view kExif{"Exif\0", 6};
	static constexpr string_view kIcc{"ICC_PROFILE", 12};
	static constexpr string_view kXmp{"http://ns.adobe.com/xap/1.0/", 29};

	out->insert(out->end(), kTem.begin(), kTem.end());
	// https://www.color.org/specification/ICC1v43_2010-12.pdf B.4
	if (!append_app(*out, 0xE1, kExif, page.exif, false, "Exif", error) ||
		!append_app(*out, 0xE2, kIcc, page.icc, true, "ICC", error) ||
		!append_app(*out, 0xE1, kXmp, page.xmp, false, "XMP", error)) {
		out->clear();
		return false;
	}
	out->push_back(0xFF);
	out->push_back(0xD9);
	return true;
}

// --- URLs --------------------------------------------------------------------

static int
hex_digit(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;
	return -1;
}

optional<string>
uri_to_path(const string &uri)
{
	constexpr string_view prefix = "file://";
	if (!uri.starts_with(prefix))
		return {};

	string_view rest = string_view(uri).substr(prefix.size());
	if (rest.starts_with("localhost/"))
		rest = rest.substr(string_view("localhost").size());

	string path;
	path.reserve(rest.size());
	for (size_t i = 0; i < rest.size(); i++) {
		if (rest[i] != '%') {
			path += rest[i];
			continue;
		}

		// A partial or invalid escape is not a URI (RFC 3986 section 2.1).
		int hi = 0, lo = 0;
		if (i + 2 >= rest.size() || (hi = hex_digit(rest[i + 1])) < 0 ||
			(lo = hex_digit(rest[i + 2])) < 0)
			return {};

		// Decoding these would change what the path is, rather than what
		// it says: a separator would fabricate a segment boundary (RFC
		// 3986 section 2.2), and no filename can contain a NUL.
		char c = char(hi << 4 | lo);
		if (!c || c == '/')
			return {};

		path += c;
		i += 2;
	}

#ifdef _WIN32
	// RFC 8089 appendix E.2: a drive letter follows the root slash, and
	// older producers spell its colon as a vertical line.
	char drive = path.size() > 2 ? path[1] : 0;
	if (path[0] == '/' &&
		((drive >= 'A' && drive <= 'Z') || (drive >= 'a' && drive <= 'z'))) {
		if (path[2] == '|')
			path[2] = ':';
		if (path[2] == ':')
			path.erase(0, 1);
	}
	for (char &c : path)
		if (c == '/')
			c = '\\';
#endif
	return path;
}

string
path_to_uri(const string &path)
{
	// RFC 2396 `pchar` (`unreserved` includes `mark`), plus the separator.
	// RFC 3986 obsoletes that grammar, but only by admitting `;`, which
	// RFC 2396 reserved to introduce a path parameter -- and the shared
	// thumbnail cache is keyed on URIs escaped the older way.  (Cator to it.)
	constexpr string_view allowed = "!$&'()*+,-./:=@_~";
	constexpr string_view hex = "0123456789ABCDEF";

	string uri = "file://";
	if (!path.starts_with("/"))
		uri += '/';

	for (char ch : path) {
#ifdef _WIN32
		if (ch == '\\')
			ch = '/';
#endif
		if ((ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') ||
			(ch >= 'a' && ch <= 'z') || allowed.find(ch) != allowed.npos) {
			uri += ch;
			continue;
		}

		uint8_t c = uint8_t(ch);
		uri += '%';
		uri += hex[c >> 4];
		uri += hex[c & 15];
	}
	return uri;
}

#ifdef _WIN32

wstring
utf8_to_wide(string_view utf8)
{
	if (utf8.empty())
		return {};

	const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
		utf8.data(), int(utf8.size()), nullptr, 0);
	if (size <= 0)
		return {};

	wstring wide(size_t(size), L'\0');
	if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
			int(utf8.size()), wide.data(), size))
		return {};
	return wide;
}

string
wide_to_utf8(wstring_view wide)
{
	if (wide.empty())
		return {};

	const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
		wide.data(), int(wide.size()), nullptr, 0, nullptr, nullptr);
	if (size <= 0)
		return {};

	string utf8(size_t(size), '\0');
	if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
			int(wide.size()), utf8.data(), size, nullptr, nullptr))
		return {};
	return utf8;
}

wstring
module_directory(void *module)
{
	// dn is longPathAware, and GetModuleFileNameW() truncates to fit.
	DWORD len = 0;
	wstring path(MAX_PATH, L'\0');
	while ((len = GetModuleFileNameW(HMODULE(module), path.data(),
				DWORD(path.size()))) == path.size()) {
		if (path.size() > 0x8000)
			return {};
		path.resize(path.size() * 2);
	}
	if (!len)
		return {};

	path.resize(len);
	size_t slash = path.find_last_of(L"\\/");
	if (slash == path.npos)
		return {};

	path.resize(slash + 1);
	return path;
}

#endif

// --- Premultiply / finish ----------------------------------------------------

void
premultiply_bgra16(Image &image)
{
	for (uint32_t y = 0; y < image.height; y++) {
		auto *p = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++) {
			uint16_t b = p[0], g = p[1], r = p[2], a = p[3];
			p[0] = premultiply16(a, b);
			p[1] = premultiply16(a, g);
			p[2] = premultiply16(a, r);
			p[3] = a;
			p += 4;
		}
	}
}

void
unpremultiply_bgra16(Image &image)
{
	for (uint32_t y = 0; y < image.height; y++) {
		auto *p = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++) {
			uint16_t b = p[0], g = p[1], r = p[2], a = p[3];
			p[0] = unpremultiply16(a, b);
			p[1] = unpremultiply16(a, g);
			p[2] = unpremultiply16(a, r);
			p[3] = a;
			p += 4;
		}
	}
}

void
unpremultiply_xxxa8(
	uint8_t *data, uint32_t width, uint32_t height, size_t stride)
{
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *p = data + y * stride;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t a = p[3];
			p[0] = unpremultiply8(a, p[0]);
			p[1] = unpremultiply8(a, p[1]);
			p[2] = unpremultiply8(a, p[2]);
			p += 4;
		}
	}
}

bool
opaque_bgra16(
	const uint8_t *data, uint32_t width, uint32_t height, size_t stride)
{
	for (uint32_t y = 0; y < height; y++) {
		const auto *row = reinterpret_cast<const uint16_t *>(data + y * stride);
		for (uint32_t x = 0; x < width; x++)
			if (row[x * 4 + 3] != 65535)
				return false;
	}
	return true;
}

// Takes straight BGRA16, always leaves it premultiplied, colour-managed on
// the way when there is a target.
static void
finish_premultiply(Cmm &cmm, Image &image, Profile *source, Profile *target)
{
	if (!target || cmm.broken_premul()) {
		if (target)
			(void) cmm.transform_bgra16(image.data.data(), image.width,
				image.height, source, target, false, false);
		premultiply_bgra16(image);
		return;
	}
	if (!cmm.transform_bgra16(image.data.data(), image.width, image.height,
			source, target, false, true))
		premultiply_bgra16(image);
}

// Loads the embedded profile when the loader named none, resolving `source`,
// and records what the pixels are to be described as coming from. The
// returned owning reference keeps `source` alive through the conversion.
static shared_ptr<Profile>
resolve_source(Image &image, const OpenContext &ctx, Profile *&source)
{
	shared_ptr<Profile> owned;
	if (!source && !image.icc.empty()) {
		owned = cmm_or_default(ctx)->get_profile(image.icc);
		source = owned.get();
	}
	if (!image.effective_profile) {
		if (owned)
			image.effective_profile = owned;
		else if (!source) {
			image.effective_profile = cmm_or_default(ctx)->get_profile_sRGB();
			image.profile_assumed = true;
		}
	}
	return owned;
}

void
finish_image(
	Image &image, const OpenContext &ctx, Profile *source, bool input_premul)
{
	shared_ptr<Profile> owned = resolve_source(image, ctx, source);
	Profile *target = ctx.screen_profile.get();
	if (input_premul && !target)
		return;
	if (input_premul)
		unpremultiply_bgra16(image);
	finish_premultiply(*cmm_or_default(ctx), image, source, target);
}

void
finish_frames(
	Image &page, const OpenContext &ctx, Profile *source, bool input_premul)
{
	// Resolve ICC once from the page head so animation frames without their
	// own profile still colour-manage against the page embedding.
	shared_ptr<Profile> owned = resolve_source(page, ctx, source);
	for (Image *frame = &page; frame != nullptr;
		frame = frame->frame_next.get()) {
		if (!frame->effective_profile) {
			frame->effective_profile = page.effective_profile;
			frame->profile_assumed = page.profile_assumed;
		}
		finish_image(*frame, ctx, source, input_premul);
	}
}

// --- Compositing -------------------------------------------------------------

static inline uint16_t
clamp_u16(int v)
{
	if (v < 0)
		return 0;
	if (v > 65535)
		return 65535;
	return uint16_t(v);
}

static inline uint16_t *
pixel_at(Image &img, uint32_t x, uint32_t y)
{
	return row_u16(img, y) + x * 4;
}

static inline const uint16_t *
pixel_at(const Image &img, uint32_t x, uint32_t y)
{
	return row_u16(img, y) + x * 4;
}

void
fill_rect(Image &dst, int x, int y, int w, int h, uint16_t b, uint16_t g,
	uint16_t r, uint16_t a)
{
	int x0 = max(0, x);
	int y0 = max(0, y);
	int x1 = min(int(dst.width), x + w);
	int y1 = min(int(dst.height), y + h);
	for (int yy = y0; yy < y1; yy++) {
		for (int xx = x0; xx < x1; xx++) {
			auto *p = pixel_at(dst, uint32_t(xx), uint32_t(yy));
			p[0] = b;
			p[1] = g;
			p[2] = r;
			p[3] = a;
		}
	}
}

static void
blend_pixel_over(uint16_t *d, const uint16_t *s)
{
	// Premultiplied OVER: out = src + dst * (1 - src.a)
	uint32_t sa = s[3];
	uint32_t inv = 65535u - sa;
	d[0] = clamp_u16(int(s[0] + (d[0] * inv + 32767u) / 65535u));
	d[1] = clamp_u16(int(s[1] + (d[1] * inv + 32767u) / 65535u));
	d[2] = clamp_u16(int(s[2] + (d[2] * inv + 32767u) / 65535u));
	d[3] = clamp_u16(int(s[3] + (d[3] * inv + 32767u) / 65535u));
}

void
blend_image(Image &dst, const Image &src, int dst_x, int dst_y, BlendOp op)
{
	for (uint32_t sy = 0; sy < src.height; sy++) {
		int dy = dst_y + int(sy);
		if (dy < 0 || dy >= int(dst.height))
			continue;

		for (uint32_t sx = 0; sx < src.width; sx++) {
			int dx = dst_x + int(sx);
			if (dx < 0 || dx >= int(dst.width))
				continue;

			auto *d = pixel_at(dst, uint32_t(dx), uint32_t(dy));
			auto *s = pixel_at(src, sx, sy);
			if (op == BlendOp::Source) {
				d[0] = s[0];
				d[1] = s[1];
				d[2] = s[2];
				d[3] = s[3];
			} else {
				blend_pixel_over(d, s);
			}
		}
	}
}

// --- Matrix ------------------------------------------------------------------

static Matrix
matrix_multiply(const Matrix &a, const Matrix &b)
{
	Matrix r;
	r.xx = a.xx * b.xx + a.xy * b.yx;
	r.yx = a.yx * b.xx + a.yy * b.yx;
	r.xy = a.xx * b.xy + a.xy * b.yy;
	r.yy = a.yx * b.xy + a.yy * b.yy;
	r.x0 = a.xx * b.x0 + a.xy * b.y0 + a.x0;
	r.y0 = a.yx * b.x0 + a.yy * b.y0 + a.y0;
	return r;
}

static Matrix
matrix_translate(double tx, double ty)
{
	Matrix m;
	m.x0 = tx;
	m.y0 = ty;
	return m;
}

static Matrix
matrix_scale(double sx, double sy)
{
	Matrix m;
	m.xx = sx;
	m.yy = sy;
	return m;
}

static Matrix
matrix_rotate(double radians)
{
	Matrix m;
	m.xx = cos(radians);
	m.yx = sin(radians);
	m.xy = -sin(radians);
	m.yy = cos(radians);
	return m;
}

// --- Orientation -------------------------------------------------------------

Matrix
orientation_matrix(Orientation orientation, double width, double height)
{
	Matrix matrix;
	constexpr double pi2 = numbers::pi / 2;
	switch (orientation) {
	case Orientation::Rotate90:
		matrix = matrix_multiply(matrix_rotate(-pi2), matrix);
		matrix = matrix_multiply(matrix_translate(-width, 0), matrix);
		break;
	case Orientation::Rotate180:
		matrix = matrix_multiply(matrix_scale(-1, -1), matrix);
		matrix = matrix_multiply(matrix_translate(-width, -height), matrix);
		break;
	case Orientation::Rotate270:
		matrix = matrix_multiply(matrix_rotate(+pi2), matrix);
		matrix = matrix_multiply(matrix_translate(0, -height), matrix);
		break;
	case Orientation::Mirror0:
		matrix = matrix_multiply(matrix_scale(-1, +1), matrix);
		matrix = matrix_multiply(matrix_translate(-width, 0), matrix);
		break;
	case Orientation::Mirror90:
		matrix = matrix_multiply(matrix_rotate(+pi2), matrix);
		matrix = matrix_multiply(matrix_scale(-1, +1), matrix);
		matrix = matrix_multiply(matrix_translate(-width, -height), matrix);
		break;
	case Orientation::Mirror180:
		matrix = matrix_multiply(matrix_scale(+1, -1), matrix);
		matrix = matrix_multiply(matrix_translate(0, -height), matrix);
		break;
	case Orientation::Mirror270:
		matrix = matrix_multiply(matrix_rotate(-pi2), matrix);
		matrix = matrix_multiply(matrix_scale(-1, +1), matrix);
		break;
	default:
		break;
	}
	return matrix;
}

Orientation
orientation_or_0(Orientation orientation)
{
	return orientation == Orientation::Unknown ? Orientation::Rotate0
											   : orientation;
}

void
orientation_display_size(uint32_t src_w, uint32_t src_h,
	Orientation orientation, uint32_t *width, uint32_t *height)
{
	orientation = orientation_or_0(orientation);
	switch (orientation) {
	case Orientation::Rotate90:
	case Orientation::Mirror90:
	case Orientation::Rotate270:
	case Orientation::Mirror270:
		*width = src_h;
		*height = src_w;
		break;
	default:
		*width = src_w;
		*height = src_h;
	}
}

static Orientation
orientation_lookup(Orientation orientation, const Orientation table[9])
{
	orientation = orientation_or_0(orientation);
	const int i = int(orientation);
	if (i < 0 || i > 8)
		return Orientation::Rotate0;
	return table[i];
}

Orientation
orientation_rotate_left(Orientation orientation)
{
	static constexpr Orientation kTable[9] = {
		Orientation::Unknown,
		Orientation::Rotate270,
		Orientation::Mirror270,
		Orientation::Rotate90,
		Orientation::Mirror90,
		Orientation::Mirror180,
		Orientation::Rotate0,
		Orientation::Mirror0,
		Orientation::Rotate180,
	};
	return orientation_lookup(orientation, kTable);
}

Orientation
orientation_rotate_right(Orientation orientation)
{
	static constexpr Orientation kTable[9] = {
		Orientation::Unknown,
		Orientation::Rotate90,
		Orientation::Mirror90,
		Orientation::Rotate270,
		Orientation::Mirror270,
		Orientation::Mirror0,
		Orientation::Rotate180,
		Orientation::Mirror180,
		Orientation::Rotate0,
	};
	return orientation_lookup(orientation, kTable);
}

Orientation
orientation_mirror(Orientation orientation)
{
	static constexpr Orientation kTable[9] = {
		Orientation::Unknown,
		Orientation::Mirror0,
		Orientation::Rotate0,
		Orientation::Mirror180,
		Orientation::Rotate180,
		Orientation::Rotate90,
		Orientation::Mirror270,
		Orientation::Rotate270,
		Orientation::Mirror90,
	};
	return orientation_lookup(orientation, kTable);
}

void
orientation_map_display_to_source(Orientation orientation, uint32_t src_w,
	uint32_t src_h, double dx, double dy, double *sx, double *sy)
{
	uint32_t dw = 0, dh = 0;
	orientation_display_size(src_w, src_h, orientation, &dw, &dh);

	const double w = double(dw);
	const double h = double(dh);
	switch (orientation_or_0(orientation)) {
	case Orientation::Mirror0:
		*sx = w - dx;
		*sy = dy;
		break;
	case Orientation::Rotate180:
		*sx = w - dx;
		*sy = h - dy;
		break;
	case Orientation::Mirror180:
		*sx = dx;
		*sy = h - dy;
		break;
	case Orientation::Mirror270:
		*sx = dy;
		*sy = dx;
		break;
	case Orientation::Rotate90:
		*sx = dy;
		*sy = w - dx;
		break;
	case Orientation::Mirror90:
		*sx = h - dy;
		*sy = w - dx;
		break;
	case Orientation::Rotate270:
		*sx = h - dy;
		*sy = dx;
		break;
	default:
		*sx = dx;
		*sy = dy;
		break;
	}
}

void
orientation_map_source_to_display(Orientation orientation, uint32_t src_w,
	uint32_t src_h, double sx, double sy, double *dx, double *dy)
{
	uint32_t dw = 0, dh = 0;
	orientation_display_size(src_w, src_h, orientation, &dw, &dh);

	const double w = double(dw);
	const double h = double(dh);
	switch (orientation_or_0(orientation)) {
	case Orientation::Mirror0:
		*dx = w - sx;
		*dy = sy;
		break;
	case Orientation::Rotate180:
		*dx = w - sx;
		*dy = h - sy;
		break;
	case Orientation::Mirror180:
		*dx = sx;
		*dy = h - sy;
		break;
	case Orientation::Mirror270:
		*dx = sy;
		*dy = sx;
		break;
	case Orientation::Rotate90:
		*dx = w - sy;
		*dy = sx;
		break;
	case Orientation::Mirror90:
		*dx = w - sy;
		*dy = h - sx;
		break;
	case Orientation::Rotate270:
		*dx = sy;
		*dy = h - sx;
		break;
	default:
		*dx = sx;
		*dy = sy;
		break;
	}
}

vector<uint8_t>
iso_exif_payload(span<const uint8_t> payload)
{
	if (payload.size() < 4)
		return {};

	size_t offset = size_t(payload[0]) << 24 | size_t(payload[1]) << 16 |
		size_t(payload[2]) << 8 | size_t(payload[3]);
	if (offset > payload.size() - 4)
		return {};
	return vector<uint8_t>(
		payload.begin() + ptrdiff_t(4 + offset), payload.end());
}

Orientation
exif_orientation(span<const uint8_t> exif)
{
	struct tiffer T = {};
	if (!tiffer_init(&T, exif.data(), exif.size()) || !tiffer_next_ifd(&T))
		return Orientation::Unknown;

	struct tiffer_entry entry = {};
	while (tiffer_next_entry(&T, &entry)) {
		int64_t orientation = 0;
		if (entry.tag == TIFF_Orientation && entry.type == TIFFER_SHORT &&
			entry.remaining_count == 1 &&
			tiffer_integer(&T, &entry, &orientation) && orientation >= 1 &&
			orientation <= 8)
			return Orientation(orientation);
	}
	return Orientation::Unknown;
}

// --- Gain maps ---------------------------------------------------------------

float
gain_map_weight(const GainMap &map, float headroom)
{
	const float range = map.alternate_headroom - map.base_headroom;
	if (!(headroom > 0) || !(range > 0))
		return 0;
	return clamp((log2(headroom) - map.base_headroom) / range, 0.f, 1.f);
}

static bool
xml_space(char c)
{
	return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static string_view
xml_trim(string_view s)
{
	while (!s.empty() && xml_space(s.front()))
		s.remove_prefix(1);
	while (!s.empty() && xml_space(s.back()))
		s.remove_suffix(1);
	return s;
}

// The prefix of the first xmlns declaration binding the namespace URI.
static string_view
xmp_prefix(string_view xmp, string_view ns)
{
	for (size_t at = 0; (at = xmp.find(ns, at)) != string_view::npos;
		at += ns.size()) {
		const size_t end = at + ns.size();
		if (at < 3 || end >= xmp.size() || xmp[end] != xmp[at - 1] ||
			(xmp[at - 1] != '"' && xmp[at - 1] != '\''))
			continue;

		string_view before = xml_trim(xmp.substr(0, at - 1));
		if (before.empty() || before.back() != '=')
			continue;
		before = xml_trim(before.substr(0, before.size() - 1));
		const size_t name = before.find_last_of(" \t\r\n<");
		string_view attribute =
			name == string_view::npos ? before : before.substr(name + 1);
		if (attribute.starts_with("xmlns:"))
			return attribute.substr(6);
	}
	return {};
}

// Whether a qualified name ends at `at`, rather than continuing.
static bool
xml_name_ends(string_view xmp, size_t at)
{
	return at >= xmp.size() || xml_space(xmp[at]) || xmp[at] == '=' ||
		xmp[at] == '>' || xmp[at] == '/';
}

static void
xmp_attribute_values(string_view xmp, string_view qname, vector<string> &out)
{
	for (size_t at = 0; (at = xmp.find(qname, at)) != string_view::npos;
		at += qname.size()) {
		size_t p = at + qname.size();
		if (!at || !xml_space(xmp[at - 1]) || !xml_name_ends(xmp, p))
			continue;
		while (p < xmp.size() && xml_space(xmp[p]))
			p++;
		if (p >= xmp.size() || xmp[p++] != '=')
			continue;
		while (p < xmp.size() && xml_space(xmp[p]))
			p++;
		if (p >= xmp.size() || (xmp[p] != '"' && xmp[p] != '\''))
			continue;
		const size_t end = xmp.find(xmp[p], p + 1);
		if (end == string_view::npos)
			break;
		out.emplace_back(xml_trim(xmp.substr(p + 1, end - p - 1)));
	}
}

static void
xmp_element_values(string_view xmp, string_view qname, vector<string> &out)
{
	const string open = "<" + string(qname), close = "</" + string(qname);
	for (size_t at = 0; (at = xmp.find(open, at)) != string_view::npos;
		at += open.size()) {
		const size_t gt = xmp.find('>', at);
		if (!xml_name_ends(xmp, at + open.size()) || gt == string_view::npos ||
			xmp[gt - 1] == '/')
			continue;
		const size_t end = xmp.find(close, gt);
		if (end == string_view::npos)
			break;

		string_view content = xmp.substr(gt + 1, end - gt - 1);
		if (content.find('<') == string_view::npos) {
			out.emplace_back(xml_trim(content));
			continue;
		}
		for (size_t li = 0;
			(li = content.find("<rdf:li", li)) != string_view::npos; li++) {
			const size_t li_gt = content.find('>', li);
			const size_t li_end = content.find("</rdf:li>", li);
			if (li_gt == string_view::npos || li_end == string_view::npos ||
				li_gt > li_end)
				break;
			out.emplace_back(
				xml_trim(content.substr(li_gt + 1, li_end - li_gt - 1)));
		}
	}
}

vector<string>
xmp_values(string_view xmp, string_view ns, string_view name)
{
	vector<string> values;
	string_view prefix = xmp_prefix(xmp, ns);
	if (prefix.empty())
		return values;

	const string qname = string(prefix) + ":" + string(name);
	xmp_attribute_values(xmp, qname, values);
	xmp_element_values(xmp, qname, values);
	return values;
}

bool
xmp_declares(string_view xmp, string_view ns)
{
	return !xmp_prefix(xmp, ns).empty();
}

static bool
parse_decimal(string_view text, double *out)
{
	istringstream in{string(text)};
	in.imbue(locale::classic());
	return in >> *out && in.eof() && isfinite(*out);
}

// ISO 21496-1 metadata is only usable with channel-independent values.
static bool
single_value(const vector<double> &values, double *out)
{
	if (values.empty())
		return false;
	for (double value : values)
		if (value != values.front())
			return false;
	*out = values.front();
	return true;
}

static bool
checked_gain_map(GainMap *map, double min, double max, double gamma,
	double base_offset, double alternate_offset, double base_headroom,
	double alternate_headroom)
{
	if (!(gamma > 0) || !(max >= min) || base_offset != alternate_offset)
		return false;
	map->min = float(min);
	map->max = float(max);
	map->gamma = float(gamma);
	map->offset = float(base_offset);
	map->base_headroom = float(base_headroom);
	map->alternate_headroom = float(alternate_headroom);
	return true;
}

namespace
{

struct BigEndianReader {
	const uint8_t *p, *end;

	bool u32(uint32_t *out)
	{
		if (end - p < 4)
			return false;
		*out = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
			uint32_t(p[2]) << 8 | p[3];
		p += 4;
		return true;
	}
	bool fraction(bool is_signed, uint32_t denominator, double *out)
	{
		uint32_t n = 0;
		if (!u32(&n) || !denominator)
			return false;
		*out = (is_signed ? double(int32_t(n)) : double(n)) / denominator;
		return true;
	}
	bool rational(bool is_signed, double *out)
	{
		uint32_t n = 0, d = 0;
		if (!u32(&n) || !u32(&d) || !d)
			return false;
		*out = (is_signed ? double(int32_t(n)) : double(n)) / d;
		return true;
	}
};

}  // namespace

bool
parse_iso_gain_map(span<const uint8_t> blob, GainMap *map)
{
	// minimum_version, writer_version, then the flags.  The primary image
	// of a JPEG carries only the versions.
	if (blob.size() < 5 || blob[0] || blob[1])
		return false;

	const uint8_t flags = blob[4];
	const bool multichannel = flags & 0x80;
	const bool backward = flags & 0x04;
	const bool common = flags & 0x08;
	BigEndianReader r{blob.data() + 5, blob.data() + blob.size()};

	uint32_t denominator = 0;
	double base_headroom = 0, alternate_headroom = 0;
	if (common) {
		if (!r.u32(&denominator) ||
			!r.fraction(false, denominator, &base_headroom) ||
			!r.fraction(false, denominator, &alternate_headroom))
			return false;
	} else if (!r.rational(false, &base_headroom) ||
		!r.rational(false, &alternate_headroom)) {
		return false;
	}

	vector<double> values[5];
	for (int c = 0; c < (multichannel ? 3 : 1); c++) {
		for (int i = 0; i < 5; i++) {
			const bool is_signed = i != 2;  // min, max, gamma, offsets
			double v = 0;
			if (common ? !r.fraction(is_signed, denominator, &v)
					   : !r.rational(is_signed, &v))
				return false;
			values[i].push_back(v);
		}
	}

	double v[5] = {};
	for (int i = 0; i < 5; i++)
		if (!single_value(values[i], &v[i]))
			return false;

	// The backward direction has an HDR base, whatever the headrooms say.
	if (backward && base_headroom <= alternate_headroom)
		swap(base_headroom, alternate_headroom);
	return checked_gain_map(
		map, v[0], v[1], v[2], v[3], v[4], base_headroom, alternate_headroom);
}

bool
parse_tmap_gain_map(span<const uint8_t> item, GainMap *map)
{
	// The item puts a version of its own in front, and only 0 is defined.
	return !item.empty() && !item[0] &&
		parse_iso_gain_map(item.subspan(1), map);
}

bool
split_jhgm_bundle(span<const uint8_t> box, span<const uint8_t> *metadata,
	span<const uint8_t> *codestream)
{
	// As libjxl's lib/extras/gain_map.cc lays it out, big-endian: version,
	// the metadata, the alternate's colour encoding and ICC profile, which
	// a map applied in display space has no use for, then the codestream.
	if (box.size() < 3 || box[0])
		return false;
	const size_t metadata_size = size_t(box[1]) << 8 | box[2];
	size_t at = 3;
	if (box.size() - at < metadata_size + 1)
		return false;
	*metadata = box.subspan(at, metadata_size);
	at += metadata_size;

	const size_t encoding_size = box[at++];
	if (box.size() - at < encoding_size + 4)
		return false;
	at += encoding_size;
	const size_t icc_size = size_t(box[at]) << 24 | size_t(box[at + 1]) << 16 |
		size_t(box[at + 2]) << 8 | box[at + 3];
	at += 4;
	if (box.size() - at <= icc_size)
		return false;
	*codestream = box.subspan(at + icc_size);
	return true;
}

bool
parse_hdrgm_gain_map(string_view xmp, GainMap *map)
{
	static constexpr string_view ns = "http://ns.adobe.com/hdr-gain-map/1.0/";
	auto get = [&](string_view name, double fallback, double *out) {
		vector<string> texts = xmp_values(xmp, ns, name);
		if (texts.empty()) {
			*out = fallback;
			return !isnan(fallback);
		}
		if (texts.size() != 1 && texts.size() != 3)
			return false;

		vector<double> values(texts.size());
		for (size_t i = 0; i < texts.size(); i++)
			if (!parse_decimal(texts[i], &values[i]))
				return false;
		return single_value(values, out);
	};

	// GainMapMax and HDRCapacityMax have no defaults.
	const double required = NAN, offset = 1. / 64;
	double min = 0, max = 0, gamma = 0, offset_sdr = 0, offset_hdr = 0,
		   capacity_min = 0, capacity_max = 0;
	if (!get("GainMapMin", 0, &min) || !get("GainMapMax", required, &max) ||
		!get("Gamma", 1, &gamma) || !get("OffsetSDR", offset, &offset_sdr) ||
		!get("OffsetHDR", offset, &offset_hdr) ||
		!get("HDRCapacityMin", 0, &capacity_min) ||
		!get("HDRCapacityMax", required, &capacity_max))
		return false;

	vector<string> hdr_base = xmp_values(xmp, ns, "BaseRenditionIsHDR");
	if (!hdr_base.empty() && hdr_base.front() == "True")
		return checked_gain_map(map, min, max, gamma, offset_hdr, offset_sdr,
			capacity_max, capacity_min);
	return checked_gain_map(map, min, max, gamma, offset_sdr, offset_hdr,
		capacity_min, capacity_max);
}

static constexpr string_view kAppleGainMapNs =
	"http://ns.apple.com/HDRGainMap/1.0/";

bool
apple_gain_map_declared(string_view xmp)
{
	return xmp_declares(xmp, kAppleGainMapNs);
}

// Tags 33 and 48 of Apple's maker note, which is an IFD at offset 14,
// preceded by "Apple iOS", a version, and a byte order mark, with offsets
// relative to its own beginning.  Tags it lacks are left alone.
static void
apple_maker_note_tags(
	span<const uint8_t> note, double *maker33, double *maker48)
{
	if (note.size() < 16 || memcmp(note.data(), "Apple iOS\0", 10))
		return;

	struct tiffer T = {};
	T.begin = note.data();
	T.end = note.data() + note.size();
	if (!memcmp(note.data() + 12, "MM", 2))
		T.un = &tiffer_unbe;
	else if (!memcmp(note.data() + 12, "II", 2))
		T.un = &tiffer_unle;
	else
		return;
	T.p = note.data() + 14;
	if (!tiffer_u16(&T, &T.remaining_fields))
		return;

	struct tiffer_entry entry = {};
	while (tiffer_next_entry(&T, &entry)) {
		if (entry.tag == 33 && entry.remaining_count == 1)
			(void) tiffer_real(&T, &entry, maker33);
		else if (entry.tag == 48 && entry.remaining_count == 1)
			(void) tiffer_real(&T, &entry, maker48);
	}
}

// Zero when the tags do not make for a headroom.
static double
apple_maker_headroom(double maker33, double maker48)
{
	if (!isfinite(maker33) || !isfinite(maker48))
		return 0;

	// https://developer.apple.com/documentation/appkit/applying-apple-hdr-effect-to-your-photos
	double stops = 0;
	if (maker33 < 1)
		stops =
			maker48 <= 0.01 ? -20 * maker48 + 1.8 : -0.101 * maker48 + 1.601;
	else
		stops = maker48 <= 0.01 ? -70 * maker48 + 3 : -0.303 * maker48 + 2.303;
	return exp2(max(stops, 0.));
}

static span<const uint8_t>
exif_maker_note(span<const uint8_t> exif)
{
	struct tiffer T = {};
	if (!tiffer_init(&T, exif.data(), exif.size()) || !tiffer_next_ifd(&T))
		return {};

	struct tiffer_entry entry = {};
	int64_t offset = -1;
	while (tiffer_next_entry(&T, &entry))
		if (entry.tag == TIFF_ExifIFDPointer && entry.remaining_count == 1)
			(void) tiffer_integer(&T, &entry, &offset);
	struct tiffer subT = {};
	if (offset < 0 || offset > UINT32_MAX ||
		!tiffer_subifd(&T, uint32_t(offset), &subT))
		return {};

	while (tiffer_next_entry(&subT, &entry))
		if (entry.tag == Exif_MakerNote && entry.type == TIFFER_UNDEFINED)
			return span<const uint8_t>(entry.p, entry.remaining_count);
	return {};
}

double
apple_gain_map_headroom_from_maker(
	string_view map_xmp, double maker33, double maker48)
{
	if (xmp_values(map_xmp, kAppleGainMapNs, "HDRGainMapVersion").empty())
		return 0;

	double headroom = 0;
	vector<string> stored =
		xmp_values(map_xmp, kAppleGainMapNs, "HDRGainMapHeadroom");
	if (!stored.empty()) {
		if (!parse_decimal(stored.front(), &headroom))
			return 0;
	} else {
		headroom = apple_maker_headroom(maker33, maker48);
	}
	return headroom >= 1 ? headroom : 0;
}

double
apple_gain_map_headroom(string_view map_xmp, span<const uint8_t> exif)
{
	double maker33 = NAN, maker48 = 0;
	apple_maker_note_tags(exif_maker_note(exif), &maker33, &maker48);
	return apple_gain_map_headroom_from_maker(map_xmp, maker33, maker48);
}

GainMap
apple_gain_map(double headroom)
{
	GainMap map;
	map.max = map.alternate_headroom = float(log2(headroom));
	return map;
}

bool
gain_map_applies(const GainMap &metadata, const OpenContext &ctx)
{
	if (metadata.base_headroom > metadata.alternate_headroom) {
		add_warning(ctx, _("Gain maps over an HDR base are not supported"));
		return false;
	}
	return metadata.alternate_headroom != metadata.base_headroom &&
		(metadata.min != 0 || metadata.max != 0);
}

// The inverse of the Rec. 709 transfer function.
static double
rec709_to_linear(double v)
{
	return v < 0.081 ? v / 4.5 : pow((v + 0.099) / 1.099, 1 / 0.45);
}

unique_ptr<GainMap>
make_gain_map(const Image &pixels, const GainMap &metadata, bool apple)
{
	if (!pixels.width || !pixels.height)
		return nullptr;

	auto map = make_unique<GainMap>(metadata);
	map->width = pixels.width;
	map->height = pixels.height;
	map->data.resize(size_t(pixels.width) * pixels.height);
	uint16_t *out = map->data.data();
	for (uint32_t y = 0; y < pixels.height; y++) {
		const uint16_t *p = row_u16(pixels, y);
		for (uint32_t x = 0; x < pixels.width; x++, p += 4) {
			if (p[0] != p[1] || p[1] != p[2])
				return nullptr;
			*out++ = p[0];
		}
	}
	if (!apple)
		return map;

	// Apple's gain is linear in the linearized texel.  Rewritten as
	// normalized log2 gain, the renderer knows just the one formula.
	const double headroom = exp2(double(metadata.max));
	vector<uint16_t> lut(65536);
	for (uint32_t i = 0; i < lut.size(); i++) {
		const double v = rec709_to_linear(i / 65535.);
		lut[i] = uint16_t(
			lround(65535 * log2(1 + (headroom - 1) * v) / log2(headroom)));
	}
	for (uint16_t &texel : map->data)
		texel = lut[texel];
	return map;
}

bool
crop_gain_map(GainMap &map, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	if (!w || !h || x > map.width || w > map.width - x || y > map.height ||
		h > map.height - y)
		return false;

	vector<uint16_t> data(size_t(w) * h);
	for (uint32_t row = 0; row < h; row++) {
		const uint16_t *src = map.data.data() + size_t(y + row) * map.width + x;
		copy(src, src + w, data.begin() + ptrdiff_t(size_t(row) * w));
	}
	map.data = std::move(data);
	map.width = w;
	map.height = h;
	return true;
}

void
rotate_gain_map(GainMap &map, int ccw)
{
	ccw = ((ccw % 360) + 360) % 360;
	if (!ccw)
		return;

	const uint32_t w = map.width, h = map.height;
	const bool quarter = ccw != 180;
	const uint32_t out_w = quarter ? h : w;
	vector<uint16_t> data(map.data.size());
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t ox = 0, oy = 0;
			if (ccw == 90)
				ox = y, oy = w - 1 - x;
			else if (ccw == 180)
				ox = w - 1 - x, oy = h - 1 - y;
			else
				ox = h - 1 - y, oy = x;
			data[size_t(oy) * out_w + ox] = map.data[size_t(y) * w + x];
		}
	map.data = std::move(data);
	if (quarter)
		swap(map.width, map.height);
}

void
mirror_gain_map(GainMap &map, bool horizontally)
{
	const uint32_t w = map.width, h = map.height;
	if (horizontally) {
		for (uint32_t y = 0; y < h; y++) {
			auto row = map.data.begin() + ptrdiff_t(size_t(y) * w);
			reverse(row, row + w);
		}
		return;
	}
	for (uint32_t y = 0; y < h / 2; y++)
		swap_ranges(map.data.begin() + ptrdiff_t(size_t(y) * w),
			map.data.begin() + ptrdiff_t(size_t(y + 1) * w),
			map.data.begin() + ptrdiff_t(size_t(h - 1 - y) * w));
}

// --- True HDR ----------------------------------------------------------------

HdrRolloff::HdrRolloff(double peak, double target)
{
	threshold_ = numeric_limits<double>::infinity();
	if (!(peak > target) || !(target > 0))
		return;

	// BT.2390 puts the knee at 1.5·T − 0.5 of the normalized source range,
	// which leaves the spline a third of the span to roll off, the least
	// that keeps it monotonic.  With that, the spline is a cubic.
	const double top = log2(peak), bottom = log2(target);
	span_ = 1.5 * (top - bottom);
	knee_ = top - span_;
	threshold_ = exp2(knee_);
}

double
HdrRolloff::apply(double x) const
{
	if (!(x > threshold_))
		return x;
	const double t = min((log2(x) - knee_) / span_, 1.);
	return exp2(knee_ + span_ * (1 - pow(1 - t, 3)) / 3);
}

// Makes a pixel what split_hdr() takes: finite, not negative, and black when
// invisible, noting what had to be replaced.
static void
sanitize_hdr_pixel(float *p, bool &nonfinite, bool &negative)
{
	if (!isfinite(p[3])) {
		nonfinite = true;
		p[3] = p[3] > 0 ? 1 : 0;
	}
	p[3] = clamp(p[3], 0.f, 1.f);

	// An invisible pixel must not attach a map or raise the peak,
	// whatever it holds.
	if (p[3] == 0) {
		p[0] = p[1] = p[2] = 0;
		return;
	}

	// Conversions into wider primaries leave rounding below zero, which is
	// neither visible nor worth a message at this fraction of the pixel.
	const float rounding = -1e-3f * max({p[0], p[1], p[2]});
	for (size_t c = 0; c < 3; c++) {
		if (!isfinite(p[c])) {
			nonfinite = true;
			p[c] = 0;
		} else if (p[c] < 0) {
			negative |= p[c] < rounding;
			p[c] = 0;
		}
	}
}

static uint16_t
hdr_encode(float linear)
{
	return uint16_t(lround(transfer_encode(linear, Transfer::Srgb) * 65535));
}

bool
split_hdr(Image &image, const OpenContext &ctx, span<float> rgba,
	bool premultiplied, const double primaries[6], Error *error)
{
	const size_t pixels = size_t(image.width) * image.height;
	if (rgba.size() != pixels * 4) {
		set_error(error, _("invalid or truncated frame"));
		return false;
	}

	for (size_t i = 0; premultiplied && i < pixels; i++) {
		float *p = &rgba[i * 4];
		if (p[3] > 0)
			for (size_t c = 0; c < 3; c++)
				p[c] /= p[3];
	}
	// What BT.2020 does not take in is clamped below.
	primaries = widen_negative(rgba, primaries);
	auto profile = cmm_or_default(ctx)->get_profile_parametric(
		nullopt, kD65White, primaries);
	if (!profile) {
		set_error(error, _("failed to describe the colour space"));
		return false;
	}

	bool nonfinite = false, negative = false;
	float peak = 0;
	for (size_t i = 0; i < pixels; i++) {
		float *p = &rgba[i * 4];
		sanitize_hdr_pixel(p, nonfinite, negative);
		peak = max({peak, p[0], p[1], p[2]});
	}
	if (nonfinite)
		add_warning(ctx, _("Non-finite values have been replaced with zero"));
	// What stays negative past BT.2020 no display could show anyway,
	// and lossy coding leaves plenty of it as noise around black.
	if (negative)
		fprintf(stderr, "libdn: %s: negative values clamped to zero\n",
			ctx.uri.empty() ? "(data)" : ctx.uri.c_str());

	// The alternate follows the input to its peak, or rolls off onto the cap,
	// and the base rolls that off onto SDR white.
	const bool split = peak > 1;
	const double headroom = min(double(peak), kSplitHeadroom);
	const HdrRolloff to_alternate(peak, headroom), to_base(headroom, 1);
	unique_ptr<GainMap> map;
	if (split && ctx.gain_maps) {
		map = make_unique<GainMap>();
		map->width = image.width;
		map->height = image.height;
		map->data.resize(pixels);
		map->max = map->alternate_headroom = float(log2(headroom));
	}

	for (uint32_t y = 0; y < image.height; y++) {
		uint16_t *out = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++, out += 4) {
			const size_t i = size_t(y) * image.width + x;
			const float *p = &rgba[i * 4];
			const float m = max({p[0], p[1], p[2]});
			double scale = 1, gain = 0;
			if (split && m > 0) {
				const double alternate = to_alternate.apply(m);
				const double base = to_base.apply(alternate);
				scale = base / m;
				gain = (log2(alternate) - log2(base)) / log2(headroom);
			}
			out[0] = hdr_encode(float(p[2] * scale));
			out[1] = hdr_encode(float(p[1] * scale));
			out[2] = hdr_encode(float(p[0] * scale));
			out[3] = uint16_t(lround(p[3] * 65535));
			if (map)
				map->data[i] = uint16_t(lround(clamp(gain, 0., 1.) * 65535));
		}
	}

	image.effective_profile = std::move(profile);
	image.profile_assumed = false;
	image.gain_map = std::move(map);
	return true;
}

const double *
widen_negative(span<float> rgba, const double primaries[6])
{
	bool negative = false;
	for (size_t i = 0; i < rgba.size() / 4 && !negative; i++) {
		const float *p = &rgba[i * 4];
		negative = p[3] > 0 && (p[0] < 0 || p[1] < 0 || p[2] < 0);
	}
	if (!negative)
		return primaries;

	const RgbMatrix m = primaries_to_primaries(primaries, kRec2020Primaries);
	for (size_t i = 0; i < rgba.size() / 4; i++) {
		float *p = &rgba[i * 4];
		const double r = p[0], g = p[1], b = p[2];
		for (size_t c = 0; c < 3; c++)
			p[c] = float(m[0][c] * r + m[1][c] * g + m[2][c] * b);
	}
	return kRec2020Primaries;
}

bool
cicp_hdr(uint8_t primaries_code, uint8_t transfer, double primaries[6])
{
	double white[2] = {};
	return (transfer == 16 || transfer == 18) &&
		cicp_primaries(primaries_code, primaries, white) &&
		white[0] == kD65White[0] && white[1] == kD65White[1];
}

// BT.2100 PQ, from signal to linear light with 1.0 at 203 cd/m² (BT.2408).
static double
pq_to_linear(double e)
{
	const double m1 = 2610. / 16384, m2 = 2523. / 4096 * 128,
				 c1 = 3424. / 4096, c2 = 2413. / 4096 * 32,
				 c3 = 2392. / 4096 * 32;
	const double p = pow(clamp(e, 0., 1.), 1 / m2);
	return 10000. / 203 * pow(max(p - c1, 0.) / (c2 - c3 * p), 1 / m1);
}

// BT.2100 HLG's inverse OETF, from signal to scene light in [0, 1].
static double
hlg_to_scene(double e)
{
	const double a = 0.17883277, b = 1 - 4 * a, c = 0.5 - a * log(4 * a);
	e = clamp(e, 0., 1.);
	return e <= 0.5 ? e * e / 3 : (exp((e - c) / a) + b) / 12;
}

bool
split_hdr_signal(Image &image, const OpenContext &ctx, uint8_t transfer,
	const double primaries[6], double hlg_peak, bool premultiplied,
	Error *error)
{
	// The OOTF weighs by luminance: BT.2020's weights, after conversion.
	static const double kRec2020Y[3] = {0.2627, 0.6780, 0.0593};
	const RgbMatrix m = primaries_to_primaries(primaries, kRec2020Primaries);
	double weights[3] = {};
	for (size_t c = 0; c < 3; c++)
		for (size_t r = 0; r < 3; r++)
			weights[c] += kRec2020Y[r] * m[c][r];

	// BT.2100's system gamma for the display, and where reference white
	// lands on it, relative to its peak.
	const double gamma = 1.2 + 0.42 * log10(hlg_peak / 1000);
	const double white = pow(hlg_to_scene(0.75), gamma);

	const size_t pixels = size_t(image.width) * image.height;
	vector<float> rgba(pixels * 4);
	for (uint32_t y = 0; y < image.height; y++) {
		const uint16_t *p = row_u16(image, y);
		for (uint32_t x = 0; x < image.width; x++, p += 4) {
			float *out = &rgba[(size_t(y) * image.width + x) * 4];
			const double a = p[3] / 65535.;
			double e[3], luminance = 0;
			for (size_t c = 0; c < 3; c++) {
				e[c] = p[2 - c] / 65535.;
				if (premultiplied && a > 0)
					e[c] = min(e[c] / a, 1.);
				if (transfer == 18) {
					e[c] = hlg_to_scene(e[c]);
					luminance += weights[c] * e[c];
				} else {
					e[c] = pq_to_linear(e[c]);
				}
			}
			double scale = 1;
			if (transfer == 18)
				scale = luminance > 0 ? pow(luminance, gamma - 1) / white : 0;
			for (size_t c = 0; c < 3; c++)
				out[c] = float(e[c] * scale);
			out[3] = float(a);
		}
	}
	return split_hdr(image, ctx, rgba, false, primaries, error);
}

// --- Loaders -----------------------------------------------------------------

// The order is the default loading order.
constexpr Loader kLoaders[] = {
	// TRANSLATORS: What a loader reads, as the settings dialog lists it.
	// These are format names throughout, bar the odd word such as "raw
	// photos" or "(subset)"; leave the names as they are.
	{"libjpeg-turbo", &load_jpeg, N_("JPEG"), {"image/jpeg"}, {}},

	{"libwebp", &load_webp, N_("WebP"), {"image/webp"}, {}},

	// NIE not mentioned as it is practically a Wuffs internal format.
	{"Wuffs", &load_wuffs,
		N_("BMP, GIF, JPEG (subset), PNG, PNM, QOI, TARGA, WBMP, "
		   "WebP (subset)"),
		{
			"image/bmp",
			"image/gif",
			"image/jpeg",
			"image/png",
			"image/qoi",
			"image/vnd.wap.wbmp",
			"image/webp",
			// Only binary P5/P6 in practice, which Wuffs is limited to.
			"image/x-portable-anymap",
			"image/x-tga",
		}},

	{"ICNS", &load_icns, N_("ICNS"), {"image/x-icns"}, {}},

	{"Photoshop", &load_psd, N_("PSD/PSB (subset)"),
		{"image/vnd.adobe.photoshop"}, {}},

	{"OpenRaster", &load_ora, N_("OpenRaster, Krita"),
		{"image/openraster", "application/x-krita"}, {}},

	// Try to extract full-size previews from TIFF/EP-compatible raws.
	// XXX: The name should be translated, though it is an exception.
	{"TIFF/EP previews", &load_tiff_ep, N_("raw photos"), {"image/x-dcraw"},
		{}},

	{"LibRaw",
#if DAWN_WITH_LIBRAW
		&load_libraw,
#else
		{},
#endif
		N_("raw photos"), {"image/x-dcraw"}, {}},

	{"resvg", &load_resvg, N_("SVG"), {"image/svg+xml"}, {}},

	{"librsvg",
#if DAWN_WITH_LIBRSVG
		&load_librsvg,
#else
		{},
#endif
		N_("SVG"), {"image/svg+xml"}, {}},

	{"libXcursor",
#if DAWN_WITH_XCURSOR
		&load_xcursor,
#else
		{},
#endif
		N_("Xcursor"), {"image/x-xcursor"}, {}},

	// Before libheif: JPEG XL's container is ISOBMFF too, and we would rather
	// not rely on libheif rejecting an unknown ftyp brand.
	{"libjxl",
#if DAWN_WITH_LIBJXL
		&load_jxl,
#else
		{},
#endif
		N_("JPEG XL"), {"image/jxl"}, {}},

	{"libheif",
#if DAWN_WITH_LIBHEIF
		&load_heif,
#else
		{},
#endif
		N_("AVIF, HEIC, HEIF"),
		{
			"image/avif",
			"image/heic",
			"image/heif",
		},
		{}},

	{"OpenJPEG",
#if DAWN_WITH_OPENJPEG
		&load_openjpeg,
#else
		{},
#endif
		N_("JPEG 2000"),
		// Not image/jpx or image/jpm: OpenJPEG decodes neither JPX (Part 2)
		// nor compound JPM, and claiming them would only fail later.
		{"image/jp2", "image/x-jp2-codestream"}, {}},

	// LibTIFF must be after LibRaw, or it will pick up thumbnails.
	{"LibTIFF",
#if DAWN_WITH_LIBTIFF
		&load_tiff,
#else
		{},
#endif
		N_("TIFF"), {"image/tiff"}, {}},

	{"jxrlib",
#if DAWN_WITH_JXRLIB
		&load_jxr,
#else
		{},
#endif
		N_("JPEG XR"), {"image/jxr", "image/vnd.ms-photo"}, {}},

	{"libwmf",
#if DAWN_WITH_LIBWMF
		&load_libwmf,
#else
		{},
#endif
		N_("WMF"), {"image/wmf", "image/x-wmf"}, {}},

	{"Rust",
#if DAWN_WITH_DNRS
		&load_dnrs,
		N_("BMP, DDS, farbfeld, GIF, ICO, JPEG, OpenEXR, PNG, PNM, QOI, "
		   "Radiance HDR, TARGA, TIFF, WebP, XBM, XPM, ..."),
		{}, &dnrs_media_types},
#else
		{}, {}, {}, {}},
#endif

	{"ImageIO",
#if DAWN_WITH_IMAGEIO
		&load_imageio, {}, {}, &imageio_media_types},
#else
		{}, {}, {}, {}},
#endif

	// Advertises no media type on purpose: it opens PDFs that are handed to
	// it, while the browser keeps filtering them out, and nothing associates.
	{"Core Graphics PDF",
#if DAWN_WITH_CGPDF
		&load_cgpdf,
#else
		{},
#endif
		{}, {}, {}},

	{"Poppler",
#if DAWN_WITH_POPPLER
		&load_poppler,
#else
		{},
#endif
		N_("PDF"), {}, {}},
};

// A subset of shared-mime-info, chiefly motivated by the suckiness of raw
// photo formats: someone else will maintain the list of file extensions for us.
vector<string>
supported_media_types()
{
	vector<string> types;
	auto add = [&](const char *type) {
		if (find(types.begin(), types.end(), type) == types.end())
			types.emplace_back(type);
	};
	for (const Loader &loader : kLoaders) {
		if (!loader.load)
			continue;

		for (const char *type : loader.media_types)
			add(type);
		if (loader.dynamic_types)
			for (const string &type : loader.dynamic_types())
				add(type.c_str());
	}
	return types;
}

span<const Loader>
loaders()
{
	return kLoaders;
}

// --- Open --------------------------------------------------------------------

static const Loader *
find_loader(string_view name)
{
	for (const Loader &loader : kLoaders) {
		if (loader.load && name == loader.name)
			return &loader;
	}
	return nullptr;
}

static ImagePtr
try_loader(ImagePtr (*fn)(span<const uint8_t>, const OpenContext &, Error *),
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	Error local;
	Error *err = error ? error : &local;
	*err = {};

	// Might want to collect them per loader, or I don't know.
	if (ctx.warnings)
		ctx.warnings->resize(0);

	return fn(data, ctx, err);
}

ImagePtr
open_from_data(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	OpenTimingGuard timing(ctx.timing);
	if (data.empty()) {
		set_error(error, _("empty input"));
		return nullptr;
	}

	ImagePtr image;
	auto attempt = [&](const Loader &loader) {
		if ((image = try_loader(loader.load, data, ctx, error))) {
			image->loader = loader.name;
			return true;
		}

		// Loaders talk about the format they know, never about themselves.
		if (error && !error->message.empty()) {
			error->message = format_message(
				_("%s: %s"), loader.name, error->message.c_str());
		}
		return false;
	};

	if (ctx.loaders.empty()) {
		for (const Loader &loader : kLoaders) {
			if (loader.load && attempt(loader))
				break;
		}
	} else {
		for (const string &name : ctx.loaders) {
			const Loader *loader = find_loader(name);
			if (loader && attempt(*loader))
				break;
		}
	}

	if (!image) {
		if (error && error->message.empty())
			set_error(error, _("unrecognized or unsupported image format"));
		return nullptr;
	}

	// Exif only fills the gap: a loader that has already established the
	// orientation, from the codestream or from container properties it has
	// itself applied to the pixels, keeps the final say.  JPEG MPF
	// follow-ups and HEIF auxiliary images may each carry their own Exif.
	for (Image *page = image.get(); page; page = page->page_next.get()) {
		if (page->orientation != Orientation::Unknown || page->exif.empty())
			continue;

		page->orientation = exif_orientation(page->exif);
	}
	return image;
}

ImagePtr
open(const OpenContext &ctx, Error *error)
{
	OpenTimingGuard timing(ctx.timing);
	if (ctx.uri.empty()) {
		set_error(error, _("empty URI"));
		return nullptr;
	}
	unique_ptr<uint8_t[]> data;
	size_t size = 0;
	{
		StageClock clk(&OpenTiming::file_ms);
		auto path = uri_to_path(ctx.uri);
		if (!path) {
			set_error(error, _("invalid URI"));
			return nullptr;
		}

		ifstream in;
		if (!open_file(*path, in, &size, error))
			return nullptr;

		data = make_unique_for_overwrite<uint8_t[]>(size);
		if (!read_opened(in, data.get(), size, *path, error))
			return nullptr;
	}
	return open_from_data({data.get(), size}, ctx, error);
}

}  // namespace dawn
