//
// test-split-hdr.cpp: true HDR split into an SDR base and a gain map
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn-loaders.hpp"
#include "libdn/libdn.hpp"
#include "test.hpp"

#include <algorithm>
#include <array>
#include <cmath>

using namespace std;

using dawn::kP3Primaries;
using dawn::kRec2020Primaries;
using dawn::kRec709Primaries;

using Rgb = array<double, 3>;

// Close enough, after both UNORM16 quantizations, in linear light.
static bool
near(double got, double want)
{
	return fabs(got - want) <= 1e-3 * fabs(want) + 2e-5;
}

static bool
near_rgb(const Rgb &got, const Rgb &want)
{
	return near(got[0], want[0]) && near(got[1], want[1]) &&
		near(got[2], want[2]);
}

namespace
{

struct Split {
	dawn::ImagePtr image;
	vector<string> warnings;
	bool ok = false;
};

}  // namespace

static Split
split(uint32_t width, const vector<float> &rgb, const vector<float> &alpha,
	bool gain_maps = true, const double *primaries = kRec709Primaries,
	shared_ptr<dawn::Profile> screen = nullptr)
{
	Split result;
	result.image = dawn::image_new(width, uint32_t(rgb.size() / 3 / width));
	dawn::OpenContext ctx;
	ctx.gain_maps = gain_maps;
	ctx.warnings = &result.warnings;
	ctx.screen_profile = std::move(screen);
	vector<float> rgba(rgb.size() / 3 * 4);
	for (size_t i = 0; i < rgba.size() / 4; i++) {
		copy_n(&rgb[i * 3], 3, &rgba[i * 4]);
		rgba[i * 4 + 3] = alpha.empty() ? 1 : alpha[i];
	}
	dawn::Error error;
	result.ok =
		dawn::split_hdr(*result.image, ctx, rgba, false, primaries, &error);
	CHECK(result.ok && !error);
	if (result.ok && ctx.screen_profile)
		dawn::finish_image(
			*result.image, ctx, result.image->effective_profile.get(), false);
	return result;
}

// Straight base pixel `i`, in linear light, unless a screen profile has
// finished it: then use base_display().
static Rgb
base(const dawn::Image &image, size_t i)
{
	const uint16_t *p = dawn::row_u16(image, 0) + i * 4;
	Rgb rgb;
	for (size_t c = 0; c < 3; c++)
		rgb[c] =
			dawn::transfer_decode(p[2 - c] / 65535.f, dawn::Transfer::Srgb);
	return rgb;
}

static double
gain(const dawn::Image &image, size_t i)
{
	const dawn::GainMap &map = *image.gain_map;
	return exp2(map.data[i] / 65535. * map.max);
}

static Rgb
scaled(const Rgb &rgb, double factor)
{
	return {rgb[0] * factor, rgb[1] * factor, rgb[2] * factor};
}

static Rgb
transformed(const dawn::RgbMatrix &m, const Rgb &rgb)
{
	Rgb out{};
	for (size_t r = 0; r < 3; r++)
		for (size_t c = 0; c < 3; c++)
			out[r] += m[c][r] * rgb[c];
	return out;
}

// --- Roll-off ----------------------------------------------------------------

static void
test_rolloff()
{
	const dawn::HdrRolloff identity(3, 4);
	CHECK(identity.apply(3) == 3 && identity.apply(0.5) == 0.5);

	// What the base's tone promises, over the whole of [0, A].
	const double headroom = dawn::kSplitHeadroom;
	const dawn::HdrRolloff to_base(headroom, 1);
	double previous = 0;
	bool bounded = true, monotonic = true;
	for (int i = 1; i <= 1000; i++) {
		const double x = headroom * i / 1000, s = to_base.apply(x);
		bounded &= s >= x / headroom - 1e-12 && s <= min(x, 1.) + 1e-12;
		monotonic &= s >= previous;
		previous = s;
	}
	CHECK(bounded);
	CHECK(monotonic);
	CHECK(fabs(to_base.apply(headroom) - 1) < 1e-12);
	CHECK(to_base.apply(0.25) == 0.25);

	// The same holds for peaks far beyond PQ's.
	const dawn::HdrRolloff to_alternate(1e6, headroom);
	CHECK(fabs(to_alternate.apply(1e6) - headroom) < 1e-9);
	CHECK(fabs(to_alternate.apply(1e6 * 2) - headroom) < 1e-9);
	CHECK(to_alternate.apply(0.005) == 0.005);
	CHECK(to_alternate.apply(0) == 0);
}

