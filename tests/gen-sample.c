#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lcms2.h>
#include <png.h>

enum {
	MAX_LEVELS = 1024,
	PIXELS_PER_STEP = 4,
	IMAGE_WIDTH = MAX_LEVELS * PIXELS_PER_STEP,
	IMAGE_HEIGHT = 2048,
	COMPONENT_COUNT = 4,
	REFERENCE_PATCH_WIDTH = 512,
	PATTERN_WIDTH = IMAGE_WIDTH - 2 * REFERENCE_PATCH_WIDTH,
	LABEL_BAND_HEIGHT = 40,
};

enum channel {
	CHANNEL_RED,
	CHANNEL_GREEN,
	CHANNEL_BLUE,
	CHANNEL_GRAY,
	CHANNEL_COUNT,
};

enum row_kind {
	ROW_RAMP,
	ROW_VERTICAL_STRIPES,
	ROW_CHECKERBOARD,
};

struct row_spec {
	enum channel channel;
	bool is_srgb;
	int bits;
	enum row_kind kind;
};

static const struct row_spec row_specs[] = {
	{CHANNEL_RED,   false, 8,  ROW_RAMP},
	{CHANNEL_RED,   false, 10, ROW_RAMP},
	{CHANNEL_RED,   true,  8,  ROW_RAMP},
	{CHANNEL_RED,   true,  10, ROW_RAMP},
	{CHANNEL_GREEN, false, 8,  ROW_RAMP},
	{CHANNEL_GREEN, false, 10, ROW_RAMP},
	{CHANNEL_GREEN, true,  8,  ROW_RAMP},
	{CHANNEL_GREEN, true,  10, ROW_RAMP},
	{CHANNEL_BLUE,  false, 8,  ROW_RAMP},
	{CHANNEL_BLUE,  false, 10, ROW_RAMP},
	{CHANNEL_BLUE,  true,  8,  ROW_RAMP},
	{CHANNEL_BLUE,  true,  10, ROW_RAMP},
	{CHANNEL_GRAY,  false, 8,  ROW_RAMP},
	{CHANNEL_GRAY,  false, 10, ROW_RAMP},
	{CHANNEL_GRAY,  true,  0,  ROW_VERTICAL_STRIPES},
	{CHANNEL_GRAY,  true,  0,  ROW_CHECKERBOARD},
};

#define ROW_COUNT ((int)(sizeof(row_specs) / sizeof(row_specs[0])))
#define ROW_HEIGHT (IMAGE_HEIGHT / ROW_COUNT)

_Static_assert(IMAGE_HEIGHT % ROW_COUNT == 0,
	       "image height must divide evenly into rows");

struct glyph {
	char character;
	uint8_t rows[7];
};

