//
// gen-fixtures.cpp: generate image fixtures in the requested build directory
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <lcms2.h>
#include <zlib.h>

using namespace std;
namespace fs = filesystem;

static void
die(const char *msg)
{
	fprintf(stderr, "gen_fixtures: %s\n", msg);
	exit(1);
}

static void
write_all(const fs::path &path, const void *data, size_t len)
{
	FILE *f = fopen(path.string().c_str(), "wb");
	if (!f)
		die(path.string().c_str());
	if (fwrite(data, 1, len, f) != len) {
		fclose(f);
		die("short write");
	}
	fclose(f);
}

static void
write_all(const fs::path &path, const string &s)
{
	write_all(path, s.data(), s.size());
}

static void
append_be32(vector<uint8_t> &o, uint32_t v)
{
	o.push_back(uint8_t(v >> 24));
	o.push_back(uint8_t(v >> 16));
	o.push_back(uint8_t(v >> 8));
	o.push_back(uint8_t(v));
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
png_chunk(vector<uint8_t> &o, const char tag[4], const uint8_t *data, size_t n)
{
	append_be32(o, uint32_t(n));
	o.insert(o.end(), tag, tag + 4);
	if (n)
		o.insert(o.end(), data, data + n);
	uint32_t crc = uint32_t(crc32(0, (const Bytef *) tag, 4));
	if (n)
		crc = uint32_t(crc32(crc, data, uInt(n)));
	append_be32(o, crc);
}

static vector<uint8_t>
zlib_compress(const uint8_t *data, size_t n)
{
	uLong bound = compressBound(uLong(n));
	vector<uint8_t> out(bound);
	uLong out_len = bound;
	if (compress2(out.data(), &out_len, data, uLong(n), 9) != Z_OK)
		die("zlib compress failed");
	out.resize(out_len);
	return out;
}

static vector<uint8_t>
profile_bytes(cmsHPROFILE profile)
{
	cmsUInt32Number size = 0;
	if (!profile || !cmsSaveProfileToMem(profile, nullptr, &size) || !size)
		die("cannot serialize ICC profile");
	vector<uint8_t> bytes(size);
	if (!cmsSaveProfileToMem(profile, bytes.data(), &size))
		die("cannot serialize ICC profile");
	bytes.resize(size);
	return bytes;
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
		!cmsWriteTag(profile, cmsSigProfileDescriptionTag, description)) {
		if (description)
			cmsMLUfree(description);
		cmsCloseProfile(profile);
		die("cannot label Display P3 ICC profile");
	}
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

	vector<uint8_t> ihdr;
	append_be32(ihdr, 200);
	append_be32(ihdr, 100);
	ihdr.push_back(8);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	vector<uint8_t> raw;
	raw.reserve(100 * (1 + 200 * 3));
	for (int y = 0; y < 100; y++) {
		raw.push_back(0);
		for (int x = 0; x < 200; x++) {
			const uint8_t *color = x < 100 ? p3_red : srgb_red_in_p3;
			raw.insert(raw.end(), color, color + 3);
		}
	}

	const vector<uint8_t> icc = profile_bytes(display_p3);
	cmsCloseProfile(display_p3);
	const vector<uint8_t> compressed_icc =
		zlib_compress(icc.data(), icc.size());
	vector<uint8_t> iccp = {
		'D', 'i', 's', 'p', 'l', 'a', 'y', ' ', 'P', '3', 0, 0};
	iccp.insert(iccp.end(), compressed_icc.begin(), compressed_icc.end());
	const vector<uint8_t> idat = zlib_compress(raw.data(), raw.size());
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	png_chunk(out, "iCCP", iccp.data(), iccp.size());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
	printf("Display P3 red=(255,0,0), sRGB red in Display P3=(%u,%u,%u)\n",
		srgb_red_in_p3[0], srgb_red_in_p3[1], srgb_red_in_p3[2]);
}

static void
write_png8_rgb(const fs::path &path, uint8_t r, uint8_t g, uint8_t b,
	const uint8_t *a = nullptr)
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, 1);
	append_be32(ihdr, 1);
	ihdr.push_back(8);
	ihdr.push_back(a ? 6 : 2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	vector<uint8_t> raw;
	raw.push_back(0);
	raw.push_back(r);
	raw.push_back(g);
	raw.push_back(b);
	if (a)
		raw.push_back(*a);

	vector<uint8_t> idat = zlib_compress(raw.data(), raw.size());
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
}

static void
write_png8_rgb_text_after_idat(const fs::path &path, uint8_t r, uint8_t g,
	uint8_t b, const char *key, const char *val)
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, 1);
	append_be32(ihdr, 1);
	ihdr.push_back(8);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	vector<uint8_t> raw;
	raw.push_back(0);
	raw.push_back(r);
	raw.push_back(g);
	raw.push_back(b);

	vector<uint8_t> idat = zlib_compress(raw.data(), raw.size());
	string text = string(key) + '\0' + val;
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "tEXt", (const uint8_t *) text.data(), text.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
}

