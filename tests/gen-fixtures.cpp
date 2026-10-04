//
// gen-fixtures.cpp: generate image fixtures in the requested build directory
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "test.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include <lcms2.h>
#include <turbojpeg.h>
#include <webp/encode.h>

using namespace std;
using test::append_be16;
using test::append_be32;
namespace fs = filesystem;

// --- Utilities ---------------------------------------------------------------

#define PREFIX "gen-fixtures: "

static void
die(const char *msg)
{
	fprintf(stderr, PREFIX "%s\n", msg);
	exit(1);
}

static void
write_all(const fs::path &path, span<const uint8_t> data)
{
	FILE *f = fopen(path.string().c_str(), "wb");
	if (!f || fwrite(data.data(), 1, data.size(), f) != data.size() ||
		fclose(f))
		die(path.string().c_str());
}

static void
write_all(const fs::path &path, const string &s)
{
	write_all(path, span((const uint8_t *) s.data(), s.size()));
}

static vector<uint8_t>
read_all(const fs::path &path)
{
	ifstream in(path, ios::binary);
	return vector<uint8_t>(istreambuf_iterator<char>(in), {});
}

static vector<uint8_t>
bytes(const string &s)
{
	return vector<uint8_t>(s.begin(), s.end());
}

static void
append_le16(vector<uint8_t> &o, uint16_t v)
{
	o.push_back(uint8_t(v));
	o.push_back(uint8_t(v >> 8));
}

static void
append_le32(vector<uint8_t> &o, uint32_t v)
{
	o.push_back(uint8_t(v));
	o.push_back(uint8_t(v >> 8));
	o.push_back(uint8_t(v >> 16));
	o.push_back(uint8_t(v >> 24));
}

static void
put_le32(vector<uint8_t> &o, size_t at, uint32_t v)
{
	o[at + 0] = uint8_t(v);
	o[at + 1] = uint8_t(v >> 8);
	o[at + 2] = uint8_t(v >> 16);
	o[at + 3] = uint8_t(v >> 24);
}

static int
run_tool(const char *tool, initializer_list<const char *> args)
{
	string cmd = tool;
	for (const char *a : args) {
		cmd += " '";
		cmd += a;
		cmd += "'";
	}
	int rc = system(cmd.c_str());
	if (rc != 0)
		fprintf(stderr, PREFIX "warning: %s failed (%d): %s\n", tool, rc,
			cmd.c_str());
	return rc;
}

// Tools that only some fixtures need, and whose absence skips their tests.
static bool
have_tool(const char *tool)
{
	return !system((string("command -v ") + tool + " >/dev/null 2>&1").c_str());
}

// --- PNG ---------------------------------------------------------------------

static void
png_chunk(vector<uint8_t> &o, const char tag[4], const vector<uint8_t> &data)
{
	append_be32(o, uint32_t(data.size()));
	o.insert(o.end(), tag, tag + 4);
	o.insert(o.end(), data.begin(), data.end());

	uint32_t crc = ~0u;
	for (auto p = o.end() - ptrdiff_t(4 + data.size()); p != o.end(); p++) {
		crc ^= *p;
		for (int i = 0; i < 8; i++)
			crc = crc >> 1 ^ (crc & 1 ? 0xEDB88320 : 0);
	}
	append_be32(o, ~crc);
}

// Makes a zlib stream from stored deflate blocks.  The data is small,
// thus compression is not necessary.
static vector<uint8_t>
zlib_store(const vector<uint8_t> &data)
{
	vector<uint8_t> o = {0x78, 0x01};
	size_t i = 0, n = data.size();
	do {
		size_t len = min(n - i, size_t(0xFFFF));
		o.push_back(i + len == n);  // BFINAL, and BTYPE 00
		append_le16(o, uint16_t(len));
		append_le16(o, uint16_t(~len));
		o.insert(o.end(), data.begin() + ptrdiff_t(i),
			data.begin() + ptrdiff_t(i + len));
		i += len;
	} while (i < n);

	uint32_t a = 1, b = 0;
	for (uint8_t v : data) {
		a = (a + v) % 65521;
		b = (b + a) % 65521;
	}
	append_be32(o, b << 16 | a);
	return o;
}

// `raw` are filtered scanlines, and `before` and `after` are chunks
// that go around IDAT.
static void
write_png(const fs::path &path, uint32_t width, uint32_t height, uint8_t depth,
	uint8_t colour_type, const vector<uint8_t> &raw,
	const vector<uint8_t> &before, const vector<uint8_t> &after)
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, width);
	append_be32(ihdr, height);
	ihdr.insert(ihdr.end(), {depth, colour_type, 0, 0, 0});

	vector<uint8_t> o = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(o, "IHDR", ihdr);
	o.insert(o.end(), before.begin(), before.end());
	png_chunk(o, "IDAT", zlib_store(raw));
	o.insert(o.end(), after.begin(), after.end());
	png_chunk(o, "IEND", {});
	write_all(path, o);
}

static void
append_png_gama(vector<uint8_t> &o, uint32_t inverse_gamma)
{
	vector<uint8_t> data;
	append_be32(data, inverse_gamma);
	png_chunk(o, "gAMA", data);
}

static void
append_png_chrm(
	vector<uint8_t> &o, const double whitepoint[2], const double primaries[6])
{
	vector<uint8_t> data;
	for (int i = 0; i < 2; i++)
		append_be32(data, uint32_t(lround(whitepoint[i] * 1e5)));
	for (int i = 0; i < 6; i++)
		append_be32(data, uint32_t(lround(primaries[i] * 1e5)));
	png_chunk(o, "cHRM", data);
}

