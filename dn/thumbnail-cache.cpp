//
// thumbnail-cache.cpp: shared {wide-,}thumbnail-spec cache
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "thumbnail-cache.hpp"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QtLogging>

#include <webp/decode.h>
#include <webp/demux.h>
#include <webp/encode.h>
#include <webp/mux.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

constexpr array<int, 4> kHeights = {128, 256, 512, 1024};
constexpr array<const char *, 4> kNames = {
	"normal", "large", "x-large", "xx-large"};

constexpr const char *kUri = "Thumb::URI";
constexpr const char *kMtime = "Thumb::MTime";
constexpr const char *kSize = "Thumb::Size";
constexpr const char *kImageWidth = "Thumb::Image::Width";
constexpr const char *kImageHeight = "Thumb::Image::Height";
constexpr const char *kColorSpace = "Thumb::ColorSpace";

struct Metadata {
	unordered_map<string, string> values;
};

bool
parse_metadata(const uint8_t *data, size_t size, Metadata *out)
{
	if (!out || !data || !size || data[size - 1] != 0)
		return false;

	out->values.clear();
	vector<string> fields;
	const char *p = reinterpret_cast<const char *>(data);
	const char *end = p + size;
	while (p < end) {
		const void *found = memchr(p, 0, size_t(end - p));
		if (!found)
			return false;
		const char *nul = static_cast<const char *>(found);
		fields.emplace_back(p, nul);
		p = nul + 1;
	}
	if (fields.empty() || fields.size() % 2)
		return false;
	for (size_t i = 0; i < fields.size(); i += 2) {
		if (fields[i].empty())
			return false;
		out->values.emplace(std::move(fields[i]), std::move(fields[i + 1]));
	}
	return true;
}

bool
number(const string &text, uint64_t *out)
{
	if (!out || text.empty())
		return false;

	uint64_t value = 0;
	auto result = from_chars(text.data(), text.data() + text.size(), value);
	if (result.ec != errc{} || result.ptr != text.data() + text.size())
		return false;

	*out = value;
	return true;
}

const string *
value(const Metadata &meta, const char *key)
{
	auto it = meta.values.find(key);
	return it == meta.values.end() ? nullptr : &it->second;
}

bool
valid_metadata(const Metadata &meta, const ThumbnailSource &source)
{
	const string *uri = value(meta, kUri);
	const string *mtime = value(meta, kMtime);
	uint64_t parsed = 0;
	if (!uri || QByteArray(uri->data(), qsizetype(uri->size())) != source.uri ||
		!mtime || !number(*mtime, &parsed) || parsed != uint64_t(source.mtime))
		return false;
	if (const string *size = value(meta, kSize))
		return number(*size, &parsed) && parsed == source.size;
	return true;
}

void
read_image_dimensions(const Metadata &meta, ThumbnailHit *hit)
{
	const string *width = value(meta, kImageWidth);
	const string *height = value(meta, kImageHeight);
	uint64_t w = 0, h = 0;
	if (hit && width && height && number(*width, &w) && number(*height, &h) &&
		w > 0 && h > 0 && w <= UINT32_MAX && h <= UINT32_MAX) {
		hit->image_width = uint32_t(w);
		hit->image_height = uint32_t(h);
	}
}

QByteArray
read_file(const QString &path)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return {};
	return file.readAll();
}

bool
webp_metadata(const QByteArray &bytes, Metadata *meta)
{
	WebPData data{reinterpret_cast<const uint8_t *>(bytes.constData()),
		size_t(bytes.size())};
	WebPDemuxer *demux = WebPDemux(&data);
	if (!demux)
		return false;

	WebPChunkIterator chunk{};
	const bool found = WebPDemuxGetChunk(demux, "THUM", 1, &chunk);
	const bool ok =
		found && parse_metadata(chunk.chunk.bytes, chunk.chunk.size, meta);
	if (found)
		WebPDemuxReleaseChunkIterator(&chunk);
	WebPDemuxDelete(demux);
	return ok;
}

dawn::ImagePtr
decode_webp(const QByteArray &bytes, dawn::Cmm &cmm, dawn::Profile *source,
	dawn::Profile *target)
{
	int width = 0, height = 0;
	uint8_t *bgra =
		WebPDecodeBGRA(reinterpret_cast<const uint8_t *>(bytes.constData()),
			size_t(bytes.size()), &width, &height);
	if (!bgra || width <= 0 || height <= 0) {
		WebPFree(bgra);
		return {};
	}
	dawn::ImagePtr image = dawn::image_new(uint32_t(width), uint32_t(height));
	if (image &&
		!cmm.transform_bgra8_to_bgra16(bgra, image->data.data(), image->width,
			image->height, source, target, true))
		image.reset();
	WebPFree(bgra);
	return image;
}

