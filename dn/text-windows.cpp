//
// text-windows.cpp: DirectWrite text layout and scalar glyph masks
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "text.hpp"

#include <QLocale>

#include <dwrite_3.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{

constexpr float kUnbounded = 16777216.f;

static void
set_error(string *error, const char *message, HRESULT hr)
{
	if (!error)
		return;
	char buffer[256] = {};
	snprintf(buffer, sizeof buffer, "%s (0x%08x)", message, uint32_t(hr));
	*error = buffer;
}

static DWRITE_FONT_STRETCH
font_stretch(int stretch)
{
	if (stretch <= 0)
		return DWRITE_FONT_STRETCH_NORMAL;
	if (stretch <= 50)
		return DWRITE_FONT_STRETCH_ULTRA_CONDENSED;
	if (stretch <= 62)
		return DWRITE_FONT_STRETCH_EXTRA_CONDENSED;
	if (stretch <= 75)
		return DWRITE_FONT_STRETCH_CONDENSED;
	if (stretch <= 87)
		return DWRITE_FONT_STRETCH_SEMI_CONDENSED;
	if (stretch <= 112)
		return DWRITE_FONT_STRETCH_NORMAL;
	if (stretch <= 125)
		return DWRITE_FONT_STRETCH_SEMI_EXPANDED;
	if (stretch <= 150)
		return DWRITE_FONT_STRETCH_EXPANDED;
	if (stretch <= 200)
		return DWRITE_FONT_STRETCH_EXTRA_EXPANDED;
	return DWRITE_FONT_STRETCH_ULTRA_EXPANDED;
}

static DWRITE_FONT_STYLE
font_style(QFont::Style style)
{
	switch (style) {
	case QFont::StyleItalic:
		return DWRITE_FONT_STYLE_ITALIC;
	case QFont::StyleOblique:
		return DWRITE_FONT_STYLE_OBLIQUE;
	case QFont::StyleNormal:
		return DWRITE_FONT_STYLE_NORMAL;
	}
	return DWRITE_FONT_STYLE_NORMAL;
}

struct FontInstance {
	IDWriteFontFace *face = nullptr;
	float em_size = 0;
	bool sideways = false;
	DWRITE_MEASURING_MODE measuring_mode = DWRITE_MEASURING_MODE_NATURAL;
};

struct TextBackendImpl {
	IDWriteFactory2 *factory = nullptr;
	IDWriteFontCollection *collection = nullptr;
	IDWriteTextFormat *normal = nullptr;
	IDWriteTextFormat *bold = nullptr;
	vector<FontInstance> fonts;

	~TextBackendImpl();
	uint32_t intern_font(
		const DWRITE_GLYPH_RUN &run, DWRITE_MEASURING_MODE measuring_mode);
	void clear_fonts();
};

struct TextLayoutImpl {
	IDWriteTextLayout *layout = nullptr;
	vector<IDWriteFontFace *> faces;

	~TextLayoutImpl();
	void retain(IDWriteFontFace *face);
};

TextBackendImpl::~TextBackendImpl()
{
	this->clear_fonts();
	if (this->bold)
		this->bold->Release();
	if (this->normal)
		this->normal->Release();
	if (this->collection)
		this->collection->Release();
	if (this->factory)
		this->factory->Release();
}

void
TextBackendImpl::clear_fonts()
{
	for (FontInstance &font : this->fonts)
		font.face->Release();
	this->fonts.clear();
}

uint32_t
TextBackendImpl::intern_font(
	const DWRITE_GLYPH_RUN &run, DWRITE_MEASURING_MODE measuring_mode)
{
	for (size_t i = 0; i < this->fonts.size(); i++) {
		const FontInstance &font = this->fonts[i];
		if (font.face == run.fontFace && font.em_size == run.fontEmSize &&
			font.sideways == bool(run.isSideways) &&
			font.measuring_mode == measuring_mode)
			return uint32_t(i);
	}
	if (this->fonts.size() >= numeric_limits<uint32_t>::max())
		return numeric_limits<uint32_t>::max();

	run.fontFace->AddRef();
	this->fonts.push_back(
		{run.fontFace, run.fontEmSize, bool(run.isSideways), measuring_mode});
	return uint32_t(this->fonts.size() - 1);
}