// --- ICC ---------------------------------------------------------------------

static vector<uint8_t>
profile_bytes(cmsHPROFILE profile)
{
	cmsUInt32Number size = 0;
	if (!profile || !cmsSaveProfileToMem(profile, nullptr, &size) || !size)
		die("cannot serialize ICC profile");
	vector<uint8_t> icc(size);
	if (!cmsSaveProfileToMem(profile, icc.data(), &size))
		die("cannot serialize ICC profile");
	icc.resize(size);
	return icc;
}

static cmsHPROFILE
create_display_p3_profile()
{
	constexpr size_t samples = 4096;
	vector<cmsUInt16Number> transfer(samples);
	for (size_t i = 0; i < samples; i++) {
		const double encoded = double(i) / double(samples - 1);
		const double linear = encoded <= 0.04045
			? encoded / 12.92
			: pow((encoded + 0.055) / 1.055, 2.4);
		transfer[i] = cmsUInt16Number(lround(linear * 65535.0));
	}
	cmsToneCurve *curve = cmsBuildTabulatedToneCurve16(
		nullptr, cmsUInt32Number(transfer.size()), transfer.data());
	if (!curve)
		die("cannot create Display P3 transfer curve");
	cmsToneCurve *curves[3] = {curve, curve, curve};
	const cmsCIExyY whitepoint{0.3127, 0.3290, 1.0};
	const cmsCIExyYTRIPLE primaries{
		{0.6800, 0.3200, 1.0},
		{0.2650, 0.6900, 1.0},
		{0.1500, 0.0600, 1.0},
	};
	cmsHPROFILE profile = cmsCreateRGBProfile(&whitepoint, &primaries, curves);
	cmsFreeToneCurve(curve);
	if (!profile)
		die("cannot create Display P3 ICC profile");
	cmsSetProfileVersion(profile, 4.3);
	cmsMLU *description = cmsMLUalloc(nullptr, 1);
	if (!description ||
		!cmsMLUsetASCII(description, "en", "US", "Display P3") ||
		!cmsWriteTag(profile, cmsSigProfileDescriptionTag, description))
		die("cannot label Display P3 ICC profile");
	cmsMLUfree(description);
	return profile;
}

static void
write_display_p3_vs_srgb_red(const fs::path &path)
{
	cmsHPROFILE display_p3 = create_display_p3_profile();
	cmsHPROFILE srgb = cmsCreate_sRGBProfile();
	if (!srgb)
		die("cannot create sRGB ICC profile");
	cmsHTRANSFORM transform = cmsCreateTransform(srgb, TYPE_RGB_8, display_p3,
		TYPE_RGB_8, INTENT_RELATIVE_COLORIMETRIC, 0);
	if (!transform)
		die("cannot create sRGB to Display P3 transform");
	const uint8_t p3_red[3] = {255, 0, 0};
	const uint8_t srgb_red[3] = {255, 0, 0};
	uint8_t srgb_red_in_p3[3] = {};
	cmsDoTransform(transform, srgb_red, srgb_red_in_p3, 1);
	cmsDeleteTransform(transform);
	cmsCloseProfile(srgb);

	vector<uint8_t> raw;
	for (int y = 0; y < 100; y++) {
		raw.push_back(0);
		for (int x = 0; x < 200; x++) {
			const uint8_t *color = x < 100 ? p3_red : srgb_red_in_p3;
			raw.insert(raw.end(), color, color + 3);
		}
	}

	const vector<uint8_t> icc = zlib_store(profile_bytes(display_p3));
	cmsCloseProfile(display_p3);
	vector<uint8_t> iccp = {
		'D', 'i', 's', 'p', 'l', 'a', 'y', ' ', 'P', '3', 0, 0};
	iccp.insert(iccp.end(), icc.begin(), icc.end());
	vector<uint8_t> chunks;
	png_chunk(chunks, "iCCP", iccp);
	write_png(path, 200, 100, 8, 2, raw, chunks, {});
}

static void
write_cmyk_lab_icc(const fs::path &path)
{
	cmsHPROFILE h = cmsCreateProfilePlaceholder(nullptr);
	if (!h)
		die("cmsCreateProfilePlaceholder");
	cmsSetProfileVersion(h, 2.1);
	cmsSetDeviceClass(h, cmsSigOutputClass);
	cmsSetColorSpace(h, cmsSigCmykData);
	cmsSetPCS(h, cmsSigLabData);
	const cmsCIEXYZ d50 = {0.9642, 1.0, 0.8249};
	cmsWriteTag(h, cmsSigMediaWhitePointTag, &d50);

	cmsUInt16Number tab[16 * 3];
	for (int i = 0; i < 16; i++) {
		tab[i * 3 + 0] = 0xFFFF;
		tab[i * 3 + 1] = 0x8080;
		tab[i * 3 + 2] = 0x8080;
	}
	cmsPipeline *lut = cmsPipelineAlloc(nullptr, 4, 3);
	cmsStage *clut = cmsStageAllocCLut16bit(nullptr, 2, 4, 3, tab);
	if (!lut || !clut)
		die("CMYK A2B0");
	cmsPipelineInsertStage(lut, cmsAT_END, clut);
	cmsWriteTag(h, cmsSigAToB0Tag, lut);
	cmsPipelineFree(lut);

	const vector<uint8_t> icc = profile_bytes(h);
	cmsCloseProfile(h);
	write_all(path, icc);
}