ThumbnailHit
read_wide(const QString &path, const ThumbnailSource &source, int tier,
	int desired_tier, const shared_ptr<dawn::Cmm> &cmm, dawn::Profile *screen)
{
	ThumbnailHit hit;
	const QByteArray bytes = read_file(path);
	Metadata meta;
	if (bytes.isEmpty() || !webp_metadata(bytes, &meta) ||
		!valid_metadata(meta, source) || !cmm || !screen)
		return hit;

	const string *tag = value(meta, kColorSpace);
	const bool p3 = tag && *tag == "Display P3";
	shared_ptr<dawn::Profile> src =
		p3 ? cmm->get_profile_display_p3(true) : cmm->get_profile_sRGB(true);
	dawn::ImagePtr image = decode_webp(bytes, *cmm, src.get(), screen);
	if (!image)
		return hit;

	hit.width = image->width;
	hit.height = image->height;
	hit.pixels.resize(size_t(hit.width) * hit.height * 4);
	for (uint32_t y = 0; y < hit.height; ++y)
		memcpy(hit.pixels.data() + size_t(y) * hit.width * 4,
			row_u16(*image, y), size_t(hit.width) * dawn::kBytesPerPixel);
	hit.tier = tier;
	hit.interim = !p3 || tier != desired_tier;
	read_image_dimensions(meta, &hit);
	return hit;
}

ThumbnailHit
read_png(const QString &path, const ThumbnailSource &source, int tier,
	const shared_ptr<dawn::Cmm> &cmm, dawn::Profile *screen)
{
	ThumbnailHit hit;
	const QByteArray bytes = read_file(path);
	if (bytes.isEmpty() || !cmm || !screen)
		return hit;

	dawn::OpenContext ctx;
	ctx.uri = path.toStdString();
	ctx.cmm = cmm;
	ctx.first_frame_only = true;
	dawn::Error error;
	dawn::ImagePtr image = open_from_data(
		span(reinterpret_cast<const uint8_t *>(bytes.constData()),
			size_t(bytes.size())),
		ctx, &error);
	if (!image)
		return hit;

	Metadata meta;
	meta.values = image->text;
	if (!valid_metadata(meta, source))
		return {};

	shared_ptr<dawn::Profile> srgb = cmm->get_profile_sRGB();
	if (!cmm->transform_bgra16(image->data.data(), image->width, image->height,
			srgb.get(), screen, true, true))
		return {};

	hit.width = image->width;
	hit.height = image->height;
	hit.pixels.resize(size_t(hit.width) * hit.height * 4);
	for (uint32_t y = 0; y < hit.height; ++y)
		memcpy(hit.pixels.data() + size_t(y) * hit.width * 4,
			row_u16(*image, y), size_t(hit.width) * dawn::kBytesPerPixel);
	hit.tier = tier;
	hit.interim = true;
	read_image_dimensions(meta, &hit);
	return hit;
}

QString
cache_path(const ThumbnailSource &source, int tier, bool wide)
{
	if (tier < 0 || tier >= int(kNames.size()))
		return {};
	const QString dir = QString::fromLatin1(wide ? "wide-%1" : "%1")
							.arg(QString::fromLatin1(kNames[size_t(tier)]));
	return QDir(QDir(thumbnail_cache_root()).filePath(dir))
		.filePath(QString::fromLatin1(source.hash) + (wide ? ".webp" : ".png"));
}

void
append_field(QByteArray &out, const char *key, const QByteArray &value)
{
	out.append(key);
	out.append('\0');
	out.append(value);
	out.append('\0');
}

QByteArray
make_metadata(
	const ThumbnailSource &source, uint32_t image_width, uint32_t image_height)
{
	QByteArray out;
	append_field(out, kUri, source.uri);
	append_field(out, kMtime, QByteArray::number(source.mtime));
	append_field(out, kSize, QByteArray::number(source.size));
	append_field(out, kImageWidth, QByteArray::number(image_width));
	append_field(out, kImageHeight, QByteArray::number(image_height));
	append_field(out, kColorSpace, QByteArrayLiteral("Display P3"));
	return out;
}

bool
remove_thumbnail(const QString &path, const QString &reason)
{
	qInfo("%s: deleting: %s", qUtf8Printable(path), qUtf8Printable(reason));
	if (QFile::remove(path))
		return true;
	qWarning("%s: cannot delete", qUtf8Printable(path));
	return false;
}

}  // namespace

QString
thumbnail_cache_root()
{
	const QString base =
		QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation);
	return base.isEmpty() ? QString() : QDir(base).filePath("thumbnails");
}