TextLayoutImpl::~TextLayoutImpl()
{
	for (IDWriteFontFace *face : this->faces)
		face->Release();
	if (this->layout)
		this->layout->Release();
}

void
TextLayoutImpl::retain(IDWriteFontFace *face)
{
	if (find(this->faces.begin(), this->faces.end(), face) != this->faces.end())
		return;
	face->AddRef();
	this->faces.push_back(face);
}

namespace
{

#if defined __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnon-virtual-dtor"
#endif
class RunCollector final : public IDWriteTextRenderer
{
	ULONG refs_ = 1;
	TextBackendImpl *backend_ = nullptr;
	TextLayoutImpl *layout_ = nullptr;
	vector<TextGlyph> *glyphs_ = nullptr;
	vector<TextLine> *lines_ = nullptr;
	HRESULT failure_ = S_OK;

	size_t line_for(float baseline, const DWRITE_GLYPH_RUN_DESCRIPTION *desc);

public:
	RunCollector(TextBackendImpl *backend, TextLayoutImpl *layout,
		vector<TextGlyph> *glyphs, vector<TextLine> *lines);
	HRESULT failure() const { return this->failure_; }

	HRESULT STDMETHODCALLTYPE QueryInterface(
		REFIID iid, void **object) override;
	ULONG STDMETHODCALLTYPE AddRef() override;
	ULONG STDMETHODCALLTYPE Release() override;
	HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
		void *, WINBOOL *disabled) override;
	HRESULT STDMETHODCALLTYPE GetCurrentTransform(
		void *, DWRITE_MATRIX *transform) override;
	HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *value) override;
	HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT baseline_x,
		FLOAT baseline_y, DWRITE_MEASURING_MODE measuring_mode,
		const DWRITE_GLYPH_RUN *run,
		const DWRITE_GLYPH_RUN_DESCRIPTION *description, IUnknown *) override;
	HRESULT STDMETHODCALLTYPE DrawUnderline(
		void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *, IUnknown *) override;
	HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT,
		const DWRITE_STRIKETHROUGH *, IUnknown *) override;
	HRESULT STDMETHODCALLTYPE DrawInlineObject(void *context, FLOAT origin_x,
		FLOAT origin_y, IDWriteInlineObject *object, WINBOOL sideways,
		WINBOOL rtl, IUnknown *effect) override;
};
#if defined __GNUC__
#pragma GCC diagnostic pop
#endif

}  // namespace

RunCollector::RunCollector(TextBackendImpl *backend, TextLayoutImpl *layout,
	vector<TextGlyph> *glyphs, vector<TextLine> *lines)
	: backend_(backend), layout_(layout), glyphs_(glyphs), lines_(lines)
{
}

HRESULT STDMETHODCALLTYPE
RunCollector::QueryInterface(REFIID iid, void **object)
{
	if (!object)
		return E_POINTER;
	*object = nullptr;
	if (iid != __uuidof(IUnknown) && iid != __uuidof(IDWritePixelSnapping) &&
		iid != __uuidof(IDWriteTextRenderer))
		return E_NOINTERFACE;
	*object = (IDWriteTextRenderer *) this;
	this->AddRef();
	return S_OK;
}

ULONG STDMETHODCALLTYPE
RunCollector::AddRef()
{
	this->refs_++;
	return this->refs_;
}

ULONG STDMETHODCALLTYPE
RunCollector::Release()
{
	this->refs_--;
	const ULONG refs = this->refs_;
	if (!refs)
		delete this;
	return refs;
}

HRESULT STDMETHODCALLTYPE
RunCollector::IsPixelSnappingDisabled(void *, WINBOOL *disabled)
{
	if (!disabled)
		return E_POINTER;
	*disabled = TRUE;
	return S_OK;
}

HRESULT STDMETHODCALLTYPE
RunCollector::GetCurrentTransform(void *, DWRITE_MATRIX *transform)
{
	if (!transform)
		return E_POINTER;
	*transform = {1, 0, 0, 1, 0, 0};
	return S_OK;
}

HRESULT STDMETHODCALLTYPE
RunCollector::GetPixelsPerDip(void *, FLOAT *value)
{
	if (!value)
		return E_POINTER;
	*value = 1;
	return S_OK;
}