// --- SVG ---------------------------------------------------------------------

static void
write_svg_solid(const fs::path &path, uint8_t r, uint8_t g, uint8_t b)
{
	char fill[8];
	snprintf(fill, sizeof fill, "#%02X%02X%02X", r, g, b);
	write_all(path,
		"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"4\" height=\"4\" "
		"viewBox=\"0 0 4 4\">\n  <rect width=\"4\" height=\"4\" fill=\"" +
			string(fill) + "\" shape-rendering=\"crispEdges\"/>\n</svg>\n");
}

static void
write_svgs(const fs::path &dir)
{
	write_all(dir / "rgbw_2x2.svg",
		R"(<svg xmlns="http://www.w3.org/2000/svg" width="2" height="2" viewBox="0 0 2 2">
  <rect x="0" y="0" width="1" height="1" fill="#FF0000" shape-rendering="crispEdges"/>
  <rect x="1" y="0" width="1" height="1" fill="#00FF00" shape-rendering="crispEdges"/>
  <rect x="0" y="1" width="1" height="1" fill="#0000FF" shape-rendering="crispEdges"/>
  <rect x="1" y="1" width="1" height="1" fill="#FFFFFF" shape-rendering="crispEdges"/>
</svg>
)");
	write_all(dir / "red_a128.svg",
		R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 4 4">
  <rect width="4" height="4" fill="#FF0000" fill-opacity="0.5" shape-rendering="crispEdges"/>
</svg>
)");
}

// --- TIFF --------------------------------------------------------------------

namespace
{

// A little-endian TIFF directory under construction. Values wider than the
// four bytes an entry holds spill into an area that follows the directory,
// so its file offset has to be known before it can be serialized.
struct Ifd {
	struct Entry {
		uint16_t tag = 0, type = 0;
		uint32_t count = 0;
		vector<uint8_t> value;  ///< Little-endian, as it goes on disk
	};

	vector<Entry> entries;

	/// `value` is the tag's value, already in little-endian byte order.
	void add(
		uint16_t tag, uint16_t type, uint32_t count, vector<uint8_t> value);
	void add_short(uint16_t tag, uint16_t value);
	void add_long(uint16_t tag, uint32_t value);
	void add_shorts(uint16_t tag, const vector<uint16_t> &values);
	void add_rationals(uint16_t tag, const vector<double> &values);
};

// A whole classic TIFF: a header, blobs, and a chain of image directories.
struct TiffFile {
	vector<uint8_t> out = {'I', 'I', 42, 0, 0, 0, 0, 0};
	size_t link = 4;  ///< Where the next directory's offset belongs

	uint32_t blob(const vector<uint8_t> &data);
	uint32_t directory(const Ifd &ifd);
	void page(const Ifd &ifd);
};

}  // namespace

void
Ifd::add(uint16_t tag, uint16_t type, uint32_t count, vector<uint8_t> value)
{
	entries.push_back({tag, type, count, std::move(value)});
}

void
Ifd::add_short(uint16_t tag, uint16_t value)
{
	vector<uint8_t> v;
	append_le16(v, value);
	add(tag, 3 /* SHORT */, 1, std::move(v));
}

void
Ifd::add_long(uint16_t tag, uint32_t value)
{
	vector<uint8_t> v;
	append_le32(v, value);
	add(tag, 4 /* LONG */, 1, std::move(v));
}

void
Ifd::add_shorts(uint16_t tag, const vector<uint16_t> &values)
{
	vector<uint8_t> v;
	for (uint16_t value : values)
		append_le16(v, value);
	add(tag, 3 /* SHORT */, uint32_t(values.size()), std::move(v));
}

void
Ifd::add_rationals(uint16_t tag, const vector<double> &values)
{
	vector<uint8_t> v;
	for (double value : values) {
		append_le32(v, uint32_t(llround(value * 1000000)));
		append_le32(v, 1000000);
	}
	add(tag, 5 /* RATIONAL */, uint32_t(values.size()), std::move(v));
}

uint32_t
TiffFile::blob(const vector<uint8_t> &data)
{
	if (out.size() & 1)
		out.push_back(0);
	uint32_t offset = uint32_t(out.size());
	out.insert(out.end(), data.begin(), data.end());
	return offset;
}

uint32_t
TiffFile::directory(const Ifd &ifd)
{
	vector<Ifd::Entry> sorted = ifd.entries;
	sort(sorted.begin(), sorted.end(),
		[](const Ifd::Entry &a, const Ifd::Entry &b) { return a.tag < b.tag; });

	if (out.size() & 1)
		out.push_back(0);
	uint32_t base = uint32_t(out.size());
	uint32_t spill = base + 2 + 12 * uint32_t(sorted.size()) + 4;

	vector<uint8_t> spilled;
	append_le16(out, uint16_t(sorted.size()));
	for (const Ifd::Entry &e : sorted) {
		append_le16(out, e.tag);
		append_le16(out, e.type);
		append_le32(out, e.count);
		if (e.value.size() <= 4) {
			out.insert(out.end(), e.value.begin(), e.value.end());
			out.insert(out.end(), 4 - e.value.size(), 0);
		} else {
			if (spilled.size() & 1)
				spilled.push_back(0);
			append_le32(out, spill + uint32_t(spilled.size()));
			spilled.insert(spilled.end(), e.value.begin(), e.value.end());
		}
	}
	append_le32(out, 0);
	out.insert(out.end(), spilled.begin(), spilled.end());
	return base;
}

