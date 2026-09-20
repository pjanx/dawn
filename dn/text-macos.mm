//
// text-macos.mm: Core Text layout and scalar Core Graphics glyph masks
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "text.hpp"

#include <QFont>
#include <QString>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{

static atomic<uint64_t> g_font_settings_generation{1};

static void
fonts_changed(CFNotificationCenterRef, void *, CFNotificationName, const void *,
	CFDictionaryRef)
{
	g_font_settings_generation.fetch_add(1, memory_order_relaxed);
}

static void
listen_for_font_changes()
{
	static const bool listening = [] {
		CFNotificationCenterAddObserver(CFNotificationCenterGetLocalCenter(),
			&g_font_settings_generation, fonts_changed,
			kCTFontManagerRegisteredFontsChangedNotification, nullptr,
			CFNotificationSuspensionBehaviorDeliverImmediately);
		return true;
	}();
	(void) listening;
}

static CFStringRef
cf_string(const QString &text)
{
	return CFStringCreateWithCharacters(kCFAllocatorDefault,
		reinterpret_cast<const UniChar *>(text.utf16()), text.size());
}

static void
set_error(string *error, const char *message)
{
	if (error)
		*error = message;
}

static bool
same_font(CTFontRef a, CTFontRef b)
{
	return a == b || (a && b && CFEqual(a, b));
}

static CGFloat
font_weight(int weight)
{
	if (weight <= int(QFont::Thin))
		return NSFontWeightUltraLight;
	if (weight <= int(QFont::ExtraLight))
		return NSFontWeightThin;
	if (weight <= int(QFont::Light))
		return NSFontWeightLight;
	if (weight <= int(QFont::Normal))
		return NSFontWeightRegular;
	if (weight <= int(QFont::Medium))
		return NSFontWeightMedium;
	if (weight <= int(QFont::DemiBold))
		return NSFontWeightSemibold;
	if (weight <= int(QFont::Bold))
		return NSFontWeightBold;
	if (weight <= int(QFont::ExtraBold))
		return NSFontWeightHeavy;
	return NSFontWeightBlack;
}

