//
// gen-fixtures.cpp: generate image fixtures in the requested build directory
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
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
	for (size_t i = 0; i < samples; ++i) {
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
	for (int y = 0; y < 100; ++y) {
		raw.push_back(0);
		for (int x = 0; x < 200; ++x) {
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

static void
write_tiff16_rgb(const fs::path &path, uint16_t r, uint16_t g, uint16_t b)
{
	vector<uint8_t> out;
	const uint32_t pixel_off = 8;
	const uint32_t bps_off = 14;
	const uint32_t sf_off = 20;
	const uint32_t ifd_off = 26;

	out.push_back('I');
	out.push_back('I');
	append_le16(out, 42);
	append_le32(out, ifd_off);

	append_le16(out, r);
	append_le16(out, g);
	append_le16(out, b);
	append_le16(out, 16);
	append_le16(out, 16);
	append_le16(out, 16);
	append_le16(out, 1);
	append_le16(out, 1);
	append_le16(out, 1);

	auto entry = [&](uint16_t tag, uint16_t typ, uint32_t count, uint32_t val) {
		append_le16(out, tag);
		append_le16(out, typ);
		append_le32(out, count);
		append_le32(out, val);
	};

	append_le16(out, 11);
	entry(256, 4, 1, 1);
	entry(257, 4, 1, 1);
	entry(258, 3, 3, bps_off);
	entry(259, 3, 1, 1);
	entry(262, 3, 1, 2);
	entry(273, 4, 1, pixel_off);
	entry(277, 3, 1, 3);
	entry(278, 4, 1, 1);
	entry(279, 4, 1, 6);
	entry(284, 3, 1, 1);
	entry(339, 3, 3, sf_off);
	append_le32(out, 0);
	(void) bps_off;
	(void) sf_off;
	write_all(path, out.data(), out.size());
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
	for (int i = 0; i < 16; ++i) {
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
run_magick(initializer_list<const char *> args)
{
	string cmd = "magick";
	for (const char *a : args) {
		cmd += " '";
		cmd += a;
		cmd += "'";
	}
	int rc = system(cmd.c_str());
	if (rc != 0)
		fprintf(stderr, "gen_fixtures: warning: magick failed (%d): %s\n", rc,
			cmd.c_str());
	return rc;
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

	const uint8_t rgbw[4][3] = {
		{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
	write_png8_rgb_2x2(out / "rgbw_2x2.png", rgbw);
	write_display_p3_vs_srgb_red(out / "display-p3-red_vs_srgb-red.png");
	write_cmyk_lab_icc(out / "cmyk-lab.icc");

	write_png16_rgb(out / "red16.png", 65535, 0, 0);
	write_png16_rgb(out / "green16.png", 0, 65535, 0);
	write_png16_rgb(out / "blue16.png", 0, 0, 65535);

	write_tiff16_rgb(out / "red.tif", 65535, 0, 0);
	write_tiff16_rgb(out / "green.tif", 0, 65535, 0);
	write_tiff16_rgb(out / "blue.tif", 0, 0, 65535);

	write_svgs(out);

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

	printf("wrote fixtures in %s\n", out.string().c_str());
	return 0;
}