ThumbnailSource
thumbnail_source(const QString &path, int64_t mtime_ms, uint64_t size)
{
	ThumbnailSource source;
	source.path = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
	source.uri = QUrl::fromLocalFile(source.path).toEncoded(QUrl::FullyEncoded);
	source.hash =
		QCryptographicHash::hash(source.uri, QCryptographicHash::Md5).toHex();
	source.mtime = mtime_ms / 1000;
	source.size = size;
	return source;
}

bool
thumbnail_cache_contains(const QString &path)
{
	const QString clean = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
#if !defined Q_OS_WIN && !defined Q_OS_MACOS
	const QString root = QDir::cleanPath(thumbnail_cache_root());
	if (!root.isEmpty() &&
		(clean == root || clean.startsWith(root + QDir::separator())))
		return true;
	// TODO(p): Also plainly look for a "/.cache/thumbnails/" substring.
#else
	const QStringList parts =
		QDir::fromNativeSeparators(clean).split(u'/', Qt::SkipEmptyParts);
	for (const QString &part : parts)
		if (part.compare(QStringLiteral("thumbnails"), Qt::CaseInsensitive) ==
			0)
			return true;
#endif
	return false;
}

int
thumbnail_tier_for_height(int pixels)
{
	for (int i = 0; i < int(kHeights.size()); i++)
		if (pixels <= kHeights[size_t(i)])
			return i;
	return int(kHeights.size()) - 1;
}

int
thumbnail_tier_height(int tier)
{
	if (tier < 0 || tier >= int(kHeights.size()))
		return kHeights[1];
	return kHeights[size_t(tier)];
}

ThumbnailHit
thumbnail_cache_lookup(const ThumbnailSource &source, int desired_tier,
	const shared_ptr<dawn::Cmm> &cmm, dawn::Profile *screen_profile)
{
	if (thumbnail_cache_root().isEmpty() ||
		thumbnail_cache_contains(source.path))
		return {};

	desired_tier = clamp(desired_tier, 0, int(kNames.size()) - 1);
	auto wide = [&](int tier) {
		return read_wide(cache_path(source, tier, true), source, tier,
			desired_tier, cmm, screen_profile);
	};
	auto png = [&](int tier) {
		return read_png(
			cache_path(source, tier, false), source, tier, cmm, screen_profile);
	};

	ThumbnailHit hit = wide(desired_tier);
	if (!hit.pixels.empty())
		return hit;
	if (hit = png(desired_tier); !hit.pixels.empty())
		return hit;

	for (int tier = desired_tier + 1; tier < int(kNames.size()); tier++)
		if (hit = wide(tier); !hit.pixels.empty())
			return hit;
	for (int tier = desired_tier - 1; tier >= 0; --tier)
		if (hit = wide(tier); !hit.pixels.empty())
			return hit;
	for (int tier = desired_tier + 1; tier < int(kNames.size()); tier++)
		if (hit = png(tier); !hit.pixels.empty())
			return hit;
	for (int tier = desired_tier - 1; tier >= 0; --tier)
		if (hit = png(tier); !hit.pixels.empty())
			return hit;
	return {};
}