static void
append_png_gama(vector<uint8_t> &o, uint32_t inverse_gamma)
{
	vector<uint8_t> data;
	append_be32(data, inverse_gamma);
	png_chunk(o, "gAMA", data.data(), data.size());
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
	png_chunk(o, "cHRM", data.data(), data.size());
}

// A 1x1 red PNG with colour chunks, which all belong in front of IDAT.
static void
write_png8_red_colour(const fs::path &path, const vector<uint8_t> &chunks)
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, 1);
	append_be32(ihdr, 1);
	ihdr.push_back(8);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	const uint8_t raw[] = {0, 255, 0, 0};
	vector<uint8_t> idat = zlib_compress(raw, sizeof raw);
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	out.insert(out.end(), chunks.begin(), chunks.end());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
}

static void
write_png8_rgb_2x2(const fs::path &path, const uint8_t px[4][3])
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, 2);
	append_be32(ihdr, 2);
	ihdr.push_back(8);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	vector<uint8_t> raw;
	for (int row = 0; row < 2; row++) {
		raw.push_back(0);
		for (int col = 0; col < 2; col++) {
			const uint8_t *c = px[row * 2 + col];
			raw.push_back(c[0]);
			raw.push_back(c[1]);
			raw.push_back(c[2]);
		}
	}

	vector<uint8_t> idat = zlib_compress(raw.data(), raw.size());
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
}

static void
write_png16_rgb(const fs::path &path, uint16_t r, uint16_t g, uint16_t b)
{
	vector<uint8_t> ihdr;
	append_be32(ihdr, 1);
	append_be32(ihdr, 1);
	ihdr.push_back(16);
	ihdr.push_back(2);
	ihdr.push_back(0);
	ihdr.push_back(0);
	ihdr.push_back(0);

	vector<uint8_t> raw;
	raw.push_back(0);
	raw.push_back(uint8_t(r >> 8));
	raw.push_back(uint8_t(r));
	raw.push_back(uint8_t(g >> 8));
	raw.push_back(uint8_t(g));
	raw.push_back(uint8_t(b >> 8));
	raw.push_back(uint8_t(b));

	vector<uint8_t> idat = zlib_compress(raw.data(), raw.size());
	vector<uint8_t> out = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
	png_chunk(out, "IHDR", ihdr.data(), ihdr.size());
	png_chunk(out, "IDAT", idat.data(), idat.size());
	png_chunk(out, "IEND", nullptr, 0);
	write_all(path, out.data(), out.size());
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

static void
put_le32(vector<uint8_t> &o, size_t at, uint32_t v)
{
	o[at + 0] = uint8_t(v);
	o[at + 1] = uint8_t(v >> 8);
	o[at + 2] = uint8_t(v >> 16);
	o[at + 3] = uint8_t(v >> 24);
}

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

static void
write_tiff_solid(const fs::path &path, uint16_t r, uint16_t g, uint16_t b)
{
	TiffFile f;
	tiff_page(f, Ifd(), 16, 2 /* RGB */, {r, g, b});
	write_all(path, f.out.data(), f.out.size());
}

// A mixed, unsaturated colour, so that a conversion out of a wider gamut
// cannot be hidden by clipping in sRGB.
static const vector<uint16_t> kTiffMid8 = {192, 128, 96};
static const vector<uint16_t> kTiffMid16 = {192 * 257, 128 * 257, 96 * 257};

static void
write_tiff_fixtures(const fs::path &out)
{
	write_tiff_solid(out / "red.tif", 65535, 0, 0);
	write_tiff_solid(out / "green.tif", 0, 65535, 0);
	write_tiff_solid(out / "blue.tif", 0, 0, 65535);

	{
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "adobergb16.tif", f.out.data(), f.out.size());
	}
	{
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "adobergb8.tif", f.out.data(), f.out.size());
	}
	{
		// Chromaticities on a non-RGB raster describe nothing about it.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		tiff_page(f, ifd, 16, 1 /* MINISBLACK */, {128 * 257});
		write_all(out / "grey-primaries.tif", f.out.data(), f.out.size());
	}
	{
		// The only colour statement the corpus's Olympus TIFFs carry.
		TiffFile f;
		Ifd exif;
		exif.add_short(40961, 1);  // ColorSpace: sRGB
		Ifd ifd;
		ifd.add_long(34665, f.directory(exif));  // ExifIFD
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "exif-srgb.tif", f.out.data(), f.out.size());
	}
	{
		TiffFile f;
		Ifd exif;
		exif.add_rationals(42240, {2.2});  // Gamma
		Ifd ifd;
		add_adobe_rgb(ifd);
		ifd.add_long(34665, f.directory(exif));
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "exif-gamma.tif", f.out.data(), f.out.size());
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
		write_all(out / "exif-pages.tif", f.out.data(), f.out.size());
	}
	{
		// One shared curve, which libtiff hands out for all three channels.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		add_transfer_function(ifd, 8, {1.0});
		tiff_page(f, ifd, 8, 2 /* RGB */, kTiffMid8);
		write_all(out / "transfer8.tif", f.out.data(), f.out.size());
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
		write_all(out / "palette-transfer.tif", f.out.data(), f.out.size());
	}
	{
		// Three distinct curves, long enough to need the float constructor.
		TiffFile f;
		Ifd ifd;
		add_adobe_rgb(ifd);
		add_transfer_function(ifd, 16, {1.5, 2.0, 3.0});
		tiff_page(f, ifd, 16, 2 /* RGB */, kTiffMid16);
		write_all(out / "transfer16.tif", f.out.data(), f.out.size());
	}
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

	cmsUInt32Number n = 0;
	if (!cmsSaveProfileToMem(h, nullptr, &n) || !n)
		die("cmsSaveProfileToMem size");
	vector<uint8_t> buf(n);
	if (!cmsSaveProfileToMem(h, buf.data(), &n))
		die("cmsSaveProfileToMem");
	cmsCloseProfile(h);
	write_all(path, buf.data(), n);
}

