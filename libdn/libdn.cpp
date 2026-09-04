//
// libdn.cpp: image loading and colour management
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <lcms2.h>
#if DAWN_WITH_LCMS2_FAST_FLOAT
#include <lcms2_fast_float.h>
#endif
#if DAWN_WITH_LCMS2_THREADED
#include <lcms2_threaded.h>
#endif

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <mutex>
#include <new>
#include <numbers>
#include <string_view>
#include <thread>

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

detail::StageClock::StageClock(double OpenTiming::*field)
	: acc(detail::open_timing ? &(detail::open_timing->*field) : nullptr)
{
	if (acc)
		t0 = chrono::steady_clock::now();
}

detail::StageClock::~StageClock()
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

constexpr cmsUInt32Number kTypeBgra8 = TYPE_BGRA_8;
constexpr cmsUInt32Number kTypeBgra16 = TYPE_BGRA_16;
constexpr cmsUInt32Number kTypeBgra16Premul = TYPE_BGRA_16_PREMUL;
// Extra (alpha) is omitted unless this is set. Out-of-place 8→16 into a
// zeroed buffer would leave A=0; TYPE_*_PREMUL then zeros RGB.
constexpr cmsUInt32Number kTransformFlags = cmsFLAGS_COPY_ALPHA;

// Packed buffers have no row padding, so a band is one contiguous pixel run.
// One cmsHTRANSFORM is not thread-safe. The lcms2 threaded plugin (when built)
// wraps DoTransform and slices internally — do not nest our own pool on top.
// Without the plugin, create one transform per worker on this thread.
#if !DAWN_WITH_LCMS2_THREADED
constexpr uint64_t kCmsMinPixels = 256ull * 256ull;

static unsigned
cms_workers(uint32_t height)
{
	unsigned n = thread::hardware_concurrency();
	if (n < 1)
		n = 1;
	return min(n, max(1u, height));
}
#endif

static bool
transform_tiled(cmsContext ctx, cmsHPROFILE src_h, cmsUInt32Number src_fmt,
	cmsHPROFILE dst_h, cmsUInt32Number dst_fmt, const uint8_t *src,
	uint8_t *dst, uint32_t width, uint32_t height, size_t src_bpp,
	size_t dst_bpp)
{
	auto create = [&]() -> cmsHTRANSFORM {
		return cmsCreateTransformTHR(ctx, src_h, src_fmt, dst_h, dst_fmt,
			INTENT_PERCEPTUAL, kTransformFlags);
	};

#if DAWN_WITH_LCMS2_THREADED
	cmsHTRANSFORM xform = create();
	if (!xform)
		return false;
	cmsDoTransformLineStride(xform, src, dst, width, height,
		cmsUInt32Number(size_t(width) * src_bpp),
		cmsUInt32Number(size_t(width) * dst_bpp), 0, 0);
	cmsDeleteTransform(xform);
	return true;
#else
	const uint64_t npx = uint64_t(width) * height;
	const size_t src_stride = size_t(width) * src_bpp;
	const size_t dst_stride = size_t(width) * dst_bpp;

	auto run_band = [&](cmsHTRANSFORM xform, uint32_t y0, uint32_t y1) {
		if (y0 >= y1)
			return;
		cmsDoTransform(xform, src + size_t(y0) * src_stride,
			dst + size_t(y0) * dst_stride,
			cmsUInt32Number(uint64_t(width) * (y1 - y0)));
	};

	if (npx < kCmsMinPixels || height < 2) {
		cmsHTRANSFORM xform = create();
		if (!xform)
			return false;
		run_band(xform, 0, height);
		cmsDeleteTransform(xform);
		return true;
	}

	const unsigned n = cms_workers(height);
	vector<cmsHTRANSFORM> xforms(n);
	for (unsigned i = 0; i < n; i++) {
		xforms[i] = create();
		if (!xforms[i]) {
			for (unsigned j = 0; j < i; j++)
				cmsDeleteTransform(xforms[j]);
			return false;
		}
	}

	vector<thread> pool;
	if (n > 1)
		pool.reserve(n - 1);
	for (unsigned i = 1; i < n; i++) {
		const uint32_t y0 = uint32_t(uint64_t(height) * i / n);
		const uint32_t y1 = uint32_t(uint64_t(height) * (i + 1) / n);
		if (y0 >= y1)
			continue;
		pool.emplace_back([&, i, y0, y1] { run_band(xforms[i], y0, y1); });
	}
	run_band(xforms[0], 0, uint32_t(uint64_t(height) / n));
	for (thread &t : pool)
		t.join();
	for (cmsHTRANSFORM xform : xforms)
		cmsDeleteTransform(xform);
	return true;
#endif
}

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