void
TiffFile::page(const Ifd &ifd)
{
	uint32_t base = directory(ifd);
	put_le32(out, link, base);
	link = base + 2 + 12 * ifd.entries.size();
}

// Appends a 1x1 uncompressed image directory of `samples` per pixel,
// keeping whatever colorimetry the caller has already put in `ifd`.
static void
tiff_page(TiffFile &f, Ifd ifd, uint16_t bps, uint16_t photometric,
	const vector<uint16_t> &samples)
{
	vector<uint8_t> pixel;
	for (uint16_t sample : samples) {
		if (bps == 16)
			append_le16(pixel, sample);
		else
			pixel.push_back(uint8_t(sample));
	}

	auto spp = uint16_t(samples.size());
	ifd.add_long(256, 1);  // ImageWidth
	ifd.add_long(257, 1);  // ImageLength
	ifd.add_shorts(258, vector<uint16_t>(spp, bps));
	ifd.add_short(259, 1);  // Compression: none
	ifd.add_short(262, photometric);
	ifd.add_long(273, f.blob(pixel));  // StripOffsets
	ifd.add_short(277, spp);
	ifd.add_long(278, 1);                       // RowsPerStrip
	ifd.add_long(279, uint32_t(pixel.size()));  // StripByteCounts
	ifd.add_short(284, 1);                      // PlanarConfiguration: chunky
	ifd.add_shorts(339, vector<uint16_t>(spp, 1));  // SampleFormat: UINT
	f.page(ifd);
}

// D65 white with Adobe RGB (1998) primaries, as TIFF states them.
static void
add_adobe_rgb(Ifd &ifd)
{
	ifd.add_rationals(318, {0.3127, 0.3290});
	ifd.add_rationals(319, {0.6400, 0.3300, 0.2100, 0.7100, 0.1500, 0.0600});
}

// Tables mapping encoded values to linear intensity, per TIFF's
// TransferFunction--one run per gamma given, all of the same length.
static void
add_transfer_function(Ifd &ifd, uint16_t bps, const vector<double> &gammas)
{
	size_t n = size_t(1) << bps;
	vector<uint8_t> tables;
	for (double gamma : gammas)
		for (size_t i = 0; i < n; i++)
			append_le16(tables,
				uint16_t(
					llround(65535 * pow(double(i) / double(n - 1), gamma))));
	ifd.add(301, 3 /* SHORT */, uint32_t(gammas.size() * n), std::move(tables));
}

// A mixed, unsaturated colour, so that a conversion out of a wider gamut
// cannot be hidden by clipping in sRGB.
static const vector<uint16_t> kTiffMid8 = {192, 128, 96};
static const vector<uint16_t> kTiffMid16 = {192 * 257, 128 * 257, 96 * 257};

static void
write_tiff_fixtures(const fs::path &out)
{
	{
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "adobergb16.tif", f.out);
	}
	{
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "adobergb8.tif", f.out);
	}
	{
		// Chromaticities on a non-RGB raster describe nothing about it.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 16, 1 /* MINISBLACK */, {128 * 257});
		write_all(out / "grey-primaries.tif", f.out);
	}
	{
		// The only colour statement the corpus's Olympus TIFFs carry.
		TiffFile f;
		Ifd exif;
		exif.add_short(40961, 1);  // ColorSpace: sRGB
		Ifd ifd;
		ifd.add_long(34665, f.directory(exif));  // ExifIFD
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "exif-srgb.tif", f.out);
	}
	{
		TiffFile f;
		Ifd exif;
		exif.add_rationals(42240, {2.2});  // Gamma
		Ifd ifd;
		add_adobe_rgb(ifd);
		ifd.add_long(34665, f.directory(exif));
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "exif-gamma.tif", f.out);
	}
	{
		// An unreadable Exif offset and one landing mid-header, around a
		// good one: none may cost us a page, or the pixels of one.
		TiffFile f;
		Ifd exif;
		exif.add_short(40961, 1);  // ColorSpace: sRGB
		Ifd unreadable, valid, malformed;
		unreadable.add_long(34665, 0xFFFFFF00);
		valid.add_long(34665, f.directory(exif));
		malformed.add_long(34665, 3);
		tiff_page(f, unreadable, 8, 2 /* RGB */, kTiffMid8);
		tiff_page(f, valid, 8, 2 /* RGB */, {96, 128, 192});
		tiff_page(f, malformed, 8, 2 /* RGB */, {128, 192, 96});
		write_all(out / "exif-pages.tif", f.out);
	}
	{
		// One shared curve, which libtiff hands out for all three channels.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		add_transfer_function(ifd, 8, {1.0});
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "transfer8.tif", f.out);
	}
	{
		// libtiff surrenders only the first of a palette image's three
		// TransferFunction tables, so none of them may be used.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		add_transfer_function(ifd, 8, {1.0, 2.0, 3.0});
		vector<uint16_t> colormap(3 * 256);
		colormap[1] = 192 * 257;
		colormap[256 + 1] = 128 * 257;
		colormap[512 + 1] = 96 * 257;
		ifd.add_shorts(320, colormap);
		tiff_page(f, ifd, 8, 3 /* PALETTE */, {1});
		write_all(out / "palette-transfer.tif", f.out);
	}
	{
		// Three distinct curves, long enough to need the float constructor.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		add_transfer_function(ifd, 16, {1.5, 2.0, 3.0});
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "transfer16.tif", f.out);
	}
}

// --- JPEG, WebP, BMP, TGA ----------------------------------------------------