// --- Splitting ---------------------------------------------------------------

static void
test_sdr()
{
	// Nothing above SDR white: no split, and no map.
	const vector<float> rgb = {1, 0.5f, 0.25f, 0.001f, 0, 0};
	auto result = split(2, rgb, {});
	CHECK(!result.image->gain_map);
	CHECK(result.warnings.empty());
	CHECK(near_rgb(base(*result.image, 0), {1, 0.5, 0.25}));
	CHECK(near_rgb(base(*result.image, 1), {0.001, 0, 0}));
	CHECK(result.image->effective_profile);
}

static void
test_round_trip()
{
	// Under the cap, base times gain is the input itself.
	vector<float> rgb = {
		3, 1.5f, 0.3f, 0.2f, 0.4f, 0.1f, 0, 0, 0, 1, 1, 1, 3, 3, 1e-6f};
	auto result = split(5, rgb, {});
	CHECK(result.image->gain_map);
	if (!result.image->gain_map)
		return;
	const dawn::GainMap &map = *result.image->gain_map;
	CHECK(map.width == 5 && map.height == 1);
	CHECK(fabs(map.max - log2(3.)) < 1e-6 && map.min == 0);
	CHECK(map.alternate_headroom == map.max && map.base_headroom == 0);
	CHECK(map.gamma == 1 && map.offset == 0);
	for (size_t i = 0; i < 5; i++) {
		const Rgb want = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]};
		if (!near_rgb(
				scaled(base(*result.image, i), gain(*result.image, i)), want))
			test::fail("pixel %zu does not come back", i);
	}
	// The peak is SDR white in the base, and the full headroom in the map.
	CHECK(dawn::row_u16(*result.image, 0)[2] == 65535);
	CHECK(map.data[0] == 65535);
	// Opaque black stays black, with no gain to it.
	CHECK(base(*result.image, 2) == Rgb{});
	CHECK(map.data[2] == 0);
	// A channel near black quantizes to zero, and no gain brings it back:
	// the loss is absolute, and tiny.
	CHECK(dawn::row_u16(*result.image, 0)[4 * 4] == 0);

	// Over the cap, it is the alternate's tone that comes back.
	rgb = {40, 10, 2, 4, 2, 1, 0.1f, 0.1f, 0.1f};
	result = split(3, rgb, {});
	CHECK(result.image->gain_map);
	if (!result.image->gain_map)
		return;
	CHECK(
		fabs(result.image->gain_map->max - log2(dawn::kSplitHeadroom)) < 1e-6);
	const dawn::HdrRolloff to_alternate(40, dawn::kSplitHeadroom);
	for (size_t i = 0; i < 3; i++) {
		const double m = rgb[i * 3];
		const Rgb want = scaled(
			{m, rgb[i * 3 + 1], rgb[i * 3 + 2]}, to_alternate.apply(m) / m);
		if (!near_rgb(
				scaled(base(*result.image, i), gain(*result.image, i)), want))
			test::fail("pixel %zu misses its alternate", i);
	}
	// Keeping RGB ratios.
	const Rgb peak = base(*result.image, 0);
	CHECK(fabs(peak[1] / peak[0] - 0.25) < 1e-3);

	// The base does not depend on whether anyone wants the map.
	auto bare = split(3, rgb, {}, false);
	CHECK(!bare.image->gain_map);
	CHECK(bare.image->data == result.image->data);
}