// Opaque RGB is necessary for Core Graphics' native font smoothing.
static CGContextRef
mask_context(uint32_t *pixels, int width, int height)
{
	CGColorSpaceRef space = CGColorSpaceCreateDeviceRGB();
	if (!space)
		return nullptr;
	CGContextRef context = CGBitmapContextCreate(pixels, width, height, 8,
		size_t(width) * sizeof *pixels, space,
		CGBitmapInfo(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Host);
	CGColorSpaceRelease(space);
	if (!context)
		return nullptr;
	CGContextSetAllowsAntialiasing(context, true);
	CGContextSetShouldAntialias(context, true);
	CGContextSetAllowsFontSmoothing(context, true);
	CGContextSetAllowsFontSubpixelPositioning(context, true);
	CGContextSetShouldSubpixelPositionFonts(context, true);
	CGContextSetAllowsFontSubpixelQuantization(context, false);
	CGContextSetShouldSubpixelQuantizeFonts(context, false);
	CGContextSetGrayFillColor(context, 0, 1);
	CGContextSetTextDrawingMode(context, kCGTextFill);
	CGContextSetTextMatrix(context, CGAffineTransformIdentity);
	return context;
}

static bool
font_smoothing_available()
{
	// Like SkCTFontGetSmoothBehavior, compare actual rasters rather than OS
	// versions: disabled smoothing also disables CG's gamma-2 transfer.
	CTFontRef font = CTFontCreateWithName(CFSTR("Helvetica"), 16, nullptr);
	if (!font)
		return false;
	const UniChar characters[] = {'A', 'o'};
	CGGlyph glyphs[2] = {};
	if (!CTFontGetGlyphsForCharacters(font, characters, glyphs, 2)) {
		CFRelease(font);
		return false;
	}
	array<uint32_t, 48 * 32> pixels;
	pixels.fill(0xFFFFFFFF);
	CGContextRef context = mask_context(pixels.data(), 48, 32);
	if (!context) {
		CFRelease(font);
		return false;
	}
	const CGPoint positions[] = {{4.25, 8}, {24.25, 8}};
	CGContextSetShouldSmoothFonts(context, false);
	CTFontDrawGlyphs(font, glyphs, positions, 2, context);
	const auto unsmoothed = pixels;
	pixels.fill(0xFFFFFFFF);
	CGContextSetShouldSmoothFonts(context, true);
	CTFontDrawGlyphs(font, glyphs, positions, 2, context);
	CGContextRelease(context);
	CFRelease(font);
	for (size_t i = 0; i < pixels.size(); i++) {
		if ((pixels[i] & 0xFFFFFF) != (unsmoothed[i] & 0xFFFFFF))
			return true;
	}
	return false;
}

static CTFontRef
make_font(const QFont &request, float device_scale, bool bold)
{
	float size = float(request.pixelSize());
	if (size <= 0)
		size = float(request.pointSizeF());
	if (size <= 0)
		size = 13.f;
	size *= device_scale;

	CFStringRef family = cf_string(request.family());
	CFMutableDictionaryRef attributes =
		CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFNumberRef size_value =
		CFNumberCreate(kCFAllocatorDefault, kCFNumberFloatType, &size);
	if (family && CFStringGetLength(family))
		CFDictionarySetValue(attributes, kCTFontFamilyNameAttribute, family);
	CFDictionarySetValue(attributes, kCTFontSizeAttribute, size_value);

	CFMutableDictionaryRef traits =
		CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const int weight = bold ? max(int(request.weight()), int(QFont::Bold))
							: int(request.weight());
	const CGFloat native_weight = font_weight(weight);
	CFNumberRef weight_value = CFNumberCreate(
		kCFAllocatorDefault, kCFNumberCGFloatType, &native_weight);
	CFDictionarySetValue(traits, kCTFontWeightTrait, weight_value);

	CTFontSymbolicTraits symbolic = 0;
	if (request.italic())
		symbolic |= kCTFontItalicTrait;
	CFNumberRef symbolic_value =
		CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &symbolic);
	CFDictionarySetValue(traits, kCTFontSymbolicTrait, symbolic_value);
	CFDictionarySetValue(attributes, kCTFontTraitsAttribute, traits);

	CTFontDescriptorRef descriptor =
		CTFontDescriptorCreateWithAttributes(attributes);
	const float stretch = request.stretch() > 0
		? float(request.stretch()) / float(QFont::Unstretched)
		: 1.f;
	const CGAffineTransform transform = CGAffineTransformMakeScale(stretch, 1);
	CTFontRef font = descriptor
		? CTFontCreateWithFontDescriptor(descriptor, size, &transform)
		: nullptr;

	if (descriptor)
		CFRelease(descriptor);
	CFRelease(symbolic_value);
	CFRelease(weight_value);
	CFRelease(traits);
	CFRelease(size_value);
	CFRelease(attributes);
	if (family)
		CFRelease(family);
	return font;
}

struct TextBackendImpl {
	uint64_t settings_generation = 0;
	bool smooth_fonts = false;
	CTFontRef regular = nullptr;
	CTFontRef bold = nullptr;
	vector<CTFontRef> fonts;

	~TextBackendImpl();
	void clear();
	uint32_t font_id(CTFontRef font);
};

TextBackendImpl::~TextBackendImpl()
{
	clear();
}

void
TextBackendImpl::clear()
{
	for (CTFontRef font : this->fonts)
		CFRelease(font);
	this->fonts.clear();
	if (this->bold)
		CFRelease(this->bold);
	if (this->regular)
		CFRelease(this->regular);
	this->bold = nullptr;
	this->regular = nullptr;
}

uint32_t
TextBackendImpl::font_id(CTFontRef font)
{
	for (size_t i = 0; i < this->fonts.size(); i++) {
		if (same_font(this->fonts[i], font))
			return uint32_t(i);
	}
	CFRetain(font);
	this->fonts.push_back(font);
	return uint32_t(this->fonts.size() - 1);
}

struct TextLayoutImpl {
	struct NativeLine {
		CTLineRef line = nullptr;
		float x = 0;
		float top = 0;
		float baseline = 0;
		float ascent = 0;
		float descent = 0;
		float leading = 0;
	};

