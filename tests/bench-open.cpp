//
// bench-open.cpp: time dawn::open() (decode + alloc + CMS)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/libdn.h"
#include "libdn/libdnvk.h"

#include <getopt.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace std;

namespace
{

void
usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-n N] [--no-cms] [--thumb] FILE...\n"
		"  Time CPU dawn::open() per file. Default applies an sRGB screen\n"
		"  profile (required for the 8-bit JPEG CMS path). --no-cms skips\n"
		"  that, so 8-bit JPEG only widens. --thumb then GPU-scales to the\n"
		"  dnthumbd 512x256 box. -n repeats each file.\n",
		argv0);
}

void
fit_size(uint32_t w, uint32_t h, uint32_t *out_w, uint32_t *out_h)
{
	const float scale =
		min(1.0f, min(512.0f / float(w), 256.0f / float(h)));
	*out_w = max(1u, uint32_t(float(w) * scale + 0.5f));
	*out_h = max(1u, uint32_t(float(h) * scale + 0.5f));
}

int
bench_one(const char *path, bool cms, int repeats, dawn::ScaleScaler *scaler)
{
	auto cmm = dawn::Cmm::get_default();
	dawn::OpenContext ctx;
	ctx.cmm = cmm;
	ctx.first_frame_only = true;
	ctx.uri = path;
	if (cms) {
		auto srgb = cmm->get_profile_sRGB();
		if (!srgb) {
			fprintf(stderr, "%s: get_profile_sRGB failed\n", path);
			return 1;
		}
		ctx.screen_profile = srgb;
	}

	int rc = 0;
	for (int i = 0; i < repeats; i++) {
		dawn::OpenTiming st;
		ctx.timing = &st;
		dawn::Error error;
		const auto t0 = chrono::steady_clock::now();
		dawn::ImagePtr img = dawn::open(ctx, &error);
		const auto t1 = chrono::steady_clock::now();
		if (!img) {
			fprintf(stderr, "open(%s): %s\n", path,
				error.message.empty() ? "failed" : error.message.c_str());
			rc = 1;
			continue;
		}
		const double open_ms =
			chrono::duration<double, milli>(t1 - t0).count();
		const double rest_ms = open_ms - st.file_ms - st.decode_ms -
			st.alloc_ms - st.cms_ms - st.widen_ms;
		if (repeats > 1)
			printf("%d  ", i + 1);
		printf(
			"file %.1f  decode %.1f  alloc %.1f  cms %.1f  widen %.1f  "
			"rest %.1f  open %.1f  %ux%u  %s",
			st.file_ms, st.decode_ms, st.alloc_ms, st.cms_ms, st.widen_ms,
			rest_ms, open_ms, img->width, img->height, path);
		if (scaler) {
			uint32_t ow = 0, oh = 0;
			fit_size(img->width, img->height, &ow, &oh);
			dawn::ScaleOutput scaled;
			string vk_err;
			const auto s0 = chrono::steady_clock::now();
			const bool ok = scaler->scale(img->width, img->height,
				img->data.data(), img->stride, ow, oh, &scaled, &vk_err);
			const auto s1 = chrono::steady_clock::now();
			const double scale_ms =
				chrono::duration<double, milli>(s1 - s0).count();
			if (!ok) {
				printf("  scale fail: %s\n", vk_err.c_str());
				rc = 1;
				continue;
			}
			printf("  scale %.1f  %ux%u\n", scale_ms, scaled.width,
				scaled.height);
		} else {
			printf("\n");
		}
	}
	return rc;
}

}  // namespace

int
main(int argc, char **argv)
{
	int repeats = 1;
	bool cms = true;
	bool thumb = false;
	const option kLong[] = {
		{"no-cms", no_argument, nullptr, 1},
		{"thumb", no_argument, nullptr, 2},
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "n:h", kLong, nullptr)) != -1) {
		switch (c) {
		case 'n':
			repeats = atoi(optarg);
			if (repeats < 1) {
				fprintf(stderr, "invalid -n\n");
				return 2;
			}
			break;
		case 1:
			cms = false;
			break;
		case 2:
			thumb = true;
			break;
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 2;
		}
	}
	if (optind >= argc) {
		usage(argv[0]);
		return 2;
	}

	dawn::ScaleScaler scaler;
	dawn::ScaleScaler *scaler_ptr = nullptr;
	if (thumb) {
		string err;
		if (!scaler.init(&err)) {
			fprintf(stderr, "Vulkan init failed: %s\n", err.c_str());
			return 1;
		}
		scaler_ptr = &scaler;
	}

	int rc = 0;
	for (int i = optind; i < argc; i++)
		rc |= bench_one(argv[i], cms, repeats, scaler_ptr);
	return rc;
}
