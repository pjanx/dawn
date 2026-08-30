//
// test-thumbnail-cache.cpp: shared thumbnail cache
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "qt-test.hpp"
#include "test.hpp"
#include "thumbnail-cache.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>

#include <webp/mux.h>

#include <cstdio>
#include <vector>

using namespace std;

namespace
{

void
append_field(QByteArray &out, const char *key, const QByteArray &value)
{
	out.append(key);
	out.append('\0');
	out.append(value);
	out.append('\0');
}

bool
retag(const QString &path, const dn::ThumbnailSource &source,
	const QByteArray &color_space)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return false;
	const QByteArray bytes = file.readAll();
	file.close();
	const WebPData input{reinterpret_cast<const uint8_t *>(bytes.constData()),
		size_t(bytes.size())};
	WebPMux *mux = WebPMuxCreate(&input, 1);
	if (!mux)
		return false;
	QByteArray metadata;
	append_field(metadata, "Thumb::URI", source.uri);
	append_field(metadata, "Thumb::MTime", QByteArray::number(source.mtime));
	append_field(metadata, "Thumb::Size", QByteArray::number(source.size));
	append_field(metadata, "Thumb::ColorSpace", color_space);
	const WebPData thum{reinterpret_cast<const uint8_t *>(metadata.constData()),
		size_t(metadata.size())};
	WebPData output{};
	const bool ok = WebPMuxSetChunk(mux, "THUM", &thum, 1) == WEBP_MUX_OK &&
		WebPMuxAssemble(mux, &output) == WEBP_MUX_OK;
	WebPMuxDelete(mux);
	if (!ok)
		return false;
	if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
		WebPDataClear(&output);
		return false;
	}
	const bool written =
		file.write(reinterpret_cast<const char *>(output.bytes),
			qint64(output.size)) == qint64(output.size);
	file.close();
	WebPDataClear(&output);
	return written;
}

void
test_cache_layout(const QTemporaryDir &cache)
{
	CHECK(dn::thumbnail_cache_root() ==
		QDir(cache.path()).filePath(QStringLiteral("thumbnails")));
	CHECK(dn::thumbnail_tier_for_height(128) == 0);
	CHECK(dn::thumbnail_tier_for_height(129) == 1);
	CHECK(dn::thumbnail_tier_for_height(2000) == 3);
}

void
test_cache_entries(const QTemporaryDir &inputs)
{
	const QString input =
		QDir(inputs.path()).filePath(QStringLiteral("red.png"));
	QFile source_file(input);
	CHECK(source_file.open(QIODevice::WriteOnly));
	CHECK(source_file.write("source") == 6);
	source_file.close();
	const QFileInfo info(input);
	const dn::ThumbnailSource source = dn::thumbnail_source(
		input, info.lastModified().toMSecsSinceEpoch(), uint64_t(info.size()));
	CHECK(source.uri.startsWith("file:"));
	CHECK(source.hash.size() == 32);
	CHECK(!dn::thumbnail_cache_contains(input));

	const vector<uint16_t> pixels = {
		0,
		0,
		65535,
		65535,
		0,
		0,
		65535,
		65535,
	};
	QString error;
	CHECK(dn::thumbnail_cache_write(
		source, 0, pixels.data(), 2, 1, 20, 10, &error));
	if (!error.isEmpty())
		fprintf(stderr, "%s\n", qUtf8Printable(error));

	auto cmm = make_shared<dawn::Cmm>();
	auto p3 = cmm->get_profile_display_p3();
	dn::ThumbnailHit hit = dn::thumbnail_cache_lookup(source, 0, cmm, p3.get());
	CHECK(!hit.pixels.empty());
	CHECK(hit.width == 2 && hit.height == 1);
	CHECK(hit.image_width == 20 && hit.image_height == 10);
	CHECK(hit.tier == 0);
	CHECK(!hit.interim);

	const QString cached = QDir(dn::thumbnail_cache_root())
							   .filePath(QStringLiteral("wide-normal/%1.webp")
									   .arg(QString::fromLatin1(source.hash)));
	CHECK(QFileInfo::exists(cached));
	CHECK(retag(cached, source, QByteArrayLiteral("sRGB")));
	hit = dn::thumbnail_cache_lookup(source, 0, cmm, p3.get());
	CHECK(!hit.pixels.empty() && hit.interim);
	CHECK(retag(cached, source, QByteArrayLiteral("unknown")));
	hit = dn::thumbnail_cache_lookup(source, 0, cmm, p3.get());
	CHECK(!hit.pixels.empty() && hit.interim);
	CHECK(dn::thumbnail_cache_write(
		source, 2, pixels.data(), 2, 1, 2, 1, &error));
	hit = dn::thumbnail_cache_lookup(source, 1, cmm, p3.get());
	CHECK(!hit.pixels.empty() && hit.tier == 2 && hit.interim);

	const QString png_input =
		QDir(inputs.path()).filePath(QStringLiteral("legacy-source.png"));
	QFile png_source_file(png_input);
	CHECK(png_source_file.open(QIODevice::WriteOnly));
	CHECK(png_source_file.write("legacy") == 6);
	png_source_file.close();
	const QFileInfo png_info(png_input);
	const dn::ThumbnailSource png_source = dn::thumbnail_source(png_input,
		png_info.lastModified().toMSecsSinceEpoch(), uint64_t(png_info.size()));
	const QString png_dir =
		QDir(dn::thumbnail_cache_root()).filePath(QStringLiteral("normal"));
	CHECK(QDir().mkpath(png_dir));
	const QString png_path = QDir(png_dir).filePath(
		QString::fromLatin1(png_source.hash) + QStringLiteral(".png"));
	QImage png(2, 1, QImage::Format_RGBA8888);
	png.fill(Qt::red);
	png.setText(
		QStringLiteral("Thumb::URI"), QString::fromUtf8(png_source.uri));
	png.setText(
		QStringLiteral("Thumb::MTime"), QString::number(png_source.mtime));
	png.setText(
		QStringLiteral("Thumb::Size"), QString::number(png_source.size));
	png.setText(QStringLiteral("Thumb::Image::Width"), QStringLiteral("8"));
	png.setText(QStringLiteral("Thumb::Image::Height"), QStringLiteral("4"));
	CHECK(png.save(png_path, "PNG"));
	hit = dn::thumbnail_cache_lookup(png_source, 0, cmm, p3.get());
	CHECK(!hit.pixels.empty() && hit.tier == 0 && hit.interim);
	CHECK(hit.image_width == 8 && hit.image_height == 4);

	CHECK(QFile::remove(input));
	CHECK(QFile::remove(png_input));
	dn::thumbnail_cache_invalidate();
	CHECK(!QFileInfo::exists(cached));
	CHECK(QFileInfo::exists(png_path));
}

}  // namespace

int
main(int argc, char **argv)
{
	test::Application application(argc, argv, "dn-test");
	return test::run({
		{"cache layout", [&] { test_cache_layout(application.cache()); }},
		{"cache formats and invalidation",
			[&] { test_cache_entries(application.inputs()); }},
	});
}
