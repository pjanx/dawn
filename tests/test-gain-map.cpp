//
// test-gain-map.cpp: gain map metadata, recognition, and attachment
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn-loaders.hpp"
#include "libdn/libdn.hpp"
#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <string_view>

using namespace std;

static bool
near(double a, double b)
{
	return fabs(a - b) < 1e-5;
}

static vector<uint8_t>
fixture(const char *name)
{
	vector<uint8_t> data;
	dawn::Error error;
	CHECK(dawn::read_file(
		string(DAWN_TEST_FIXTURES_DIR) + "/" + name, &data, &error));
	return data;
}

static dawn::ImagePtr
load(const char *name, bool gain_maps, vector<string> *warnings)
{
	dawn::OpenContext ctx;
	ctx.gain_maps = gain_maps;
	ctx.warnings = warnings;
	dawn::Error error;
	auto image = dawn::open_from_data(fixture(name), ctx, &error);
	CHECK(image);
	return image;
}

static void
append_be16(vector<uint8_t> &o, uint16_t v)
{
	o.push_back(uint8_t(v >> 8));
	o.push_back(uint8_t(v));
}

static void
append_be32(vector<uint8_t> &o, uint32_t v)
{
	append_be16(o, uint16_t(v >> 16));
	append_be16(o, uint16_t(v));
}

// --- Weight ------------------------------------------------------------------

static void
test_weight()
{
	dawn::GainMap map;
	map.base_headroom = 0;
	map.alternate_headroom = 2;
	CHECK(dawn::gain_map_weight(map, 1) == 0);
	CHECK(dawn::gain_map_weight(map, 0.5) == 0);
	CHECK(near(dawn::gain_map_weight(map, 2), 0.5));
	CHECK(dawn::gain_map_weight(map, 4) == 1);
	CHECK(dawn::gain_map_weight(map, 16) == 1);
	CHECK(dawn::gain_map_weight(map, numeric_limits<float>::infinity()) == 1);
	CHECK(dawn::gain_map_weight(map, 0) == 0);
	CHECK(dawn::gain_map_weight(map, -1) == 0);
	CHECK(dawn::gain_map_weight(map, NAN) == 0);

	map.base_headroom = 1;
	CHECK(dawn::gain_map_weight(map, 2) == 0);
	CHECK(near(dawn::gain_map_weight(map, sqrtf(8)), 0.5));

	map.alternate_headroom = 1;
	CHECK(dawn::gain_map_weight(map, 4) == 0);
}

// --- ISO 21496-1 -------------------------------------------------------------

// Headrooms 0 and 2, gain within [-0.5, 2], gamma 1.25, the given offsets.
static vector<uint8_t>
iso_blob(uint8_t flags, int channels, uint32_t alternate_offset)
{
	vector<uint8_t> o = {0, 0, 0, 0, flags};
	for (uint32_t v : {0u, 1u, 2u, 1u})
		append_be32(o, v);
	for (int c = 0; c < channels; c++)
		for (uint32_t v : {uint32_t(-1), 2u, 2u + uint32_t(c), 1u, 5u, 4u, 1u,
				 64u, alternate_offset, 64u})
			append_be32(o, v);
	return o;
}

static void
check_reference(const dawn::GainMap &map)
{
	CHECK(near(map.min, -0.5));
	CHECK(near(map.max, 2));
	CHECK(near(map.gamma, 1.25));
	CHECK(near(map.offset, 1. / 64));
	CHECK(map.base_headroom == 0);
	CHECK(map.alternate_headroom == 2);
}

