//
// text-linux.cpp: PangoFT2 layout and FreeType scalar glyph masks
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "text.hpp"

#include <QByteArray>

#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_PARAMETER_TAGS_H
#include <pango/pangofc-font.h>
#include <pango/pangofc-fontmap.h>
#include <pango/pangoft2.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <utility>

using namespace std;

namespace dn
{

constexpr float kPangoScale = float(PANGO_SCALE);

struct TextLayoutImpl {
	PangoLayout *layout = nullptr;
	QByteArray utf8;
	vector<int> utf16_bytes;
	vector<int> byte_utf16;
	vector<int> scalar_utf16;

	~TextLayoutImpl();
};

struct TextBackendImpl {
	PangoFontMap *font_map = nullptr;
	PangoContext *context = nullptr;
	PangoFontDescription *regular = nullptr;
	PangoFontDescription *bold = nullptr;
	vector<PangoFont *> fonts;
	guint serial = 0;

	~TextBackendImpl();
	uint32_t font_id(PangoFont *font);
};

TextLayoutImpl::~TextLayoutImpl()
{
	if (this->layout)
		g_object_unref(this->layout);
}

TextBackendImpl::~TextBackendImpl()
{
	for (PangoFont *font : this->fonts)
		g_object_unref(font);
	if (this->regular)
		pango_font_description_free(this->regular);
	if (this->bold)
		pango_font_description_free(this->bold);
	if (this->context)
		g_object_unref(this->context);
	if (this->font_map)
		g_object_unref(this->font_map);
}

uint32_t
TextBackendImpl::font_id(PangoFont *font)
{
	for (size_t i = 0; i < this->fonts.size(); i++) {
		if (this->fonts[i] == font)
			return uint32_t(i);
	}
	this->fonts.push_back(PANGO_FONT(g_object_ref(font)));
	return uint32_t(this->fonts.size() - 1);
}

static void
map_indexes(TextLayoutImpl &layout, const QString &text)
{
	layout.utf8 = text.toUtf8();
	layout.utf16_bytes.resize(size_t(text.size()) + 1);
	layout.byte_utf16.resize(size_t(layout.utf8.size()) + 1);
	layout.scalar_utf16.clear();
	int utf16 = 0;
	int byte = 0;
	while (utf16 < text.size()) {
		layout.scalar_utf16.push_back(utf16);
		const bool pair = text[utf16].isHighSurrogate() &&
			utf16 + 1 < text.size() && text[utf16 + 1].isLowSurrogate();
		const int units = pair ? 2 : 1;
		int bytes = 0;
		if (byte < layout.utf8.size()) {
			const char *at = layout.utf8.constData() + byte;
			bytes = int(g_utf8_next_char(at) - at);
		}
		for (int i = 0; i < units; i++)
			layout.utf16_bytes[size_t(utf16 + i)] = byte;
		for (int i = 0; i < bytes; i++)
			layout.byte_utf16[size_t(byte + i)] = utf16;
		utf16 += units;
		byte += bytes;
	}
	layout.scalar_utf16.push_back(utf16);
	layout.utf16_bytes[size_t(utf16)] = byte;
	while (byte < layout.utf8.size())
		layout.byte_utf16[size_t(byte++)] = utf16;
	layout.byte_utf16[size_t(byte)] = utf16;
}

static int
utf8_to_utf16(const TextLayoutImpl &layout, int byte)
{
	byte = clamp(byte, 0, int(layout.byte_utf16.size()) - 1);
	return layout.byte_utf16[size_t(byte)];
}

static int
utf16_to_utf8(const TextLayoutImpl &layout, int index)
{
	index = clamp(index, 0, int(layout.utf16_bytes.size()) - 1);
	return layout.utf16_bytes[size_t(index)];
}

static int
previous_scalar(const QString &text, int index)
{
	index = clamp(index, 0, int(text.size()));
	if (index > 1 && text[index - 1].isLowSurrogate() &&
		text[index - 2].isHighSurrogate())
		return index - 2;
	return max(0, index - 1);
}

static void
scalar_antialias(FcPattern *pattern, gpointer)
{
	FcPatternDel(pattern, FC_ANTIALIAS);
	FcPatternAddBool(pattern, FC_ANTIALIAS, FcTrue);
	FcPatternDel(pattern, FC_RGBA);
	FcPatternAddInteger(pattern, FC_RGBA, FC_RGBA_NONE);
}

static PangoStretch
pango_stretch(int stretch)
{
	if (stretch == 0)
		return PANGO_STRETCH_NORMAL;
	if (stretch <= 56)
		return PANGO_STRETCH_ULTRA_CONDENSED;
	if (stretch <= 69)
		return PANGO_STRETCH_EXTRA_CONDENSED;
	if (stretch <= 81)
		return PANGO_STRETCH_CONDENSED;
	if (stretch <= 93)
		return PANGO_STRETCH_SEMI_CONDENSED;
	if (stretch <= 106)
		return PANGO_STRETCH_NORMAL;
	if (stretch <= 118)
		return PANGO_STRETCH_SEMI_EXPANDED;
	if (stretch <= 137)
		return PANGO_STRETCH_EXPANDED;
	if (stretch <= 175)
		return PANGO_STRETCH_EXTRA_EXPANDED;
	return PANGO_STRETCH_ULTRA_EXPANDED;
}

static PangoFontDescription *
font_description(const QFont &font, float device_scale)
{
	PangoFontDescription *result = pango_font_description_new();
	const QByteArray family = font.family().toUtf8();
	if (!family.isEmpty())
		pango_font_description_set_family(result, family.constData());
	pango_font_description_set_weight(
		result, PangoWeight(clamp(int(font.weight()), 100, 1000)));
	pango_font_description_set_stretch(result, pango_stretch(font.stretch()));
	switch (font.style()) {
	case QFont::StyleNormal:
		pango_font_description_set_style(result, PANGO_STYLE_NORMAL);
		break;
	case QFont::StyleItalic:
		pango_font_description_set_style(result, PANGO_STYLE_ITALIC);
		break;
	case QFont::StyleOblique:
		pango_font_description_set_style(result, PANGO_STYLE_OBLIQUE);
		break;
	}
	if (font.pixelSize() > 0) {
		pango_font_description_set_absolute_size(
			result, double(font.pixelSize()) * device_scale * PANGO_SCALE);
	} else if (font.pointSizeF() > 0) {
		pango_font_description_set_size(
			result, int(lround(font.pointSizeF() * PANGO_SCALE)));
	}
	return result;
}

static PangoLayout *
make_layout(
	TextBackendImpl &backend, const QString &text, const TextOptions &options)
{
	PangoLayout *layout = pango_layout_new(backend.context);
	const QByteArray utf8 = text.toUtf8();
	pango_layout_set_text(layout, utf8.constData(), int(utf8.size()));
	pango_layout_set_font_description(
		layout, options.bold ? backend.bold : backend.regular);
	if (options.wrap_width > 0) {
		pango_layout_set_width(layout, options.wrap_width * PANGO_SCALE);
		pango_layout_set_wrap(layout, PANGO_WRAP_WORD_CHAR);
	}
	pango_layout_set_alignment(layout,
		options.align == TextAlign::Center ? PANGO_ALIGN_CENTER
										   : PANGO_ALIGN_LEFT);
	return layout;
}

static vector<int>
cursor_boundaries(PangoLayout *layout, const QString &text)
{
	gint count = 0;
	const PangoLogAttr *attrs =
		pango_layout_get_log_attrs_readonly(layout, &count);
	vector<int> result;
	result.reserve(size_t(max(0, count)));
	TextLayoutImpl indexes;
	map_indexes(indexes, text);
	const int limit = min(count, int(indexes.scalar_utf16.size()));
	for (int i = 0; i < limit; i++) {
		if (attrs[i].is_cursor_position)
			result.push_back(indexes.scalar_utf16[size_t(i)]);
	}
	if (result.empty() || result.back() != text.size())
		result.push_back(int(text.size()));
	return result;
}

static QString
elide(TextBackendImpl &backend, const QString &text, const TextOptions &options)
{
	if (options.max_lines <= 0 || options.wrap_width <= 0)
		return text;
	TextOptions unlimited = options;
	unlimited.max_lines = 0;
	PangoLayout *source = make_layout(backend, text, unlimited);
	auto fits = [&](PangoLayout *layout) {
		if (pango_layout_get_line_count(layout) > options.max_lines)
			return false;
		PangoLayoutIter *iter = pango_layout_get_iter(layout);
		bool result = true;
		do {
			PangoRectangle logical{};
			pango_layout_iter_get_line_extents(iter, nullptr, &logical);
			result &= logical.width <= options.wrap_width * PANGO_SCALE;
		} while (result && pango_layout_iter_next_line(iter));
		pango_layout_iter_free(iter);
		return result;
	};
	if (fits(source)) {
		g_object_unref(source);
		return text;
	}
	const vector<int> boundaries = cursor_boundaries(source, text);
	g_object_unref(source);

	const QString ellipsis(QChar(0x2026));
	PangoLayout *minimum = make_layout(backend, ellipsis, unlimited);
	const bool minimum_fits = fits(minimum);
	g_object_unref(minimum);
	if (!minimum_fits)
		return {};
	size_t low = 0;
	size_t high = boundaries.size();
	while (low < high) {
		const size_t middle = low + (high - low) / 2;
		const QString candidate = text.left(boundaries[middle]) + ellipsis;
		PangoLayout *probe = make_layout(backend, candidate, unlimited);
		const bool candidate_fits = fits(probe);
		g_object_unref(probe);
		if (candidate_fits)
			low = middle + 1;
		else
			high = middle;
	}
	const size_t chosen = low ? low - 1 : 0;
	return text.left(boundaries[chosen]) + ellipsis;
}

static void
line_underline(PangoLayoutLine *line, const PangoFontDescription *description,
	PangoContext *context, float *position, float *thickness)
{
	PangoFontMetrics *metrics = nullptr;
	if (line->runs) {
		auto *run = (PangoLayoutRun *) line->runs->data;
		metrics = pango_font_get_metrics(
			run->item->analysis.font, run->item->analysis.language);
	} else {
		metrics = pango_context_get_metrics(context, description, nullptr);
	}
	*position = -float(pango_font_metrics_get_underline_position(metrics)) /
		kPangoScale;
	*thickness = max(1.f,
		float(pango_font_metrics_get_underline_thickness(metrics)) /
			kPangoScale);
	pango_font_metrics_unref(metrics);
}

static void
collect_layout(TextBackendImpl &backend, TextLayoutImpl &layout_impl,
	const TextOptions &options, vector<TextGlyph> &glyphs,
	vector<TextLine> &lines, float *width, float *height)
{
	PangoLayout *layout = layout_impl.layout;
	PangoRectangle all_logical{};
	pango_layout_get_extents(layout, nullptr, &all_logical);
	*height = float(all_logical.height) / kPangoScale;

	PangoLayoutIter *iter = pango_layout_get_iter(layout);
	do {
		PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
		PangoRectangle logical{};
		pango_layout_iter_get_line_extents(iter, nullptr, &logical);
		int y0 = 0, y1 = 0;
		pango_layout_iter_get_line_yrange(iter, &y0, &y1);

		TextLine out;
		out.text_start = utf8_to_utf16(layout_impl, line->start_index);
		out.text_length =
			utf8_to_utf16(layout_impl, line->start_index + line->length) -
			out.text_start;
		out.baseline =
			float(pango_layout_iter_get_baseline(iter)) / kPangoScale;
		out.advance = float(logical.width) / kPangoScale;
		out.height = float(y1 - y0) / kPangoScale;
		line_underline(line, options.bold ? backend.bold : backend.regular,
			backend.context, &out.underline_position, &out.underline_thickness);

		float x = float(logical.x) / kPangoScale;
		for (GSList *node = line->runs; node; node = node->next) {
			auto *run = (PangoLayoutRun *) node->data;
			const uint32_t font_id = backend.font_id(run->item->analysis.font);
			x += float(run->start_x_offset) / kPangoScale;
			for (int i = 0; i < run->glyphs->num_glyphs; i++) {
				const PangoGlyphInfo &glyph = run->glyphs->glyphs[i];
				if (glyph.glyph != PANGO_GLYPH_EMPTY) {
					glyphs.push_back({font_id, glyph.glyph,
						x + float(glyph.geometry.x_offset) / kPangoScale,
						out.baseline +
							float(glyph.geometry.y_offset - run->y_offset) /
								kPangoScale});
				}
				x += float(glyph.geometry.width) / kPangoScale;
			}
			x += float(run->end_x_offset) / kPangoScale;
		}
		*width = max(*width, out.advance);
		lines.push_back(out);
	} while (pango_layout_iter_next_line(iter));
	pango_layout_iter_free(iter);
}

static TextRect
line_geometry(PangoLayout *layout, int wanted)
{
	PangoLayoutIter *iter = pango_layout_get_iter(layout);
	int line = 0;
	while (line < wanted && pango_layout_iter_next_line(iter))
		line++;
	int y0 = 0, y1 = 0;
	pango_layout_iter_get_line_yrange(iter, &y0, &y1);
	PangoRectangle logical{};
	pango_layout_iter_get_line_extents(iter, nullptr, &logical);
	pango_layout_iter_free(iter);
	return {float(logical.x) / kPangoScale, float(y0) / kPangoScale,
		float(logical.width) / kPangoScale, float(y1 - y0) / kPangoScale};
}

TextLayout::TextLayout() : impl_(make_unique<TextLayoutImpl>())
{
}

TextLayout::~TextLayout() = default;

TextRect
TextLayout::caret(int index, TextAffinity affinity) const
{
	if (!this->impl_->layout || this->lines_.empty())
		return {};
	index = clamp(index, 0, int(this->text_.size()));
	int byte = utf16_to_utf8(*this->impl_, index);
	gboolean trailing = FALSE;
	if (affinity == TextAffinity::Trailing && index > 0) {
		byte = utf16_to_utf8(*this->impl_, previous_scalar(this->text_, index));
		trailing = TRUE;
	}
	int line = 0, x = 0;
	pango_layout_index_to_line_x(
		this->impl_->layout, byte, trailing, &line, &x);
	line = clamp(line, 0, int(this->lines_.size()) - 1);
	const TextRect geometry = line_geometry(this->impl_->layout, line);
	return {
		geometry.x + float(x) / kPangoScale, geometry.y, 1.f, geometry.height};
}

TextHit
TextLayout::hit_test(float x, float y) const
{
	if (!this->impl_->layout)
		return {};
	int byte = 0, trailing = 0;
	pango_layout_xy_to_index(this->impl_->layout, int(lround(x * kPangoScale)),
		int(lround(y * kPangoScale)), &byte, &trailing);
	int end = byte;
	if (trailing > 0) {
		const char *start = this->impl_->utf8.constData() + byte;
		const char *tail = g_utf8_offset_to_pointer(start, trailing);
		end = int(tail - this->impl_->utf8.constData());
	}
	return {utf8_to_utf16(*this->impl_, end),
		trailing ? TextAffinity::Trailing : TextAffinity::Leading};
}

vector<TextRect>
TextLayout::range_rects(int start, int length) const
{
	vector<TextRect> result;
	if (!this->impl_->layout || length <= 0)
		return result;
	start = clamp(start, 0, int(this->text_.size()));
	const int end = clamp(start + length, start, int(this->text_.size()));
	const int first = utf16_to_utf8(*this->impl_, start);
	const int last = utf16_to_utf8(*this->impl_, end);
	PangoLayoutIter *iter = pango_layout_get_iter(this->impl_->layout);
	do {
		PangoLayoutLine *line = pango_layout_iter_get_line_readonly(iter);
		const int line_end = line->start_index + line->length;
		if (last <= line->start_index || first >= line_end)
			continue;
		int *ranges = nullptr;
		int count = 0;
		pango_layout_line_get_x_ranges(line, max(first, line->start_index),
			min(last, line_end), &ranges, &count);
		int y0 = 0, y1 = 0;
		pango_layout_iter_get_line_yrange(iter, &y0, &y1);
		for (int i = 0; i < count; i++) {
			const int x0 = min(ranges[i * 2], ranges[i * 2 + 1]);
			const int x1 = max(ranges[i * 2], ranges[i * 2 + 1]);
			result.push_back({float(x0) / kPangoScale, float(y0) / kPangoScale,
				float(x1 - x0) / kPangoScale, float(y1 - y0) / kPangoScale});
		}
		g_free(ranges);
	} while (pango_layout_iter_next_line(iter));
	pango_layout_iter_free(iter);
	return result;
}

TextBackend::TextBackend() = default;

TextBackend::~TextBackend() = default;

bool
TextBackend::reset(const QFont &font, float device_scale, string *error)
{
	if (!(device_scale > 0) || !isfinite(device_scale)) {
		if (error)
			*error = "invalid text device scale";
		return false;
	}
	// A new Pango map otherwise inherits the stale process-wide config and
	// settings_changed() keeps requesting another reset on every frame.
	if (!FcInitBringUptoDate()) {
		if (error)
			*error = "Fontconfig could not refresh its configuration";
		return false;
	}
	auto next = make_unique<TextBackendImpl>();
	next->font_map = pango_ft2_font_map_new();
	if (!next->font_map) {
		if (error)
			*error = "PangoFT2 could not create a font map";
		return false;
	}
	pango_ft2_font_map_set_resolution(PANGO_FT2_FONT_MAP(next->font_map),
		96. * device_scale, 96. * device_scale);
	pango_fc_font_map_set_default_substitute(
		PANGO_FC_FONT_MAP(next->font_map), scalar_antialias, nullptr, nullptr);
	next->context = pango_font_map_create_context(next->font_map);
	if (!next->context) {
		if (error)
			*error = "PangoFT2 could not create a layout context";
		return false;
	}
	pango_context_set_round_glyph_positions(next->context, FALSE);
	next->regular = font_description(font, device_scale);
	next->bold = pango_font_description_copy(next->regular);
	pango_font_description_set_weight(next->bold,
		max(pango_font_description_get_weight(next->regular),
			PANGO_WEIGHT_BOLD));
	next->serial = pango_font_map_get_serial(next->font_map);
	this->impl_ = std::move(next);
	this->generation_++;
	if (error)
		error->clear();
	return true;
}

bool
TextBackend::settings_changed() const
{
	if (!this->impl_)
		return false;
	FcConfig *config =
		pango_fc_font_map_get_config(PANGO_FC_FONT_MAP(this->impl_->font_map));
	if (!config)
		config = FcConfigGetCurrent();
	return (config && !FcConfigUptoDate(config)) ||
		this->impl_->serial != pango_font_map_get_serial(this->impl_->font_map);
}

unique_ptr<TextLayout>
TextBackend::layout(
	const QString &text, const TextOptions &options, string *error)
{
	if (!this->impl_) {
		if (error)
			*error = "text backend has not been initialized";
		return {};
	}
	auto result = unique_ptr<TextLayout>(new TextLayout);
	result->text_ = elide(*this->impl_, text, options);
	map_indexes(*result->impl_, result->text_);
	result->impl_->layout = make_layout(*this->impl_, result->text_, options);
	collect_layout(*this->impl_, *result->impl_, options, result->glyphs_,
		result->lines_, &result->width_, &result->height_);
	if (error)
		error->clear();
	return result;
}

// Pango does not promise that pango_font_get_hb_font() uses hb-ft.  Keep the
// supported PangoFc bridge, and its deprecation, in these two functions.
#if defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
static FT_Face
lock_face(PangoFont *font)
{
	return pango_fc_font_lock_face(PANGO_FC_FONT(font));
}

static void
unlock_face(PangoFont *font)
{
	pango_fc_font_unlock_face(PANGO_FC_FONT(font));
}
#if defined __GNUC__
#pragma GCC diagnostic pop
#endif

class LockedFace
{
	PangoFont *font_ = nullptr;
	FT_Matrix matrix_{};
	FT_Vector delta_{};

public:
	FT_Face face = nullptr;