static void
test_sanitation()
{
	// An invisible pixel neither raises the peak nor warns.
	auto result = split(2, {0.5f, 0.5f, 0.5f, 1e9f, NAN, -1}, {1, 0});
	CHECK(!result.image->gain_map);
	CHECK(result.warnings.empty());
	const uint16_t *p = dawn::row_u16(*result.image, 0);
	CHECK(p[4] == 0 && p[5] == 0 && p[6] == 0 && p[7] == 0);

	// Non-finite values warn just once, however often they occur.
	result = split(3, {NAN, 2, 2, INFINITY, 1, 1, 0.5f, INFINITY, 0.5f}, {});
	CHECK(result.warnings.size() == 1);
	CHECK(base(*result.image, 0)[0] == 0 && base(*result.image, 1)[0] == 0);
	CHECK(result.image->gain_map);

	// What stays negative in BT.2020 clamps without a warning, small and
	// large alike: it only goes to stderr.
	result = split(1, {6, 6, -3e-4f}, {}, true, kRec2020Primaries);
	CHECK(result.warnings.empty() && base(*result.image, 0)[2] == 0);
	result = split(1, {6, 6, -7e-3f}, {}, true, kRec2020Primaries);
	CHECK(result.warnings.empty() && base(*result.image, 0)[2] == 0);

	// Infinite and NaN alpha are sanitized as well.
	result = split(2, {1, 1, 1, 1, 1, 1}, {NAN, INFINITY});
	CHECK(result.warnings.size() == 1);
	p = dawn::row_u16(*result.image, 0);
	CHECK(p[3] == 0 && p[7] == 65535);
	CHECK(p[0] == 0 && p[4] == 65535);

	// Alpha stays on the side, and the base goes out straight.
	result = split(1, {2, 2, 2}, {0.5f});
	p = dawn::row_u16(*result.image, 0);
	CHECK(p[0] == 65535 && p[3] == 32768);
}

// --- Colour management -------------------------------------------------------

// The finished base in the display's linear light, straight.
static Rgb
base_display(
	const dawn::Image &image, const dawn::ProfileEncoding &encoding, size_t i)
{
	const uint16_t *p = dawn::row_u16(image, 0) + i * 4;
	const array<float, 3> encoded = {
		p[2] / 65535.f, p[1] / 65535.f, p[0] / 65535.f};
	const auto linear = dawn::sample_curves(encoding.decode, encoded);
	return {linear[0], linear[1], linear[2]};
}

static void
test_finish()
{
	auto cmm = dawn::Cmm::get_default();
	auto p3 = cmm->get_profile_display_p3();
	const auto p3_encoding = dawn::profile_encoding(p3.get());
	CHECK(p3_encoding.matrix_trc);

	// BT.709 fits within Display P3, so the conversion stays in range,
	// and the gain carries over, to within what lcms interpolates.
	const vector<float> rgb = {4, 1, 0.5f, 2, 0.2f, 0.05f, 0.3f, 0.2f, 0.1f};
	auto result = split(3, rgb, {}, true, kRec709Primaries, p3);
	const auto to_p3 =
		dawn::primaries_to_primaries(kRec709Primaries, kP3Primaries);
	for (size_t i = 0; i < 3; i++) {
		const Rgb want =
			transformed(to_p3, {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2]});
		const Rgb got = scaled(base_display(*result.image, p3_encoding, i),
			gain(*result.image, i));
		bool agrees = true;
		for (size_t c = 0; c < 3; c++)
			agrees &= fabs(got[c] - want[c]) <= 1e-2 * fabs(want[c]) + 2e-3;
		if (!agrees)
			test::fail("pixel %zu: %g %g %g, want %g %g %g", i, got[0], got[1],
				got[2], want[0], want[1], want[2]);
	}

	// BT.2020 green lies outside sRGB: the base clips, and so does the
	// reconstruction.  That is the documented loss.
	auto srgb = cmm->get_profile_sRGB();
	const auto srgb_encoding = dawn::profile_encoding(srgb.get());
	result = split(1, {0.1f, 3, 0.1f}, {}, true, kRec2020Primaries, srgb);
	const Rgb want = transformed(
		dawn::primaries_to_primaries(kRec2020Primaries, kRec709Primaries),
		{0.1, 3, 0.1});
	CHECK(want[0] < 0 && want[2] < 0);
	const Rgb got = scaled(
		base_display(*result.image, srgb_encoding, 0), gain(*result.image, 0));
	CHECK(got[0] >= 0 && got[2] >= 0);
	CHECK(fabs(got[1] - want[1]) > 1e-2);
}

// --- Loader helpers ----------------------------------------------------------

// A row of grey BT.2100 signal, straight unless `alpha` is below 1,
// then premultiplied, split as a loader would split it.
static dawn::ImagePtr
split_signal(
	uint8_t transfer, const vector<double> &signal, double peak, double alpha)
{
	auto image = dawn::image_new(uint32_t(signal.size()), 1);
	uint16_t *p = dawn::row_u16(*image, 0);
	for (size_t i = 0; i < signal.size(); i++, p += 4) {
		p[0] = p[1] = p[2] = uint16_t(lround(signal[i] * alpha * 65535));
		p[3] = uint16_t(lround(alpha * 65535));
	}
	dawn::OpenContext ctx;
	ctx.gain_maps = true;
	dawn::Error error;
	CHECK(dawn::split_hdr_signal(
		*image, ctx, transfer, kRec2020Primaries, peak, alpha < 1, &error));
	return image;
}