// Encodes 8-bit RGB pixels, or grey pixels for TJSAMP_GRAY.
static vector<uint8_t>
encode_jpeg(const vector<uint8_t> &pixels, int width, int height, int subsamp,
	int quality, bool progressive)
{
	tjhandle tj = tj3Init(TJINIT_COMPRESS);
	if (!tj)
		die("tj3Init failed");
	tj3Set(tj, TJPARAM_SUBSAMP, subsamp);
	tj3Set(tj, TJPARAM_QUALITY, quality);
	tj3Set(tj, TJPARAM_PROGRESSIVE, progressive);

	unsigned char *jpeg = nullptr;
	size_t len = 0;
	int format = subsamp == TJSAMP_GRAY ? TJPF_GRAY : TJPF_RGB;
	if (tj3Compress8(tj, pixels.data(), width, 0, height, format, &jpeg, &len))
		die(tj3GetErrorStr(tj));
	vector<uint8_t> o(jpeg, jpeg + len);
	tj3Free(jpeg);
	tj3Destroy(tj);
	return o;
}

// A vertical gradient from `top` to `bottom`, with their number of channels.
static vector<uint8_t>
gradient(int width, int height, const vector<uint8_t> &top,
	const vector<uint8_t> &bottom)
{
	vector<uint8_t> o;
	for (int y = 0; y < height; y++)
		for (int x = 0; x < width; x++)
			for (size_t i = 0; i < top.size(); i++)
				o.push_back(uint8_t(lround(
					top[i] + (bottom[i] - top[i]) * double(y) / (height - 1))));
	return o;
}

static void
write_webp(const fs::path &path, uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t rgb[] = {r, g, b};
	uint8_t *webp = nullptr;
	size_t len = WebPEncodeLosslessRGB(rgb, 1, 1, sizeof rgb, &webp);
	if (!len)
		die("WebP encoding failed");
	write_all(path, span(webp, len));
	WebPFree(webp);
}

// A 1x1 BMP with a BITMAPINFOHEADER, and a 24-bit row padded to 4 bytes.
static void
write_bmp(const fs::path &path, uint8_t r, uint8_t g, uint8_t b)
{
	vector<uint8_t> o = {'B', 'M'};
	for (uint32_t v : {58u, 0u, 54u, 40u, 1u, 1u})
		append_le32(o, v);
	append_le16(o, 1);   // Planes
	append_le16(o, 24);  // Bits per pixel
	for (uint32_t v : {0u, 4u, 0u, 0u, 0u, 0u})
		append_le32(o, v);
	o.insert(o.end(), {b, g, r, 0});
	write_all(path, o);
}

// A 1x1 uncompressed true colour TGA, with the origin at the top left.
static void
write_tga(const fs::path &path, uint8_t r, uint8_t g, uint8_t b)
{
	const uint8_t o[] = {
		0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 1, 0, 24, 0x20, b, g, r};
	write_all(path, o);
}

static void
write_pgm(
	const fs::path &path, const vector<uint8_t> &pixels, int width, int height)
{
	string s = "P5\n" + to_string(width) + " " + to_string(height) + "\n255\n";
	s.append(pixels.begin(), pixels.end());
	write_all(path, s);
}

// --- Gain maps ---------------------------------------------------------------

// A marker segment whose payload starts with a NUL-terminated identifier.
static void
append_jpeg_app(vector<uint8_t> &o, uint8_t marker, const string &id,
	const vector<uint8_t> &payload)
{
	o.push_back(0xFF);
	o.push_back(marker);
	append_be16(o, uint16_t(2 + id.size() + 1 + payload.size()));
	o.insert(o.end(), id.begin(), id.end());
	o.push_back(0);
	o.insert(o.end(), payload.begin(), payload.end());
}

static const string kXmpNs = "http://ns.adobe.com/xap/1.0/";
static const string kIsoNs = "urn:iso:std:iso:ts:21496:-1";

static string
xmp_packet(const string &description)
{
	return "<x:xmpmeta xmlns:x=\"adobe:ns:meta/\"><rdf:RDF xmlns:rdf="
		   "\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\"><rdf:Description "
		   "rdf:about=\"\" "
		   "xmlns:hdrgm=\"http://ns.adobe.com/hdr-gain-map/1.0/\" "
		   "xmlns:Container=\"http://ns.google.com/photos/1.0/container/\" "
		   "xmlns:Item=\"http://ns.google.com/photos/1.0/container/item/\" " +
		description + "</rdf:Description></rdf:RDF></x:xmpmeta>";
}

// ISO 21496-1 metadata for one channel: headrooms 0 and 2, the gain within
// [-0.5, 2], gamma 1.25, offsets 1/64--as the hdrgm XMP of write_gain_maps().
static vector<uint8_t>
iso_gain_map_metadata()
{
	vector<uint8_t> o = {0, 0, 0, 0, 0};
	for (uint32_t v :
		{0u, 1u, 2u, 1u, uint32_t(-1), 2u, 2u, 1u, 5u, 4u, 1u, 64u, 1u, 64u})
		append_be32(o, v);
	return o;
}