static void
test_iso()
{
	dawn::GainMap map;
	CHECK(dawn::parse_iso_gain_map(iso_blob(0, 1, 1), &map));
	check_reference(map);

	// Per-channel metadata of equal values is scalar metadata in effect.
	vector<uint8_t> equal = {0, 0, 0, 0, 0x80};
	for (uint32_t v : {0u, 1u, 2u, 1u})
		append_be32(equal, v);
	for (int c = 0; c < 3; c++)
		for (uint32_t v : {uint32_t(-1), 2u, 2u, 1u, 5u, 4u, 1u, 64u, 1u, 64u})
			append_be32(equal, v);
	map = {};
	CHECK(dawn::parse_iso_gain_map(equal, &map));
	check_reference(map);

	CHECK(!dawn::parse_iso_gain_map(iso_blob(0x80, 3, 1), &map));
	CHECK(!dawn::parse_iso_gain_map(iso_blob(0, 1, 0), &map));

	// A common denominator of 64.
	vector<uint8_t> common = {0, 0, 0, 1, 0x08};
	for (uint32_t v : {64u, 0u, 128u, uint32_t(-32), 128u, 80u, 1u, 1u})
		append_be32(common, v);
	map = {};
	CHECK(dawn::parse_iso_gain_map(common, &map));
	check_reference(map);

	// An HDR base, however the headrooms are ordered.
	map = {};
	CHECK(dawn::parse_iso_gain_map(iso_blob(0x04, 1, 1), &map));
	CHECK(map.base_headroom == 2 && map.alternate_headroom == 0);

	vector<uint8_t> future = iso_blob(0, 1, 1);
	future[1] = 1;
	CHECK(!dawn::parse_iso_gain_map(future, &map));
	CHECK(!dawn::parse_iso_gain_map(vector<uint8_t>{0, 0, 0, 0}, &map));
	vector<uint8_t> truncated = iso_blob(0, 1, 1);
	truncated.pop_back();
	CHECK(!dawn::parse_iso_gain_map(truncated, &map));
	vector<uint8_t> zero = iso_blob(0, 1, 1);
	fill(zero.begin() + 9, zero.begin() + 13, 0);
	CHECK(!dawn::parse_iso_gain_map(zero, &map));

	// A HEIF `tmap` item puts a version of its own in front.
	vector<uint8_t> tmap = iso_blob(0, 1, 1);
	tmap.insert(tmap.begin(), 0);
	map = {};
	CHECK(dawn::parse_tmap_gain_map(tmap, &map));
	check_reference(map);
	tmap[0] = 1;
	CHECK(!dawn::parse_tmap_gain_map(tmap, &map));
	CHECK(!dawn::parse_tmap_gain_map({}, &map));

	// A JPEG XL `jhgm` box, with an alternate's colour encoding and ICC
	// profile to skip over.
	const vector<uint8_t> blob = iso_blob(0, 1, 1);
	vector<uint8_t> jhgm = {0};
	append_be16(jhgm, uint16_t(blob.size()));
	jhgm.insert(jhgm.end(), blob.begin(), blob.end());
	jhgm.insert(jhgm.end(), {2, 0xAA, 0xBB});
	append_be32(jhgm, 3);
	jhgm.insert(jhgm.end(), {0xCC, 0xDD, 0xEE, 0xFF, 0x0A, 0x42});
	span<const uint8_t> metadata, codestream;
	CHECK(dawn::split_jhgm_bundle(jhgm, &metadata, &codestream));
	CHECK(vector<uint8_t>(metadata.begin(), metadata.end()) == blob);
	CHECK((vector<uint8_t>(codestream.begin(), codestream.end()) ==
		vector<uint8_t>{0xFF, 0x0A, 0x42}));

	jhgm.resize(jhgm.size() - 3);
	CHECK(!dawn::split_jhgm_bundle(jhgm, &metadata, &codestream));
	jhgm = {1, 0, 0, 0};
	append_be32(jhgm, 0);
	jhgm.push_back(0xFF);
	CHECK(!dawn::split_jhgm_bundle(jhgm, &metadata, &codestream));
	jhgm[0] = 0;
	CHECK(dawn::split_jhgm_bundle(jhgm, &metadata, &codestream));
	CHECK(metadata.empty() && codestream.size() == 1);
}

// --- XMP ---------------------------------------------------------------------

static string
xmp(const string &attributes, const string &elements)
{
	return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\">\n"
		   "<rdf:RDF "
		   "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">\n"
		   " <rdf:Description rdf:about=\"\"\n"
		   "   xmlns:g=\"http://ns.adobe.com/hdr-gain-map/1.0/\"\n"
		   "   g:Version=\"1.0\" " +
		attributes + ">\n" + elements +
		" </rdf:Description>\n</rdf:RDF>\n</x:xmpmeta>\n";
}