	vector<NativeLine> lines;
	vector<CTFontRef> fonts;

	~TextLayoutImpl();
	void retain_font(CTFontRef font);
};

TextLayoutImpl::~TextLayoutImpl()
{
	for (const NativeLine &line : this->lines) {
		if (line.line)
			CFRelease(line.line);
	}
	for (CTFontRef font : this->fonts)
		CFRelease(font);
}

void
TextLayoutImpl::retain_font(CTFontRef font)
{
	for (CTFontRef current : this->fonts) {
		if (same_font(current, font))
			return;
	}
	CFRetain(font);
	this->fonts.push_back(font);
}

static CFAttributedStringRef
make_attributed(const QString &text, CTFontRef font)
{
	CFStringRef string = cf_string(text);
	if (!string)
		return nullptr;
	const void *keys[] = {kCTFontAttributeName};
	const void *values[] = {font};
	CFDictionaryRef attributes =
		CFDictionaryCreate(kCFAllocatorDefault, keys, values, 1,
			&kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFAttributedStringRef result =
		CFAttributedStringCreate(kCFAllocatorDefault, string, attributes);
	CFRelease(attributes);
	CFRelease(string);
	return result;
}

static bool
line_separator(QChar c)
{
	return c == QChar('\n') || c == QChar('\r') || c == QChar(0x2028) ||
		c == QChar(0x2029);
}

// Core Text may return zero when the first cluster is wider than the line.
// Find the smallest width at which it returns a complete native cluster.
static CFIndex
first_cluster(CTTypesetterRef typesetter, CFIndex start)
{
	double high = 1;
	CFIndex count = 0;
	while (!count && high < 1.0e8) {
		count = CTTypesetterSuggestClusterBreak(typesetter, start, high);
		high *= 2;
	}
	if (!count)
		return 0;

	double low = 0;
	for (int i = 0; i < 24; i++) {
		const double middle = (low + high) * .5;
		const CFIndex candidate =
			CTTypesetterSuggestClusterBreak(typesetter, start, middle);
		if (candidate) {
			count = candidate;
			high = middle;
		} else
			low = middle;
	}
	return count;
}

static bool
fits_lines(const QString &text, CTFontRef font, const TextOptions &options)
{
	CFAttributedStringRef attributed = make_attributed(text, font);
	CTTypesetterRef typesetter = attributed
		? CTTypesetterCreateWithAttributedString(attributed)
		: nullptr;
	if (!typesetter) {
		if (attributed)
			CFRelease(attributed);
		return false;
	}

	CFIndex start = 0;
	bool last_within_width = true;
	for (int line = 0; line < options.max_lines && start < text.size();
		line++) {
		CFIndex count =
			CTTypesetterSuggestLineBreak(typesetter, start, options.wrap_width);
		if (!count)
			count = first_cluster(typesetter, start);
		if (!count)
			break;
		CTLineRef native =
			CTTypesetterCreateLine(typesetter, CFRangeMake(start, count));
		if (!native)
			break;
		last_within_width = CTLineGetTypographicBounds(native, nullptr, nullptr,
								nullptr) <= double(options.wrap_width) + .01;
		CFRelease(native);
		start += count;
	}
	const bool fits = start == text.size() && last_within_width;
	CFRelease(typesetter);
	CFRelease(attributed);
	return fits;
}

static CFIndex
previous_cluster(
	CTTypesetterRef typesetter, CFIndex start, CFIndex current, double width)
{
	double low = 0;
	double high = width;
	CFIndex previous = 0;
	for (int i = 0; i < 24; i++) {
		const double middle = (low + high) * .5;
		const CFIndex candidate =
			CTTypesetterSuggestClusterBreak(typesetter, start, middle);
		if (candidate < current) {
			previous = max(previous, candidate);
			low = middle;
		} else
			high = middle;
	}
	return previous;
}

static QString
materialize_text(
	const QString &text, CTFontRef font, const TextOptions &options)
{
	if (options.max_lines <= 0 || options.wrap_width <= 0 || text.isEmpty())
		return text;
	CFAttributedStringRef attributed = make_attributed(text, font);
	CTTypesetterRef typesetter = attributed
		? CTTypesetterCreateWithAttributedString(attributed)
		: nullptr;
	if (!typesetter) {
		if (attributed)
			CFRelease(attributed);
		return text;
	}

	CFIndex start = 0;
	for (int line = 0; line < options.max_lines && start < text.size();
		line++) {
		CFIndex count =
			CTTypesetterSuggestLineBreak(typesetter, start, options.wrap_width);
		if (count <= 0)
			count = first_cluster(typesetter, start);
		if (!count)
			break;
		CTLineRef proposed =
			CTTypesetterCreateLine(typesetter, CFRangeMake(start, count));
		const bool too_wide = !proposed ||
			CTLineGetTypographicBounds(proposed, nullptr, nullptr, nullptr) >
				double(options.wrap_width) + .01;
		if (proposed)
			CFRelease(proposed);
		const bool terminal_empty = start + count == text.size() && count &&
			line_separator(text.at(start + count - 1));
		if ((line + 1 != options.max_lines && !too_wide) ||
			(start + count >= text.size() && !terminal_empty && !too_wide)) {
			start += count;
			continue;
		}

		CFAttributedStringRef ellipsis_text =
			make_attributed(QString(QChar(0x2026)), font);
		CTLineRef ellipsis = ellipsis_text
			? CTLineCreateWithAttributedString(ellipsis_text)
			: nullptr;
		const double ellipsis_width = ellipsis
			? CTLineGetTypographicBounds(ellipsis, nullptr, nullptr, nullptr)
			: 0;
		if (!ellipsis || ellipsis_width > double(options.wrap_width) + .01) {
			if (ellipsis)
				CFRelease(ellipsis);
			if (ellipsis_text)
				CFRelease(ellipsis_text);
			CFRelease(typesetter);
			CFRelease(attributed);
			return text.left(start);
		}
		const double available =
			max(0., double(options.wrap_width) - ellipsis_width);
		CFIndex keep =
			CTTypesetterSuggestClusterBreak(typesetter, start, available);
		CFIndex line_content = count;
		while (
			line_content && line_separator(text.at(start + line_content - 1)))
			line_content--;
		keep = min(keep, line_content);
		QString candidate = text.left(start + keep) + QChar(0x2026);
		while (keep && !fits_lines(candidate, font, options)) {
			keep = previous_cluster(typesetter, start, keep, available);
			candidate = text.left(start + keep) + QChar(0x2026);
		}
		if (ellipsis)
			CFRelease(ellipsis);
		if (ellipsis_text)
			CFRelease(ellipsis_text);
		CFRelease(typesetter);
		CFRelease(attributed);
		return candidate;
	}
	CFRelease(typesetter);
	CFRelease(attributed);
	return text;
}

static void
append_glyphs(TextBackendImpl *backend, TextLayoutImpl *layout,
	vector<TextGlyph> &output, const TextLayoutImpl::NativeLine &native)
{
	if (!native.line)
		return;

	CFArrayRef runs = CTLineGetGlyphRuns(native.line);
	const CFIndex run_count = runs ? CFArrayGetCount(runs) : 0;
	for (CFIndex r = 0; r < run_count; r++) {
		CTRunRef run = CTRunRef(CFArrayGetValueAtIndex(runs, r));
		CFDictionaryRef attributes = CTRunGetAttributes(run);
		CTFontRef font =
			CTFontRef(CFDictionaryGetValue(attributes, kCTFontAttributeName));
		if (!font)
			continue;
		layout->retain_font(font);
		const uint32_t font_id = backend->font_id(font);

		const CFIndex count = CTRunGetGlyphCount(run);
		vector<CGGlyph> glyphs((size_t(count)));
		vector<CGPoint> positions((size_t(count)));
		if (count) {
			CTRunGetGlyphs(run, CFRangeMake(0, 0), glyphs.data());
			CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
		}
		for (CFIndex i = 0; i < count; i++) {
			TextGlyph glyph;
			glyph.font_id = font_id;
			glyph.glyph_id = glyphs[size_t(i)];
			glyph.x = native.x + float(positions[size_t(i)].x);
			glyph.y = native.baseline - float(positions[size_t(i)].y);
			output.push_back(glyph);
		}
	}
}

TextLayout::TextLayout() = default;
TextLayout::~TextLayout() = default;

TextBackend::TextBackend() : impl_(make_unique<TextBackendImpl>())
{
	listen_for_font_changes();
}
TextBackend::~TextBackend() = default;

bool
TextBackend::reset(const QFont &font, float device_scale, string *error)
{
	if (!(device_scale > 0) || !isfinite(device_scale)) {
		set_error(error, "invalid text device scale");
		return false;
	}

	CTFontRef regular = make_font(font, device_scale, false);
	CTFontRef bold = make_font(font, device_scale, true);
	if (!regular || !bold) {
		if (bold)
			CFRelease(bold);
		if (regular)
			CFRelease(regular);
		set_error(error, "Core Text could not resolve the requested font");
		return false;
	}

	this->impl_->clear();
	this->impl_->regular = regular;
	this->impl_->bold = bold;
	this->impl_->smooth_fonts = font_smoothing_available();
	this->impl_->settings_generation =
		g_font_settings_generation.load(memory_order_relaxed);
	this->generation_++;
	if (error)
		error->clear();
	return true;
}

bool
TextBackend::settings_changed() const
{
	return this->impl_->settings_generation !=
		g_font_settings_generation.load(memory_order_relaxed);
}

unique_ptr<TextLayout>
TextBackend::layout(
	const QString &text, const TextOptions &options, string *error)
{
	CTFontRef base = options.bold ? this->impl_->bold : this->impl_->regular;
	if (!base) {
		set_error(error, "text backend has not been initialized");
		return nullptr;
	}

	const QString displayed = materialize_text(text, base, options);
	CFAttributedStringRef attributed = make_attributed(displayed, base);
	CTTypesetterRef typesetter = attributed
		? CTTypesetterCreateWithAttributedString(attributed)
		: nullptr;
	if (!attributed || !typesetter) {
		if (typesetter)
			CFRelease(typesetter);
		if (attributed)
			CFRelease(attributed);
		set_error(error, "Core Text could not create a typesetter");
		return nullptr;
	}

	auto result = unique_ptr<TextLayout>(new TextLayout);
	result->impl_ = make_unique<TextLayoutImpl>();
	result->text_ = displayed;
	const CFIndex text_length = displayed.size();
	const double wrap =
		options.wrap_width > 0 ? double(options.wrap_width) : 1.0e8;
	CFIndex start = 0;
	float top = 0;
	int line_number = 0;

	do {
		CFIndex count = start < text_length
			? CTTypesetterSuggestLineBreak(typesetter, start, wrap)
			: 0;
		if (start < text_length && count <= 0)
			count = first_cluster(typesetter, start);
		if (start < text_length && !count) {
			set_error(error, "Core Text could not find a cluster break");
			CFRelease(typesetter);
			CFRelease(attributed);
			return nullptr;
		}
		CTLineRef native_line =
			CTTypesetterCreateLine(typesetter, CFRangeMake(start, count));
		if (!native_line && count) {
			set_error(error, "Core Text could not create a line");
			CFRelease(typesetter);
			CFRelease(attributed);
			return nullptr;
		}

		TextLayoutImpl::NativeLine native;
		native.line = native_line;
		double ascent = CTFontGetAscent(base);
		double descent = CTFontGetDescent(base);
		double leading = CTFontGetLeading(base);
		double advance = 0;
		if (native_line && count)
			advance = CTLineGetTypographicBounds(
				native_line, &ascent, &descent, &leading);
		native.ascent = float(ascent);
		native.descent = float(descent);
		native.leading = float(leading);
		native.top = top;
		native.baseline = top + native.ascent;
		native.x = options.align == TextAlign::Center && options.wrap_width > 0
			? max(0.f, (float(options.wrap_width) - float(advance)) * .5f)
			: 0;

		TextLine line;
		line.text_start = int(start);
		line.text_length = int(count);
		line.baseline = native.baseline;
		line.advance = float(advance);
		line.height = native.ascent + native.descent + native.leading;
		if (!(line.height > 0))
			line.height = float(CTFontGetSize(base));
		line.underline_position = float(-CTFontGetUnderlinePosition(base));
		line.underline_thickness =
			max(1.f, float(CTFontGetUnderlineThickness(base)));
		result->impl_->lines.push_back(native);
		append_glyphs(this->impl_.get(), result->impl_.get(), result->glyphs_,
			result->impl_->lines.back());
		result->lines_.push_back(line);
		result->width_ = max(result->width_, float(advance));
		top += line.height;
		line_number++;
		start += count;
		if (!count)
			break;
	} while (start < text_length || !line_number ||
		(start == text_length && text_length &&
			line_separator(displayed.back())));

	result->height_ = top;
	CFRelease(typesetter);
	CFRelease(attributed);
	if (error)
		error->clear();
	return result;
}

static size_t
line_for_index(span<const TextLine> lines, int text_length, int index,
	TextAffinity affinity)
{
	if (lines.empty())
		return 0;
	index = clamp(index, 0, text_length);
	for (size_t i = 0; i < lines.size(); i++) {
		const TextLine &line = lines[i];
		const int end = line.text_start + line.text_length;
		if (index < end ||
			(index == end &&
				(affinity == TextAffinity::Trailing || i + 1 == lines.size())))
			return i;
	}
	return lines.size() - 1;
}

static pair<CGFloat, CGFloat>
run_bounds(CTRunRef run)
{
	const CFIndex count = CTRunGetGlyphCount(run);
	if (!count)
		return {};
	vector<CGPoint> positions((size_t(count)));
	vector<CGSize> advances((size_t(count)));
	CTRunGetPositions(run, CFRangeMake(0, 0), positions.data());
	CTRunGetAdvances(run, CFRangeMake(0, 0), advances.data());
	CGFloat left = numeric_limits<CGFloat>::infinity();
	CGFloat right = -numeric_limits<CGFloat>::infinity();
	for (CFIndex i = 0; i < count; i++) {
		const CGFloat a = positions[size_t(i)].x;
		const CGFloat b = a + advances[size_t(i)].width;
		left = min(left, min(a, b));
		right = max(right, max(a, b));
	}
	return {left, right};
}

static CGFloat
run_offset(CTLineRef line, CTRunRef run, CFIndex index)
{
	CGFloat secondary = 0;
	const CGFloat primary =
		CTLineGetOffsetForStringIndex(line, index, &secondary);
	if (primary == secondary)
		return primary;

	const CFRange range = CTRunGetStringRange(run);
	if (range.location == kCFNotFound || !CTRunGetGlyphCount(run))
		return primary;
	const auto [left, right] = run_bounds(run);
	CGFloat expected = primary;
	const bool rtl = CTRunGetStatus(run) & kCTRunStatusRightToLeft;
	if (index <= range.location)
		expected = rtl ? right : left;
	else if (index >= range.location + range.length)
		expected = rtl ? left : right;
	else
		return primary;
	return abs(secondary - expected) < abs(primary - expected) ? secondary
															   : primary;
}

static CGFloat
caret_offset(CTLineRef line, const TextLine &logical, CFIndex index,
	TextAffinity affinity)
{
	if (!line)
		return 0;
	const CFIndex begin = logical.text_start;
	const CFIndex end = begin + logical.text_length;
	const CFIndex character = affinity == TextAffinity::Leading
		? min(index, end - 1)
		: max(begin, index - 1);
	CFArrayRef runs = CTLineGetGlyphRuns(line);
	const CFIndex count = runs ? CFArrayGetCount(runs) : 0;
	for (CFIndex i = 0; i < count; i++) {
		CTRunRef run = CTRunRef(CFArrayGetValueAtIndex(runs, i));
		const CFRange range = CTRunGetStringRange(run);
		if (range.location == kCFNotFound || character < range.location ||
			character >= range.location + range.length)
			continue;
		return run_offset(line, run, index);
	}
	return CTLineGetOffsetForStringIndex(line, index, nullptr);
}

TextRect
TextLayout::caret(int index, TextAffinity affinity) const
{
	if (this->lines_.empty())
		return {};
	const size_t i =
		line_for_index(this->lines_, int(this->text_.size()), index, affinity);
	const TextLine &line = this->lines_[i];
	const TextLayoutImpl::NativeLine &native = this->impl_->lines[i];
	index = clamp(index, line.text_start, line.text_start + line.text_length);
	const CGFloat offset = caret_offset(native.line, line, index, affinity);
	return {native.x + float(offset), native.top, 1, line.height};
}

TextHit
TextLayout::hit_test(float x, float y) const
{
	if (this->lines_.empty())
		return {};
	size_t line_index = this->lines_.size() - 1;
	for (size_t i = 0; i < this->lines_.size(); i++) {
		if (y < this->impl_->lines[i].top + this->lines_[i].height) {
			line_index = i;
			break;
		}
	}
	const TextLine &line = this->lines_[line_index];
	const TextLayoutImpl::NativeLine &native = this->impl_->lines[line_index];
	if (!native.line)
		return {line.text_start, TextAffinity::Leading};
	CFIndex index = CTLineGetStringIndexForPosition(
		native.line, CGPointMake(x - native.x, 0));
	if (index == kCFNotFound)
		index =
			x < native.x ? line.text_start : line.text_start + line.text_length;
	index = clamp(index, CFIndex(line.text_start),
		CFIndex(line.text_start + line.text_length));
	const CGFloat leading =
		caret_offset(native.line, line, index, TextAffinity::Leading);
	const CGFloat trailing =
		caret_offset(native.line, line, index, TextAffinity::Trailing);
	TextAffinity affinity =
		abs(x - native.x - trailing) < abs(x - native.x - leading)
		? TextAffinity::Trailing
		: TextAffinity::Leading;
	if (index == line.text_start && line_index)
		affinity = TextAffinity::Leading;
	else if (index == line.text_start + line.text_length &&
		line_index + 1 < this->lines_.size())
		affinity = TextAffinity::Trailing;
	return {int(index), affinity};
}

vector<TextRect>
TextLayout::range_rects(int start, int length) const
{
	vector<TextRect> result;
	const int text_length = int(this->text_.size());
	const int selection_start = clamp(start, 0, text_length);
	const int selection_end =
		clamp(start + max(0, length), selection_start, text_length);
	if (selection_start == selection_end)
		return result;

	for (size_t i = 0; i < this->lines_.size(); i++) {
		const TextLine &line = this->lines_[i];
		const TextLayoutImpl::NativeLine &native = this->impl_->lines[i];
		if (!native.line)
			continue;
		CFArrayRef runs = CTLineGetGlyphRuns(native.line);
		const CFIndex run_count = runs ? CFArrayGetCount(runs) : 0;
		for (CFIndex r = 0; r < run_count; r++) {
			CTRunRef run = CTRunRef(CFArrayGetValueAtIndex(runs, r));
			const CFRange range = CTRunGetStringRange(run);
			if (range.location == kCFNotFound)
				continue;
			const int from = max(selection_start, int(range.location));
			const int to =
				min(selection_end, int(range.location + range.length));
			if (from >= to)
				continue;
			const CGFloat a = run_offset(native.line, run, from);
			const CGFloat b = run_offset(native.line, run, to);
			result.push_back({native.x + float(min(a, b)), native.top,
				float(abs(b - a)), line.height});
		}
	}
	return result;
}

static GlyphImage
raster_mask(CTFontRef font, CGGlyph glyph, float offset, int pad, bool smooth)
{
	GlyphImage result;
	result.kind = GlyphImageKind::Mask;
	CGRect bounds = CTFontGetBoundingRectsForGlyphs(
		font, kCTFontOrientationHorizontal, &glyph, nullptr, 1);
	if (CGRectIsNull(bounds) || CGRectIsInfinite(bounds)) {
		result.kind = GlyphImageKind::Missing;
		return result;
	}
	if (CGRectIsEmpty(bounds))
		return result;
	// Coloured glyphs and bitmap-only glyphs are images, not scalar masks.
	// Keep them explicit until the coloured-glyph result is implemented.
	if (CTFontGetSymbolicTraits(font) & kCTFontColorGlyphsTrait) {
		result.kind = GlyphImageKind::Missing;
		return result;
	}
	CGPathRef outline = CTFontCreatePathForGlyph(font, glyph, nullptr);
	if (!outline) {
		result.kind = GlyphImageKind::Missing;
		return result;
	}
	CGPathRelease(outline);

	const int left = int(floor(CGRectGetMinX(bounds) + offset)) - pad;
	const int bottom = int(floor(CGRectGetMinY(bounds))) - pad;
	const int right = int(ceil(CGRectGetMaxX(bounds) + offset)) + pad;
	const int top = int(ceil(CGRectGetMaxY(bounds))) + pad;
	const int width = right - left;
	const int height = top - bottom;
	if (width <= 0 || height <= 0 || width > 65536 || height > 65536 ||
		size_t(width) >
			numeric_limits<size_t>::max() / sizeof(uint32_t) / size_t(height)) {
		result.kind = GlyphImageKind::Missing;
		return result;
	}

	vector<uint32_t> rgb(size_t(width) * size_t(height), 0xFFFFFFFF);
	CGContextRef context = mask_context(rgb.data(), width, height);
	if (!context) {
		result.kind = GlyphImageKind::Missing;
		return result;
	}
	CGContextSetShouldSmoothFonts(context, smooth);
	const CGPoint position = CGPointMake(offset - left, -bottom);
	CTFontDrawGlyphs(font, &glyph, &position, 1, context);
	CGContextRelease(context);

	// Follow Skia's SkScalerContext_mac_ct.cpp: undo the smoothing transfer
	// on RGB before inversion, then collapse to A8.  This is not an sRGB
	// decode or a power curve on alpha, and preserves native stem darkening.
	vector<uint8_t> pixels(rgb.size());
	for (size_t i = 0; i < rgb.size(); i++) {
		unsigned r = (rgb[i] >> 16) & 255;
		unsigned g = (rgb[i] >> 8) & 255;
		unsigned b = rgb[i] & 255;
		if (smooth) {
			r = (r * r + 128) / 255;
			g = (g * g + 128) / 255;
			b = (b * b + 128) / 255;
		}
		pixels[i] =
			uint8_t((54 * (255 - r) + 183 * (255 - g) + 19 * (255 - b)) >> 8);
	}

	int ink_left = width, min_row = height, ink_right = -1, max_row = -1;
	for (int y = 0; y < height; y++) {
		const uint8_t *row = pixels.data() + size_t(y) * size_t(width);
		for (int x = 0; x < width; x++) {
			if (!row[x])
				continue;
			ink_left = min(ink_left, x);
			ink_right = max(ink_right, x);
			min_row = min(min_row, y);
			max_row = max(max_row, y);
		}
	}
	if (ink_right < ink_left)
		return result;
	if ((ink_left == 0 || ink_right == width - 1 || min_row == 0 ||
			max_row == height - 1) &&
		pad < 16)
		return raster_mask(font, glyph, offset, 16, smooth);

	result.width = ink_right - ink_left + 1;
	result.height = max_row - min_row + 1;
	result.stride = result.width;
	result.origin_x = left + ink_left;
	// CGBitmapContext exposes its first scanline as the top row.  The native
	// bounds are y-up, so the top of the allocation is -top in Dawn's space.
	result.origin_y = -top + min_row;
	result.pixels.resize(size_t(result.width) * size_t(result.height));
	for (int y = 0; y < result.height; y++) {
		const int source_y = min_row + y;
		copy_n(pixels.data() + size_t(source_y) * size_t(width) + ink_left,
			result.width,
			result.pixels.data() + size_t(y) * size_t(result.stride));
	}
	return result;
}

GlyphImage
TextBackend::rasterize(uint32_t font_id, uint32_t glyph_id, int phase) const
{
	if (font_id >= this->impl_->fonts.size() || phase < 0 || phase >= 4 ||
		glyph_id > numeric_limits<CGGlyph>::max())
		return {};
	return raster_mask(this->impl_->fonts[font_id], CGGlyph(glyph_id),
		float(phase) * .25f, 4, this->impl_->smooth_fonts);
}

}  // namespace dn
