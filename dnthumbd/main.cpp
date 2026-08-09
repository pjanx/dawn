//
// main.cpp: dnthumbd thumbnail daemon entry point
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "encode-webp.hpp"
#include "orient.hpp"

#include "libdnvk.h"
#include "libdn.h"

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <getopt.h>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

using namespace std;
namespace fs = filesystem;

namespace {

mutex io_mu;
atomic<bool> any_fail{false};

void log_err(const string &msg)
{
	lock_guard lock(io_mu);
	fprintf(stderr, "%s\n", msg.c_str());
}

void fit_size(uint32_t w, uint32_t h, uint32_t *out_w, uint32_t *out_h)
{
	const float scale =
		min(1.0f, min(512.0f / float(w), 256.0f / float(h)));
	*out_w = max(1u, uint32_t(float(w) * scale + 0.5f));
	*out_h = max(1u, uint32_t(float(h) * scale + 0.5f));
}

fs::path thumb_path(const fs::path &input)
{
	const fs::path dir = input.parent_path();
	const string stem = input.stem().string();
	return dir / (stem + ".thumb.webp");
}

bool process_one(const string &path, dn::ScaleScaler *scaler)
{
	dn::OpenContext ctx;
	ctx.uri = path;
	// One Cmm per job — do not share get_default() across worker threads.
	ctx.cmm = make_shared<dn::Cmm>();
	ctx.screen_profile = ctx.cmm->get_profile_sRGB();
	ctx.first_frame_only = true;

	dn::Error error;
	dn::ImagePtr image = dn::open(ctx, &error);
	if (!image) {
		log_err(path + ": " +
			(error.message.empty() ? "open failed" : error.message));
		return false;
	}
	if (image->width == 0 || image->height == 0) {
		log_err(path + ": empty image");
		return false;
	}

	if (!dnthumbd::bake_orientation(*image)) {
		log_err(path + ": bake_orientation failed");
		return false;
	}

	uint32_t out_w = 0, out_h = 0;
	fit_size(image->width, image->height, &out_w, &out_h);

	dn::ScaleOutput scaled;
	string vk_err;
	if (!scaler->scale(image->width, image->height, image->data.data(),
			   image->stride, out_w, out_h, &scaled, &vk_err)) {
		log_err(path + ": scale failed: " + vk_err);
		return false;
	}

	vector<uint8_t> webp;
	string enc_err;
	if (!dnthumbd::encode_webp_rgba8(scaled.width, scaled.height,
					 scaled.rgba8.data(), &webp, &enc_err)) {
		log_err(path + ": encode failed: " + enc_err);
		return false;
	}

	const fs::path out_path = thumb_path(path);
	FILE *f = fopen(out_path.string().c_str(), "wb");
	if (!f) {
		log_err(out_path.string() + ": cannot write");
		return false;
	}
	const size_t n = fwrite(webp.data(), 1, webp.size(), f);
	fclose(f);
	if (n != webp.size()) {
		log_err(out_path.string() + ": short write");
		return false;
	}

	lock_guard lock(io_mu);
	printf("%s -> %s (%ux%u)\n", path.c_str(), out_path.string().c_str(), out_w,
	       out_h);
	return true;
}

struct JobQueue {
	mutex mu;
	condition_variable cv;
	queue<string> paths;
	bool done = false;

	void push(string path)
	{
		lock_guard lock(mu);
		paths.push(std::move(path));
		cv.notify_one();
	}

	void finish()
	{
		lock_guard lock(mu);
		done = true;
		cv.notify_all();
	}
};

void worker(JobQueue *jobs, dn::ScaleScaler *scaler)
{
	for (;;) {
		string path;
		{
			unique_lock lock(jobs->mu);
			jobs->cv.wait(lock, [&] { return !jobs->paths.empty() || jobs->done; });
			if (jobs->paths.empty() && jobs->done)
				return;
			path = std::move(jobs->paths.front());
			jobs->paths.pop();
		}
		if (!process_one(path, scaler))
			any_fail = true;
	}
}

void usage(const char *argv0)
{
	fprintf(stderr, "Usage: %s [-j N] IMAGE...\n", argv0);
}

} // namespace

int main(int argc, char **argv)
{
	unsigned nthreads = thread::hardware_concurrency();
	if (nthreads < 1)
		nthreads = 1;

	const option kLong[] = {
		{"help", no_argument, nullptr, 'h'},
		{nullptr, 0, nullptr, 0},
	};
	int c;
	while ((c = getopt_long(argc, argv, "j:h", kLong, nullptr)) != -1) {
		switch (c) {
		case 'j': {
			const long n = strtol(optarg, nullptr, 10);
			if (n > 0)
				nthreads = unsigned(n);
			break;
		}
		case 'h':
			usage(argv[0]);
			return 0;
		default:
			usage(argv[0]);
			return 1;
		}
	}

	vector<string> paths;
	for (int i = optind; i < argc; i++)
		paths.push_back(argv[i]);

	if (paths.empty()) {
		usage(argv[0]);
		return 1;
	}

	dn::ScaleScaler scaler;
	string err;
	if (!scaler.init(&err)) {
		fprintf(stderr, "Vulkan init failed: %s\n", err.c_str());
		return 1;
	}

	JobQueue jobs;
	for (const string &p : paths)
		jobs.push(p);
	jobs.finish();

	vector<thread> pool;
	pool.reserve(nthreads);
	for (unsigned i = 0; i < nthreads; i++)
		pool.emplace_back(worker, &jobs, &scaler);
	for (thread &t : pool)
		t.join();

	return any_fail ? 1 : 0;
}