bool
thumbnail_cache_write(const ThumbnailSource &source, int tier,
	const uint16_t *pixels, uint32_t width, uint32_t height,
	uint32_t image_width, uint32_t image_height, QString *error)
{
	if (error)
		error->clear();

	if (!pixels || !width || !height || tier < 0 ||
		tier >= int(kNames.size()) || thumbnail_cache_root().isEmpty() ||
		thumbnail_cache_contains(source.path))
		return false;

	vector<uint8_t> bgra(size_t(width) * height * 4);
	for (size_t i = 0, n = size_t(width) * height; i < n; i++) {
		const uint32_t a = pixels[i * 4 + 3];
		bgra[i * 4 + 3] = uint8_t((a + 128) / 257);
		for (int c = 0; c < 3; ++c) {
			const uint32_t straight = a
				? min(65535u,
					  uint32_t(
						  (uint64_t(pixels[i * 4 + c]) * 65535u + a / 2) / a))
				: 0;
			bgra[i * 4 + c] = uint8_t((straight + 128) / 257);
		}
	}

	WebPConfig config{};
	WebPPicture picture{};
	WebPMemoryWriter writer{};
	WebPData assembled{};
	WebPMux *mux = nullptr;
	bool ok = WebPConfigInit(&config) && WebPConfigLosslessPreset(&config, 6);
	config.near_lossless = 95;
	config.thread_level = 0;
	ok = ok && WebPValidateConfig(&config) && WebPPictureInit(&picture);
	if (ok) {
		picture.use_argb = 1;
		picture.width = int(width);
		picture.height = int(height);
		ok = WebPPictureImportBGRA(&picture, bgra.data(), int(width * 4));
	}
	WebPMemoryWriterInit(&writer);
	if (ok) {
		picture.writer = WebPMemoryWrite;
		picture.custom_ptr = &writer;
		ok = WebPEncode(&config, &picture);
	}
	if (ok) {
		mux = WebPMuxNew();
		const WebPData image{writer.mem, writer.size};
		const QByteArray metadata =
			make_metadata(source, image_width, image_height);
		const WebPData thum{
			reinterpret_cast<const uint8_t *>(metadata.constData()),
			size_t(metadata.size())};
		ok = mux && WebPMuxSetImage(mux, &image, 1) == WEBP_MUX_OK &&
			WebPMuxSetChunk(mux, "THUM", &thum, 1) == WEBP_MUX_OK &&
			WebPMuxAssemble(mux, &assembled) == WEBP_MUX_OK;
	}
	WebPPictureFree(&picture);
	WebPMemoryWriterClear(&writer);
	if (mux)
		WebPMuxDelete(mux);
	if (!ok) {
		WebPDataClear(&assembled);
		if (error)
			*error = QStringLiteral("WebP encoding failed");
		return false;
	}

	const QString path = cache_path(source, tier, true);
	const QString dir = QFileInfo(path).dir().absolutePath();
	if (!QDir().mkpath(dir)) {
		WebPDataClear(&assembled);
		if (error)
			*error = QStringLiteral("cannot create %1").arg(dir);
		return false;
	}
	QFile::setPermissions(thumbnail_cache_root(),
		QFileDevice::ReadOwner | QFileDevice::WriteOwner |
			QFileDevice::ExeOwner);
	QFile::setPermissions(dir,
		QFileDevice::ReadOwner | QFileDevice::WriteOwner |
			QFileDevice::ExeOwner);
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly)) {
		WebPDataClear(&assembled);
		if (error)
			*error = file.errorString();
		return false;
	}
	file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	const size_t assembled_size = assembled.size;
	const qint64 written =
		file.write(reinterpret_cast<const char *>(assembled.bytes),
			qint64(assembled_size));
	WebPDataClear(&assembled);
	if (written < 0 || uint64_t(written) != assembled_size || !file.commit()) {
		if (error)
			*error = file.errorString();
		return false;
	}
	return true;
}

static void
thumbnail_cache_invalidate_one(const QString &path)
{
	Metadata meta;
	if (!webp_metadata(read_file(path), &meta)) {
		remove_thumbnail(
			path, QStringLiteral("invalid thumbnail metadata"));
		return;
	}
	const string *uri_text = value(meta, kUri);
	const string *mtime_text = value(meta, kMtime);
	uint64_t mtime = 0, size = 0;
	if (!uri_text || !mtime_text || !number(*mtime_text, &mtime)) {
		remove_thumbnail(
			path, QStringLiteral("missing thumbnail identity"));
		return;
	}
	const QByteArray uri(uri_text->data(), qsizetype(uri_text->size()));
	const QByteArray expected =
		QCryptographicHash::hash(uri, QCryptographicHash::Md5).toHex() +
		".webp";
	if (QFileInfo(path).fileName().toLatin1() != expected) {
		remove_thumbnail(path, QStringLiteral("URI checksum mismatch"));
		return;
	}
	const QUrl url = QUrl::fromEncoded(uri);
	if (!url.isLocalFile()) {
		qWarning(
			"%s: cannot verify non-local URI", qUtf8Printable(path));
		return;
	}
	const QFileInfo target(url.toLocalFile());
	if (!target.exists()) {
		remove_thumbnail(
			path, QStringLiteral("source no longer exists"));
		return;
	}
	if (!target.isReadable()) {
		qWarning("%s: source is not readable", qUtf8Printable(path));
		return;
	}
	if (target.lastModified().toSecsSinceEpoch() != qint64(mtime)) {
		remove_thumbnail(
			path, QStringLiteral("modification time mismatch"));
		return;
	}
	if (const string *size_text = value(meta, kSize)) {
		if (!number(*size_text, &size) ||
			uint64_t(target.size()) != size)
			remove_thumbnail(
				path, QStringLiteral("file size mismatch"));
	}
}

void
thumbnail_cache_invalidate()
{
	const QString root = thumbnail_cache_root();
	if (root.isEmpty())
		return;
	for (const char *name : kNames) {
		const QString dir = QDir(root).filePath(
			QStringLiteral("wide-%1").arg(QString::fromLatin1(name)));
		const QFileInfo dir_info(dir);
		if (!dir_info.exists())
			continue;
		if (!dir_info.isDir() || !dir_info.isReadable()) {
			qWarning(
				"%s: cannot scan thumbnail directory", qUtf8Printable(dir));
			continue;
		}
		QDirIterator it(dir, {QStringLiteral("*.webp")}, QDir::Files);
		while (it.hasNext()) {
			const QString path = it.next();
			thumbnail_cache_invalidate_one(path);
		}
	}
}

}  // namespace dn
