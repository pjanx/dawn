//
// test-text.cpp: Native text layout and scalar mask contract tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "test.hpp"
#include "text.hpp"

#include <QFont>
#include <QString>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

using namespace std;
using namespace dn;

static bool
valid_cluster_index(int index)
{
	return index == 0 || index == 1 || index == 3 || index == 5 || index == 6;
}

static bool
check_raster_sizes(
	TextBackend &backend, const QFont &font, const TextLayout &retained)
{
	string error;
	TextOptions plain;
	float previous_height = 0;
	for (float scale : {1.f, 1.25f, 1.5f, 2.f}) {
		const uint64_t old_generation = backend.generation();
		if (!CHECK(backend.reset(font, scale, &error) &&
				backend.generation() == old_generation + 1))
			return false;
		if (!CHECK(!backend.settings_changed()))
			return false;
		if (!CHECK(retained.caret(1, TextAffinity::Leading).height > 0))
			return false;
		auto regular = backend.layout(QStringLiteral("H"), plain, &error);
		TextOptions bold_options;
		bold_options.bold = true;
		auto bold = backend.layout(QStringLiteral("H"), bold_options, &error);
		if (!CHECK(regular && bold && regular->height() >= previous_height))
			return false;
		previous_height = regular->height();
		for (TextLayout *layout : {regular.get(), bold.get()}) {
			const TextGlyph &glyph = layout->glyphs()[0];
			for (int phase = 0; phase < 4; phase++) {
				const GlyphImage image =
					backend.rasterize(glyph.font_id, glyph.glyph_id, phase);
				if (!CHECK(image.kind == GlyphImageKind::Mask &&
						image.width > 0 && image.stride >= image.width &&
						image.pixels.size() ==
							size_t(image.stride) * size_t(image.height)))
					return false;
				// Rasterizing another phase must restore the shared native
				// face.
				(void) backend.rasterize(
					glyph.font_id, glyph.glyph_id, (phase + 1) % 4);
				const GlyphImage repeated =
					backend.rasterize(glyph.font_id, glyph.glyph_id, phase);
				if (!CHECK(image.pixels == repeated.pixels &&
						image.origin_x == repeated.origin_x &&
						image.origin_y == repeated.origin_y &&
						image.width == repeated.width &&
						image.height == repeated.height))
					return false;
			}
		}
	}

	return true;
}

static void
test_truncation(TextBackend &backend)
{
	string error;
	const QString ellipsis(QChar(0x2026));
	TextOptions options;
	options.wrap_width = 1000;
	options.max_lines = 1;
	for (const QString &source : {QString(), QStringLiteral("head  "),
			 QStringLiteral("office e\u0301 😀 אבג")}) {
		auto layout = backend.layout(source, options, &error);
		if (!CHECK(layout && layout->text() == source))
			return;
	}
	for (const QString &separator : {QStringLiteral("\n"),
			 QStringLiteral("\r\n"), QStringLiteral("\u2029")}) {
		for (const QString &tail : {QString(), QStringLiteral("tail")}) {
			const QString source = QStringLiteral("head  ") + separator + tail;
			auto layout = backend.layout(source, options, &error);
			if (!CHECK(layout &&
					layout->text() == QStringLiteral("head") + ellipsis &&
					layout->lines().size() == 1))
				return;
		}
	}
	options.max_lines = 2;
	const QString terminal = QStringLiteral("head\n");
	auto layout = backend.layout(terminal, options, &error);
	if (!CHECK(layout && layout->text() == terminal &&
			layout->lines().size() == 2))
		return;
	options.max_lines = 0;
	layout = backend.layout(QStringLiteral("a\nb\nc"), options, &error);
	if (!CHECK(layout && layout->text() == QStringLiteral("a\nb\nc")))
		return;
	options.max_lines = 1;
	options.wrap_width = 0;
	layout = backend.layout(QStringLiteral("a\nb\nc"), options, &error);
	if (!CHECK(layout && layout->text() == QStringLiteral("a\nb\nc")))
		return;

	// Sweep widths across cluster boundaries and line limits, with both
	// alignments. No fixed font metrics or particular fallback fonts needed.
	for (const QString &source : {QString::fromUtf8("Ae\u0301😀Z long caption"),
			 QString::fromUtf8("אבג office e\u0301 Ελληνικά long caption"),
			 QString::fromUtf8(
				 "A👨‍👩‍👧‍👦🇨🇿Z long caption"),
			 QStringLiteral("head\nwide wide wide\ntail\n")}) {
		QTextBoundaryFinder boundaries(QTextBoundaryFinder::Grapheme, source);
		for (int lines : {1, 2}) {
			for (TextAlign align : {TextAlign::Start, TextAlign::Center}) {
				options.max_lines = lines;
				options.align = align;
				for (int width = 1; width <= 160; width += 3) {
					options.wrap_width = width;
					layout = backend.layout(source, options, &error);
					if (!CHECK(layout && int(layout->lines().size()) <= lines))
						return;
					for (const TextLine &line : layout->lines()) {
						if (!CHECK(line.advance <= float(width) + .01f))
							return;
					}
					const QString &shown = layout->text();
					if (shown == source || shown.isEmpty())
						continue;
					const int cut = int(shown.size()) - 1;
					boundaries.setPosition(cut);
					if (!CHECK(shown.endsWith(ellipsis) &&
							shown.left(cut) == source.left(cut) &&
							boundaries.isAtBoundary() &&
							(!cut || !shown[cut - 1].isSpace())))
						return;
				}
			}
		}
	}
}