size_t
RunCollector::line_for(
	float baseline, const DWRITE_GLYPH_RUN_DESCRIPTION *description)
{
	if (description) {
		const size_t position = description->textPosition;
		for (size_t i = 0; i < this->lines_->size(); i++) {
			const TextLine &line = (*this->lines_)[i];
			const size_t start = size_t(max(line.text_start, 0));
			const size_t end = start + size_t(max(line.text_length, 0));
			if ((position >= start && position < end) ||
				(position == end && i + 1 == this->lines_->size()))
				return i;
		}
	}
	for (size_t i = 0; i < this->lines_->size(); i++) {
		if (fabs((*this->lines_)[i].baseline - baseline) < .5f)
			return i;
	}
	return this->lines_->empty() ? 0 : this->lines_->size() - 1;
}

HRESULT STDMETHODCALLTYPE
RunCollector::DrawGlyphRun(void *, FLOAT baseline_x, FLOAT baseline_y,
	DWRITE_MEASURING_MODE measuring_mode, const DWRITE_GLYPH_RUN *run,
	const DWRITE_GLYPH_RUN_DESCRIPTION *description, IUnknown *)
{
	if (!run || !run->fontFace)
		return E_INVALIDARG;
	const uint32_t font_id = this->backend_->intern_font(*run, measuring_mode);
	if (font_id == numeric_limits<uint32_t>::max()) {
		this->failure_ = E_OUTOFMEMORY;
		return this->failure_;
	}
	this->layout_->retain(run->fontFace);

	const size_t line_index = this->line_for(baseline_y, description);
	TextLine *line =
		this->lines_->empty() ? nullptr : &(*this->lines_)[line_index];

	DWRITE_FONT_METRICS metrics{};
	run->fontFace->GetMetrics(&metrics);
	if (line && !line->underline_thickness && metrics.designUnitsPerEm) {
		const float scale = run->fontEmSize / metrics.designUnitsPerEm;
		line->underline_position = -metrics.underlinePosition * scale;
		line->underline_thickness =
			max(1.f, metrics.underlineThickness * scale);
	}

	const float direction = run->bidiLevel & 1 ? -1.f : 1.f;
	float pen = baseline_x;
	for (UINT32 i = 0; i < run->glyphCount; i++) {
		float advance = 0;
		if (run->glyphAdvances) {
			advance = run->glyphAdvances[i];
		} else {
			DWRITE_GLYPH_METRICS glyph_metrics{};
			if (SUCCEEDED(run->fontFace->GetDesignGlyphMetrics(
					run->glyphIndices + i, 1, &glyph_metrics))) {
				DWRITE_FONT_METRICS font_metrics{};
				run->fontFace->GetMetrics(&font_metrics);
				if (font_metrics.designUnitsPerEm)
					advance = float(glyph_metrics.advanceWidth) *
						run->fontEmSize / font_metrics.designUnitsPerEm;
			}
		}
		if (direction < 0)
			pen -= advance;
		const DWRITE_GLYPH_OFFSET offset =
			run->glyphOffsets ? run->glyphOffsets[i] : DWRITE_GLYPH_OFFSET{};
		this->glyphs_->push_back({font_id, run->glyphIndices[i],
			pen + direction * offset.advanceOffset,
			baseline_y - offset.ascenderOffset});
		if (direction > 0)
			pen += advance;
	}
	return S_OK;
}

HRESULT STDMETHODCALLTYPE
RunCollector::DrawUnderline(
	void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *, IUnknown *)
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE
RunCollector::DrawStrikethrough(
	void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *, IUnknown *)
{
	return S_OK;
}

HRESULT STDMETHODCALLTYPE
RunCollector::DrawInlineObject(void *context, FLOAT origin_x, FLOAT origin_y,
	IDWriteInlineObject *object, WINBOOL sideways, WINBOOL rtl,
	IUnknown *effect)
{
	if (!object)
		return E_INVALIDARG;
	return object->Draw(
		context, this, origin_x, origin_y, sideways, rtl, effect);
}