static const struct glyph font[] = {
	{' ', {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
	{'-', {0x00, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00}},
	{'0', {0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e}},
	{'1', {0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e}},
	{'2', {0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f}},
	{'3', {0x1e, 0x01, 0x01, 0x0e, 0x01, 0x01, 0x1e}},
	{'4', {0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02}},
	{'5', {0x1f, 0x10, 0x10, 0x1e, 0x01, 0x01, 0x1e}},
	{'6', {0x0e, 0x10, 0x10, 0x1e, 0x11, 0x11, 0x0e}},
	{'7', {0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
	{'8', {0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e}},
	{'9', {0x0e, 0x11, 0x11, 0x0f, 0x01, 0x01, 0x0e}},
	{'A', {0x0e, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
	{'B', {0x1e, 0x11, 0x11, 0x1e, 0x11, 0x11, 0x1e}},
	{'C', {0x0e, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0e}},
	{'D', {0x1e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1e}},
	{'E', {0x1f, 0x10, 0x10, 0x1e, 0x10, 0x10, 0x1f}},
	{'G', {0x0e, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0f}},
	{'H', {0x11, 0x11, 0x11, 0x1f, 0x11, 0x11, 0x11}},
	{'I', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x1f}},
	{'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
	{'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1f}},
	{'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
	{'O', {0x0e, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
	{'P', {0x1e, 0x11, 0x11, 0x1e, 0x10, 0x10, 0x10}},
	{'R', {0x1e, 0x11, 0x11, 0x1e, 0x14, 0x12, 0x11}},
	{'S', {0x0f, 0x10, 0x10, 0x0e, 0x01, 0x01, 0x1e}},
	{'T', {0x1f, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
	{'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0e}},
	{'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0a, 0x04}},
	{'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0a}},
	{'Y', {0x11, 0x11, 0x0a, 0x04, 0x04, 0x04, 0x04}},
};

static const uint8_t fallback_glyph[7] = {
	0x0e, 0x11, 0x01, 0x02, 0x04, 0x00, 0x04,
};

static const char *const channel_names[CHANNEL_COUNT] = {
	"RED", "GREEN", "BLUE", "GRAY",
};

static void report_errno(const char *action, const char *path)
{
	fprintf(stderr, "%s '%s': %s\n", action, path, strerror(errno));
}

static const uint8_t *find_glyph(char character)
{
	for (size_t i = 0; i < sizeof(font) / sizeof(font[0]); ++i) {
		if (font[i].character == character)
			return font[i].rows;
	}
	return fallback_glyph;
}

static void set_pixel(uint16_t *pixels, int x, int y,
		      uint16_t red, uint16_t green, uint16_t blue)
{
	if (x < 0 || x >= IMAGE_WIDTH || y < 0 || y >= IMAGE_HEIGHT)
		return;

	size_t offset = ((size_t)y * IMAGE_WIDTH + (size_t)x) * COMPONENT_COUNT;
	pixels[offset + 0] = red;
	pixels[offset + 1] = green;
	pixels[offset + 2] = blue;
	pixels[offset + 3] = UINT16_MAX;
}

static void draw_text(uint16_t *pixels, int x, int y, const char *text)
{
	const int scale = 3;
	for (; *text; ++text, x += 6 * scale) {
		const uint8_t *rows = find_glyph(*text);
		for (int glyph_y = 0; glyph_y < 7; ++glyph_y) {
			for (int glyph_x = 0; glyph_x < 5; ++glyph_x) {
				if ((rows[glyph_y] & (1u << (4 - glyph_x))) == 0)
					continue;
				for (int yy = 0; yy < scale; ++yy) {
					for (int xx = 0; xx < scale; ++xx) {
						set_pixel(pixels, x + glyph_x * scale + xx,
							  y + glyph_y * scale + yy,
							  UINT16_MAX, UINT16_MAX, UINT16_MAX);
					}
				}
			}
		}
	}
}

static cmsHPROFILE create_display_p3_profile(void)
{
	const cmsCIExyY white = {0.3127, 0.3290, 1.0};
	const cmsCIExyYTRIPLE primaries = {
		{0.6800, 0.3200, 1.0},
		{0.2650, 0.6900, 1.0},
		{0.1500, 0.0600, 1.0},
	};
	const double srgb_parameters[7] = {
		2.4, 1.0 / 1.055, 0.055 / 1.055, 0.0,
		0.04045, 1.0 / 12.92, 0.0,
	};
	cmsToneCurve *curves[3] = {NULL, NULL, NULL};
	cmsHPROFILE profile = NULL;
	cmsMLU *description = NULL;

	for (size_t i = 0; i < 3; ++i) {
		curves[i] = cmsBuildParametricToneCurve(NULL, 4, srgb_parameters);
		if (curves[i] == NULL)
			goto cleanup;
	}

	profile = cmsCreateRGBProfile(&white, &primaries, curves);
	if (profile == NULL)
		goto cleanup;

	cmsSetProfileVersion(profile, 4.3);
	description = cmsMLUalloc(NULL, 1);
	if (description == NULL ||
	    !cmsMLUsetASCII(description, "en", "US", "Display P3") ||
	    !cmsWriteTag(profile, cmsSigProfileDescriptionTag, description)) {
		cmsCloseProfile(profile);
		profile = NULL;
	}

cleanup:
	cmsMLUfree(description);
	for (size_t i = 0; i < 3; ++i)
		cmsFreeToneCurve(curves[i]);
	return profile;
}

static bool save_profile(cmsHPROFILE profile, uint8_t **data, uint32_t *size)
{
	cmsUInt32Number profile_size = 0;
	if (!cmsSaveProfileToMem(profile, NULL, &profile_size) || profile_size == 0)
		return false;

	uint8_t *profile_data = malloc(profile_size);
	if (profile_data == NULL)
		return false;

	if (!cmsSaveProfileToMem(profile, profile_data, &profile_size)) {
		free(profile_data);
		return false;
	}

	*data = profile_data;
	*size = profile_size;
	return true;
}

static bool build_row_lut(cmsHTRANSFORM srgb_to_p3, int channel, int levels,
			  bool is_srgb, uint16_t output[MAX_LEVELS][3])
{
	uint16_t input[MAX_LEVELS][3] = {{0}};
	if (levels > MAX_LEVELS)
		return false;

	for (int i = 0; i < levels; ++i) {
		uint16_t value = (uint16_t)(((uint32_t)i * UINT16_MAX) /
					    (uint32_t)(levels - 1));
		if (channel == 3) {
			input[i][0] = value;
			input[i][1] = value;
			input[i][2] = value;
		} else {
			input[i][channel] = value;
		}
	}

	if (is_srgb)
		cmsDoTransform(srgb_to_p3, input, output, (cmsUInt32Number)levels);
	else
		memcpy(output, input, (size_t)levels * sizeof(output[0]));
	return true;
}

static void render_gamma_test(uint16_t *pixels, int y_start, int y_end,
			      enum row_kind kind)
{
	const uint16_t linear_midpoint = 188u * 257u;
	const uint16_t encoded_midpoint = 128u * 257u;

	for (int y = y_start; y < y_end; ++y) {
		for (int x = 0; x < IMAGE_WIDTH; ++x) {
			uint16_t value;
			if (y < y_start + LABEL_BAND_HEIGHT) {
				value = 8192;
			} else if (x < PATTERN_WIDTH) {
				int parity = kind == ROW_CHECKERBOARD ? x + y : x;
				value = parity % 2 ? UINT16_MAX : 0;
			} else if (x < PATTERN_WIDTH + REFERENCE_PATCH_WIDTH) {
				value = linear_midpoint;
			} else {
				value = encoded_midpoint;
			}
			set_pixel(pixels, x, y, value, value, value);
		}
	}

	draw_text(pixels, 24, y_start + 9,
		  kind == ROW_CHECKERBOARD ?
		  "SRGB CHECKERBOARD - SCALE DOWN" :
		  "SRGB VERTICAL STRIPES - SCALE DOWN");
	draw_text(pixels, PATTERN_WIDTH + 24, y_start + 9, "LINEAR 188");
	draw_text(pixels, PATTERN_WIDTH + REFERENCE_PATCH_WIDTH + 24,
		  y_start + 9, "SRGB 128");
}

static bool render_chart(uint16_t *pixels, cmsHTRANSFORM srgb_to_p3)
{
	uint16_t lut[MAX_LEVELS][3];
	char label[64];

	for (int row = 0; row < ROW_COUNT; ++row) {
		const struct row_spec *spec = &row_specs[row];
		int channel = spec->channel;
		bool is_srgb = spec->is_srgb;
		int bits = spec->bits;
		int levels = 1 << bits;
		int y_start = row * ROW_HEIGHT;
		int y_end = y_start + ROW_HEIGHT;

		if (spec->kind == ROW_RAMP) {
			if (!build_row_lut(srgb_to_p3, channel, levels, is_srgb, lut))
				return false;

			for (int x = 0; x < IMAGE_WIDTH; ++x) {
				int level = (int)(((uint64_t)x * (uint64_t)levels) /
						  IMAGE_WIDTH);
				for (int y = y_start; y < y_end; ++y) {
					set_pixel(pixels, x, y, lut[level][0], lut[level][1],
						  lut[level][2]);
				}
			}

			for (int level = 0; level < levels; ++level) {
				int x = (int)(((uint64_t)level * IMAGE_WIDTH) /
					      (uint64_t)levels);
				uint16_t marker = level < levels / 2 ? 49151 : 16384;
				for (int y = y_end - 9; y < y_end - 1; ++y)
					set_pixel(pixels, x, y, marker, marker, marker);
			}

			int label_length = snprintf(label, sizeof(label),
						    "%s %d-BIT %s - %d LEVELS",
						    is_srgb ? "SRGB" : "P3", bits,
						    channel_names[channel], levels);
			if (label_length < 0 || (size_t)label_length >= sizeof(label))
				return false;
			draw_text(pixels, 24, y_start + 20, label);
		} else {
			render_gamma_test(pixels, y_start, y_end, spec->kind);
		}

		uint16_t separator = 32768;
		if (row + 1 == ROW_COUNT ||
		    row_specs[row + 1].channel != spec->channel)
			separator = 49151;
		else if (row_specs[row + 1].is_srgb != spec->is_srgb)
			separator = 40959;
		for (int x = 0; x < IMAGE_WIDTH; ++x) {
			set_pixel(pixels, x, y_end - 1, separator, separator, separator);
		}

	}

	return true;
}

static bool write_png(const char *path, const uint16_t *pixels,
		      const uint8_t *profile_data, uint32_t profile_size)
{
	FILE *file = fopen(path, "wb");
	if (file == NULL) {
		report_errno("cannot create", path);
		return false;
	}

	png_structp png = png_create_write_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
	png_infop info = png ? png_create_info_struct(png) : NULL;
	if (png == NULL || info == NULL) {
		fprintf(stderr, "cannot initialize PNG writer for '%s'\n", path);
		if (png != NULL)
			png_destroy_write_struct(&png, NULL);
		fclose(file);
		remove(path);
		return false;
	}

	if (setjmp(png_jmpbuf(png))) {
		fprintf(stderr, "cannot write PNG '%s'\n", path);
		png_destroy_write_struct(&png, &info);
		fclose(file);
		remove(path);
		return false;
	}

	png_init_io(png, file);
	png_set_IHDR(png, info, IMAGE_WIDTH, IMAGE_HEIGHT, 16,
		     PNG_COLOR_TYPE_RGBA, PNG_INTERLACE_NONE,
		     PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	png_set_iCCP(png, info, "Display P3", PNG_COMPRESSION_TYPE_BASE,
		     profile_data, profile_size);
	png_set_gAMA(png, info, 1.0 / 2.2);
	png_set_cHRM(png, info, 0.3127, 0.3290,
		     0.6800, 0.3200, 0.2650, 0.6900, 0.1500, 0.0600);
	png_write_info(png, info);
	const uint16_t endian_probe = 1;
	if (*(const uint8_t *)&endian_probe == 1)
		png_set_swap(png);
	for (int y = 0; y < IMAGE_HEIGHT; ++y) {
		png_const_bytep row = (png_const_bytep)(pixels +
			(size_t)y * IMAGE_WIDTH * COMPONENT_COUNT);
		png_write_row(png, row);
	}
	png_write_end(png, info);
	png_destroy_write_struct(&png, &info);

	if (fclose(file) != 0) {
		report_errno("cannot close", path);
		remove(path);
		return false;
	}
	return true;
}

int main(int argc, char **argv)
{
	const char *default_output = "p3-srgb-ramps.png";
	size_t pixel_count = (size_t)IMAGE_WIDTH * IMAGE_HEIGHT * COMPONENT_COUNT;
	uint16_t *pixels = NULL;
	cmsHPROFILE srgb_profile = NULL;
	cmsHPROFILE p3_profile = NULL;
	cmsHTRANSFORM transform = NULL;
	uint8_t *profile_data = NULL;
	uint32_t profile_size = 0;
	int status = EXIT_FAILURE;

	if (pixel_count > SIZE_MAX / sizeof(*pixels)) {
		fprintf(stderr, "image dimensions overflow address space\n");
		goto cleanup;
	}
	pixels = malloc(pixel_count * sizeof(*pixels));
	if (pixels == NULL) {
		fprintf(stderr, "cannot allocate image buffer\n");
		goto cleanup;
	}

	srgb_profile = cmsCreate_sRGBProfile();
	p3_profile = create_display_p3_profile();
	if (srgb_profile == NULL || p3_profile == NULL) {
		fprintf(stderr, "cannot create color profiles\n");
		goto cleanup;
	}
	transform = cmsCreateTransform(srgb_profile, TYPE_RGB_16,
				       p3_profile, TYPE_RGB_16,
				       INTENT_RELATIVE_COLORIMETRIC,
				       cmsFLAGS_NOOPTIMIZE | cmsFLAGS_NOCACHE);
	if (transform == NULL) {
		fprintf(stderr, "cannot create sRGB-to-Display-P3 transform\n");
		goto cleanup;
	}
	if (!save_profile(p3_profile, &profile_data, &profile_size)) {
		fprintf(stderr, "cannot serialize Display P3 profile\n");
		goto cleanup;
	}
	if (!render_chart(pixels, transform)) {
		fprintf(stderr, "cannot render chart\n");
		goto cleanup;
	}

	if (argc == 1) {
		if (!write_png(default_output, pixels, profile_data, profile_size))
			goto cleanup;
	} else {
		for (int i = 1; i < argc; ++i) {
			if (!write_png(argv[i], pixels, profile_data, profile_size))
				goto cleanup;
		}
	}
	status = EXIT_SUCCESS;

cleanup:
	free(profile_data);
	if (transform != NULL)
		cmsDeleteTransform(transform);
	if (p3_profile != NULL)
		cmsCloseProfile(p3_profile);
	if (srgb_profile != NULL)
		cmsCloseProfile(srgb_profile);
	free(pixels);
	return status;
}