/// An Ultra HDR JPEG: the base with a Multi-Picture Format index of two
/// images, then the map.  Each image gets `*_app` segments right after SOI.
static void
write_mpf(const fs::path &path, const vector<uint8_t> &base,
	const vector<uint8_t> &base_app, const vector<uint8_t> &map,
	const vector<uint8_t> &map_app)
{
	vector<uint8_t> second = {0xFF, 0xD8};
	second.insert(second.end(), map_app.begin(), map_app.end());
	second.insert(second.end(), map.begin() + 2, map.end());

	// The MPF header starts after SOI, the application segments,
	// APP2's marker and length, and the MPF identifier.
	const size_t mpf_payload = 4 + 8 + 2 + 3 * 12 + 4 + 2 * 16;
	const size_t origin = 2 + base_app.size() + 4 + 4;
	const size_t first_size =
		2 + base_app.size() + 4 + mpf_payload + (base.size() - 2);

	vector<uint8_t> mpf = {'M', 'M', 0, 42};
	append_be32(mpf, 8);
	append_be16(mpf, 3);
	append_be16(mpf, 0xB000);  // MPFVersion
	append_be16(mpf, 7);
	append_be32(mpf, 4);
	mpf.insert(mpf.end(), {'0', '1', '0', '0'});
	append_be16(mpf, 0xB001);  // NumberOfImages
	append_be16(mpf, 4);
	append_be32(mpf, 1);
	append_be32(mpf, 2);
	append_be16(mpf, 0xB002);  // MPEntry
	append_be16(mpf, 7);
	append_be32(mpf, 32);
	append_be32(mpf, 8 + 2 + 3 * 12 + 4);
	append_be32(mpf, 0);
	append_be32(mpf, 0x20030000);  // Representative, primary
	append_be32(mpf, uint32_t(first_size));
	append_be32(mpf, 0);
	append_be32(mpf, 0);
	append_be32(mpf, 0);  // Undefined, as gain maps are
	append_be32(mpf, uint32_t(second.size()));
	append_be32(mpf, uint32_t(first_size - origin));
	append_be32(mpf, 0);

	vector<uint8_t> out = {0xFF, 0xD8};
	out.insert(out.end(), base_app.begin(), base_app.end());
	append_jpeg_app(out, 0xE2, "MPF", mpf);
	out.insert(out.end(), base.begin() + 2, base.end());
	if (out.size() != first_size)
		die("MPF size mismatch");
	out.insert(out.end(), second.begin(), second.end());
	write_all(path, out);
}

// Clears the hidden flag of every hidden `av01` item, which in these files
// is just the gain map, making it a top-level image.
static void
reveal_av01_items(vector<uint8_t> &avif)
{
	for (size_t i = 4; i + 16 <= avif.size(); i++) {
		if (memcmp(&avif[i], "infe", 4))
			continue;

		// Version, flags, item_ID, item_protection_index, item_type.
		const uint8_t version = avif[i + 4];
		const size_t type = i + 8 + (version >= 3 ? 4 : 2) + 2;
		if (version >= 2 && type + 4 <= avif.size() &&
			!memcmp(&avif[type], "av01", 4))
			avif[i + 7] &= uint8_t(~1u);
	}
}

// libavif puts the base's clap, irot and imir on the map item as well, the
// crop unscaled, which the map is a quarter the size to exercise.  avifenc
// only converts JPEG gain maps when built with libxml2, and otherwise drops
// them silently, which the tests notice and skip.
static void
write_gain_map_avifs(const fs::path &out)
{
	const string jpeg = (out / "gainmap.jpg").string(),
				 colour = (out / "gainmap-colour.jpg").string(),
				 avif = (out / "gainmap.avif").string();
	if (run_tool("avifenc",
			{"-q", "100", "--irot", "1", "--imir", "1", "--crop", "8,4,48,40",
				jpeg.c_str(), avif.c_str()}))
		return;

	vector<uint8_t> visible = read_all(avif);
	reveal_av01_items(visible);
	write_all(out / "gainmap-visible.avif", visible);

	run_tool("avifenc",
		{"-q", "100", colour.c_str(),
			(out / "gainmap-colour.avif").string().c_str()});
}

static void
append_box(vector<uint8_t> &o, const char type[4], const vector<uint8_t> &data)
{
	append_be32(o, uint32_t(8 + data.size()));
	o.insert(o.end(), type, type + 4);
	o.insert(o.end(), data.begin(), data.end());
}

// A JPEG XL container with the base as `jxlc`, and a `jhgm` bundle after it
// that carries the same ISO 21496-1 metadata as gainmap-iso.jpg.
static void
write_gain_map_jxl(const fs::path &out)
{
	const fs::path base_pgm = out / "gainmap-base.pgm",
				   map_pgm = out / "gainmap-map.pgm",
				   base_jxl = out / "gainmap-base.jxl",
				   map_jxl = out / "gainmap-map.jxl";
	if (run_tool("cjxl",
			{"--quiet", "-d", "0", base_pgm.string().c_str(),
				base_jxl.string().c_str()}) ||
		run_tool("cjxl",
			{"--quiet", "-d", "0", map_pgm.string().c_str(),
				map_jxl.string().c_str()}))
		return;

	// Both must be naked codestreams.
	const vector<uint8_t> base = read_all(base_jxl), map = read_all(map_jxl);
	if (base.size() < 2 || base[0] != 0xFF || base[1] != 0x0A ||
		map.size() < 2 || map[0] != 0xFF || map[1] != 0x0A) {
		fprintf(
			stderr, PREFIX "warning: cjxl made no naked codestreams\n");
		return;
	}

	// As libjxl's JxlGainMapWriteBundle() would, which it does not export:
	// version 0, the metadata, no colour encoding, no ICC profile, the map.
	const vector<uint8_t> metadata = iso_gain_map_metadata();
	vector<uint8_t> jhgm = {0};
	append_be16(jhgm, uint16_t(metadata.size()));
	jhgm.insert(jhgm.end(), metadata.begin(), metadata.end());
	jhgm.push_back(0);
	append_be32(jhgm, 0);
	jhgm.insert(jhgm.end(), map.begin(), map.end());

	vector<uint8_t> o;
	append_box(o, "JXL ", {0x0D, 0x0A, 0x87, 0x0A});
	append_box(o, "ftyp", {'j', 'x', 'l', ' ', 0, 0, 0, 0, 'j', 'x', 'l', ' '});
	append_box(o, "jxlc", base);
	append_box(o, "jhgm", jhgm);
	write_all(out / "gainmap.jxl", o);
}