static string
seq(const string &name, const string &a, const string &b, const string &c)
{
	return "  <g:" + name + ">\n   <rdf:Seq>\n    <rdf:li>" + a +
		"</rdf:li>\n    <rdf:li>" + b + "</rdf:li>\n    <rdf:li>" + c +
		"</rdf:li>\n   </rdf:Seq>\n  </g:" + name + ">\n";
}

static void
test_hdrgm()
{
	static const string reference =
		"g:GainMapMin=\"-0.5\" g:Gamma=\"1.25\" g:HDRCapacityMin=\"0\" ";
	dawn::GainMap map;
	CHECK(dawn::parse_hdrgm_gain_map(
		xmp(reference + "g:GainMapMax=\"2\" g:HDRCapacityMax='2'", ""), &map));
	check_reference(map);

	// Elements, and equal sequences.
	map = {};
	CHECK(dawn::parse_hdrgm_gain_map(
		xmp("g:HDRCapacityMin=\"0\"",
			seq("GainMapMin", "-0.5", "-0.5", "-0.5") +
				"  <g:GainMapMax>2.0</g:GainMapMax>\n" +
				seq("Gamma", "1.25", "1.25", "1.25") +
				"  <g:HDRCapacityMax> 2 </g:HDRCapacityMax>\n"),
		&map));
	check_reference(map);

	// Unequal sequences are channel-dependent metadata.
	CHECK(!dawn::parse_hdrgm_gain_map(xmp(reference + "g:HDRCapacityMax=\"2\"",
										  seq("GainMapMax", "2", "2.1", "2")),
		&map));
	CHECK(!dawn::parse_hdrgm_gain_map(
		xmp(reference +
				"g:GainMapMax=\"2\" g:HDRCapacityMax=\"2\" "
				"g:OffsetSDR=\"0\"",
			""),
		&map));

	// Two values are required, the rest have defaults.
	CHECK(!dawn::parse_hdrgm_gain_map(
		xmp(reference + "g:HDRCapacityMax=\"2\"", ""), &map));
	CHECK(!dawn::parse_hdrgm_gain_map(
		xmp(reference + "g:GainMapMax=\"2\"", ""), &map));
	map = {};
	CHECK(dawn::parse_hdrgm_gain_map(
		xmp("g:GainMapMax=\"3\" g:HDRCapacityMax=\"3\"", ""), &map));
	CHECK(map.min == 0 && map.gamma == 1 && near(map.offset, 1. / 64) &&
		map.base_headroom == 0 && map.alternate_headroom == 3);
	CHECK(!dawn::parse_hdrgm_gain_map(
		xmp("g:GainMapMax=\"3,5\" g:HDRCapacityMax=\"3\"", ""), &map));

	map = {};
	CHECK(dawn::parse_hdrgm_gain_map(
		xmp("g:GainMapMax=\"3\" g:HDRCapacityMax=\"3\" "
			"g:BaseRenditionIsHDR=\"True\"",
			""),
		&map));
	CHECK(map.base_headroom == 3 && map.alternate_headroom == 0);
	vector<string> warnings;
	dawn::OpenContext ctx;
	ctx.warnings = &warnings;
	CHECK(!dawn::gain_map_applies(map, ctx));
	CHECK(warnings.size() == 1);

	// Neither a map from another namespace, nor a longer name.
	CHECK(!dawn::parse_hdrgm_gain_map(
		"<rdf:Description xmlns:hdrgm=\"urn:other\" hdrgm:GainMapMax=\"2\" "
		"hdrgm:HDRCapacityMax=\"2\"/>",
		&map));
	CHECK(dawn::xmp_values(xmp("g:GainMapMaxX=\"2\"", ""),
		"http://ns.adobe.com/hdr-gain-map/1.0/", "GainMapMax")
			.empty());
}

// --- Apple -------------------------------------------------------------------

static string
apple_xmp(const string &elements)
{
	return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf="
		   "\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\">"
		   "<rdf:Description rdf:about=\"\" "
		   "xmlns:HDRGainMap=\"http://ns.apple.com/HDRGainMap/1.0/\">" +
		elements + "</rdf:Description></rdf:RDF></x:xmpmeta>";
}

