//
// test-text.cpp: Native text layout and scalar mask contract tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "text.hpp"

#include <QFont>
#include <QString>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using namespace std;
using namespace dn;

static bool
check(bool condition, const char *message)
{
	if (!condition)
		fprintf(stderr, "%s\n", message);
	return condition;
}

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
		if (!check(backend.reset(font, scale, &error) &&
					backend.generation() == old_generation + 1,
				"DPR reset did not create a new font generation"))
			return false;
		if (!check(!backend.settings_changed(),
				"new native font configuration is already stale"))
			return false;
		if (!check(retained.caret(1, TextAffinity::Leading).height > 0,
				"reset invalidated retained layout geometry"))
			return false;
		auto regular = backend.layout(QStringLiteral("H"), plain, &error);
		TextOptions bold_options;
		bold_options.bold = true;
		auto bold = backend.layout(QStringLiteral("H"), bold_options, &error);
		if (!check(regular && bold && regular->height() >= previous_height,
				"font metrics did not scale with DPR"))
			return false;
		previous_height = regular->height();
		for (TextLayout *layout : {regular.get(), bold.get()}) {
			const TextGlyph &glyph = layout->glyphs()[0];
			for (int phase = 0; phase < 4; phase++) {
				const GlyphImage image =
					backend.rasterize(glyph.font_id, glyph.glyph_id, phase);
				if (!check(image.kind == GlyphImageKind::Mask &&
							image.width > 0 && image.stride >= image.width &&
							image.pixels.size() ==
								size_t(image.stride) * size_t(image.height),
						"regular or bold phase has invalid scalar mask "
						"storage"))
					return false;
				// Rasterizing another phase must restore the shared native
				// face.
				(void) backend.rasterize(
					glyph.font_id, glyph.glyph_id, (phase + 1) % 4);
				const GlyphImage repeated =
					backend.rasterize(glyph.font_id, glyph.glyph_id, phase);
				if (!check(image.pixels == repeated.pixels &&
							image.origin_x == repeated.origin_x &&
							image.origin_y == repeated.origin_y &&
							image.width == repeated.width &&
							image.height == repeated.height,
						"rasterization left the native face in a different "
						"state"))
					return false;
			}
		}
	}

	return true;
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
	if (!check(backend.reset(font, 1.f, &error), error.c_str()))
		return 1;
	if (!check(backend.generation() == 1, "reset did not advance generation"))
		return 1;

	TextOptions plain;
	auto empty = backend.layout({}, plain, &error);
	if (!check(empty && !empty->lines().empty() && empty->height() > 0,
			"empty text has no normal line metrics"))
		return 1;

	const QString sample = QString::fromUtf8("office e\u0301 😀 Ελληνικά אבג");
	auto shaped = backend.layout(sample, plain, &error);
	if (!check(shaped && !shaped->glyphs().empty() && !shaped->lines().empty(),
			"mixed-script text did not shape"))
		return 1;
	bool saw_mask = false;
	for (const TextGlyph &glyph : shaped->glyphs()) {
		GlyphImage image = backend.rasterize(glyph.font_id, glyph.glyph_id, 0);
		if (image.kind == GlyphImageKind::Mask && image.width > 0) {
			saw_mask = true;
			if (!check(image.stride >= image.width &&
						image.pixels.size() ==
							size_t(image.stride * image.height),
					"scalar mask storage is inconsistent"))
				return 1;
		}
	}
	if (!check(saw_mask, "no shaped glyph produced a scalar mask"))
		return 1;

	auto phase_layout = backend.layout(QStringLiteral("o"), plain, &error);
	if (!check(phase_layout && phase_layout->glyphs().size() == 1,
			"phase probe did not produce one glyph"))
		return 1;
	vector<GlyphImage> phases;
	for (int phase = 0; phase < 4; phase++)
		phases.push_back(backend.rasterize(phase_layout->glyphs()[0].font_id,
			phase_layout->glyphs()[0].glyph_id, phase));
	for (const GlyphImage &image : phases) {
		if (!check(image.kind == GlyphImageKind::Mask && image.width > 0,
				"a horizontal phase did not produce an A8 mask"))
			return 1;
		if (!check(abs(image.origin_x) < 64 && image.origin_y < 0 &&
					float(image.height) < phase_layout->height() + 8,
				"glyph mask bearings disagree with layout-scale metrics"))
			return 1;
	}
	bool phase_changed = false;
	for (size_t i = 1; i < phases.size(); i++) {
		phase_changed |= phases[i].origin_x != phases[0].origin_x ||
			phases[i].pixels != phases[0].pixels;
	}
	if (!check(
			phase_changed, "native rasterizer discarded all fractional phases"))
		return 1;
	if (!check(
			backend.rasterize(phase_layout->glyphs()[0].font_id,
					   phase_layout->glyphs()[0].glyph_id, 4)
					.kind == GlyphImageKind::Missing,
			"an invalid phase was accepted"))
		return 1;

	const int measured = int(ceil(shaped->width()));
	TextOptions wrapped;
	wrapped.wrap_width = measured;
	auto exact = backend.layout(sample, wrapped, &error);
	if (!check(exact && exact->lines().size() == 1,
			"text wrapped at its measured width"))
		return 1;
	wrapped.wrap_width = max(1, measured / 2);
	auto multiple = backend.layout(sample, wrapped, &error);
	if (!check(multiple && multiple->lines().size() >= 2,
			"narrow text did not wrap"))
		return 1;
	const int wrap_index = multiple->lines()[1].text_start;
	const TextRect wrap_caret =
		multiple->caret(wrap_index, TextAffinity::Leading);
	const TextHit wrap_hit =
		multiple->hit_test(wrap_caret.x, wrap_caret.y + wrap_caret.height / 2);
	const TextRect wrap_roundtrip =
		multiple->caret(wrap_hit.index, wrap_hit.affinity);
	if (!check(fabs(wrap_roundtrip.x - wrap_caret.x) < 1.f &&
				fabs(wrap_roundtrip.y - wrap_caret.y) < 1.f,
			"wrap-boundary affinity did not preserve caret geometry"))
		return 1;

	TextOptions centered_options;
	centered_options.wrap_width = measured + 40;
	centered_options.align = TextAlign::Center;
	auto centered = backend.layout(sample, centered_options, &error);
	const TextRect centered_caret = centered->caret(0, TextAffinity::Leading);
	const vector<TextRect> centered_range =
		centered->range_rects(0, int(centered->text().size()));
	if (!check(centered_caret.x > 10 && !centered_range.empty() &&
				fabs(centered_range[0].x - centered_caret.x) < 1.f,
			"centered caret and range omitted the line offset"))
		return 1;
	if (!check(
			fabs(centered_range[0].y) < 0.01f && centered_range[0].height > 0,
			"full-string visual range does not begin at the layout top"))
		return 1;

	TextOptions elided_options;
	elided_options.wrap_width = max(1, measured / 3);
	elided_options.max_lines = 2;
	auto elided = backend.layout(sample + sample, elided_options, &error);
	if (!check(elided && elided->lines().size() <= 2 &&
				elided->text().endsWith(QChar(0x2026)),
			"multi-line elision did not materialize the drawn ellipsis"))
		return 1;
	for (const TextLine &line : elided->lines()) {
		if (!check(line.advance <= float(elided_options.wrap_width) + .01f,
				"elided line exceeds its requested width"))
			return 1;
	}
	TextOptions minimum_options;
	minimum_options.wrap_width = 1;
	minimum_options.max_lines = 1;
	auto minimum = backend.layout(QStringLiteral("W"), minimum_options, &error);
	if (!check(minimum && minimum->width() <= 1.01f,
			"minimum-width elision overflowed instead of becoming empty"))
		return 1;

	const QString clusters = QString::fromUtf8("A😀e\u0301Z");
	auto clustered = backend.layout(clusters, plain, &error);
	if (!check(clustered && clustered->range_rects(1, 4).size() >= 1,
			"cluster range has no visual geometry"))
		return 1;
	for (int x = 0; x <= int(ceil(clustered->width())); x++) {
		const TextHit hit =
			clustered->hit_test(float(x), clustered->height() / 2);
		if (!check(valid_cluster_index(hit.index),
				"hit testing split a surrogate pair or combining cluster"))
			return 1;
	}

	auto bidi = backend.layout(QString::fromUtf8("abc אבג"), plain, &error);
	const int boundary = 7;
	const TextRect trailing = bidi->caret(boundary, TextAffinity::Trailing);
	const TextHit roundtrip =
		bidi->hit_test(trailing.x, trailing.y + trailing.height / 2);
	const TextRect bidi_roundtrip =
		bidi->caret(roundtrip.index, roundtrip.affinity);
	if (!check(fabs(bidi_roundtrip.x - trailing.x) < 1.f,
			"trailing bidi affinity did not preserve caret geometry"))
		return 1;
	if (!check(bidi->range_rects(0, int(bidi->text().size())).size() >= 2,
			"bidi range did not preserve visual rectangles"))
		return 1;

	QString malformed;
	malformed.append(QChar(0xd800));
	malformed.append(QChar('x'));
	auto defensive = backend.layout(malformed, plain, &error);
	if (!check(defensive && defensive->height() > 0,
			"isolated UTF-16 surrogate broke native layout"))
		return 1;
	const uint64_t valid_generation = backend.generation();
	if (!check(!backend.reset(font, 0, &error) &&
				backend.generation() == valid_generation &&
				shaped->height() > 0,
			"invalid reset damaged the active text generation"))
		return 1;

	for (int size : {9, 13, 18, 20}) {
		font.setPixelSize(size);
		if (!check_raster_sizes(backend, font, *shaped))
			return 1;
	}

	return 0;
}