	explicit LockedFace(PangoFont *font);
	~LockedFace();
};

LockedFace::LockedFace(PangoFont *font) : font_(font), face(lock_face(font))
{
	if (this->face)
		FT_Get_Transform(this->face, &this->matrix_, &this->delta_);
}

LockedFace::~LockedFace()
{
	if (this->face) {
		FT_Set_Transform(this->face, &this->matrix_, &this->delta_);
		unlock_face(this->font_);
	}
}

static FT_Int32
load_flags(PangoFont *font, FT_Face face)
{
	FcPattern *pattern = pango_fc_font_get_pattern(PANGO_FC_FONT(font));

	FcBool hinting = FcTrue;
	FcPatternGetBool(pattern, FC_HINTING, 0, &hinting);

	int style = FC_HINT_FULL;
	FcPatternGetInteger(pattern, FC_HINT_STYLE, 0, &style);

	// Match Skia's grayscale load policy; full hinting also uses NORMAL.
	FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_COLOR | FT_LOAD_TARGET_NORMAL;
	if (FT_IS_SCALABLE(face))
		flags |= FT_LOAD_NO_BITMAP;

	if (!hinting || style == FC_HINT_NONE)
		flags |= FT_LOAD_NO_HINTING;
	else if (style == FC_HINT_SLIGHT)
		flags |= FT_LOAD_TARGET_LIGHT;

	FcBool autohint = FcFalse;
	if (FcPatternGetBool(pattern, FC_AUTOHINT, 0, &autohint) == FcResultMatch &&
		autohint)
		flags |= FT_LOAD_FORCE_AUTOHINT;
	return flags;
}

static const unsigned char *
bitmap_row(const FT_Bitmap &bitmap, int y)
{
	const int pitch = abs(bitmap.pitch);
	if (bitmap.pitch >= 0)
		return bitmap.buffer + size_t(y) * size_t(pitch);
	return bitmap.buffer + size_t(int(bitmap.rows) - 1 - y) * size_t(pitch);
}

static const array<uint8_t, 256> kLinearTextContrast = [] {
	// Skia enables SK_GAMMA_APPLY_TO_A8 in its build.  Linear destinations
	// retain kBoostContrast; ignoreGamma() sets gamma=1 and luminance=black.
	// Its default contrast of 0.5 is quantized to 128/255 in the scaler record.
	array<uint8_t, 256> result;
	for (size_t i = 0; i < result.size(); i++) {
		const float a = float(i) / 255.f;
		const float boosted = a + ((1.f - a) * (128.f / 255.f) * a);
		result[i] = uint8_t(lround(255.f * boosted));
	}
	return result;
}();

GlyphImage
TextBackend::rasterize(uint32_t font_id, uint32_t glyph_id, int phase) const
{
	GlyphImage result;
	if (!this->impl_ || font_id >= this->impl_->fonts.size() || phase < 0 ||
		phase >= 4 || (glyph_id & PANGO_GLYPH_UNKNOWN_FLAG))
		return result;

	PangoFont *font = this->impl_->fonts[font_id];
	LockedFace locked(font);
	if (!locked.face)
		return result;

	// It appears that linear composition is being difficult.
	FT_Bool darken_stems = true;
	FT_Parameter darkening{FT_PARAM_TAG_STEM_DARKENING, &darken_stems};
	if (FT_Face_Properties(locked.face, 1, &darkening))
		return result;

	FT_Matrix matrix{};
	FT_Vector delta{};
	FT_Get_Transform(locked.face, &matrix, &delta);
	delta.x += FT_Pos(phase * 16);
	FT_Set_Transform(locked.face, &matrix, &delta);
	if (FT_Load_Glyph(locked.face, glyph_id, load_flags(font, locked.face)))
		return result;

	FT_GlyphSlot slot = locked.face->glyph;
	// Embedded strikes cannot express a fractional pen.  Scalable faces were
	// forced through their outline above, so a bitmap here is bitmap-only.
	if (slot->format == FT_GLYPH_FORMAT_BITMAP)
		return result;
	if (FT_Render_Glyph(slot, FT_RENDER_MODE_NORMAL))
		return result;

	const FT_Bitmap &bitmap = slot->bitmap;
	if (!bitmap.buffer || !bitmap.width || !bitmap.rows)
		return GlyphImage{GlyphImageKind::Mask};
	if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY &&
		bitmap.pixel_mode != FT_PIXEL_MODE_MONO &&
		bitmap.pixel_mode != FT_PIXEL_MODE_BGRA)
		return result;

