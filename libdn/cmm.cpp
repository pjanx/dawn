//
// cmm.cpp: colour management
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <lcms2.h>
#if DAWN_WITH_LCMS2_FAST_FLOAT
#include <lcms2_fast_float.h>
#endif
#if DAWN_WITH_LCMS2_THREADED
#include <lcms2_threaded.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstring>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

using namespace std;

namespace dawn
{

// --- Transforms --------------------------------------------------------------

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

// --- Profiles ----------------------------------------------------------------

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

	// ICC.1 lays the header out identically in every version: 128 bytes,
	// with the creation dateTimeNumber at 24.  It says when the profile
	// was made, never what it does.
	constexpr size_t header = 128, created = 24, created_size = 12;
	vector<uint8_t> x = a->to_bytes(), y = b->to_bytes();
	if (x.size() != y.size() || x.size() < header)
		return x == y;

	memset(x.data() + created, 0, created_size);
	memset(y.data() + created, 0, created_size);
	return x == y;
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
	for (int i = 0; i < kTransferSamples; i++) {
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
		for (int i = 0; i < 6; i++) {
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
		for (int i = 0; i < 3; i++) {
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

array<float, 3>
sample_curves(span<const array<float, 3>> curves, array<float, 3> rgb)
{
	for (size_t c = 0; c < 3; c++) {
		const float x = clamp(rgb[c], 0.f, 1.f) * float(curves.size() - 1);
		const size_t lo = min(size_t(x), curves.size() - 2);
		rgb[c] = lerp(curves[lo][c], curves[lo + 1][c], x - float(lo));
	}
	return rgb;
}

ProfileEncoding
profile_encoding(const Profile *profile)
{
	ProfileEncoding result;
	// A fully populated, explicitly labelled approximation on every failure.
	for (size_t i = 0; i < ProfileEncoding::kSamples; i++) {
		const float x = float(i) / float(ProfileEncoding::kSamples - 1);
		result.decode[i].fill(transfer_decode(x, Transfer::Srgb));
		result.encode[i].fill(transfer_encode(x, Transfer::Srgb));
	}
	if (!profile || !profile->profile_)
		return result;

	cmsHPROFILE h = cmsHPROFILE(profile->profile_);
	if (cmsGetColorSpace(h) != cmsSigRgbData || cmsGetPCS(h) != cmsSigXYZData ||
		!cmsIsMatrixShaper(h))
		return result;
	// A profile may contain both matrix/TRC tags and LUT transforms.
	const cmsTagSignature luts[] = {cmsSigAToB0Tag, cmsSigAToB1Tag,
		cmsSigAToB2Tag, cmsSigBToA0Tag, cmsSigBToA1Tag, cmsSigBToA2Tag,
		cmsSigDToB0Tag, cmsSigDToB1Tag, cmsSigDToB2Tag, cmsSigDToB3Tag,
		cmsSigBToD0Tag, cmsSigBToD1Tag, cmsSigBToD2Tag, cmsSigBToD3Tag};
	for (auto tag : luts)
		if (cmsIsTag(h, tag))
			return result;

	const cmsTagSignature trcs[] = {
		cmsSigRedTRCTag, cmsSigGreenTRCTag, cmsSigBlueTRCTag};
	const cmsTagSignature colorants[] = {
		cmsSigRedColorantTag, cmsSigGreenColorantTag, cmsSigBlueColorantTag};
	ProfileEncoding matrix;
	for (size_t c = 0; c < 3; c++) {
		auto *curve = (cmsToneCurve *) cmsReadTag(h, trcs[c]);
		auto *xyz = (cmsCIEXYZ *) cmsReadTag(h, colorants[c]);
		if (!curve || !xyz || !cmsIsToneCurveMonotonic(curve) ||
			cmsIsToneCurveDescending(curve) || !isfinite(xyz->X) ||
			!isfinite(xyz->Y) || !isfinite(xyz->Z))
			return result;
		matrix.rgb_to_xyz[c] = {xyz->X, xyz->Y, xyz->Z};
		cmsToneCurve *inverse =
			cmsReverseToneCurveEx(ProfileEncoding::kSamples, curve);
		if (!inverse)
			return result;
		bool valid = true;
		for (size_t i = 0; i < ProfileEncoding::kSamples; i++) {
			const float x = float(i) / float(ProfileEncoding::kSamples - 1);
			const float d = cmsEvalToneCurveFloat(curve, x);
			const float e = cmsEvalToneCurveFloat(inverse, x);
			valid &= isfinite(d) && isfinite(e) && d >= -1e-6f &&
				d <= 1.000001f && e >= -1e-6f && e <= 1.000001f;
			matrix.decode[i][c] = clamp(d, 0.f, 1.f);
			matrix.encode[i][c] = clamp(e, 0.f, 1.f);
		}
		cmsFreeToneCurve(inverse);
		if (!valid)
			return result;
	}
	const auto &m = matrix.rgb_to_xyz;
	const double det = m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
		m[1][0] * (m[0][1] * m[2][2] - m[0][2] * m[2][1]) +
		m[2][0] * (m[0][1] * m[1][2] - m[0][2] * m[1][1]);
	if (abs(det) < 1e-10)
		return result;
	matrix.matrix_trc = true;
	return matrix;
}

// --- Colour management -------------------------------------------------------

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
Cmm::get_profile_data(const void *data, size_t len)
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
	return get_profile_data(bytes.data(), bytes.size());
}

shared_ptr<Profile>
Cmm::get_profile_sRGB()
{
	// We may use these a lot, no need to recreate each time.
	if (auto cached = this->cached_sRGB.lock())
		return cached;

	cmsHPROFILE p = cmsCreate_sRGBProfileTHR(cmsContext(context_));
	if (!p)
		return nullptr;

	shared_ptr<Profile> result(new Profile(shared_from_this(), p));
	this->cached_sRGB = result;
	return result;
}

/// The IEC 61966-2-1 sRGB EOTF. lcms2 type 4 is "y = (aX+b)^g for X >= d,
/// else cX", the shape shared by this and the BT.709/601/2020 curves.
/// Parameters are {g, a, b, c, d}.
static cmsToneCurve *
srgb_tone_curve(cmsContext context)
{
	const cmsFloat64Number p[5] = {
		2.4, 1 / 1.055, 0.055 / 1.055, 1 / 12.92, 0.04045};
	return cmsBuildParametricToneCurve(context, 4, p);
}

shared_ptr<Profile>
Cmm::get_profile_display_p3()
{
	// We may use these a lot, no need to recreate each time.
	if (auto cached = this->cached_display_p3.lock())
		return cached;

	cmsToneCurve *curve = srgb_tone_curve(cmsContext(context_));
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
	this->cached_display_p3 = result;
	return result;
}

shared_ptr<Profile>
Cmm::get_profile_parametric(optional<double> gamma, const double whitepoint[2],
	const double primaries[6])
{
	const cmsCIExyY wp{whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE prim{
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};

	cmsToneCurve *curve = gamma ? cmsBuildGamma(cmsContext(context_), *gamma)
								: srgb_tone_curve(cmsContext(context_));
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

// Little CMS refuses more than 65530 entries in its 16-bit constructor,
// which a 16-bit image's 65536-entry table exceeds--the segmented float
// curve takes the whole table instead of dropping its tail.
static cmsToneCurve *
tabulated_tone_curve(cmsContext context, span<const uint16_t> table)
{
	if (table.empty())
		return nullptr;
	if (table.size() <= 65530)
		return cmsBuildTabulatedToneCurve16(
			context, cmsUInt32Number(table.size()), table.data());

	vector<cmsFloat32Number> samples(table.size());
	for (size_t i = 0; i < table.size(); i++)
		samples[i] = table[i] / 65535.f;
	return cmsBuildTabulatedToneCurveFloat(
		context, cmsUInt32Number(samples.size()), samples.data());
}

shared_ptr<Profile>
Cmm::get_profile_tabulated(const double whitepoint[2],
	const double primaries[6], span<const uint16_t> curves[3])
{
	const cmsCIExyY wp{whitepoint[0], whitepoint[1], 1.0};
	const cmsCIExyYTRIPLE prim{
		{primaries[0], primaries[1], 1.0},
		{primaries[2], primaries[3], 1.0},
		{primaries[4], primaries[5], 1.0},
	};

	cmsToneCurve *built[3] = {};
	for (int i = 0; i < 3; i++) {
		if (!(built[i] =
					tabulated_tone_curve(cmsContext(context_), curves[i]))) {
			while (i-- > 0)
				cmsFreeToneCurve(built[i]);
			return nullptr;
		}
	}

	cmsHPROFILE p =
		cmsCreateRGBProfileTHR(cmsContext(context_), &wp, &prim, built);
	for (cmsToneCurve *curve : built)
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
	// Type 4 is the piecewise shape described at srgb_tone_curve().
	switch (code) {
	case 1:     // BT.709
	case 6:     // BT.601
	case 14:    // BT.2020 10-bit; the curve is the same, only depth differs
	case 15: {  // BT.2020 12-bit
		const cmsFloat64Number p[5] = {
			1 / 0.45, 1 / 1.099, 0.099 / 1.099, 1 / 4.5, 4.5 * 0.018};
		return cmsBuildParametricToneCurve(context, 4, p);
	}
	case 13:  // sRGB (IEC 61966-2-1)
		return srgb_tone_curve(context);
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

	StageClock clk(&OpenTiming::cms_ms);
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

	StageClock clk(&OpenTiming::cms_ms);
	cmsUInt32Number dst_fmt = target_premul ? kTypeBgra16Premul : kTypeBgra16;

	return transform_tiled(cmsContext(context_), cmsHPROFILE(source->profile_),
		kTypeBgra8, cmsHPROFILE(target->profile_), dst_fmt, src, dst, width,
		height, 4, kBytesPerPixel);
}

void
Cmm::convert_cmyk8(const uint8_t *src, uint8_t *dst, uint32_t width,
	uint32_t height, Profile *source, Profile *target)
{
	const uint32_t n = width * height;
	if (source && target) {
		StageClock clk(&OpenTiming::cms_ms);
		// CMYK has no extra/alpha. TYPE_*_PREMUL would see A=0 and zero RGB
		// (same trap as kTransformFlags on the RGB path). Straight BGRA, then
		// force opaque — premul is then a no-op.
		cmsHTRANSFORM transform = cmsCreateTransformTHR(cmsContext(context_),
			cmsHPROFILE(source->profile_), TYPE_CMYK_8_REV,
			cmsHPROFILE(target->profile_), kTypeBgra16, INTENT_PERCEPTUAL, 0);
		if (transform) {
			cmsDoTransform(transform, src, dst, n);
			cmsDeleteTransform(transform);
			auto *out = assume_aligned<uint16_t>(dst);
			for (uint32_t i = 0; i < n; i++)
				out[i * 4 + 3] = 65535;
			return;
		}
	}

	auto *out = assume_aligned<uint16_t>(dst);
	for (uint32_t i = 0; i < n; i++) {
		int c = src[i * 4 + 0], m = src[i * 4 + 1], y = src[i * 4 + 2],
			k = src[i * 4 + 3];
		out[0] = uint16_t((k * y / 255) * 257);
		out[1] = uint16_t((k * m / 255) * 257);
		out[2] = uint16_t((k * c / 255) * 257);
		out[3] = 65535;
		out += 4;
	}
}

}  // namespace dawn