static void
test_layout(TextBackend &backend, QFont font)
{
	string error;
	TextOptions plain;
	auto empty = backend.layout({}, plain, &error);
	if (!CHECK(empty && !empty->lines().empty() && empty->height() > 0))
		return;

	const QString sample = QString::fromUtf8("office e\u0301 😀 Ελληνικά אבג");
	auto shaped = backend.layout(sample, plain, &error);
	if (!CHECK(shaped && !shaped->glyphs().empty() && !shaped->lines().empty()))
		return;
	bool saw_mask = false;
	for (const TextGlyph &glyph : shaped->glyphs()) {
		GlyphImage image = backend.rasterize(glyph.font_id, glyph.glyph_id, 0);
		if (image.kind == GlyphImageKind::Mask && image.width > 0) {
			saw_mask = true;
			if (!CHECK(image.stride >= image.width &&
					image.pixels.size() == size_t(image.stride * image.height)))
				return;
		}
	}
	if (!CHECK(saw_mask))
		return;

	auto phase_layout = backend.layout(QStringLiteral("o"), plain, &error);
	if (!CHECK(phase_layout && phase_layout->glyphs().size() == 1))
		return;
	vector<GlyphImage> phases;
	for (int phase = 0; phase < 4; phase++)
		phases.push_back(backend.rasterize(phase_layout->glyphs()[0].font_id,
			phase_layout->glyphs()[0].glyph_id, phase));
	for (const GlyphImage &image : phases) {
		if (!CHECK(image.kind == GlyphImageKind::Mask && image.width > 0))
			return;
		if (!CHECK(abs(image.origin_x) < 64 && image.origin_y < 0 &&
				float(image.height) < phase_layout->height() + 8))
			return;
	}
	bool phase_changed = false;
	for (size_t i = 1; i < phases.size(); i++) {
		phase_changed |= phases[i].origin_x != phases[0].origin_x ||
			phases[i].pixels != phases[0].pixels;
	}
	if (!CHECK(phase_changed))
		return;
	if (!CHECK(backend
				   .rasterize(phase_layout->glyphs()[0].font_id,
					   phase_layout->glyphs()[0].glyph_id, 4)
				   .kind == GlyphImageKind::Missing))
		return;

	const int measured = int(ceil(shaped->width()));
	TextOptions wrapped;
	wrapped.wrap_width = measured;
	auto exact = backend.layout(sample, wrapped, &error);
	if (!CHECK(exact && exact->lines().size() == 1))
		return;
	wrapped.wrap_width = max(1, measured / 2);
	auto multiple = backend.layout(sample, wrapped, &error);
	if (!CHECK(multiple && multiple->lines().size() >= 2))
		return;
	const int wrap_index = multiple->lines()[1].text_start;
	const TextRect wrap_caret =
		multiple->caret(wrap_index, TextAffinity::Leading);
	const TextHit wrap_hit =
		multiple->hit_test(wrap_caret.x, wrap_caret.y + wrap_caret.height / 2);
	const TextRect wrap_roundtrip =
		multiple->caret(wrap_hit.index, wrap_hit.affinity);
	if (!CHECK(fabs(wrap_roundtrip.x - wrap_caret.x) < 1.f &&
			fabs(wrap_roundtrip.y - wrap_caret.y) < 1.f))
		return;

	TextOptions centered_options;
	centered_options.wrap_width = measured + 40;
	centered_options.align = TextAlign::Center;
	auto centered = backend.layout(sample, centered_options, &error);
	const TextRect centered_caret = centered->caret(0, TextAffinity::Leading);
	const vector<TextRect> centered_range =
		centered->range_rects(0, int(centered->text().size()));
	if (!CHECK(centered_caret.x > 10 && !centered_range.empty() &&
			fabs(centered_range[0].x - centered_caret.x) < 1.f))
		return;
	if (!CHECK(
			fabs(centered_range[0].y) < 0.01f && centered_range[0].height > 0))
		return;

	TextOptions elided_options;
	elided_options.wrap_width = max(1, measured / 3);
	elided_options.max_lines = 2;
	auto elided = backend.layout(sample + sample, elided_options, &error);
	if (!CHECK(elided && elided->lines().size() <= 2 &&
			elided->text().endsWith(QChar(0x2026))))
		return;
	for (const TextLine &line : elided->lines()) {
		if (!CHECK(line.advance <= float(elided_options.wrap_width) + .01f))
			return;
	}
	TextOptions minimum_options;
	minimum_options.wrap_width = 1;
	minimum_options.max_lines = 1;
	auto minimum = backend.layout(QStringLiteral("W"), minimum_options, &error);
	if (!CHECK(minimum && minimum->width() <= 1.01f))
		return;

	const QString clusters = QString::fromUtf8("A😀e\u0301Z");
	auto clustered = backend.layout(clusters, plain, &error);
	if (!CHECK(clustered && clustered->range_rects(1, 4).size() >= 1))
		return;
	for (int x = 0; x <= int(ceil(clustered->width())); x++) {
		const TextHit hit =
			clustered->hit_test(float(x), clustered->height() / 2);
		if (!CHECK(valid_cluster_index(hit.index)))
			return;
	}

	auto bidi = backend.layout(QString::fromUtf8("abc אבג"), plain, &error);
	const int boundary = 7;
	const TextRect trailing = bidi->caret(boundary, TextAffinity::Trailing);
	const TextHit roundtrip =
		bidi->hit_test(trailing.x, trailing.y + trailing.height / 2);
	const TextRect bidi_roundtrip =
		bidi->caret(roundtrip.index, roundtrip.affinity);
	if (!CHECK(fabs(bidi_roundtrip.x - trailing.x) < 1.f))
		return;
	if (!CHECK(bidi->range_rects(0, int(bidi->text().size())).size() >= 2))
		return;

	QString malformed;
	malformed.append(QChar(0xd800));
	malformed.append(QChar('x'));
	auto defensive = backend.layout(malformed, plain, &error);
	if (!CHECK(defensive && defensive->height() > 0))
		return;
	const uint64_t valid_generation = backend.generation();
	if (!CHECK(!backend.reset(font, 0, &error) &&
			backend.generation() == valid_generation && shaped->height() > 0))
		return;

	for (int size : {9, 13, 18, 20}) {
		font.setPixelSize(size);
		if (!check_raster_sizes(backend, font, *shaped))
			return;
	}
}

int
main(int argc, char **argv)
{
	TextBackend backend;
	// Optional family for exercising installed CFF and variable fonts without
	// making those particular fonts a prerequisite of the regular suite.
	QFont font(
		argc > 1 ? QString::fromUtf8(argv[1]) : QStringLiteral("sans-serif"));
	font.setPixelSize(18);
	string error;
	if (!backend.reset(font, 1.f, &error)) {
		test::fail("%s", error.c_str());
		return 1;
	}
	CHECK(backend.generation() == 1);
	return test::run({
		{"truncation", [&] { test_truncation(backend); }},
		{"layout", [&] { test_layout(backend, font); }},
	});
}