static HRESULT
make_layout(TextBackendImpl &backend, const QString &text,
	const TextOptions &options, IDWriteTextLayout **out)
{
	const wstring value = text.toStdWString();
	IDWriteTextLayout *layout = nullptr;
	const float width =
		options.wrap_width > 0 ? float(options.wrap_width) : kUnbounded;
	HRESULT hr = backend.factory->CreateTextLayout(value.c_str(),
		UINT32(value.size()), options.bold ? backend.bold : backend.normal,
		width, kUnbounded, &layout);
	if (FAILED(hr))
		return hr;

	hr = layout->SetWordWrapping(options.wrap_width > 0
			? DWRITE_WORD_WRAPPING_WRAP
			: DWRITE_WORD_WRAPPING_NO_WRAP);
	if (SUCCEEDED(hr))
		hr = layout->SetTextAlignment(options.align == TextAlign::Center
				? DWRITE_TEXT_ALIGNMENT_CENTER
				: DWRITE_TEXT_ALIGNMENT_LEADING);
	if (SUCCEEDED(hr) && text.isRightToLeft())
		hr =
			layout->SetReadingDirection(DWRITE_READING_DIRECTION_RIGHT_TO_LEFT);
	if (SUCCEEDED(hr) && options.wrap_width <= 0) {
		DWRITE_TEXT_METRICS metrics{};
		hr = layout->GetMetrics(&metrics);
		if (SUCCEEDED(hr))
			hr = layout->SetMaxWidth(
				max(1.f, metrics.widthIncludingTrailingWhitespace));
	}
	if (FAILED(hr)) {
		layout->Release();
		return hr;
	}
	*out = layout;
	return S_OK;
}

static HRESULT
line_metrics(IDWriteTextLayout *layout, vector<DWRITE_LINE_METRICS> *out)
{
	UINT32 count = 0;
	HRESULT hr = layout->GetLineMetrics(nullptr, 0, &count);
	if (FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER)
		return hr;
	out->resize(count);
	if (!count)
		return S_OK;
	return layout->GetLineMetrics(out->data(), count, &count);
}

static HRESULT
cluster_boundaries(IDWriteTextLayout *layout, vector<int> *out)
{
	UINT32 count = 0;
	HRESULT hr = layout->GetClusterMetrics(nullptr, 0, &count);
	if (FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER)
		return hr;
	vector<DWRITE_CLUSTER_METRICS> metrics(count);
	if (count) {
		hr = layout->GetClusterMetrics(metrics.data(), count, &count);
		if (FAILED(hr))
			return hr;
	}
	out->clear();
	out->push_back(0);
	int offset = 0;
	for (const DWRITE_CLUSTER_METRICS &metric : metrics) {
		offset += metric.length;
		out->push_back(offset);
	}
	return S_OK;
}

static int
previous_boundary(const vector<int> &boundaries, int before)
{
	auto it = lower_bound(boundaries.begin(), boundaries.end(), before);
	if (it == boundaries.begin())
		return 0;
	it--;
	return *it;
}

static int
trim_space(const QString &text, int length)
{
	length = min(length, int(text.size()));
	while (length > 0 && text[length - 1].isSpace())
		length--;
	return length;
}

static HRESULT
elide_layout(TextBackendImpl &backend, const QString &source,
	const TextOptions &options, QString *text, IDWriteTextLayout **layout)
{
	HRESULT hr = make_layout(backend, source, options, layout);
	if (FAILED(hr) || options.max_lines <= 0 || options.wrap_width <= 0)
		return hr;

	vector<DWRITE_LINE_METRICS> lines;
	hr = line_metrics(*layout, &lines);
	DWRITE_TEXT_METRICS metrics{};
	if (SUCCEEDED(hr))
		hr = (*layout)->GetMetrics(&metrics);
	if (FAILED(hr) ||
		(int(lines.size()) <= options.max_lines &&
			metrics.widthIncludingTrailingWhitespace <=
				float(options.wrap_width)))
		return hr;

	vector<int> boundaries;
	hr = cluster_boundaries(*layout, &boundaries);
	if (FAILED(hr))
		return hr;
	int cut = 0;
	for (int i = 0; i < min(options.max_lines, int(lines.size())); i++)
		cut += int(lines[size_t(i)].length);
	cut = trim_space(source, cut);

	while (true) {
		const QString candidate = source.left(cut) + QChar(0x2026);
		IDWriteTextLayout *candidate_layout = nullptr;
		hr = make_layout(backend, candidate, options, &candidate_layout);
		if (FAILED(hr))
			return hr;
		vector<DWRITE_LINE_METRICS> candidate_lines;
		hr = line_metrics(candidate_layout, &candidate_lines);
		if (SUCCEEDED(hr))
			hr = candidate_layout->GetMetrics(&metrics);
		if (FAILED(hr)) {
			candidate_layout->Release();
			return hr;
		}
		if (int(candidate_lines.size()) <= options.max_lines &&
			metrics.widthIncludingTrailingWhitespace <=
				float(options.wrap_width)) {
			(*layout)->Release();
			*layout = candidate_layout;
			*text = candidate;
			return S_OK;
		}
		candidate_layout->Release();
		if (!cut) {
			hr = make_layout(backend, {}, options, &candidate_layout);
			if (FAILED(hr))
				return hr;
			(*layout)->Release();
			*layout = candidate_layout;
			text->clear();
			return S_OK;
		}
		cut = trim_space(source, previous_boundary(boundaries, cut));
	}
}