static void
write_gain_maps(const fs::path &out)
{
	// The base goes from white to 30 % grey.
	const vector<uint8_t> base_pixels = gradient(64, 48, {255}, {77}),
						  map_pixels = gradient(16, 12, {0}, {255});

	const vector<uint8_t> base = encode_jpeg(
							  base_pixels, 64, 48, TJSAMP_GRAY, 95, false),
						  map = encode_jpeg(
							  map_pixels, 16, 12, TJSAMP_GRAY, 100, false),
						  colour = encode_jpeg(
							  gradient(16, 12, {255, 0, 0}, {0, 0, 255}), 16,
							  12, TJSAMP_444, 100, false);
	write_all(out / "gainmap-map.jpg", map);

	vector<uint8_t> base_app;
	append_jpeg_app(base_app, 0xE1, kXmpNs,
		bytes(xmp_packet(
			"hdrgm:Version=\"1.0\"><Container:Directory><rdf:Seq>"
			"<rdf:li rdf:parseType=\"Resource\"><Container:Item "
			"Item:Semantic=\"Primary\" Item:Mime=\"image/jpeg\"/></rdf:li>"
			"<rdf:li rdf:parseType=\"Resource\"><Container:Item "
			"Item:Semantic=\"GainMap\" Item:Mime=\"image/jpeg\"/></rdf:li>"
			"</rdf:Seq></Container:Directory>")));

	auto hdrgm = [](const string &offset_hdr, const string &max,
					 const string &capacity_max) {
		vector<uint8_t> app;
		append_jpeg_app(app, 0xE1, kXmpNs,
			bytes(
				xmp_packet("hdrgm:Version=\"1.0\" hdrgm:GainMapMin=\"-0.5\" "
						   "hdrgm:Gamma=\"1.25\" hdrgm:OffsetSDR=\"0.015625\" "
						   "hdrgm:OffsetHDR=\"" +
					offset_hdr + "\" hdrgm:HDRCapacityMin=\"0\" " +
					"hdrgm:HDRCapacityMax=\"" + capacity_max + "\">" + max)));
		return app;
	};
	const string max = "<hdrgm:GainMapMax>2</hdrgm:GainMapMax>";
	write_mpf(
		out / "gainmap.jpg", base, base_app, map, hdrgm("0.015625", max, "2"));
	write_mpf(out / "gainmap-colour.jpg", base, base_app, colour,
		hdrgm("0.015625", max, "2"));
	write_mpf(
		out / "gainmap-offsets.jpg", base, base_app, map, hdrgm("0", max, "2"));
	write_mpf(out / "gainmap-flat.jpg", base, base_app, map,
		hdrgm("0.015625", max, "0"));
	write_mpf(out / "gainmap-seq.jpg", base, base_app, map,
		hdrgm("0.015625",
			"<hdrgm:GainMapMax><rdf:Seq><rdf:li>2</rdf:li><rdf:li>2.1</rdf:li>"
			"<rdf:li>2</rdf:li></rdf:Seq></hdrgm:GainMapMax>",
			"2"));

	vector<uint8_t> iso_base, iso_map;
	append_jpeg_app(iso_base, 0xE2, kIsoNs, {0, 0, 0, 0});
	append_jpeg_app(iso_map, 0xE2, kIsoNs, iso_gain_map_metadata());
	write_mpf(out / "gainmap-iso.jpg", base, iso_base, map, iso_map);

	if (have_tool("avifenc"))
		write_gain_map_avifs(out);
	if (have_tool("cjxl")) {
		write_pgm(out / "gainmap-base.pgm", base_pixels, 64, 48);
		write_pgm(out / "gainmap-map.pgm", map_pixels, 16, 12);
		write_gain_map_jxl(out);
	}
}

// --- TIFF/EP -----------------------------------------------------------------

// A raw as far as the TIFF/EP loader looks at one: a main image it never
// decodes, and a preview of the same size, whose colour space is stated by
// DNG's PreviewColorSpace, or in a Nikon type 3 MakerNote.  Zero omits either.
static void
write_tiff_ep(const fs::path &path, const vector<uint8_t> &jpeg,
	uint32_t preview_colorspace, uint16_t nikon_colorspace)
{
	TiffFile f;
	Ifd preview;
	preview.add_long(254, 1);                      // NewSubfileType: reduced
	preview.add_short(259, 6);                     // Compression: JPEG
	preview.add_long(513, f.blob(jpeg));           // JPEGInterchangeFormat
	preview.add_long(514, uint32_t(jpeg.size()));  // ...Length
	if (preview_colorspace)
		preview.add_long(50970, preview_colorspace);  // PreviewColorSpace

	Ifd exif;
	if (nikon_colorspace) {
		TiffFile note;
		Ifd nikon;
		nikon.add_short(30, nikon_colorspace);  // ColorSpace
		note.page(nikon);
		vector<uint8_t> makernote = {'N', 'i', 'k', 'o', 'n', 0, 2, 0x10, 0, 0};
		makernote.insert(makernote.end(), note.out.begin(), note.out.end());
		exif.add(
			37500, 7 /* UNDEFINED */, uint32_t(makernote.size()), makernote);
	}

	Ifd ifd;
	ifd.add_long(254, 0);                           // NewSubfileType: main
	ifd.add_long(256, 1);                           // ImageWidth
	ifd.add_long(257, 1);                           // ImageLength
	ifd.add(37398, 1 /* BYTE */, 4, {1, 0, 0, 0});  // TIFF/EPStandardID
	ifd.add_long(330, f.directory(preview));        // SubIFDs
	ifd.add_long(34665, f.directory(exif));         // ExifIFD
	f.page(ifd);
	write_all(path, f.out);
}

