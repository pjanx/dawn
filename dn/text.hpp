//
// text.hpp: Native text layout and scalar glyph masks
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QFont>
#include <QString>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dn
{

// All geometry is in device pixels, from a top-left origin with Y increasing
// downwards.  Positions remain fractional until the glyph atlas quad is made.
struct TextRect {
	float x = 0;
	float y = 0;
	float width = 0;
	float height = 0;
};

enum class TextAlign : uint8_t {
	Start,
	Center,
};

// Affinity associates an insertion point with the character on one side of
// it.  Leading means the character beginning at index, trailing the character
// ending there.  This disambiguates visually distinct bidi caret positions.
enum class TextAffinity : uint8_t {
	Leading,
	Trailing,
};

struct TextHit {
	int index = 0;  // UTF-16 insertion index
	TextAffinity affinity = TextAffinity::Leading;
};

struct TextGlyph {
	uint32_t font_id = 0;
	uint32_t glyph_id = 0;
	float x = 0;  // baseline position
	float y = 0;
};

struct TextLine {
	int text_start = 0;  // UTF-16 source range
	int text_length = 0;
	float baseline = 0;
	float advance = 0;  // logical advance, distinct from ink bounds
	float height = 0;
	float underline_position = 0;  // relative to baseline, down is positive
	float underline_thickness = 0;
};

struct TextOptions {
	int wrap_width = 0;  // <= 0 means an unbounded line
	// With a positive wrap_width, truncate to this many lines and append an
	// ellipsis after trimming trailing whitespace. <= 0 means unlimited.
	// If the ellipsis cannot fit, the displayed string is empty.
	int max_lines = 0;
	bool bold = false;
	TextAlign align = TextAlign::Start;
};

enum class GlyphImageKind : uint8_t {
	// Also used when a bitmap strike cannot represent the requested phase.
	Missing,
	Mask,
	ColourBgra8SrgbPremultiplied,
};

struct GlyphImage {
	GlyphImageKind kind = GlyphImageKind::Missing;
	std::vector<uint8_t> pixels;
	int width = 0;
	int height = 0;
	int stride = 0;    // positive bytes per row
	int origin_x = 0;  // relative to the integral baseline position
	int origin_y = 0;
};

struct TextLayoutImpl;
struct TextBackendImpl;

class TextLayout
{
	std::unique_ptr<TextLayoutImpl> impl_;
	QString text_;
	std::vector<TextGlyph> glyphs_;
	std::vector<TextLine> lines_;
	float width_ = 0;
	float height_ = 0;

	TextLayout();
	// Sorted native cut positions in UTF-16, including zero and text end.
	bool cut_positions(std::vector<int> &out, std::string *error) const;
	friend class TextBackend;

public:
	~TextLayout();
	TextLayout(const TextLayout &) = delete;
	TextLayout &operator=(const TextLayout &) = delete;

	// The materialized string these glyphs draw, including any ellipsis.
	[[nodiscard]] const QString &text() const { return this->text_; }
	[[nodiscard]] std::span<const TextGlyph> glyphs() const
	{
		return this->glyphs_;
	}
	[[nodiscard]] std::span<const TextLine> lines() const
	{
		return this->lines_;
	}
	[[nodiscard]] float width() const { return this->width_; }
	[[nodiscard]] float height() const { return this->height_; }

	[[nodiscard]] TextRect caret(int index, TextAffinity affinity) const;
	[[nodiscard]] TextHit hit_test(float x, float y) const;
	[[nodiscard]] std::vector<TextRect> range_rects(
		int start, int length) const;
};

class TextBackend
{
	std::unique_ptr<TextBackendImpl> impl_;
	uint64_t generation_ = 0;
	friend class TextLayout;

	std::unique_ptr<TextLayout> layout_native(
		const QString &text, const TextOptions &options, std::string *error);

public:
	TextBackend();
	~TextBackend();
	TextBackend(const TextBackend &) = delete;
	TextBackend &operator=(const TextBackend &) = delete;

	// QFont carries the user's system-font request, not a resolved face.  The
	// device scale converts its logical size to device pixels.  Reset starts a
	// new generation of font IDs.  Retained layout geometry stays valid, but
	// its glyph IDs must not be rasterized through the new generation.
	bool reset(const QFont &font, float device_scale, std::string *error);
	// Native configuration can change without the QFont request changing.
	[[nodiscard]] bool settings_changed() const;
	[[nodiscard]] uint64_t generation() const { return this->generation_; }

	[[nodiscard]] std::unique_ptr<TextLayout> layout(
		const QString &text, const TextOptions &options, std::string *error);
	// phase is one of four horizontal quarter-pixel positions.
	[[nodiscard]] GlyphImage rasterize(
		uint32_t font_id, uint32_t glyph_id, int phase) const;
};

}  // namespace dn