// Exif with an Apple maker note carrying tag 33, and 48 if not NaN.
static vector<uint8_t>
apple_exif(double maker33, double maker48)
{
	vector<uint8_t> note = {
		'A', 'p', 'p', 'l', 'e', ' ', 'i', 'O', 'S', 0, 0, 1, 'M', 'M'};
	const uint16_t count = isnan(maker48) ? 1 : 2;
	append_be16(note, count);
	uint32_t values = 14 + 2 + count * 12 + 4;
	for (auto [tag, value] : {pair{33, maker33}, pair{48, maker48}}) {
		if (isnan(value))
			continue;
		append_be16(note, uint16_t(tag));
		append_be16(note, 10);  // SRATIONAL
		append_be32(note, 1);
		append_be32(note, values);
		values += 8;
	}
	append_be32(note, 0);
	for (double value : {maker33, maker48}) {
		if (isnan(value))
			continue;
		append_be32(note, uint32_t(int32_t(lround(value * 1000000))));
		append_be32(note, 1000000);
	}

	vector<uint8_t> exif = {'M', 'M', 0, 42};
	append_be32(exif, 8);
	append_be16(exif, 1);
	append_be16(exif, 34665);  // ExifIFDPointer
	append_be16(exif, 4);
	append_be32(exif, 1);
	append_be32(exif, 26);
	append_be32(exif, 0);
	append_be16(exif, 1);
	append_be16(exif, 37500);  // MakerNote
	append_be16(exif, 7);
	append_be32(exif, uint32_t(note.size()));
	append_be32(exif, 26 + 2 + 12 + 4);
	append_be32(exif, 0);
	exif.insert(exif.end(), note.begin(), note.end());
	return exif;
}

static void
test_apple()
{
	const string version =
		"<HDRGainMap:HDRGainMapVersion>65536</HDRGainMap:HDRGainMapVersion>";
	CHECK(dawn::apple_gain_map_declared(apple_xmp("")));
	CHECK(!dawn::apple_gain_map_declared(xmp("", "")));

	// The version is what makes it valid.
	CHECK(dawn::apple_gain_map_headroom(apple_xmp(""), apple_exif(0.5, NAN)) ==
		0);
	CHECK(dawn::apple_gain_map_headroom(
			  apple_xmp(version +
				  "<HDRGainMap:HDRGainMapHeadroom>4.532783"
				  "</HDRGainMap:HDRGainMapHeadroom>"),
			  {}) == 4.532783);

	const string map_xmp = apple_xmp(version);
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(0.5, NAN)),
		exp2(1.8)));
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(0.5, 0.005)),
		exp2(-20 * 0.005 + 1.8)));
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(0.5, 0.02)),
		exp2(-0.101 * 0.02 + 1.601)));
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(1.5, NAN)),
		exp2(3.)));
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(1.5, 0.005)),
		exp2(-70 * 0.005 + 3)));
	CHECK(near(dawn::apple_gain_map_headroom(map_xmp, apple_exif(1.5, 0.1)),
		exp2(-0.303 * 0.1 + 2.303)));
	CHECK(dawn::apple_gain_map_headroom(map_xmp, {}) == 0);
	CHECK(dawn::apple_gain_map_headroom(map_xmp, apple_exif(NAN, 0.1)) == 0);

	// ImageIO reads the maker note out itself.
	CHECK(dawn::apple_gain_map_headroom_from_maker(map_xmp, 1.5, 0.005) ==
		dawn::apple_gain_map_headroom(map_xmp, apple_exif(1.5, 0.005)));
	CHECK(dawn::apple_gain_map_headroom_from_maker(map_xmp, NAN, 0) == 0);
	CHECK(dawn::apple_gain_map_headroom_from_maker(apple_xmp(""), 1.5, 0) == 0);

	// Stops that are not positive make a map with no HDR effect.
	const double none =
		dawn::apple_gain_map_headroom(map_xmp, apple_exif(1.5, 10));
	CHECK(none == 1);
	CHECK(!dawn::gain_map_applies(dawn::apple_gain_map(none), {}));

	// Texels become normalized log2 gain.
	auto pixels = dawn::image_new(3, 1);
	uint16_t *p = dawn::row_u16(*pixels, 0);
	const uint16_t texels[3] = {0, 32768, 65535};
	for (int i = 0; i < 3; i++)
		p[i * 4] = p[i * 4 + 1] = p[i * 4 + 2] = texels[i];
	auto map = dawn::make_gain_map(*pixels, dawn::apple_gain_map(4), true);
	CHECK(map && map->width == 3 && map->height == 1);
	CHECK(map->max == 2 && map->alternate_headroom == 2 && map->gamma == 1);
	const double v = pow((32768 / 65535. + 0.099) / 1.099, 1 / 0.45);
	CHECK(map->data[0] == 0 && map->data[2] == 65535);
	CHECK(abs(map->data[1] - 65535 * log2(1 + 3 * v) / 2) <= 0.5);

	p[4] = 0;
	CHECK(!dawn::make_gain_map(*pixels, dawn::apple_gain_map(4), true));
}