static void
write_svgs(const fs::path &dir)
{
	write_all(dir / "red.svg",
		R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 4 4">
  <rect width="4" height="4" fill="#FF0000" shape-rendering="crispEdges"/>
</svg>
)");
	write_all(dir / "green.svg",
		R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 4 4">
  <rect width="4" height="4" fill="#00FF00" shape-rendering="crispEdges"/>
</svg>
)");
	write_all(dir / "blue.svg",
		R"(<svg xmlns="http://www.w3.org/2000/svg" width="4" height="4" viewBox="0 0 4 4">
  <rect width="4" height="4" fill="#0000FF" shape-rendering="crispEdges"/>
</svg>
)");
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
		fprintf(stderr, "gen_fixtures: warning: %s failed (%d): %s\n", tool, rc,
			cmd.c_str());
	return rc;
}

static int
run_magick(initializer_list<const char *> args)
{
	return run_tool("magick", args);
}

// Tools that only some fixtures need, and whose absence skips their tests.
static bool
have_tool(const char *tool)
{
	return !system((string("command -v ") + tool + " >/dev/null 2>&1").c_str());
}

// --- Gain maps ---------------------------------------------------------------

static vector<uint8_t>
read_all(const fs::path &path)
{
	ifstream in(path, ios::binary);
	return vector<uint8_t>(istreambuf_iterator<char>(in), {});
}

static void
append_be16(vector<uint8_t> &o, uint16_t v)
{
	o.push_back(uint8_t(v >> 8));
	o.push_back(uint8_t(v));
}

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

static vector<uint8_t>
bytes(const string &s)
{
	return vector<uint8_t>(s.begin(), s.end());
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
	write_all(path, out.data(), out.size());
}