static vector<DWRITE_HIT_TEST_METRICS>
hit_metrics(IDWriteTextLayout *layout, UINT32 start, UINT32 length)
{
	UINT32 count = 0;
	HRESULT hr =
		layout->HitTestTextRange(start, length, 0, 0, nullptr, 0, &count);
	if (FAILED(hr) && hr != E_NOT_SUFFICIENT_BUFFER)
		return {};
	vector<DWRITE_HIT_TEST_METRICS> metrics(count);
	if (count &&
		FAILED(layout->HitTestTextRange(
			start, length, 0, 0, metrics.data(), count, &count)))
		return {};
	metrics.resize(count);
	return metrics;
}

TextLayout::TextLayout() = default;
TextLayout::~TextLayout() = default;

TextRect
TextLayout::caret(int index, TextAffinity affinity) const
{
	if (!this->impl_ || !this->impl_->layout)
		return {};
	index = clamp(index, 0, int(this->text_.size()));
	UINT32 position = UINT32(index);
	WINBOOL trailing = FALSE;
	if (affinity == TextAffinity::Trailing && position) {
		position--;
		trailing = TRUE;
	}
	FLOAT x = 0, y = 0;
	DWRITE_HIT_TEST_METRICS metrics{};
	if (FAILED(this->impl_->layout->HitTestTextPosition(
			position, trailing, &x, &y, &metrics)))
		return {};
	return {x, metrics.top, 0, metrics.height};
}

TextHit
TextLayout::hit_test(float x, float y) const
{
	if (!this->impl_ || !this->impl_->layout)
		return {};
	WINBOOL trailing = FALSE, inside = FALSE;
	DWRITE_HIT_TEST_METRICS metrics{};
	if (FAILED(this->impl_->layout->HitTestPoint(
			x, y, &trailing, &inside, &metrics)))
		return {};
	const UINT32 position =
		metrics.textPosition + (trailing ? metrics.length : 0);
	return {min(int(position), int(this->text_.size())),
		trailing ? TextAffinity::Trailing : TextAffinity::Leading};
}

vector<TextRect>
TextLayout::range_rects(int start, int length) const
{
	vector<TextRect> result;
	if (!this->impl_ || !this->impl_->layout || length <= 0)
		return result;
	start = clamp(start, 0, int(this->text_.size()));
	length = clamp(length, 0, int(this->text_.size()) - start);
	for (const DWRITE_HIT_TEST_METRICS &metric :
		hit_metrics(this->impl_->layout, UINT32(start), UINT32(length)))
		result.push_back(
			{metric.left, metric.top, metric.width, metric.height});
	return result;
}

TextBackend::TextBackend() : impl_(make_unique<TextBackendImpl>())
{
}
TextBackend::~TextBackend() = default;