static void
test_signal()
{
	// PQ puts SDR white at 203 cd/m², whatever the display.
	const double m1 = 2610. / 16384, m2 = 2523. / 4096 * 128,
				 c1 = 3424. / 4096, c2 = 2413. / 4096 * 32,
				 c3 = 2392. / 4096 * 32;
	const double y = pow(203. / 10000, m1);
	const double white = pow((c1 + c2 * y) / (1 + c3 * y), m2);
	auto image = split_signal(16, {white, 0}, 1000, 1);
	CHECK(near_rgb(base(*image, 0), {1, 1, 1}));
	CHECK(base(*image, 1) == Rgb{});

	// HLG puts it at 75 % signal, whatever the display's peak.
	for (double peak : {1000., 2000., 400.}) {
		image = split_signal(18, {0.75}, peak, 1);
		if (!near_rgb(base(*image, 0), {1, 1, 1}))
			test::fail("HLG reference white misses at %g cd/m²", peak);
	}

	// With a 1000 cd/m² display, the full signal comes to BT.2408's 4.93×,
	// which is the cap, and black stays black.
	image = split_signal(18, {1, 0}, 1000, 1);
	CHECK(image->gain_map);
	if (image->gain_map)
		CHECK(fabs(exp2(image->gain_map->max) - dawn::kSplitHeadroom) < 0.01);
	CHECK(base(*image, 1) == Rgb{});

	// Premultiplied signal is straightened before it is decoded.
	image = split_signal(18, {0.75}, 1000, 0.5);
	CHECK(near_rgb(base(*image, 0), {1, 1, 1}));
	CHECK(dawn::row_u16(*image, 0)[3] == 32768);
}

static void
test_widen()
{
	// Nothing negative, nothing changes.
	vector<float> rgba = {2, 1, 0.5f, 1, 0, 0, 0, 1};
	CHECK(dawn::widen_negative(rgba, kRec709Primaries) == kRec709Primaries);
	CHECK(rgba[0] == 2 && rgba[2] == 0.5f);

	// Nor does anything negative that is invisible.
	rgba = {2, 1, 0.5f, 1, -1, 0, 0, 0};
	CHECK(dawn::widen_negative(rgba, kRec709Primaries) == kRec709Primaries);

	// Otherwise, everything moves over to BT.2020.
	rgba = {2, 1, 0.5f, 1, -0.1f, 1, 0, 1};
	const double *primaries = dawn::widen_negative(rgba, kRec709Primaries);
	CHECK(equal(primaries, primaries + 6, kRec2020Primaries));
	const auto m =
		dawn::primaries_to_primaries(kRec709Primaries, kRec2020Primaries);
	const Rgb want[2] = {
		transformed(m, {2, 1, 0.5}), transformed(m, {-0.1, 1, 0})};
	for (size_t i = 0; i < 2; i++)
		CHECK(
			near_rgb({rgba[i * 4], rgba[i * 4 + 1], rgba[i * 4 + 2]}, want[i]));
	CHECK(rgba[4] >= 0);
}

static void
test_cicp()
{
	double primaries[6] = {};
	CHECK(dawn::cicp_hdr(9, 16, primaries));
	CHECK(equal(primaries, primaries + 6, kRec2020Primaries));
	CHECK(dawn::cicp_hdr(12, 18, primaries));
	// SDR is left to other means, and DCI-P3's white is not D65.
	CHECK(!dawn::cicp_hdr(1, 13, primaries));
	CHECK(!dawn::cicp_hdr(11, 16, primaries));
	CHECK(!dawn::cicp_hdr(2, 16, primaries));
}

int
main()
{
	return test::run({{"roll-off", test_rolloff}, {"SDR", test_sdr},
		{"round trip", test_round_trip}, {"sanitation", test_sanitation},
		{"finishing", test_finish}, {"BT.2100 signal", test_signal},
		{"negative values", test_widen}, {"cICP", test_cicp}});
}