// --- Loading -----------------------------------------------------------------

static void
test_loading()
{
	vector<string> warnings;
	auto image = load("gainmap.jpg", true, &warnings);
	CHECK(!image->page_next);
	CHECK(image->gain_map);
	CHECK(warnings.empty());
	if (image->gain_map) {
		const dawn::GainMap &map = *image->gain_map;
		check_reference(map);
		CHECK(map.width == 16 && map.height == 12);

		auto pixels = load("gainmap-map.jpg", false, nullptr);
		bool same = map.data.size() == 16 * 12;
		for (uint32_t y = 0; same && y < 12; y++)
			for (uint32_t x = 0; x < 16; x++)
				same &=
					map.data[y * 16 + x] == dawn::row_u16(*pixels, y)[x * 4];
		CHECK(same);
	}

	image = load("gainmap.jpg", false, nullptr);
	CHECK(!image->page_next && !image->gain_map);

	image = load("gainmap-iso.jpg", true, nullptr);
	CHECK(!image->page_next && image->gain_map);
	if (image->gain_map)
		check_reference(*image->gain_map);

	// Unsupported maps and maps without an effect leave just the SDR base.
	for (const char *name : {"gainmap-colour.jpg", "gainmap-offsets.jpg",
			 "gainmap-seq.jpg", "gainmap-flat.jpg"}) {
		warnings.clear();
		image = load(name, true, &warnings);
		if (image->page_next || image->gain_map || !warnings.empty())
			test::fail("%s: expected just the SDR base", name);
	}
}

// --- Containers --------------------------------------------------------------

static void
test_geometry()
{
	auto make = [] {
		dawn::GainMap map;
		map.width = 3;
		map.height = 2;
		map.data = {0, 1, 2, 3, 4, 5};
		return map;
	};
	auto check = [](const dawn::GainMap &map, uint32_t width, uint32_t height,
					 const vector<uint16_t> &data) {
		return map.width == width && map.height == height && map.data == data;
	};

	auto map = make();
	dawn::rotate_gain_map(map, 90);
	CHECK(check(map, 2, 3, {2, 5, 1, 4, 0, 3}));
	map = make();
	dawn::rotate_gain_map(map, 180);
	CHECK(check(map, 3, 2, {5, 4, 3, 2, 1, 0}));
	map = make();
	dawn::rotate_gain_map(map, 270);
	CHECK(check(map, 2, 3, {3, 0, 4, 1, 5, 2}));
	map = make();
	dawn::rotate_gain_map(map, 0);
	CHECK(check(map, 3, 2, {0, 1, 2, 3, 4, 5}));

	map = make();
	dawn::mirror_gain_map(map, true);
	CHECK(check(map, 3, 2, {2, 1, 0, 5, 4, 3}));
	map = make();
	dawn::mirror_gain_map(map, false);
	CHECK(check(map, 3, 2, {3, 4, 5, 0, 1, 2}));

	map = make();
	CHECK(dawn::crop_gain_map(map, 1, 0, 2, 2));
	CHECK(check(map, 2, 2, {1, 2, 4, 5}));
	map = make();
	CHECK(!dawn::crop_gain_map(map, 2, 0, 2, 2));
	CHECK(!dawn::crop_gain_map(map, 0, 0, 0, 2));
	CHECK(check(map, 3, 2, {0, 1, 2, 3, 4, 5}));
}