	const bool colour = bitmap.pixel_mode == FT_PIXEL_MODE_BGRA;
	int left = int(bitmap.width), top = int(bitmap.rows), right = -1,
		bottom = -1;
	for (int y = 0; y < int(bitmap.rows); y++) {
		const unsigned char *row = bitmap_row(bitmap, y);
		for (int x = 0; x < int(bitmap.width); x++) {
			unsigned char alpha = 0;
			if (colour)
				alpha = row[x * 4 + 3];
			else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO)
				alpha = row[x / 8] & (0x80 >> (x % 8));
			else
				alpha = row[x];
			if (alpha) {
				left = min(left, x);
				top = min(top, y);
				right = max(right, x);
				bottom = max(bottom, y);
			}
		}
	}
	if (right < left || bottom < top)
		return GlyphImage{GlyphImageKind::Mask};

	result.kind = colour ? GlyphImageKind::ColourBgra8SrgbPremultiplied
						 : GlyphImageKind::Mask;
	result.width = right - left + 1;
	result.height = bottom - top + 1;
	result.stride = result.width * (colour ? 4 : 1);
	result.origin_x = slot->bitmap_left + left;
	result.origin_y = -slot->bitmap_top + top;
	result.pixels.resize(size_t(result.stride) * size_t(result.height));
	for (int y = 0; y < result.height; y++) {
		const unsigned char *src = bitmap_row(bitmap, top + y);
		unsigned char *dst = result.pixels.data() + size_t(y * result.stride);
		if (colour) {
			memcpy(dst, src + left * 4, size_t(result.stride));
		} else if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
			for (int x = 0; x < result.width; x++)
				dst[x] = (src[(left + x) / 8] & (0x80 >> ((left + x) % 8)))
					? 255
					: 0;
		} else if (bitmap.num_grays > 1 && bitmap.num_grays != 256) {
			for (int x = 0; x < result.width; x++)
				dst[x] = uint8_t((unsigned(src[left + x]) * 255U) /
					unsigned(bitmap.num_grays - 1));
		} else {
			memcpy(dst, src + left, size_t(result.width));
		}
		if (!colour) {
			for (int x = 0; x < result.width; x++)
				dst[x] = kLinearTextContrast[dst[x]];
		}
	}
	return result;
}

}  // namespace dn