bool
TextBackend::reset(const QFont &font, float device_scale, string *error)
{
	if (!(device_scale > 0) || !isfinite(device_scale)) {
		set_error(error, "invalid text device scale", E_INVALIDARG);
		return false;
	}
	if (!this->impl_->factory) {
		IDWriteFactory2 *factory = nullptr;
		const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
			__uuidof(IDWriteFactory2), reinterpret_cast<IUnknown **>(&factory));
		if (FAILED(hr)) {
			set_error(error, "cannot create DirectWrite factory 2", hr);
			return false;
		}
		this->impl_->factory = factory;
	}

	const QString family = font.family();
	const wstring family_w = family.toStdWString();
	const wstring locale = QLocale::system().bcp47Name().toStdWString();
	float size = float(font.pixelSize());
	if (size <= 0)
		size = float(font.pointSizeF() * 96. / 72.);
	size *= device_scale;
	if (family_w.empty() || !(size > 0) || !isfinite(size)) {
		set_error(error, "invalid DirectWrite font request", E_INVALIDARG);
		return false;
	}

	IDWriteFontCollection *collection = nullptr;
	HRESULT hr =
		this->impl_->factory->GetSystemFontCollection(&collection, TRUE);
	if (FAILED(hr)) {
		set_error(error, "cannot obtain DirectWrite font collection", hr);
		return false;
	}
	const DWRITE_FONT_WEIGHT normal_weight = DWRITE_FONT_WEIGHT(font.weight());
	const DWRITE_FONT_WEIGHT bold_weight =
		max(normal_weight, DWRITE_FONT_WEIGHT_BOLD);
	IDWriteTextFormat *normal = nullptr, *bold = nullptr;
	hr = this->impl_->factory->CreateTextFormat(family_w.c_str(), collection,
		normal_weight, font_style(font.style()), font_stretch(font.stretch()),
		size, locale.c_str(), &normal);
	if (SUCCEEDED(hr))
		hr = this->impl_->factory->CreateTextFormat(family_w.c_str(),
			collection, bold_weight, font_style(font.style()),
			font_stretch(font.stretch()), size, locale.c_str(), &bold);
	if (FAILED(hr)) {
		if (normal)
			normal->Release();
		collection->Release();
		set_error(error, "cannot create DirectWrite text format", hr);
		return false;
	}

	this->impl_->clear_fonts();
	if (this->impl_->bold)
		this->impl_->bold->Release();
	if (this->impl_->normal)
		this->impl_->normal->Release();
	if (this->impl_->collection)
		this->impl_->collection->Release();
	this->impl_->collection = collection;
	this->impl_->normal = normal;
	this->impl_->bold = bold;
	this->generation_++;
	if (error)
		error->clear();
	return true;
}

bool
TextBackend::settings_changed() const
{
	if (!this->impl_->factory || !this->impl_->collection)
		return false;
	IDWriteFontCollection *collection = nullptr;
	const HRESULT hr =
		this->impl_->factory->GetSystemFontCollection(&collection, TRUE);
	if (FAILED(hr))
		return true;
	const bool changed = collection != this->impl_->collection;
	collection->Release();
	return changed;
}

unique_ptr<TextLayout>
TextBackend::layout(
	const QString &text, const TextOptions &options, string *error)
{
	if (!this->impl_->factory || !this->impl_->normal || !this->impl_->bold) {
		set_error(error, "DirectWrite text backend is not initialized", E_FAIL);
		return {};
	}
	auto result = unique_ptr<TextLayout>(new TextLayout);
	result->impl_ = make_unique<TextLayoutImpl>();
	result->text_ = text;
	HRESULT hr = elide_layout(
		*this->impl_, text, options, &result->text_, &result->impl_->layout);
	if (FAILED(hr)) {
		set_error(error, "cannot create DirectWrite text layout", hr);
		return {};
	}

	vector<DWRITE_LINE_METRICS> native_lines;
	hr = line_metrics(result->impl_->layout, &native_lines);
	if (FAILED(hr)) {
		set_error(error, "cannot obtain DirectWrite line metrics", hr);
		return {};
	}
	float y = 0;
	float width = 0;
	int start = 0;
	for (const DWRITE_LINE_METRICS &line : native_lines) {
		TextLine stored;
		stored.text_start = start;
		stored.text_length = int(line.length);
		stored.baseline = y + line.baseline;
		stored.height = line.height;
		for (const DWRITE_HIT_TEST_METRICS &metric :
			hit_metrics(result->impl_->layout, UINT32(start), line.length))
			stored.advance += metric.width;
		width = max(width, stored.advance);
		result->lines_.push_back(stored);
		start += int(line.length);
		y += line.height;
	}

	DWRITE_TEXT_METRICS metrics{};
	hr = result->impl_->layout->GetMetrics(&metrics);
	if (FAILED(hr)) {
		set_error(error, "cannot obtain DirectWrite text metrics", hr);
		return {};
	}
	result->width_ = width;
	result->height_ = metrics.height;

	RunCollector *collector = new RunCollector(this->impl_.get(),
		result->impl_.get(), &result->glyphs_, &result->lines_);
	hr = result->impl_->layout->Draw(nullptr, collector, 0, 0);
	if (SUCCEEDED(hr))
		hr = collector->failure();
	collector->Release();
	if (FAILED(hr)) {
		set_error(error, "cannot enumerate DirectWrite glyph runs", hr);
		return {};
	}
	if (error)
		error->clear();
	return result;
}