static void
write_gain_maps(const fs::path &out)
{
	const fs::path base_path = out / "gainmap-base.jpg",
				   map_path = out / "gainmap-map.jpg",
				   colour_path = out / "gainmap-colour-map.jpg";
	run_magick({"-size", "64x48", "gradient:white-gray30", "-strip", "-quality",
		"95", base_path.string().c_str()});
	run_magick({"-size", "16x12", "gradient:black-white", "-colorspace", "Gray",
		"-strip", "-quality", "100", map_path.string().c_str()});
	run_magick({"-size", "16x12", "gradient:red-blue", "-strip", "-quality",
		"100", colour_path.string().c_str()});

	const vector<uint8_t> base = read_all(base_path), map = read_all(map_path),
						  colour = read_all(colour_path);
	if (base.size() < 4 || map.size() < 4 || colour.size() < 4) {
		fprintf(stderr, "gen_fixtures: warning: no gain map fixture parts\n");
		return;
	}

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
	if (!have_tool("avifenc"))
		return;

	const string jpeg = (out / "gainmap.jpg").string(),
				 colour = (out / "gainmap-colour.jpg").string(),
				 avif = (out / "gainmap.avif").string();
	if (run_tool("avifenc",
			{"-q", "100", "--qgain-map", "100", "--irot", "1", "--imir", "1",
				"--crop", "8,4,48,40", jpeg.c_str(), avif.c_str()}))
		return;

	vector<uint8_t> visible = read_all(avif);
	reveal_av01_items(visible);
	write_all(out / "gainmap-visible.avif", visible.data(), visible.size());

	run_tool("avifenc",
		{"-q", "100", "--qgain-map", "100", colour.c_str(),
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
	if (!have_tool("cjxl"))
		return;

	const fs::path base_png = out / "gainmap-base.png",
				   map_png = out / "gainmap-map.png",
				   base_jxl = out / "gainmap-base.jxl",
				   map_jxl = out / "gainmap-map.jxl";
	if (run_magick({(out / "gainmap-base.jpg").string().c_str(),
			base_png.string().c_str()}) ||
		run_magick({(out / "gainmap-map.jpg").string().c_str(), "-colorspace",
			"Gray", map_png.string().c_str()}) ||
		run_tool("cjxl",
			{"--quiet", "-d", "0", base_png.string().c_str(),
				base_jxl.string().c_str()}) ||
		run_tool("cjxl",
			{"--quiet", "-d", "0", map_png.string().c_str(),
				map_jxl.string().c_str()}))
		return;

	// Both must be naked codestreams.
	const vector<uint8_t> base = read_all(base_jxl), map = read_all(map_jxl);
	if (base.size() < 2 || base[0] != 0xFF || base[1] != 0x0A ||
		map.size() < 2 || map[0] != 0xFF || map[1] != 0x0A)
		die("cjxl did not produce naked codestreams");

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
	write_all(out / "gainmap.jxl", o.data(), o.size());
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

	write_png8_rgb(out / "red.png", 255, 0, 0);
	write_png8_rgb(out / "green.png", 0, 255, 0);
	write_png8_rgb(out / "blue.png", 0, 0, 255);
	write_png8_rgb(out / "white.png", 255, 255, 255);
	write_png8_rgb(out / "black.png", 0, 0, 0);
	uint8_t a128 = 128;
	write_png8_rgb(out / "red_a128.png", 255, 0, 0, &a128);
	write_png8_rgb_text_after_idat(
		out / "text-after-idat.png", 255, 0, 0, "prompt", "hello");

	// PNG colour chunks, in all the combinations the loader has to rank.
	const double d65[2] = {0.3127, 0.3290};
	const double p3[6] = {0.6800, 0.3200, 0.2650, 0.6900, 0.1500, 0.0600};
	const uint8_t perceptual = 0;
	vector<uint8_t> chunks;
	png_chunk(chunks, "sRGB", &perceptual, 1);
	append_png_gama(chunks, 100000);
	write_png8_red_colour(out / "srgb-chunk.png", chunks);

	chunks.clear();
	append_png_gama(chunks, 45455);
	write_png8_red_colour(out / "gama22.png", chunks);

	chunks.clear();
	append_png_chrm(chunks, d65, p3);
	write_png8_red_colour(out / "chrm-p3.png", chunks);

	append_png_gama(chunks, 100000);
	write_png8_red_colour(out / "chrm-p3-gama1.png", chunks);

	const uint8_t rgbw[4][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
	write_png8_rgb_2x2(out / "rgbw_2x2.png", rgbw);
	write_display_p3_vs_srgb_red(out / "display-p3-red_vs_srgb-red.png");
	write_cmyk_lab_icc(out / "cmyk-lab.icc");

	write_png16_rgb(out / "red16.png", 65535, 0, 0);
	write_png16_rgb(out / "green16.png", 0, 65535, 0);
	write_png16_rgb(out / "blue16.png", 0, 0, 65535);

	write_tiff_fixtures(out);

	write_svgs(out);
	string quads = (out / "rgbw_2x2.png").string();
	string jpeg = (out / "quads420.jpg").string();
	run_magick({quads.c_str(), "-filter", "point", "-resize", "64x48!",
		"-sampling-factor", "2x2", "-quality", "100", jpeg.c_str()});

	string progressive = (out / "quads420-progressive.jpg").string();
	run_magick({jpeg.c_str(), "-interlace", "Plane", progressive.c_str()});

	for (const char *name : {"red", "green", "blue"}) {
		string src = (out / (string(name) + ".png")).string();
		string jpg = (out / (string(name) + ".jpg")).string();
		string webp = (out / (string(name) + ".webp")).string();
		string bmp = (out / (string(name) + ".bmp")).string();
		string tga = (out / (string(name) + ".tga")).string();
		run_magick({src.c_str(), "-quality", "100", "-sampling-factor", "4:4:4",
			jpg.c_str()});
		run_magick(
			{src.c_str(), "-define", "webp:lossless=true", webp.c_str()});
		string bmp3 = string("BMP3:") + bmp;
		run_magick({src.c_str(), bmp3.c_str()});
		run_magick({src.c_str(), tga.c_str()});
	}

	write_gain_maps(out);
	write_gain_map_avifs(out);
	write_gain_map_jxl(out);

	printf("wrote fixtures in %s\n", out.string().c_str());
	return 0;
}