static void
write_tiff_ep_fixtures(const fs::path &out)
{
	const vector<uint8_t> preview =
		encode_jpeg({192, 128, 96}, 1, 1, TJSAMP_444, 100, false);
	write_tiff_ep(out / "nikon-srgb.nef", preview, 0, 1);
	write_tiff_ep(out / "nikon-adobergb.nef", preview, 0, 2);
	write_tiff_ep(out / "preview-adobergb.tif", preview, 3, 0);
}

// --- Main --------------------------------------------------------------------

static void
write_solid(
	const fs::path &out, const string &name, uint8_t r, uint8_t g, uint8_t b)
{
	write_png(out / (name + ".png"), 1, 1, 8, 2, {0, r, g, b}, {}, {});
	write_png(
		out / (name + "16.png"), 1, 1, 16, 2, {0, r, r, g, g, b, b}, {}, {});
	write_svg_solid(out / (name + ".svg"), r, g, b);

	TiffFile f;
	tiff_page(f, Ifd(), 16, 2 /* RGB */,
		{uint16_t(r * 257), uint16_t(g * 257), uint16_t(b * 257)});
	write_all(out / (name + ".tif"), f.out);

	write_all(out / (name + ".jpg"),
		encode_jpeg({r, g, b}, 1, 1, TJSAMP_444, 100, false));
	write_webp(out / (name + ".webp"), r, g, b);
	write_bmp(out / (name + ".bmp"), r, g, b);
	write_tga(out / (name + ".tga"), r, g, b);
}

int
main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "Usage: %s OUTPUT_DIRECTORY\n", argv[0]);
		return 2;
	}
	const fs::path out = argv[1];
	error_code ec;
	fs::create_directories(out, ec);

	write_solid(out, "red", 255, 0, 0);
	write_solid(out, "green", 0, 255, 0);
	write_solid(out, "blue", 0, 0, 255);
	write_png(out / "white.png", 1, 1, 8, 2, {0, 255, 255, 255}, {}, {});
	write_png(out / "black.png", 1, 1, 8, 2, {0, 0, 0, 0}, {}, {});
	write_png(out / "red_a128.png", 1, 1, 8, 6, {0, 255, 0, 0, 128}, {}, {});

	const vector<uint8_t> red = {0, 255, 0, 0};
	vector<uint8_t> chunks;
	png_chunk(chunks, "tEXt", bytes(string("prompt") + '\0' + "hello"));
	write_png(out / "text-after-idat.png", 1, 1, 8, 2, red, {}, chunks);

	// PNG colour chunks, in all the combinations the loader has to rank.
	const double d65[2] = {0.3127, 0.3290};
	const double p3[6] = {0.6800, 0.3200, 0.2650, 0.6900, 0.1500, 0.0600};
	chunks.clear();
	png_chunk(chunks, "sRGB", {0 /* Perceptual */});
	append_png_gama(chunks, 100000);
	write_png(out / "srgb-chunk.png", 1, 1, 8, 2, red, chunks, {});

	chunks.clear();
	append_png_gama(chunks, 45455);
	write_png(out / "gama22.png", 1, 1, 8, 2, red, chunks, {});

	chunks.clear();
	append_png_chrm(chunks, d65, p3);
	write_png(out / "chrm-p3.png", 1, 1, 8, 2, red, chunks, {});

	append_png_gama(chunks, 100000);
	write_png(out / "chrm-p3-gama1.png", 1, 1, 8, 2, red, chunks, {});

	write_display_p3_vs_srgb_red(out / "display-p3-red_vs_srgb-red.png");
	write_cmyk_lab_icc(out / "cmyk-lab.icc");

	const uint8_t rgbw[2][2][3] = {
		{{255, 0, 0}, {0, 255, 0}}, {{0, 0, 255}, {255, 255, 255}}};
	vector<uint8_t> raw;
	for (const auto &row : rgbw) {
		raw.push_back(0);
		for (const auto &px : row)
			raw.insert(raw.end(), px, px + 3);
	}
	write_png(out / "rgbw_2x2.png", 2, 2, 8, 2, raw, {}, {});
	write_svgs(out);

	// The 2x2 quadrants, enlarged to 64x48.
	vector<uint8_t> quads;
	for (int y = 0; y < 48; y++)
		for (int x = 0; x < 64; x++)
			quads.insert(
				quads.end(), rgbw[y / 24][x / 32], rgbw[y / 24][x / 32] + 3);
	write_all(out / "quads420.jpg",
		encode_jpeg(quads, 64, 48, TJSAMP_420, 100, false));
	write_all(out / "quads420-progressive.jpg",
		encode_jpeg(quads, 64, 48, TJSAMP_420, 100, true));

	write_tiff_fixtures(out);
	write_tiff_ep_fixtures(out);
	write_gain_maps(out);
	return 0;
}