GlyphImage
TextBackend::rasterize(uint32_t font_id, uint32_t glyph_id, int phase) const
{
	GlyphImage result;
	if (!this->impl_->factory || font_id >= this->impl_->fonts.size() ||
		phase < 0 || phase >= 4 || glyph_id > numeric_limits<UINT16>::max())
		return result;

	const FontInstance &font = this->impl_->fonts[font_id];
	const UINT16 glyph = UINT16(glyph_id);
	const FLOAT advance = 0;
	const DWRITE_GLYPH_OFFSET offset{};
	const DWRITE_GLYPH_RUN run = {font.face, font.em_size, 1, &glyph, &advance,
		&offset, font.sideways, 0};
	IDWriteFontFace4 *face4 = nullptr;
	if (SUCCEEDED(font.face->QueryInterface(
			__uuidof(IDWriteFontFace4), (void **) &face4))) {
		DWRITE_GLYPH_IMAGE_FORMATS formats = DWRITE_GLYPH_IMAGE_FORMATS_NONE;
		const UINT32 ppem = UINT32(max(1L, lround(font.em_size)));
		const HRESULT formats_hr =
			face4->GetGlyphImageFormats_(glyph, ppem, ppem + 1, &formats);
		face4->Release();
		constexpr uint32_t kImageFormats = DWRITE_GLYPH_IMAGE_FORMATS_COLR |
			DWRITE_GLYPH_IMAGE_FORMATS_SVG | DWRITE_GLYPH_IMAGE_FORMATS_PNG |
			DWRITE_GLYPH_IMAGE_FORMATS_JPEG | DWRITE_GLYPH_IMAGE_FORMATS_TIFF |
			DWRITE_GLYPH_IMAGE_FORMATS_PREMULTIPLIED_B8G8R8A8 |
			DWRITE_GLYPH_IMAGE_FORMATS_COLR_PAINT_TREE;
		if (SUCCEEDED(formats_hr) && (uint32_t(formats) & kImageFormats))
			return result;
	}

	IDWriteColorGlyphRunEnumerator *colours = nullptr;
	if (SUCCEEDED(this->impl_->factory->TranslateColorGlyphRun(float(phase) / 4,
			0, &run, nullptr, font.measuring_mode, nullptr, 0, &colours))) {
		if (colours)
			colours->Release();
		return result;
	}

	IDWriteGlyphRunAnalysis *analysis = nullptr;
	const HRESULT hr = this->impl_->factory->CreateGlyphRunAnalysis(&run,
		nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC, font.measuring_mode,
		DWRITE_GRID_FIT_MODE_ENABLED, DWRITE_TEXT_ANTIALIAS_MODE_GRAYSCALE,
		float(phase) / 4, 0, &analysis);
	if (FAILED(hr))
		return result;

	RECT bounds{};
	if (FAILED(analysis->GetAlphaTextureBounds(
			DWRITE_TEXTURE_ALIASED_1x1, &bounds))) {
		analysis->Release();
		return result;
	}
	const int64_t width = int64_t(bounds.right) - bounds.left;
	const int64_t height = int64_t(bounds.bottom) - bounds.top;
	if (width < 0 || height < 0 || width > numeric_limits<int>::max() ||
		height > numeric_limits<int>::max() ||
		(width && uint64_t(height) > numeric_limits<size_t>::max() / width)) {
		analysis->Release();
		return result;
	}
	result.kind = GlyphImageKind::Mask;
	result.width = int(width);
	result.height = int(height);
	result.stride = result.width;
	result.origin_x = bounds.left;
	result.origin_y = bounds.top;
	if (uint64_t(width) * uint64_t(height) > numeric_limits<UINT32>::max()) {
		analysis->Release();
		return {};
	}
	result.pixels.resize(size_t(width) * size_t(height));
	if (!result.pixels.empty() &&
		FAILED(analysis->CreateAlphaTexture(DWRITE_TEXTURE_ALIASED_1x1, &bounds,
			result.pixels.data(), UINT32(result.pixels.size()))))
		result = {};
	analysis->Release();
	return result;
}

}  // namespace dn