// Fixtures that need avifenc or cjxl, which may be missing.  avifenc without
// libxml2 also drops JPEG gain maps silently, and then there is no `tmap`.
static bool
have_fixture(const char *name, string_view marker)
{
	vector<uint8_t> data;
	dawn::Error error;
	if (!dawn::read_file(
			string(DAWN_TEST_FIXTURES_DIR) + "/" + name, &data, &error)) {
		fprintf(stderr, "skipping %s: not generated\n", name);
		return false;
	}
	if (search(data.begin(), data.end(), marker.begin(), marker.end()) ==
		data.end()) {
		fprintf(stderr, "skipping %s: no %.*s\n", name, int(marker.size()),
			marker.data());
		return false;
	}
	return true;
}

// In these fixtures, the base darkens from top to bottom in its stored frame,
// and the map brightens.  Wherever the container turns them both, the two
// still go against each other, pixel for pixel.
static double
map_base_correlation(const dawn::Image &image)
{
	const dawn::GainMap &map = *image.gain_map;
	vector<double> a, b;
	for (uint32_t y = 0; y < map.height; y++)
		for (uint32_t x = 0; x < map.width; x++) {
			const auto bx = uint32_t((x + .5) * image.width / map.width),
					   by = uint32_t((y + .5) * image.height / map.height);
			a.push_back(dawn::row_u16(image, by)[bx * 4 + 1]);
			b.push_back(map.data[size_t(y) * map.width + x]);
		}

	const double n = double(a.size());
	double ma = 0, mb = 0;
	for (size_t i = 0; i < a.size(); i++)
		ma += a[i] / n, mb += b[i] / n;
	double ab = 0, aa = 0, bb = 0;
	for (size_t i = 0; i < a.size(); i++) {
		ab += (a[i] - ma) * (b[i] - mb);
		aa += (a[i] - ma) * (a[i] - ma);
		bb += (b[i] - mb) * (b[i] - mb);
	}
	return aa && bb ? ab / sqrt(aa * bb) : 0;
}

static void
test_containers()
{
	vector<string> warnings;
	if (have_fixture("gainmap.avif", "tmap")) {
		// Cropped 48×40 from 64×48, turned, and mirrored, as is the map.
		auto image = load("gainmap.avif", true, &warnings);
		CHECK(!image->page_next);
		CHECK(warnings.empty());
		CHECK(image->width == 40 && image->height == 48);
		CHECK(image->gain_map);
		if (image->gain_map) {
			check_reference(*image->gain_map);
			CHECK(
				image->gain_map->width == 10 && image->gain_map->height == 12);
			CHECK(map_base_correlation(*image) < -0.9);
		}

		image = load("gainmap.avif", false, nullptr);
		CHECK(!image->page_next && !image->gain_map);

		// A map item that is not hidden is still no page.
		image = load("gainmap-visible.avif", true, nullptr);
		CHECK(!image->page_next && image->gain_map);
	}
	if (have_fixture("gainmap-colour.avif", "tmap")) {
		warnings.clear();
		auto image = load("gainmap-colour.avif", true, &warnings);
		CHECK(!image->page_next && !image->gain_map && warnings.empty());
	}
	if (have_fixture("gainmap.jxl", "jhgm")) {
		warnings.clear();
		auto image = load("gainmap.jxl", true, &warnings);
		CHECK(!image->page_next && warnings.empty());
		CHECK(image->gain_map);
		if (image->gain_map) {
			check_reference(*image->gain_map);
			CHECK(
				image->gain_map->width == 16 && image->gain_map->height == 12);
			CHECK(map_base_correlation(*image) < -0.9);
		}

		image = load("gainmap.jxl", false, nullptr);
		CHECK(!image->gain_map);
	}
}

int
main()
{
	return test::run({{"weight", test_weight}, {"ISO 21496-1", test_iso},
		{"Ultra HDR XMP", test_hdrgm}, {"Apple", test_apple},
		{"loading", test_loading}, {"geometry", test_geometry},
		{"containers", test_containers}});
}