// --- Image -------------------------------------------------------------------

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
	if (height > UINT32_MAX / stride)
		return nullptr;

	auto image = make_shared<Image>();
	try {
		detail::StageClock clk(&OpenTiming::alloc_ms);
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
	detail::StageClock clk(&OpenTiming::widen_ms);
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
		const uint32_t *s =
			(const uint32_t *) ((const uint8_t *) src + y * src_stride_bytes);
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
		const uint16_t *s =
			(const uint16_t *) ((const uint8_t *) src + y * src_stride_bytes);
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
		const uint16_t *s =
			(const uint16_t *) ((const uint8_t *) src + y * src_stride_bytes);
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

bool
read_file(const string &path, vector<uint8_t> *out, Error *error)
{
	ifstream in(path, ios::binary);
	if (!in) {
		if (error) {
			error->code = Error::Code::Io;
			error->message = "failed to open: " + path;
		}
		return false;
	}
	in.seekg(0, ios::end);
	auto sz = in.tellg();
	if (sz < 0) {
		if (error) {
			error->code = Error::Code::Io;
			error->message = "failed to size: " + path;
		}
		return false;
	}
	in.seekg(0, ios::beg);
	out->resize(size_t(sz));
	if (sz > 0 && !in.read((char *) out->data(), streamsize(sz))) {
		if (error) {
			error->code = Error::Code::Io;
			error->message = "failed to read: " + path;
		}
		return false;
	}
	return true;
}

string
uri_to_path(const string &uri)
{
	constexpr string_view prefix = "file://";
	if (uri.starts_with(prefix)) {
		string path = uri.substr(prefix.size());
		if (path.starts_with("localhost/"))
			path = path.substr(string_view("localhost").size());
		return path;
	}
	return uri;
}

// --- Premultiply / blend -----------------------------------------------------

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
unpremultiply_bgra8(
	uint8_t *data, uint32_t width, uint32_t height, size_t stride)
{
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *p = data + y * stride;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t b = p[0], g = p[1], r = p[2], a = p[3];
			p[0] = unpremultiply8(a, b);
			p[1] = unpremultiply8(a, g);
			p[2] = unpremultiply8(a, r);
			p[3] = a;
			p += 4;
		}
	}
}

void
ensure_working_premul(
	Image &image, const OpenContext &ctx, Profile *source, bool input_premul)
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

	Profile *target = ctx.screen_profile.get();
	if (input_premul && !target)
		return;
	if (input_premul)
		unpremultiply_bgra16(image);
	cmm_or_default(ctx)->finish_premultiply(image, source, target);
}

