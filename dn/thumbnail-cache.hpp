//
// thumbnail-cache.hpp: shared thumbnail-spec cache
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn/libdn.h"

#include <QByteArray>
#include <QString>

#include <cstdint>
#include <memory>
#include <vector>

namespace dn
{

struct ThumbnailSource {
	QString path;
	QByteArray uri;
	QByteArray hash;
	int64_t mtime = 0;  // seconds since the epoch
	uint64_t size = 0;
};

struct ThumbnailHit {
	std::vector<uint16_t> pixels;  // display-profile BGRA16 premul
	uint32_t width = 0;
	uint32_t height = 0;
	uint32_t image_width = 0;
	uint32_t image_height = 0;
	int tier = 0;
	bool interim = false;
};

QString thumbnail_cache_root();
ThumbnailSource thumbnail_source(
	const QString &path, int64_t mtime_ms, uint64_t size);
bool thumbnail_cache_contains(const QString &path);

int thumbnail_tier_for_height(int pixels);
int thumbnail_tier_height(int tier);

ThumbnailHit thumbnail_cache_lookup(const ThumbnailSource &source,
	int desired_tier, const std::shared_ptr<dawn::Cmm> &cmm,
	dawn::Profile *screen_profile);

bool thumbnail_cache_write(const ThumbnailSource &source, int tier,
	const uint16_t *pixels, uint32_t width, uint32_t height,
	uint32_t image_width, uint32_t image_height, QString *error = nullptr);

void thumbnail_cache_invalidate();

}  // namespace dn