void
ensure_working_premul_pages(
	Image &page, const OpenContext &ctx, Profile *source, bool input_premul)
{
	// Resolve ICC once from the page head so animation frames without their
	// own profile still colour-manage against the page embedding.
	shared_ptr<Profile> owned;
	if (!source && !page.icc.empty()) {
		owned = cmm_or_default(ctx)->get_profile(page.icc);
		source = owned.get();
	}
	if (!page.effective_profile) {
		if (owned)
			page.effective_profile = owned;
		else if (!source) {
			page.effective_profile = cmm_or_default(ctx)->get_profile_sRGB();
			page.profile_assumed = true;
		}
	}
	for (Image *frame = &page; frame != nullptr;
		frame = frame->frame_next.get()) {
		if (!frame->effective_profile) {
			frame->effective_profile = page.effective_profile;
			frame->profile_assumed = page.profile_assumed;
		}
		ensure_working_premul(*frame, ctx, source, input_premul);
	}
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

// --- Profile / CMM -----------------------------------------------------------

Profile::Profile(shared_ptr<Cmm> cmm, void *cms_profile)
	: cmm_(std::move(cmm)), profile_(cms_profile)
{
}

Profile::~Profile()
{
	if (profile_)
		cmsCloseProfile(cmsHPROFILE(profile_));
}

vector<uint8_t>
Profile::to_bytes() const
{
	cmsUInt32Number len = 0;
	(void) cmsSaveProfileToMem(cmsHPROFILE(profile_), nullptr, &len);
	vector<uint8_t> data(len);
	if (!cmsSaveProfileToMem(cmsHPROFILE(profile_), data.data(), &len))
		return {};
	data.resize(len);
	return data;
}

bool
profiles_equal(const Profile *a, const Profile *b)
{
	if (a == b)
		return true;
	if (!a || !b)
		return false;
	return a->to_bytes() == b->to_bytes();
}

float
transfer_decode(float encoded, Transfer transfer)
{
	switch (transfer) {
	case Transfer::Srgb:
		if (encoded > 0.04045f)
			return powf((encoded + 0.055f) / 1.055f, 2.4f);
		return encoded / 12.92f;
	case Transfer::AdobeRgb:
		return powf(fmaxf(encoded, 0.f), 2.2f);
	case Transfer::Linear:
		break;
	}
	return encoded;
}

float
transfer_encode(float linear, Transfer transfer)
{
	switch (transfer) {
	case Transfer::Srgb:
		linear = fminf(fmaxf(linear, 0.f), 1.f);
		if (linear > 0.0031308f)
			return 1.055f * powf(linear, 1.f / 2.4f) - 0.055f;
		return linear * 12.92f;
	case Transfer::AdobeRgb:
		return powf(fminf(fmaxf(linear, 0.f), 1.f), 1.f / 2.2f);
	case Transfer::Linear:
		break;
	}
	return linear;
}

constexpr int kTransferSamples = 1024;
constexpr float kTransferMaxErr = 1.5f / 65535.f;

static bool
curve_matches(const cmsToneCurve *curve, Transfer transfer)
{
	for (int i = 0; i < kTransferSamples; ++i) {
		const float x = float(i) / float(kTransferSamples - 1);
		const float y = cmsEvalToneCurveFloat(curve, x);
		if (fabsf(y - transfer_decode(x, transfer)) > kTransferMaxErr)
			return false;
	}
	return true;
}

static Transfer
classify_curve(const cmsToneCurve *curve)
{
	if (curve_matches(curve, Transfer::Linear))
		return Transfer::Linear;
	if (curve_matches(curve, Transfer::Srgb))
		return Transfer::Srgb;
	if (curve_matches(curve, Transfer::AdobeRgb))
		return Transfer::AdobeRgb;
	return Transfer::Srgb;
}

static void
xyz_to_xy(const cmsCIEXYZ &xyz, double *x, double *y)
{
	const double s = xyz.X + xyz.Y + xyz.Z;
	if (s <= 0.0) {
		*x = 0;
		*y = 0;
		return;
	}
	*x = xyz.X / s;
	*y = xyz.Y / s;
}

// CIE 1931 D65, Y=1. PCS is D50; plot on a D65 xy diagram.
const cmsCIEXYZ kD65Xyz = {0.95047, 1.0, 1.08883};

static cmsHTRANSFORM
xyz_xf(cmsContext ctx, cmsHPROFILE h, cmsUInt32Number fmt)
{
	cmsHPROFILE xyz = cmsCreateXYZProfileTHR(ctx);
	if (!xyz)
		return nullptr;
	cmsHTRANSFORM xf = cmsCreateTransformTHR(
		ctx, h, fmt, xyz, TYPE_XYZ_DBL, INTENT_RELATIVE_COLORIMETRIC, 0);
	cmsCloseProfile(xyz);
	return xf;
}

static bool
xf_xy(cmsHTRANSFORM xf, const void *pix, double *x, double *y)
{
	cmsCIEXYZ pcs{};
	cmsDoTransform(xf, pix, &pcs, 1);
	if (pcs.X + pcs.Y + pcs.Z <= 0.0)
		return false;

	cmsCIEXYZ illum{};
	if (!cmsAdaptToIlluminant(&illum, cmsD50_XYZ(), &kD65Xyz, &pcs))
		illum = pcs;

	xyz_to_xy(illum, x, y);
	return true;
}

Chromaticities
profile_chromaticities(const Profile *profile)
{
	Chromaticities c;
	if (!profile || !profile->profile_ || !profile->cmm_ ||
		!profile->cmm_->context())
		return c;

	cmsHPROFILE h = cmsHPROFILE(profile->profile_);
	cmsContext ctx = cmsContext(profile->cmm_->context());
	switch (cmsGetColorSpace(h)) {
	case cmsSigGrayData: {
		c.model = ColorModel::Gray;
		cmsHTRANSFORM xf = xyz_xf(ctx, h, TYPE_GRAY_8);
		if (!xf)
			return c;

		const uint8_t white = 255;
		c.have_white = xf_xy(xf, &white, &c.wx, &c.wy);
		cmsDeleteTransform(xf);
		break;
	}
	case cmsSigCmykData:
	case cmsSigCmyData: {
		c.model = ColorModel::Cmyk;
		cmsHTRANSFORM xf = xyz_xf(ctx, h, TYPE_CMYK_8);
		if (!xf)
			return c;

		// Winding: R Y G C B M
		const uint8_t corners[6][4] = {
			{0x00, 0xff, 0xff, 0x00},
			{0x00, 0x00, 0xff, 0x00},
			{0xff, 0x00, 0xff, 0x00},
			{0xff, 0x00, 0x00, 0x00},
			{0xff, 0xff, 0x00, 0x00},
			{0x00, 0xff, 0x00, 0x00},
		};
		const uint8_t paper[4] = {0, 0, 0, 0};
		c.have_white = xf_xy(xf, paper, &c.wx, &c.wy);
		for (int i = 0; i < 6; ++i) {
			if (!xf_xy(xf, corners[i], &c.x[i], &c.y[i])) {
				cmsDeleteTransform(xf);
				return c;
			}
		}
		cmsDeleteTransform(xf);
		c.n = 6;
		c.have_primaries = true;
		break;
	}
	case cmsSigRgbData: {
		c.model = ColorModel::Rgb;
		cmsHTRANSFORM xf = xyz_xf(ctx, h, TYPE_RGB_8);
		if (!xf)
			return c;

		const uint8_t corners[3][3] = {
			{0xff, 0x00, 0x00},
			{0x00, 0xff, 0x00},
			{0x00, 0x00, 0xff},
		};
		const uint8_t white[3] = {255, 255, 255};
		c.have_white = xf_xy(xf, white, &c.wx, &c.wy);
		for (int i = 0; i < 3; ++i) {
			if (!xf_xy(xf, corners[i], &c.x[i], &c.y[i])) {
				cmsDeleteTransform(xf);
				return c;
			}
		}
		cmsDeleteTransform(xf);
		c.n = 3;
		c.have_primaries = true;
		break;
	}
	default:
		break;
	}
	return c;
}

Transfer
profile_transfer(const Profile *profile)
{
	if (!profile || !profile->profile_)
		return Transfer::Srgb;

	cmsHPROFILE h = cmsHPROFILE(profile->profile_);
	auto *r = (cmsToneCurve *) cmsReadTag(h, cmsSigRedTRCTag);
	auto *g = (cmsToneCurve *) cmsReadTag(h, cmsSigGreenTRCTag);
	auto *b = (cmsToneCurve *) cmsReadTag(h, cmsSigBlueTRCTag);
	if (r && g && b) {
		const Transfer tr = classify_curve(r);
		if (classify_curve(g) == tr && classify_curve(b) == tr)
			return tr;
		return Transfer::Srgb;
	}

	auto *k = (cmsToneCurve *) cmsReadTag(h, cmsSigGrayTRCTag);
	if (k)
		return classify_curve(k);
	return Transfer::Srgb;
}

Cmm::Cmm()
{
	context_ = cmsCreateContext(nullptr, this);
#if DAWN_WITH_LCMS2_FAST_FLOAT
	if (cmsPluginTHR(cmsContext(context_), cmsFastFloatExtensions()))
		broken_premul_ = LCMS_VERSION <= 2160;
#endif
#if DAWN_WITH_LCMS2_THREADED
	// After fast_float: the parallelization plugin wraps whatever xform
	// the transform factory installed.
	(void) cmsPluginTHR(cmsContext(context_),
		cmsThreadedExtensions(CMS_THREADED_GUESS_MAX_THREADS, 0));
#endif
}

Cmm::~Cmm()
{
	if (context_)
		cmsDeleteContext(cmsContext(context_));
}

shared_ptr<Cmm>
Cmm::get_default()
{
	static once_flag once;
	static shared_ptr<Cmm> instance;
	call_once(once, [] { instance = make_shared<Cmm>(); });
	return instance;
}

shared_ptr<Profile>
Cmm::get_profile(const void *data, size_t len)
{
	cmsHPROFILE p = cmsOpenProfileFromMemTHR(
		cmsContext(context_), data, cmsUInt32Number(len));
	if (!p)
		return nullptr;
	return shared_ptr<Profile>(new Profile(shared_from_this(), p));
}

shared_ptr<Profile>
Cmm::get_profile(span<const uint8_t> bytes)
{
	return get_profile(bytes.data(), bytes.size());
}

shared_ptr<Profile>
Cmm::get_profile_sRGB(bool cache)
{
	// We may use these a lot, no need to recreate each time.
	if (this->cached_sRGB)
		return this->cached_sRGB;

	cmsHPROFILE p = cmsCreate_sRGBProfileTHR(cmsContext(context_));
	if (!p)
		return nullptr;

	shared_ptr<Profile> result(new Profile(shared_from_this(), p));
	if (cache)
		this->cached_sRGB = result;
	return result;
}

shared_ptr<Profile>
Cmm::get_profile_display_p3(bool cache)
{
	// We may use these a lot, no need to recreate each time.
	if (this->cached_display_p3)
		return this->cached_display_p3;

	constexpr size_t samples = 4096;
	vector<cmsUInt16Number> transfer(samples);
	for (size_t i = 0; i < samples; ++i) {
		const double encoded = double(i) / double(samples - 1);
		const double linear = encoded <= 0.04045
			? encoded / 12.92
			: pow((encoded + 0.055) / 1.055, 2.4);
		transfer[i] = cmsUInt16Number(lround(linear * 65535.0));
	}
	cmsToneCurve *curve = cmsBuildTabulatedToneCurve16(cmsContext(context_),
		cmsUInt32Number(transfer.size()), transfer.data());
	if (!curve)
		return nullptr;
	cmsToneCurve *curves[3] = {curve, curve, curve};
	const cmsCIExyY whitepoint{0.3127, 0.3290, 1.0};
	const cmsCIExyYTRIPLE primaries{
		{0.6800, 0.3200, 1.0},
		{0.2650, 0.6900, 1.0},
		{0.1500, 0.0600, 1.0},
	};
	cmsHPROFILE p = cmsCreateRGBProfileTHR(
		cmsContext(context_), &whitepoint, &primaries, curves);
	cmsFreeToneCurve(curve);
	if (!p)
		return nullptr;
	cmsSetProfileVersion(p, 4.3);

	shared_ptr<Profile> result(new Profile(shared_from_this(), p));
	if (cache)
		this->cached_display_p3 = result;
	return result;
}

shared_ptr<Profile>
Cmm::get_profile_parametric(
	double gamma, double whitepoint[2], double primaries[6])
{
	const cmsCIExyY wp{whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE prim{
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};
	cmsToneCurve *curve = cmsBuildGamma(cmsContext(context_), gamma);
	if (!curve)
		return nullptr;
	cmsToneCurve *curves[3] = {curve, curve, curve};
	cmsHPROFILE p =
		cmsCreateRGBProfileTHR(cmsContext(context_), &wp, &prim, curves);
	cmsFreeToneCurve(curve);
	if (!p)
		return nullptr;
	return shared_ptr<Profile>(new Profile(shared_from_this(), p));
}

shared_ptr<Profile>
Cmm::get_profile_sRGB_gamma(double gamma)
{
	double wp[2] = {0.3127, 0.3290};
	double prim[6] = {0.6400, 0.3300, 0.3000, 0.6000, 0.1500, 0.0600};
	return get_profile_parametric(gamma, wp, prim);
}

/// H.273 Table 2 primaries as CIE 1931 xy, in R,G,B order, plus the
/// illuminant. False for reserved, unspecified, or non-RGB code points.
static bool
cicp_primaries(uint8_t code, double primaries[6], double whitepoint[2])
{
	static const double kD65[2] = {0.3127, 0.3290};
	const double *wp = kD65;
	const double *p = nullptr;
	switch (code) {
	case 1: {  // BT.709 / sRGB
		static const double v[6] = {0.640, 0.330, 0.300, 0.600, 0.150, 0.060};
		p = v;
		break;
	}
	case 4: {  // BT.470 System M, illuminant C
		static const double v[6] = {0.670, 0.330, 0.210, 0.710, 0.140, 0.080};
		static const double c[2] = {0.310, 0.316};
		p = v;
		wp = c;
		break;
	}
	case 5: {  // BT.470 System B/G (EBU 3213)
		static const double v[6] = {0.640, 0.330, 0.290, 0.600, 0.150, 0.060};
		p = v;
		break;
	}
	case 6:    // BT.601 525 / SMPTE 170M
	case 7: {  // SMPTE 240M, identical primaries
		static const double v[6] = {0.630, 0.340, 0.310, 0.595, 0.155, 0.070};
		p = v;
		break;
	}
	case 9: {  // BT.2020 / BT.2100
		static const double v[6] = {0.708, 0.292, 0.170, 0.797, 0.131, 0.046};
		p = v;
		break;
	}
	case 11: {  // SMPTE RP 431-2 (DCI-P3), DCI white
		static const double v[6] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
		static const double dci[2] = {0.314, 0.351};
		p = v;
		wp = dci;
		break;
	}
	case 12: {  // SMPTE EG 432-1 (Display P3), D65 white
		static const double v[6] = {0.680, 0.320, 0.265, 0.690, 0.150, 0.060};
		p = v;
		break;
	}
	default:
		// 0/3 reserved, 2 unspecified, 10 is XYZ, 22 has no ICC analogue.
		return false;
	}
	memcpy(primaries, p, sizeof(double) * 6);
	memcpy(whitepoint, wp, sizeof(double) * 2);
	return true;
}

/// Tone curve for an H.273 transfer characteristic; null when unrepresentable.
static cmsToneCurve *
cicp_tone_curve(cmsContext context, uint8_t code)
{
	// lcms2 type 4 is "y = (aX+b)^g for X >= d, else cX", the shape shared
	// by the BT.709/601/2020 and sRGB curves. Parameters are {g, a, b, c, d}.
	switch (code) {
	case 1:     // BT.709
	case 6:     // BT.601
	case 14:    // BT.2020 10-bit; the curve is the same, only depth differs
	case 15: {  // BT.2020 12-bit
		const cmsFloat64Number p[5] = {
			1 / 0.45, 1 / 1.099, 0.099 / 1.099, 1 / 4.5, 4.5 * 0.018};
		return cmsBuildParametricToneCurve(context, 4, p);
	}
	case 13: {  // sRGB (IEC 61966-2-1)
		const cmsFloat64Number p[5] = {
			2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
		return cmsBuildParametricToneCurve(context, 4, p);
	}
	case 4:  // BT.470 System M
		return cmsBuildGamma(context, 2.2);
	case 5:  // BT.470 System B/G
		return cmsBuildGamma(context, 2.8);
	case 8:  // Linear
		return cmsBuildGamma(context, 1.0);
	default:
		// Notably 16 (PQ) and 18 (HLG). Both are HDR curves defined against
		// absolute or scene-referred luminance, with no ICC v2 parametric
		// equivalent; substituting an SDR curve would silently and badly
		// shift tone, so we decline and let the caller assume sRGB.
		return nullptr;
	}
}

shared_ptr<Profile>
Cmm::get_profile_cicp(uint8_t color_primaries, uint8_t transfer_characteristics)
{
	double primaries[6] = {}, whitepoint[2] = {};
	if (!cicp_primaries(color_primaries, primaries, whitepoint))
		return nullptr;

	cmsToneCurve *curve =
		cicp_tone_curve(cmsContext(context_), transfer_characteristics);
	if (!curve)
		return nullptr;

	const cmsCIExyY wp{whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE prim{
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};
	cmsToneCurve *curves[3] = {curve, curve, curve};
	cmsHPROFILE p =
		cmsCreateRGBProfileTHR(cmsContext(context_), &wp, &prim, curves);
	cmsFreeToneCurve(curve);
	if (!p)
		return nullptr;
	return shared_ptr<Profile>(new Profile(shared_from_this(), p));
}

bool
Cmm::transform_bgra16(uint8_t *data, uint32_t width, uint32_t height,
	Profile *source, Profile *target, bool source_premul, bool target_premul)
{
	shared_ptr<Profile> src_fallback;
	if (target && !source) {
		src_fallback = get_profile_sRGB();
		source = src_fallback.get();
	}
	if (!source || !target)
		return false;

	detail::StageClock clk(&OpenTiming::cms_ms);
	cmsUInt32Number src_fmt = source_premul ? kTypeBgra16Premul : kTypeBgra16;
	cmsUInt32Number dst_fmt = target_premul ? kTypeBgra16Premul : kTypeBgra16;

	return transform_tiled(cmsContext(context_), cmsHPROFILE(source->profile_),
		src_fmt, cmsHPROFILE(target->profile_), dst_fmt, data, data, width,
		height, kBytesPerPixel, kBytesPerPixel);
}

bool
Cmm::transform_bgra8_to_bgra16(const uint8_t *src, uint8_t *dst, uint32_t width,
	uint32_t height, Profile *source, Profile *target, bool target_premul)
{
	shared_ptr<Profile> src_fallback;
	if (target && !source) {
		src_fallback = get_profile_sRGB();
		source = src_fallback.get();
	}
	if (!src || !dst || !source || !target)
		return false;

	detail::StageClock clk(&OpenTiming::cms_ms);
	cmsUInt32Number dst_fmt = target_premul ? kTypeBgra16Premul : kTypeBgra16;

	return transform_tiled(cmsContext(context_), cmsHPROFILE(source->profile_),
		kTypeBgra8, cmsHPROFILE(target->profile_), dst_fmt, src, dst, width,
		height, 4, kBytesPerPixel);
}

void
Cmm::convert_cmyk8(
	Image &dst, const uint8_t *cmyk, Profile *source, Profile *target)
{
	const uint32_t n = dst.width * dst.height;
	assert(dst.data.size() >= size_t(n) * kBytesPerPixel);
	if (source && target) {
		detail::StageClock clk(&OpenTiming::cms_ms);
		// CMYK has no extra/alpha. TYPE_*_PREMUL would see A=0 and zero RGB
		// (same trap as kTransformFlags on the RGB path). Straight BGRA, then
		// force opaque — premul is then a no-op.
		cmsHTRANSFORM transform = cmsCreateTransformTHR(cmsContext(context_),
			cmsHPROFILE(source->profile_), TYPE_CMYK_8_REV,
			cmsHPROFILE(target->profile_), kTypeBgra16, INTENT_PERCEPTUAL, 0);
		if (transform) {
			cmsDoTransform(transform, cmyk, dst.data.data(), n);
			cmsDeleteTransform(transform);
			auto *out = (uint16_t *) dst.data.data();
			for (uint32_t i = 0; i < n; i++)
				out[i * 4 + 3] = 65535;
			return;
		}
	}

	auto *out = (uint16_t *) dst.data.data();
	for (uint32_t i = 0; i < n; i++) {
		int c = cmyk[i * 4 + 0], m = cmyk[i * 4 + 1], y = cmyk[i * 4 + 2],
			k = cmyk[i * 4 + 3];
		out[0] = uint16_t((k * y / 255) * 257);
		out[1] = uint16_t((k * m / 255) * 257);
		out[2] = uint16_t((k * c / 255) * 257);
		out[3] = 65535;
		out += 4;
	}
}

void
Cmm::finish_premultiply(Image &image, Profile *source, Profile *target)
{
	if (!target || broken_premul_) {
		if (target)
			(void) transform_bgra16(image.data.data(), image.width,
				image.height, source, target, false, false);
		premultiply_bgra16(image);
		return;
	}
	if (!transform_bgra16(image.data.data(), image.width, image.height, source,
			target, false, true)) {
		premultiply_bgra16(image);
	}
}

void
Cmm::finish_page(Image &page, Profile *target)
{
	shared_ptr<Profile> source;
	if (!page.icc.empty())
		source = get_profile(page.icc);
	if (!page.effective_profile) {
		if (source)
			page.effective_profile = source;
		else {
			page.effective_profile = get_profile_sRGB();
			page.profile_assumed = true;
		}
	}
	for (Image *frame = &page; frame != nullptr;
		frame = frame->frame_next.get()) {
		if (!frame->effective_profile) {
			frame->effective_profile = page.effective_profile;
			frame->profile_assumed = page.profile_assumed;
		}
		// Expects straight BGRA16; always leaves premul (CMS when target set).
		finish_premultiply(*frame, source.get(), target);
	}
}

ImagePtr
Cmm::finish(ImagePtr image, Profile *target)
{
	for (Image *page = image.get(); page != nullptr;
		page = page->page_next.get())
		finish_page(*page, target);
	return image;
}

// --- Matrix ------------------------------------------------------------------

static Matrix
matrix_identity()
{
	return {};
}

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

void
orientation_dimensions(
	const Image &image, Orientation orientation, double *w, double *h)
{
	switch (orientation) {
	case Orientation::Rotate90:
	case Orientation::Mirror90:
	case Orientation::Rotate270:
	case Orientation::Mirror270:
		*w = image.height;
		*h = image.width;
		break;
	default:
		*w = image.width;
		*h = image.height;
	}
}

Matrix
orientation_matrix(Orientation orientation, double width, double height)
{
	Matrix matrix = matrix_identity();
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

Matrix
orientation_apply(
	const Image &image, Orientation orientation, double *width, double *height)
{
	orientation_dimensions(image, orientation, width, height);
	return orientation_matrix(orientation, *width, *height);
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

// --- Loaders -----------------------------------------------------------------

namespace
{

// The order is the default loading order.
constexpr Loader kLoaders[] = {
	{"libjpeg-turbo", &detail::load_jpeg, "JPEG", {"image/jpeg"}, {}},

	{"libwebp", &detail::load_webp, "WebP", {"image/webp"}, {}},

	{"Wuffs", &detail::load_wuffs,
		"BMP, GIF, JPEG (subset), PNG, PNM, QOI, TARGA, WBMP, WebP (subset)",
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

	{"ICNS", &detail::load_icns, "ICNS", {"image/x-icns"}, {}},

	// Try to extract full-size previews from TIFF/EP-compatible raws.
	{"TIFF-EP previews", &detail::load_tiff_ep, "raw photos",
		{"image/x-dcraw"}, {}},

	{"LibRaw",
#if DAWN_WITH_LIBRAW
		&detail::load_libraw,
#else
		{},
#endif
		"raw photos", {"image/x-dcraw"}, {}},

	{"resvg", &detail::load_resvg, "SVG", {"image/svg+xml"}, {}},

	{"librsvg",
#if DAWN_WITH_LIBRSVG
		&detail::load_librsvg,
#else
		{},
#endif
		"SVG", {"image/svg+xml"}, {}},

	{"libXcursor",
#if DAWN_WITH_XCURSOR
		&detail::load_xcursor,
#else
		{},
#endif
		"Xcursor", {"image/x-xcursor"}, {}},

	// Before libheif: JPEG XL's container is ISOBMFF too, and we would rather
	// not rely on libheif rejecting an unknown ftyp brand.
	{"libjxl",
#if DAWN_WITH_LIBJXL
		&detail::load_jxl,
#else
		{},
#endif
		"JPEG XL", {"image/jxl"}, {}},

	{"libheif",
#if DAWN_WITH_LIBHEIF
		&detail::load_heif,
#else
		{},
#endif
		"AVIF, HEIC, HEIF",
		{
			"image/avif",
			"image/heic",
			"image/heif",
		},
		{}},

	{"OpenJPEG",
#if DAWN_WITH_OPENJPEG
		&detail::load_openjpeg,
#else
		{},
#endif
		"JPEG 2000",
		// Not image/jpx or image/jpm: OpenJPEG decodes neither JPX (Part 2)
		// nor compound JPM, and claiming them would only fail later.
		{"image/jp2", "image/x-jp2-codestream"}, {}},

	{"LibTIFF",
#if DAWN_WITH_LIBTIFF
		&detail::load_tiff,
#else
		{},
#endif
		"TIFF", {"image/tiff"}, {}},

	{"Glycin",
#if DAWN_WITH_GLYCIN
		&detail::load_glycin, {}, {}, &detail::glycin_media_types},
#else
		{}, {}, {}, {}},
#endif

	{"GdkPixbuf",
#if DAWN_WITH_GDKPIXBUF
		&detail::load_gdkpixbuf, {}, {}, &detail::gdkpixbuf_media_types},
#else
		{}, {}, {}, {}},
#endif
};

}  // namespace

// A subset of shared-mime-info, chiefly motivated by the suckiness of raw
// photo formats: someone else will maintain the list of file extensions for us.
std::vector<std::string>
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

std::span<const Loader>
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
	return fn(data, ctx, err);
}

ImagePtr
open_from_data(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	detail::OpenTimingGuard timing(ctx.timing);
	if (data.empty()) {
		set_error(error, "empty input");
		return nullptr;
	}

	ImagePtr image;
	auto attempt = [&](const Loader &loader) {
		if (!(image = try_loader(loader.load, data, ctx, error)))
			return false;
		image->loader = loader.name;
		return true;
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
			set_error(error, "unrecognized or unsupported image format");
		return nullptr;
	}

	if (!image->exif.empty()) {
		Orientation o = exif_orientation(image->exif);
		if (o != Orientation::Unknown)
			image->orientation = o;
	}
	return image;
}

ImagePtr
open(const OpenContext &ctx, Error *error)
{
	detail::OpenTimingGuard timing(ctx.timing);
	if (ctx.uri.empty()) {
		set_error(error, "empty URI");
		return nullptr;
	}
	vector<uint8_t> data;
	{
		detail::StageClock clk(&OpenTiming::file_ms);
		if (!read_file(uri_to_path(ctx.uri), &data, error))
			return nullptr;
	}
	return open_from_data(data, ctx, error);
}

}  // namespace dawn
