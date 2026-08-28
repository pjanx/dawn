//
// kit.cpp: Vulkan overlay kit and atlas engine
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "kit.hpp"
#include "renderer.hpp"

#include <QFile>
#include <QFontDatabase>
#include <QFontInfo>
#include <QFontMetricsF>
#include <QGlyphRun>
#include <QImage>
#include <QKeyEvent>
#include <QPainter>
#include <QPen>
#include <QTextBoundaryFinder>
#include <QTextLayout>
#include <QTextLine>
#include <QTextOption>

#include <resvg.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace dn
{

// --- Atlas -------------------------------------------------------------------

namespace
{

constexpr int kAtlasStart = 512;
constexpr int kAtlasMax = 4096;

uint16_t
widen8(uint8_t v)
{
	return uint16_t((uint16_t(v) << 8) | v);
}

Colour
u8_colour(uint8_t r, uint8_t g, uint8_t b)
{
	return {r / 255.0f, g / 255.0f, b / 255.0f, 1.0f};
}

Colour
bake_rgb(Cmm *cmm, Profile *target, uint8_t r, uint8_t g, uint8_t b)
{
	Colour colour = u8_colour(r, g, b);
	if (!cmm || !target)
		return colour;
	uint16_t pixel[4] = {
		uint16_t(b * 257), uint16_t(g * 257), uint16_t(r * 257), 65535};
	auto srgb = cmm->get_profile_sRGB();
	if (!srgb)
		return colour;
	if (!cmm->transform_bgra16(
			(uint8_t *) pixel, 1, 1, srgb.get(), target, false, false))
		return colour;
	return {
		pixel[2] / 65535.0f, pixel[1] / 65535.0f, pixel[0] / 65535.0f, 1.0f};
}

Colour
bake_grey(Cmm *cmm, Profile *target, uint8_t v)
{
	return bake_rgb(cmm, target, v, v, v);
}

QImage
raster_symbolic(const char *name, int px)
{
	if (px < 1)
		return {};
	QFile file(QStringLiteral(":/dn/icons/%1.svg").arg(QLatin1String(name)));
	if (!file.open(QIODevice::ReadOnly))
		return {};
	const QByteArray bytes = file.readAll();
	resvg_options *opt = resvg_options_create();
	resvg_render_tree *tree = nullptr;
	const int32_t err = resvg_parse_tree_from_data(
		bytes.constData(), uintptr_t(bytes.size()), opt, &tree);
	resvg_options_destroy(opt);
	if (err != RESVG_OK || !tree)
		return {};
	const resvg_size size = resvg_get_image_size(tree);
	if (size.width <= 0.f || size.height <= 0.f) {
		resvg_tree_destroy(tree);
		return {};
	}
	resvg_transform transform = resvg_transform_identity();
	const float scale = float(px) / max(size.width, size.height);
	transform.a = scale;
	transform.d = scale;
	vector<char> pixmap(size_t(px) * size_t(px) * 4, 0);
	resvg_render(tree, transform, uint32_t(px), uint32_t(px), pixmap.data());
	resvg_tree_destroy(tree);

	QImage image(px, px, QImage::Format_ARGB32);
	constexpr int cr = 0xff, cg = 0xff, cb = 0xff;
	const size_t stride = size_t(px) * 4;
	for (int y = 0; y < px; ++y) {
		auto *row = (QRgb *) image.scanLine(y);
		const auto *src =
			(const unsigned char *) (pixmap.data() + size_t(y) * stride);
		for (int x = 0; x < px; ++x)
			row[x] = qRgba(cr, cg, cb, src[x * 4 + 3]);
	}
	return image;
}

QImage
raster_window_button(const char *name, int px)
{
	enum class Kind : uint8_t { None, Min, Max, Rest, Close };
	Kind kind = Kind::None;
	if (name && strcmp(name, "window-minimize") == 0)
		kind = Kind::Min;
	else if (name && strcmp(name, "window-maximize") == 0)
		kind = Kind::Max;
	else if (name && strcmp(name, "window-restore") == 0)
		kind = Kind::Rest;
	else if (name && strcmp(name, "window-close") == 0)
		kind = Kind::Close;
	if (kind == Kind::None || px < 1)
		return {};
	QImage image(px, px, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::transparent);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	const qreal sw = max(1.25, qreal(px) / 12.0);
	painter.setPen(QPen(Qt::white, sw, Qt::SolidLine, Qt::SquareCap,
		Qt::MiterJoin));
	painter.setBrush(Qt::NoBrush);
	const qreal m = qreal(px) * 0.22;
	const QRectF box(m, m, qreal(px) - 2.0 * m, qreal(px) - 2.0 * m);
	switch (kind) {
	case Kind::Min:
		painter.drawLine(QPointF(m, qreal(px) * 0.55),
			QPointF(qreal(px) - m, qreal(px) * 0.55));
		break;
	case Kind::Max:
		painter.drawRect(box);
		break;
	case Kind::Rest: {
		const qreal s = box.width() * 0.68;
		const QRectF back(box.right() - s, box.top(), s, s);
		const QRectF front(box.left(), box.bottom() - s, s, s);
		painter.drawRect(back);
		painter.setCompositionMode(QPainter::CompositionMode_Source);
		painter.fillRect(front.adjusted(-sw, -sw, sw, sw), Qt::transparent);
		painter.setCompositionMode(QPainter::CompositionMode_SourceOver);
		painter.drawRect(front);
		break;
	}
	case Kind::Close:
		painter.drawLine(box.topLeft(), box.bottomRight());
		painter.drawLine(box.topRight(), box.bottomLeft());
		break;
	case Kind::None:
		break;
	}
	return image;
}

// boundingRect is the outline box, not the bitmap origin (Core Text pads).
QImage
raster_glyph(const QRawFont &raw, quint32 gid, QPoint *origin)
{
	constexpr int kPad = 4;
	const QRect box = raw.boundingRect(gid).toAlignedRect().adjusted(
		-kPad, -kPad, kPad, kPad);
	if (box.isEmpty())
		return {};
	QImage img(box.size(), QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	QPainter painter(&img);
	painter.setPen(Qt::black);
	QGlyphRun run;
	run.setRawFont(raw);
	run.setGlyphIndexes({gid});
	run.setPositions({-box.topLeft()});
	painter.drawGlyphRun({}, run);
	painter.end();

	QRect ink;
	for (int y = 0; y < img.height(); ++y) {
		const auto *row = (const QRgb *) img.constScanLine(y);
		for (int x = 0; x < img.width(); ++x) {
			if (qAlpha(row[x]))
				ink |= QRect(x, y, 1, 1);
		}
	}
	if (ink.isEmpty())
		return {};
	*origin = box.topLeft() + ink.topLeft();
	return img.copy(ink);
}

void
layout_text(QTextLayout *layout, float dpr, float wrap_pts, bool center = false)
{
	if (wrap_pts > 0.0f) {
		QTextOption opt = layout->textOption();
		opt.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);
		layout->setTextOption(opt);
	}
	layout->beginLayout();
	float y = 0.0f;
	const float wrap_px = (wrap_pts > 0.0f ? wrap_pts : 1.0e8f) * dpr;
	for (;;) {
		QTextLine line = layout->createLine();
		if (!line.isValid())
			break;
		line.setLineWidth(wrap_px);
		float x = 0.0f;
		if (center)
			x = (wrap_px - float(line.naturalTextWidth())) * 0.5f;
		line.setPosition(QPointF(x, y));
		y += float(line.height());
	}
	layout->endLayout();
}

Qt::CursorShape
resize_cursor(Qt::Edges edges)
{
	const bool l = edges & Qt::LeftEdge;
	const bool r = edges & Qt::RightEdge;
	const bool t = edges & Qt::TopEdge;
	const bool b = edges & Qt::BottomEdge;
	if ((l && t) || (r && b))
		return Qt::SizeFDiagCursor;
	if ((r && t) || (l && b))
		return Qt::SizeBDiagCursor;
	if (l || r)
		return Qt::SizeHorCursor;
	if (t || b)
		return Qt::SizeVerCursor;
	return Qt::ArrowCursor;
}

}  // namespace

constexpr float kDialogPad = 16.0f;

static Colour
col(const Colour &c, float alpha = 1.0f)
{
	return {c.r, c.g, c.b, c.a * alpha};
}

static Kit::Packed
pack_or_grow(Kit &kit, int width, int height)
{
	Kit::Packed packed = kit.atlas_.alloc(width, height);
	while (packed.empty()) {
		const int cur = max(kit.atlas_.w, kit.atlas_.h);
		if (cur >= kAtlasMax)
			return {};
		const int next = min(kAtlasMax, max(cur * 2, kAtlasStart));
		if (next <= cur)
			return {};
		kit.atlas_.grow(next);
		packed = kit.atlas_.alloc(width, height);
	}
	return packed;
}

static void
blit(Kit &kit, const Kit::Packed &rect, const QImage &src, bool coverage)
{
	QImage img = src;
	if (coverage) {
		if (img.format() != QImage::Format_Alpha8 &&
			img.format() != QImage::Format_Grayscale8)
			img = img.convertToFormat(QImage::Format_Alpha8);
	}
	const int rows = min(rect.h, img.height());
	const int cols = min(rect.w, img.width());
	for (int y = 0; y < rows; ++y) {
		uint16_t *dst = kit.atlas_.pixels.data() +
			(size_t(rect.y + y) * size_t(kit.atlas_.w) + size_t(rect.x)) * 4;
		if (coverage) {
			const uchar *row = img.constScanLine(y);
			for (int x = 0; x < cols; ++x) {
				dst[x * 4 + 0] = 65535;
				dst[x * 4 + 1] = 65535;
				dst[x * 4 + 2] = 65535;
				dst[x * 4 + 3] = widen8(row[x]);
			}
		} else {
			const auto *row = (const QRgb *) img.constScanLine(y);
			for (int x = 0; x < cols; ++x) {
				dst[x * 4 + 0] = widen8(uint8_t(qRed(row[x])));
				dst[x * 4 + 1] = widen8(uint8_t(qGreen(row[x])));
				dst[x * 4 + 2] = widen8(uint8_t(qBlue(row[x])));
				dst[x * 4 + 3] = widen8(uint8_t(qAlpha(row[x])));
			}
		}
	}
	kit.atlas_.dirty = true;
}

static int
font_id(Kit &kit, const QRawFont &raw)
{
	for (size_t i = 0; i < kit.fonts_.size(); ++i) {
		if (kit.fonts_[i] == raw)
			return int(i);
	}
	kit.fonts_.push_back(raw);
	return int(kit.fonts_.size()) - 1;
}

static const Kit::Glyph *
cache_glyph(Kit &kit, const QRawFont &raw, quint32 gid)
{
	if (!raw.isValid())
		return nullptr;
	const uint64_t key = (uint64_t(font_id(kit, raw)) << 32) | gid;
	if (auto it = kit.glyphs_.find(key); it != kit.glyphs_.end())
		return &it->second;
	QPoint origin;
	QImage map = raster_glyph(raw, gid, &origin);
	if (map.isNull() || map.width() <= 0 || map.height() <= 0) {
		Kit::Glyph glyph;
		auto [it, _] = kit.glyphs_.emplace(key, glyph);
		return &it->second;
	}
	const Kit::Packed packed = pack_or_grow(kit, map.width(), map.height());
	if (packed.empty())
		return nullptr;
	blit(kit, packed, map, true);
	Kit::Glyph glyph;
	glyph.rect = packed;
	glyph.bearing_x = float(origin.x());
	glyph.bearing_y = float(origin.y());
	auto [it, _] = kit.glyphs_.emplace(key, glyph);
	return &it->second;
}

static void
cache_ascii(Kit &kit, bool bold)
{
	const QRawFont &raw = bold ? kit.raw_bold_ : kit.raw_;
	if (!raw.isValid())
		return;
	for (int cp = 0x20; cp <= 0x7E; ++cp) {
		const QList<quint32> indexes =
			raw.glyphIndexesForString(QString(QChar(cp)));
		if (indexes.isEmpty())
			continue;
		cache_glyph(kit, raw, indexes.front());
	}
}

static void
cache_text(Kit &kit, const QString &text, bool bold, float wrap_pts)
{
	const QFont &font = bold ? kit.font_bold_px_ : kit.font_px_;
	const QRawFont &raw = bold ? kit.raw_bold_ : kit.raw_;
	if (!raw.isValid() || text.isEmpty())
		return;
	QTextLayout layout(text, font);
	layout_text(&layout, kit.dpr_, wrap_pts);
	for (const QGlyphRun &run : layout.glyphRuns()) {
		for (quint32 gid : run.glyphIndexes())
			cache_glyph(kit, run.rawFont(), gid);
	}
}

static void
rebuild_atlas(Kit &kit)
{
	++kit.atlas_epoch_;
	kit.icons_.clear();
	kit.glyphs_.clear();
	kit.fonts_.clear();
	kit.glow_ = {};
	kit.atlas_.clear();
	kit.atlas_.grow(kAtlasStart);

	const Kit::Packed white = pack_or_grow(kit, 4, 4);
	if (white.empty())
		return;
	kit.white_ = white;
	for (int y = 0; y < white.h; ++y) {
		uint16_t *dst = kit.atlas_.pixels.data() +
			size_t(white.y + y) * size_t(kit.atlas_.w) * 4 +
			size_t(white.x) * 4;
		for (int x = 0; x < white.w; ++x) {
			dst[x * 4 + 0] = 65535;
			dst[x * 4 + 1] = 65535;
			dst[x * 4 + 2] = 65535;
			dst[x * 4 + 3] = 65535;
		}
	}
	kit.atlas_.dirty = true;

	const int gn = max(1, int(lround(double(kGlowPts) * double(kit.dpr_))));
	const Kit::Packed glow = pack_or_grow(kit, gn, gn);
	if (!glow.empty()) {
		kit.glow_ = glow;
		const int x_max = gn - 1;
		const int y_max = gn - 1;
		const double x_scale = 1.0 / double(max(1, x_max));
		const double y_scale = 1.0 / double(max(1, y_max));
		for (int y = 0; y <= y_max; ++y) {
			uint16_t *dst = kit.atlas_.pixels.data() +
				size_t(glow.y + y) * size_t(kit.atlas_.w) * 4 +
				size_t(glow.x) * 4;
			for (int x = 0; x <= x_max; ++x) {
				const double xn = x_scale * double(x_max - x);
				const double yn = y_scale * double(y_max - y);
				const double v = min(sqrt(xn * xn + yn * yn), 1.0);
				const uint8_t a = uint8_t(lround(pow(1.0 - v, 1.5) * 255.0));
				dst[x * 4 + 0] = 65535;
				dst[x * 4 + 1] = 65535;
				dst[x * 4 + 2] = 65535;
				dst[x * 4 + 3] = widen8(a);
			}
		}
		kit.atlas_.dirty = true;
	}

	QFont qfont = QFontDatabase::systemFont(QFontDatabase::GeneralFont);
	int logical_px = QFontInfo(qfont).pixelSize();
	if (logical_px <= 0)
		logical_px = 13;
	// Keep font atlas an alpha map, at the cost of screwing up emoji.
	const auto aa = QFont::StyleStrategy(
		QFont::PreferAntialias | QFont::NoSubpixelAntialias);
	kit.font_ = qfont;
	kit.font_.setPixelSize(logical_px);
	kit.font_.setStyleStrategy(aa);
	kit.font_bold_ = kit.font_;
	kit.font_bold_.setBold(true);
	kit.font_px_ = kit.font_;
	kit.font_px_.setPixelSize(
		int(lround(double(logical_px) * double(kit.dpr_))));
	kit.font_bold_px_ = kit.font_px_;
	kit.font_bold_px_.setBold(true);
	kit.raw_ = QRawFont::fromFont(kit.font_px_);
	kit.raw_bold_ = QRawFont::fromFont(kit.font_bold_px_);
	cache_ascii(kit, false);
	cache_ascii(kit, true);
}

// The mnemonic is an index into text, and gets underlined; -1 for none.
static void
emit_text(Kit &kit, float x, float y, const QString &text, Colour colour,
	bool bold, int mnemonic, float wrap_pts = 0.0f, bool center = false)
{
	const QFont &font = bold ? kit.font_bold_px_ : kit.font_px_;
	const QRawFont &raw = bold ? kit.raw_bold_ : kit.raw_;
	if (!raw.isValid() || text.isEmpty())
		return;
	const float dpr = max(kit.dpr_, 0.01f);
	QTextLayout layout(text, font);
	layout_text(&layout, dpr, wrap_pts, center);
	const float inv = 1.0f / dpr;
	for (const QGlyphRun &run : layout.glyphRuns()) {
		const QList<quint32> gids = run.glyphIndexes();
		const QList<QPointF> pos = run.positions();
		const int n = int(min(gids.size(), pos.size()));
		for (int i = 0; i < n; ++i) {
			const Kit::Glyph *glyph = cache_glyph(kit, run.rawFont(), gids[i]);
			if (!glyph || glyph->rect.w <= 0 || glyph->rect.h <= 0)
				continue;
			const float gx = kit.snap(
				x + float(pos[i].x() + double(glyph->bearing_x)) * inv);
			const float gy = kit.snap(
				y + float(pos[i].y() + double(glyph->bearing_y)) * inv);
			const float gw = float(glyph->rect.w) * inv;
			const float gh = float(glyph->rect.h) * inv;
			float u0, v0, u1, v1;
			kit.atlas_.uv(glyph->rect, &u0, &v0, &u1, &v1);
			kit.list_.add_image(
				gx, gy, gx + gw, gy + gh, u0, v0, u1, v1, colour);
		}
	}
	if (mnemonic < 0 || mnemonic >= text.size())
		return;

	const QTextLine line = layout.lineForTextPosition(mnemonic);
	if (!line.isValid())
		return;

	// cursorToX() runs backwards within an RTL run.
	const float cx0 = float(line.cursorToX(mnemonic));
	const float cx1 = float(line.cursorToX(mnemonic + 1));
	// Both font metrics grow downwards from the baseline, which is where
	// the glyphs of this line sit as well.
	const float uy =
		y + float(line.y() + line.ascent() + raw.underlinePosition()) * inv;
	kit.list_.add_line(x + min(cx0, cx1) * inv, uy, x + max(cx0, cx1) * inv, uy,
		colour, float(raw.lineThickness()) * inv);
}

static void
emit_icon(
	Kit &kit, float x, float y, float size, const char *name, Colour colour)
{
	if (!name)
		return;
	auto it = kit.icons_.find(name);
	if (it == kit.icons_.end())
		return;
	float u0, v0, u1, v1;
	kit.atlas_.uv(it->second, &u0, &v0, &u1, &v1);
	kit.list_.add_image(x, y, x + size, y + size, u0, v0, u1, v1, colour);
}

// --- Layout kit --------------------------------------------------------------

namespace
{

constexpr float kSepW = 8.0f;
constexpr float kSepH = 7.0f;
constexpr float kTooltipDelayMs = 500.0f;
constexpr float kTooltipMovePts = 6.0f;

int
sooner(int a, int b)
{
	if (a < 0)
		return b;
	return b < 0 ? a : min(a, b);
}

}  // namespace

// --- Rect --------------------------------------------------------------------

Rect
Rect::inset(float px, float py) const
{
	const float nw = this->w - px * 2.0f;
	const float nh = this->h - py * 2.0f;
	return {this->x + px, this->y + py, nw > 0.0f ? nw : 0.0f,
		nh > 0.0f ? nh : 0.0f};
}

// --- Widget ------------------------------------------------------------------

void
Widget::paint_children(Kit &kit) const
{
	const size_t n = child_count();
	for (size_t i = 0; i < n; ++i) {
		if (const Widget *k = child(i))
			k->paint(kit);
	}
}

void
Widget::paint(Kit &kit) const
{
	if (!shown())
		return;
	paint_children(kit);
}

Widget *
Widget::hit_at(float x, float y)
{
	if (!shown() || this->r.w <= 0.0f || this->r.h <= 0.0f ||
		!this->r.contains(x, y))
		return nullptr;
	for (size_t i = child_count(); i > 0; --i) {
		if (Widget *k = child(i - 1))
			if (Widget *h = k->hit_at(x, y))
				return h;
	}
	return this->hittable ? this : nullptr;
}

void
Widget::prepare(Kit &kit)
{
	if (!shown())
		return;
	const size_t n = child_count();
	for (size_t i = 0; i < n; ++i) {
		if (Widget *k = child(i))
			k->prepare(kit);
	}
}

bool
Widget::press(Kit &, float, float, Qt::MouseButton)
{
	return false;
}

bool
Widget::release(Kit &, float, float, Qt::MouseButton)
{
	return false;
}

bool
Widget::motion(Kit &, float, float)
{
	return false;
}

bool
Widget::scroll(Kit &, float, float, int)
{
	return false;
}

bool
Widget::pan(Kit &, float, float, float, float)
{
	return false;
}

bool
Widget::gesture(Kit &, float, float, float, float)
{
	return false;
}

bool
Widget::key(Kit &, const Key &)
{
	return false;
}

bool
Widget::input_method(Kit &, const QString &, const QString &, int)
{
	return false;
}

bool
Widget::text_target(const Kit &, TextTarget &) const
{
	return false;
}

bool
Widget::double_click(Kit &, float, float, Qt::MouseButton, unsigned)
{
	return false;
}

// --- Button ------------------------------------------------------------------

namespace
{

float
button_text_avail(const Button &b)
{
	const float px = kFramePadX + b.pad_x;
	const float left = px + (b.icon ? kIconPx + 4.0f : 0.0f);
	return max(1.0f, b.r.w - left - px);
}

QString
button_shown(const Kit &kit, const Button &b)
{
	if (b.text.isEmpty())
		return b.text;
	return kit.elide_lines(b.text, button_text_avail(b), 1, false);
}

}  // namespace

void
Button::measure(Kit &kit, float, float)
{
	const float px = kFramePadX + this->pad_x;
	float cw = 0.0f;
	float ch = kit.text_height(QStringLiteral("Ag"), 0.0f, false);
	if (this->icon) {
		cw = kIconPx;
		ch = max(ch, kIconPx);
	}
	if (!this->text.isEmpty()) {
		if (this->icon)
			cw += 4.0f;
		cw += kit.text_width(this->text, false);
		ch = max(ch, kit.text_height(this->text, 0.0f, false));
	}
	this->r = {0, 0, kit.snap_size(px * 2.0f + cw),
		kit.snap_size(kFramePadY * 2.0f + ch)};
}

void
Button::arrange(Kit &kit, Rect alloc)
{
	this->r = shown() ? kit.snap_rect(alloc) : Rect{};
}

void
Button::paint(Kit &kit) const
{
	if (!shown())
		return;
	const bool hot = kit.hot_ == this;
	const bool pressed = kit.left_down_ && kit.pressed_ == this;
	if ((this->enabled_ && pressed) || this->active)
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourPress]));
	else if (this->enabled_ && hot)
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourHover]));
	const float px = kFramePadX + this->pad_x;
	const float ink_a = (this->enabled_ ? 1.0f : 0.375f) *
		(this->dim ? 0.5f : 1.0f) * kit.ink_alpha();
	if (this->icon)
		emit_icon(kit, this->r.x + px, this->r.y + (this->r.h - kIconPx) * 0.5f,
			kIconPx, this->icon, col(kit.colours_[ColourInk], ink_a));
	if (!this->text.isEmpty()) {
		const float tx = this->r.x + px + (this->icon ? kIconPx + 4.0f : 0.0f);
		const float th = kit.text_height(this->text, 0.0f, false);
		emit_text(kit, tx, this->r.y + (this->r.h - th) * 0.5f,
			button_shown(kit, *this), col(kit.colours_[ColourInk], ink_a),
			false, -1);
	}
	if (kit.focus_ == this && kit.focus_visible_)
		kit.focus_ring(this->r);
}

void
Button::prepare(Kit &kit)
{
	if (shown() && !this->text.isEmpty())
		cache_text(kit, button_shown(kit, *this), false, 0.0f);
}

bool
Button::focusable() const
{
	return this->enabled_ && shown() && this->hittable && this->r.w > 0;
}

bool
Button::press(Kit &kit, float, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	kit.pressed_ = this;
	if (this->activate_on_press)
		activate(kit);
	return true;
}

bool
Button::release(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	if (kit.pressed_ != this)
		return false;
	if (this->activate_on_press)
		return true;
	Widget *hit = kit.hit(x, y);
	if (hit == this)
		return activate(kit);
	return true;
}

bool
Button::key(Kit &kit, const Key &ev)
{
	if (ev.mods)
		return false;
	if (ev.key != Qt::Key_Space && ev.key != Qt::Key_Return &&
		ev.key != Qt::Key_Enter)
		return false;
	return activate(kit);
}

bool
Button::activate(Kit &kit)
{
	if (!this->enabled_ || !this->on_click)
		return false;
	this->on_click(kit);
	for (Widget *w = this->parent_; w; w = w->parent_) {
		if (w->traps_focus() && w->shown()) {
			kit.close_popups();
			break;
		}
	}
	return true;
}

// --- Label -------------------------------------------------------------------

void
Label::measure(Kit &kit, float max_w, float)
{
	const float iw = max(0.0f, max_w - this->pad_x * 2.0f);
	float w = max(kit.text_width(this->text, this->bold), this->min_w);
	if (this->wrap)
		w = max(1.0f, iw < kUnlim ? iw : w);
	this->r.w = kit.snap_size(w + this->pad_x * 2.0f);
	this->r.h = kit.snap_size(
		kit.text_height(this->text, this->wrap ? w : 0.0f, this->bold) +
		this->pad_y * 2.0f);
}

void
Label::arrange(Kit &kit, Rect alloc)
{
	this->r = shown() ? kit.snap_rect(alloc) : Rect{};
	if (!shown() || !this->wrap)
		return;
	const float w = max(1.0f, this->r.w - this->pad_x * 2.0f);
	this->r.h = kit.snap_size(max(this->r.h,
		kit.text_height(this->text, w, this->bold) + this->pad_y * 2.0f));
}

void
Label::paint(Kit &kit) const
{
	if (!shown())
		return;
	float tx = this->r.x + this->pad_x;
	const bool wrap_center = this->wrap && this->align == Align::Center;
	if (this->align == Align::Center && !this->wrap)
		tx = kit.snap(this->r.x +
			max(0.0f,
				(this->r.w - kit.text_width(this->text, this->bold)) * 0.5f));
	const float wrap_w =
		this->wrap ? max(1.0f, this->r.w - this->pad_x * 2.0f) : 0.0f;
	const float th = kit.text_height(this->text, wrap_w, this->bold);
	float ty = this->r.y + this->pad_y;
	if (this->valign == Align::Center)
		ty = this->r.y + (this->r.h - th) * 0.5f;
	else if (this->valign == Align::End)
		ty = this->r.y + this->r.h - this->pad_y - th;
	emit_text(kit, tx, ty, this->text,
		col(kit.colours_[ColourInk],
			(this->dim ? 0.5f : 1.0f) * kit.ink_alpha()),
		this->bold, -1, wrap_w, wrap_center);
}

void
Label::prepare(Kit &kit)
{
	if (shown() && !this->text.isEmpty())
		cache_text(kit, this->text, this->bold,
			this->wrap ? max(1.0f, this->r.w - this->pad_x * 2.0f) : 0.0f);
}

// --- Entry -------------------------------------------------------------------

namespace
{

constexpr float kCaretBlinkMs = 530.0f;
constexpr float kEntryPadY = 3.0f;

// Both walk whole grapheme clusters, so that combining marks and surrogate
// pairs never get split down the middle.
int
grapheme_before(const QString &text, int at)
{
	if (at <= 0)
		return 0;
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	finder.setPosition(min(at, int(text.size())));
	const int prev = finder.toPreviousBoundary();
	return prev < 0 ? 0 : prev;
}

int
grapheme_after(const QString &text, int at)
{
	const int end = int(text.size());
	if (at >= end)
		return end;
	QTextBoundaryFinder finder(QTextBoundaryFinder::Grapheme, text);
	finder.setPosition(max(at, 0));
	const int next = finder.toNextBoundary();
	return next < 0 ? end : next;
}

// Qt hands us the control codes as text too; none of them are insertable.
QString
printable_only(const QString &text)
{
	QString out;
	out.reserve(text.size());
	for (const QChar c : text) {
		if (c == QChar(0x7F) || c.unicode() < 0x20)
			continue;
		out.append(c);
	}
	return out;
}

// How far through the on-off cycle a caret last touched then is: [0, 1),
// with the first half lit.
double
blink_phase(chrono::steady_clock::time_point since)
{
	const double elapsed =
		chrono::duration<double, milli>(chrono::steady_clock::now() - since)
			.count();
	return fmod(elapsed, kCaretBlinkMs * 2.0) / (kCaretBlinkMs * 2.0);
}

}  // namespace

float
Entry::inner_w() const
{
	return max(1.0f, this->r.w - this->pad_x * 2.0f);
}

QString
Entry::painted() const
{
	if (!this->preedit.isEmpty())
		return QString(this->text).insert(this->caret, this->preedit);
	return this->text;
}

void
Entry::touch_caret(const Kit &kit)
{
	this->caret_at_ = chrono::steady_clock::now();
	rescroll(kit);
}

void
Entry::rescroll(const Kit &kit)
{
	// The caret sits mid-preedit while an input method is composing.
	const QString full = painted();
	const int at =
		this->caret + (this->preedit.isEmpty() ? 0 : this->preedit_caret);
	const float caret = kit.caret_x(full, at, false);
	const float text_w = kit.text_width(full, false);
	const float view = inner_w();
	if (text_w <= view) {
		this->scroll_ = 0.0f;
		return;
	}

	// Keep the caret around the centre, but never leave a gap at either end:
	// the text fills the box before the caret gets to be centred.
	this->scroll_ = clamp(caret - view * 0.5f, 0.0f, text_w - view);
}

void
Entry::move_caret(Kit &kit, int to)
{
	this->caret = clamp(to, 0, int(this->text.size()));
	touch_caret(kit);
	if (kit.input_method_changed)
		kit.input_method_changed();
}

void
Entry::set_text(Kit &kit, const QString &next)
{
	this->text = next;
	this->caret = clamp(this->caret, 0, int(this->text.size()));
	this->preedit.clear();
	this->preedit_caret = 0;
	touch_caret(kit);
	if (this->on_change)
		this->on_change(kit);
	if (kit.input_method_changed)
		kit.input_method_changed();
}

void
Entry::measure(Kit &kit, float, float)
{
	const float h = kit.text_height(QStringLiteral("Ag"), 0.0f, false);
	this->r = {0, 0, kit.snap_size(max(this->min_w, this->pad_x * 2.0f)),
		kit.snap_size(h + kEntryPadY * 2.0f)};
}

void
Entry::arrange(Kit &kit, Rect alloc)
{
	this->r = this->Widget::shown() ? kit.snap_rect(alloc) : Rect{};
	// Only the scroll: resetting the blink here would restart it every
	// frame, and the caret would never reach the dark half.
	rescroll(kit);
}

void
Entry::paint(Kit &kit) const
{
	if (!this->Widget::shown() || this->r.w <= 0.0f || this->r.h <= 0.0f)
		return;

	const float hair = 1.0f / max(kit.dpr_, 0.01f);
	kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, col(kit.colours_[ColourFrame]));
	kit.list_.add_rect_stroke(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, col(kit.colours_[ColourDivider]), hair);

	const Rect in = this->r.inset(this->pad_x, kEntryPadY);
	kit.list_.push_clip(in.x, in.y, in.x + in.w, in.y + in.h);

	const QString full = painted();
	const float th = kit.text_height(QStringLiteral("Ag"), 0.0f, false);
	const float ty = this->r.y + (this->r.h - th) * 0.5f;
	const float tx = in.x - this->scroll_;
	if (full.isEmpty()) {
		if (!this->placeholder.isEmpty())
			emit_text(kit, tx, ty, this->placeholder,
				col(kit.colours_[ColourInk], 0.4f * kit.ink_alpha()), false,
				-1);
	} else {
		emit_text(kit, tx, ty, full,
			col(kit.colours_[ColourInk], kit.ink_alpha()), false, -1);
	}

	// The preedit is underlined for its whole length, the way every other
	// toolkit marks text the input method still owns.
	if (!this->preedit.isEmpty()) {
		const float x0 = tx + kit.caret_x(full, this->caret, false);
		const float x1 = tx +
			kit.caret_x(full, this->caret + int(this->preedit.size()), false);
		const float uy = kit.snap(ty + th) - hair;
		kit.list_.add_line(min(x0, x1), uy, max(x0, x1), uy,
			col(kit.colours_[ColourInk], kit.ink_alpha()), hair);
	}

	if (this->caret_on_) {
		const int at =
			this->caret + (this->preedit.isEmpty() ? 0 : this->preedit_caret);
		const float cx = kit.snap(tx + kit.caret_x(full, at, false));
		kit.list_.add_rect_filled(cx, kit.snap(ty), cx + hair,
			kit.snap(ty + th), col(kit.colours_[ColourInk], kit.ink_alpha()));
	}

	kit.list_.pop_clip();
	if (kit.focus_ == this && kit.focus_visible_)
		kit.focus_ring(this->r);
}

void
Entry::prepare(Kit &kit)
{
	if (!this->Widget::shown()) {
		this->focused_ = false;
		this->caret_on_ = false;
		return;
	}

	// Whether this frame draws a caret is decided once, here: paint and
	// wake_ms both just read the answer, so they cannot disagree.
	const bool focused = kit.focus_ == this;
	if (focused && !this->focused_)
		this->caret_at_ = chrono::steady_clock::now();
	this->focused_ = focused;
	this->caret_on_ = focused &&
		(!this->preedit.isEmpty() || blink_phase(this->caret_at_) < 0.5);

	const QString full = painted();
	if (full.isEmpty())
		cache_text(kit, this->placeholder, false, 0.0f);
	else
		cache_text(kit, full, false, 0.0f);
}

bool
Entry::focusable() const
{
	return this->Widget::shown() && this->hittable && this->r.w > 0;
}

bool
Entry::press(Kit &kit, float x, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;

	// Unlike a button, which does not take focus at all, clicking a text
	// field is a deliberate grab of the keyboard, and is shown as one: the
	// ring says which field the next keystroke goes to, however focus got
	// here.  The caret marks the spot within it, which is a different job.
	kit.set_focus(this, true);
	kit.pressed_ = this;

	// A click during composition would land in the middle of text the input
	// method still owns; let it finish rather than fighting over the caret.
	if (this->preedit.isEmpty()) {
		const Rect in = this->r.inset(this->pad_x, kEntryPadY);
		move_caret(
			kit, kit.index_at(this->text, x - in.x + this->scroll_, false));
	}
	return true;
}

bool
Entry::key(Kit &kit, const Key &ev)
{
	// Let the window keep its shortcuts; only bare typing belongs to us.
	const unsigned extra = ev.mods &
		unsigned(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (extra)
		return false;

	// Anything the input method is still composing is its business.
	if (!this->preedit.isEmpty())
		return false;

	switch (ev.key) {
	case Qt::Key_Left:
		move_caret(kit, grapheme_before(this->text, this->caret));
		return true;
	case Qt::Key_Right:
		move_caret(kit, grapheme_after(this->text, this->caret));
		return true;
	case Qt::Key_Home:
		move_caret(kit, 0);
		return true;
	case Qt::Key_End:
		move_caret(kit, int(this->text.size()));
		return true;
	case Qt::Key_Backspace: {
		if (this->caret <= 0)
			return true;
		const int from = grapheme_before(this->text, this->caret);
		QString next = this->text;
		next.remove(from, this->caret - from);
		this->caret = from;
		set_text(kit, next);
		return true;
	}
	case Qt::Key_Delete: {
		const int to = grapheme_after(this->text, this->caret);
		if (to <= this->caret)
			return true;
		QString next = this->text;
		next.remove(this->caret, to - this->caret);
		set_text(kit, next);
		return true;
	}
	case Qt::Key_Escape:
		if (this->on_cancel)
			this->on_cancel(kit);
		return true;
	default:
		break;
	}

	// Whatever the layout composed goes in as typed.  Consuming it here is
	// also what keeps the browser's bare-letter accelerators from firing --
	// the swallow and the insert are one decision, so they cannot disagree.
	const QString insert = printable_only(ev.text);
	if (insert.isEmpty())
		return false;
	QString next = this->text;
	next.insert(this->caret, insert);
	this->caret += int(insert.size());
	set_text(kit, next);
	return true;
}

bool
Entry::input_method(
	Kit &kit, const QString &commit, const QString &pre, int pre_caret)
{
	if (!commit.isEmpty()) {
		QString next = this->text;
		const QString insert = printable_only(commit);
		next.insert(this->caret, insert);
		this->caret += int(insert.size());
		this->text = next;
	}
	this->preedit = pre;
	this->preedit_caret = clamp(pre_caret, 0, int(pre.size()));
	this->caret = clamp(this->caret, 0, int(this->text.size()));
	touch_caret(kit);
	if (!commit.isEmpty() && this->on_change)
		this->on_change(kit);
	if (kit.input_method_changed)
		kit.input_method_changed();
	return true;
}

bool
Entry::text_target(const Kit &kit, TextTarget &out) const
{
	if (!this->Widget::shown())
		return false;

	out.text = this->text;
	out.caret = this->caret;

	// The preedit is not in text, but the caret still has to be placed past
	// it on screen, or the candidate window covers what is being composed.
	const QString full = painted();
	const int at =
		this->caret + (this->preedit.isEmpty() ? 0 : this->preedit_caret);
	const float cx =
		this->r.x + this->pad_x - this->scroll_ + kit.caret_x(full, at, false);
	out.caret_rect = {cx, this->r.y, 1.0f, this->r.h};
	return true;
}

int
Entry::wake_ms() const
{
	// An unfocused field has no caret, and a composing one holds it steady:
	// neither needs waking.  Otherwise, sleep until the caret next flips.
	if (!this->focused_ || !this->preedit.isEmpty())
		return -1;
	const double phase = blink_phase(this->caret_at_);
	const double until = (phase < 0.5 ? 0.5 : 1.0) - phase;
	return max(1, int(ceil(until * kCaretBlinkMs * 2.0)));
}

// --- Sep ---------------------------------------------------------------------

void
Sep::measure(Kit &, float max_w, float max_h)
{
	if (max_w > max_h)
		this->r = {0, 0, kSepW, 0};
	else
		this->r = {0, 0, 0, kSepH};
}

void
Sep::arrange(Kit &kit, Rect alloc)
{
	this->r = shown() ? kit.snap_rect(alloc) : Rect{};
}

void
Sep::paint(Kit &kit) const
{
	if (!shown() || this->r.w <= 0.0f || this->r.h <= 0.0f)
		return;
	const Colour c = col(kit.colours_[ColourDivider]);
	if (this->r.h > this->r.w)
		kit.list_.add_line(this->r.x + this->r.w * 0.5f, this->r.y + 2.0f,
			this->r.x + this->r.w * 0.5f, this->r.y + this->r.h - 2.0f, c);
	else
		kit.list_.add_line(this->r.x + 4.0f, this->r.y + this->r.h * 0.5f,
			this->r.x + this->r.w - 4.0f, this->r.y + this->r.h * 0.5f, c);
}

// --- Splitter ----------------------------------------------------------------

void
Splitter::measure(Kit &, float, float max_h)
{
	this->r = {0, 0, this->min_w > 0.0f ? this->min_w : 8.0f, max_h};
}

void
Splitter::arrange(Kit &kit, Rect alloc)
{
	this->r = shown() ? kit.snap_rect(alloc) : Rect{};
}

void
Splitter::paint(Kit &kit) const
{
	if (!shown())
		return;
	const float x = this->r.x + this->r.w * 0.5f;
	const Colour &c = (kit.hot_ == this || kit.pressed_ == this)
		? kit.colours_[ColourInk]
		: kit.colours_[ColourDivider];
	kit.list_.add_line(x, this->r.y, x, this->r.y + this->r.h, col(c));
}

bool
Splitter::press(Kit &kit, float, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	kit.pressed_ = this;
	return true;
}

bool
Splitter::motion(Kit &kit, float x, float)
{
	if (kit.pressed_ != this)
		return false;
	if (this->on_drag)
		this->on_drag(kit, x);
	return true;
}

bool
Splitter::release(Kit &, float, float, Qt::MouseButton button)
{
	return button == Qt::LeftButton;
}

// --- Composite ---------------------------------------------------------------

Widget *
Composite::add_child(unique_ptr<Widget> child, size_t at)
{
	Widget *raw = child.get();
	if (!raw)
		return nullptr;
	raw->parent_ = this;
	at = min(at, this->kids.size());
	this->kids.insert(this->kids.begin() + ptrdiff_t(at), std::move(child));
	return raw;
}

void
Composite::erase_children(size_t from)
{
	from = min(from, this->kids.size());
	for (size_t i = from; i < this->kids.size(); ++i) {
		if (this->kids[i])
			this->kids[i]->parent_ = nullptr;
	}
	this->kids.erase(this->kids.begin() + ptrdiff_t(from), this->kids.end());
}

// --- Container ---------------------------------------------------------------

void
Container::measure_pack(Kit &kit, float max_w, float max_h, bool hz)
{
	const float iw = max(0.0f, max_w - this->pad_x * 2.0f);
	const float ih = max(0.0f, max_h - this->pad_y * 2.0f);
	int growers = 0, vis = 0;
	float used = 0.0f, cross = 0.0f;
	for (const auto &child : this->kids) {
		Widget *k = child.get();
		if (!k || !k->shown())
			continue;
		if (k->grows()) {
			++growers;
			++vis;
			continue;
		}
		k->measure(kit, hz ? kUnlim : iw, hz ? ih : kUnlim);
		used += hz ? k->r.w : k->r.h;
		cross = max(cross, hz ? k->r.h : k->r.w);
		++vis;
	}
	const float gaps = this->gap * float(max(0, vis - 1));
	if (growers) {
		const float share = max(0.0f, (hz ? iw : ih) - used - gaps) / growers;
		for (const auto &child : this->kids) {
			Widget *k = child.get();
			if (!k || !k->shown() || !k->grows())
				continue;
			k->measure(kit, hz ? share : iw, hz ? ih : share);
			used += hz ? k->r.w : k->r.h;
			cross = max(cross, hz ? k->r.h : k->r.w);
		}
	}
	used += gaps;
	this->r.w = kit.snap_size(this->pad_x * 2.0f + (hz ? used : cross));
	this->r.h = kit.snap_size(this->pad_y * 2.0f + (hz ? cross : used));
	if (this->grow)
		this->r.w = max_w;
}

void
Container::arrange_pack(Kit &kit, Rect alloc, bool hz, Align align)
{
	if (!shown()) {
		this->r = {};
		return;
	}
	alloc = kit.snap_rect(alloc);
	this->r = alloc;
	const Rect in = alloc.inset(this->pad_x, this->pad_y);
	const float imain = hz ? in.w : in.h;
	int growers = 0, vis = 0;
	float used = 0.0f;
	for (const auto &child : this->kids) {
		Widget *k = child.get();
		if (!k || !k->shown())
			continue;
		++vis;
		if (k->grows()) {
			++growers;
			continue;
		}
		k->measure(kit, hz ? kUnlim : in.w, hz ? in.h : kUnlim);
		used += hz ? k->r.w : k->r.h;
	}
	if (growers) {
		const float gaps = this->gap * float(max(0, vis - 1));
		const float share = max(0.0f, imain - used - gaps) / growers;
		for (const auto &child : this->kids) {
			Widget *k = child.get();
			if (!k || !k->shown() || !k->grows())
				continue;
			k->measure(kit, hz ? share : in.w, hz ? in.h : share);
			if (hz)
				k->r.w = share;
			else {
				k->r.h = share;
				k->r.w = in.w;
			}
		}
	}
	float packed = 0.0f;
	int nv = 0;
	for (const auto &k : this->kids) {
		if (!k || !k->shown())
			continue;
		packed += (hz ? k->r.w : k->r.h) + this->gap;
		++nv;
	}
	if (nv)
		packed -= this->gap;
	float p = hz ? in.x : in.y;
	if (align == Align::Center)
		p += max(0.0f, (imain - packed) * 0.5f);
	else if (align == Align::End)
		p += max(0.0f, imain - packed);
	for (auto &k : this->kids) {
		if (!k || !k->shown()) {
			if (k)
				k->r = {};
			continue;
		}
		if (hz)
			k->arrange(kit, {p, in.y, k->r.w, in.h});
		else
			k->arrange(kit, {in.x, p, in.w, k->r.h});
		p = (hz ? k->r.x + k->r.w : k->r.y + k->r.h) + this->gap;
	}
	if (this->grow)
		return;
	float bottom = this->r.y + this->r.h - this->pad_y;
	for (const auto &k : this->kids) {
		if (k && k->shown())
			bottom = max(bottom, k->r.y + k->r.h);
	}
	this->r.h = max(this->r.h, bottom + this->pad_y - this->r.y);
}

// --- Row ---------------------------------------------------------------------

void
Row::measure(Kit &kit, float max_w, float max_h)
{
	measure_pack(kit, max_w, max_h, true);
}

void
Row::arrange(Kit &kit, Rect alloc)
{
	arrange_pack(kit, alloc, true, this->align);
}

// --- Column ------------------------------------------------------------------

void
Column::measure(Kit &kit, float max_w, float max_h)
{
	measure_pack(kit, max_w, max_h, false);
}

void
Column::arrange(Kit &kit, Rect alloc)
{
	arrange_pack(kit, alloc, false, Align::Start);
}

// --- Scroll ------------------------------------------------------------------

float
Scroll::max_offset() const
{
	return max(0.0f, this->content - this->view);
}

void
Scroll::clamp()
{
	this->offset = std::clamp(this->offset, 0.0f, max_offset());
}

void
Scroll::set_metrics(float content_h, float view_h)
{
	this->content = max(0.0f, content_h);
	this->view = max(0.0f, view_h);
	clamp();
}

void
Scroll::reveal()
{
	if (this->content <= this->view)
		return;
	this->shown_at_ = chrono::steady_clock::now();
}

bool
Scroll::visible() const
{
	if (this->content <= this->view)
		return false;
	if (this->dragging)
		return true;
	if (this->shown_at_.time_since_epoch().count() == 0)
		return false;
	const float elapsed = chrono::duration<float, milli>(
		chrono::steady_clock::now() - this->shown_at_)
							  .count();
	return elapsed < kScrollHideMs;
}

int
Scroll::wake_ms() const
{
	if (this->content <= this->view || this->dragging)
		return -1;
	if (this->shown_at_.time_since_epoch().count() == 0)
		return -1;
	const float elapsed = chrono::duration<float, milli>(
		chrono::steady_clock::now() - this->shown_at_)
							  .count();
	if (elapsed >= kScrollHideMs)
		return -1;
	return int(ceil(double(kScrollHideMs - elapsed)));
}

Rect
Scroll::bar_rect(Rect viewport) const
{
	const float w = min(kScrollBarW, viewport.w);
	return {viewport.x + viewport.w - w, viewport.y, w, viewport.h};
}

Rect
Scroll::thumb_rect(Rect viewport) const
{
	const Rect bar = bar_rect(viewport);
	if (bar.h <= 0.0f || this->content <= 0.0f)
		return bar;
	float th = bar.h * (this->view / this->content);
	th = std::clamp(th, min(kScrollStep, bar.h), bar.h);
	const float travel = bar.h - th;
	const float range = max_offset();
	const float ty =
		range > 0.0f && travel > 0.0f ? (this->offset / range) * travel : 0.0f;
	return {bar.x, bar.y + ty, bar.w, th};
}

void
Scroll::set_from_y(float y, Rect viewport)
{
	const Rect bar = bar_rect(viewport);
	const Rect thumb = thumb_rect(viewport);
	const float travel = bar.h - thumb.h;
	const float range = max_offset();
	if (travel <= 0.0f || range <= 0.0f) {
		this->offset = 0.0f;
		return;
	}
	const float ty = y - this->grab_ - bar.y;
	this->offset = std::clamp(ty / travel, 0.0f, 1.0f) * range;
}

bool
Scroll::wheel(int delta, float step)
{
	const float s = float(delta) / 120.0f;
	this->offset = std::clamp(this->offset - s * step, 0.0f, max_offset());
	reveal();
	return true;
}

bool
Scroll::pan(float dy)
{
	this->offset = std::clamp(this->offset - dy, 0.0f, max_offset());
	reveal();
	return true;
}

bool
Scroll::page(int dir)
{
	if (this->content <= this->view)
		return false;
	const float step =
		this->view > kScrollStep ? this->view - kScrollStep : this->view;
	const float prev = this->offset;
	this->offset =
		std::clamp(this->offset + float(dir) * step, 0.0f, max_offset());
	reveal();
	return this->offset != prev;
}

bool
Scroll::press(float x, float y, Qt::MouseButton button, Rect viewport)
{
	if (button != Qt::LeftButton || !visible())
		return false;
	if (!bar_rect(viewport).contains(x, y))
		return false;
	const Rect thumb = thumb_rect(viewport);
	if (thumb.contains(x, y))
		this->grab_ = y - thumb.y;
	else {
		this->grab_ = thumb.h * 0.5f;
		set_from_y(y, viewport);
	}
	this->dragging = true;
	reveal();
	return true;
}

bool
Scroll::motion(float y, Rect viewport)
{
	if (!this->dragging)
		return false;
	set_from_y(y, viewport);
	reveal();
	return true;
}

bool
Scroll::release(Qt::MouseButton button)
{
	if (button != Qt::LeftButton || !this->dragging)
		return false;
	this->dragging = false;
	reveal();
	return true;
}

void
Scroll::paint(Kit &kit, Rect viewport) const
{
	if (!visible())
		return;
	const Rect thumb = thumb_rect(viewport);
	if (thumb.w <= 0.0f || thumb.h <= 0.0f)
		return;
	kit.list_.add_rect_filled(thumb.x, thumb.y, thumb.x + thumb.w,
		thumb.y + thumb.h, kit.colours_[ColourDivider]);
}

void
ScrollColumn::arrange(Kit &kit, Rect alloc)
{
	Column::arrange(kit, alloc);
	float bottom = alloc.y;
	for (const auto &k : this->kids) {
		if (k && k->shown())
			bottom = max(bottom, k->r.y + k->r.h);
	}
	this->scroll_.set_metrics(max(0.0f, bottom - alloc.y), alloc.h);
	if (this->follow_focus && kit.focus_ != this->followed_) {
		this->followed_ = kit.focus_;
		if (Widget *f = this->followed_) {
			for (Widget *p = f; p; p = p->parent_) {
				if (p != this)
					continue;
				const float y0 = f->r.y - alloc.y;
				if (y0 < this->scroll_.offset)
					this->scroll_.offset = y0;
				else if (y0 + f->r.h > this->scroll_.offset + alloc.h)
					this->scroll_.offset = y0 + f->r.h - alloc.h;
				this->scroll_.clamp();
				break;
			}
		}
	}
	if (this->scroll_.offset <= 0.0f)
		return;
	for (auto &k : this->kids) {
		if (k)
			k->arrange(
				kit, {k->r.x, k->r.y - this->scroll_.offset, k->r.w, k->r.h});
	}
}

void
ScrollColumn::paint(Kit &kit) const
{
	if (!shown())
		return;
	kit.list_.push_clip(
		this->r.x, this->r.y, this->r.x + this->r.w, this->r.y + this->r.h);
	paint_children(kit);
	this->scroll_.paint(kit, this->r);
	kit.list_.pop_clip();
}

Widget *
ScrollColumn::hit_at(float x, float y)
{
	if (!shown() || this->r.w <= 0.0f || this->r.h <= 0.0f ||
		!this->r.contains(x, y))
		return nullptr;
	if (this->scroll_.visible() &&
		this->scroll_.bar_rect(this->r).contains(x, y))
		return this;
	return Column::hit_at(x, y);
}

bool
ScrollColumn::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (!this->scroll_.press(x, y, button, this->r))
		return false;
	kit.pressed_ = this;
	return true;
}

bool
ScrollColumn::release(Kit &, float, float, Qt::MouseButton button)
{
	return this->scroll_.release(button);
}

bool
ScrollColumn::motion(Kit &, float, float y)
{
	if (this->scroll_.dragging)
		return this->scroll_.motion(y, this->r);
	return false;
}

bool
ScrollColumn::scroll(Kit &, float, float, int delta)
{
	return this->scroll_.wheel(delta, kScrollStep * 3.0f);
}

bool
ScrollColumn::pan(Kit &, float, float, float, float dy)
{
	return this->scroll_.pan(dy);
}

bool
ScrollColumn::key(Kit &, const Key &ev)
{
	if (ev.mods)
		return false;
	if (ev.key == Qt::Key_PageUp)
		return this->scroll_.page(-1);
	if (ev.key == Qt::Key_PageDown)
		return this->scroll_.page(1);
	return false;
}

int
ScrollColumn::wake_ms() const
{
	return this->scroll_.wake_ms();
}

// --- Panel -------------------------------------------------------------------

void
Panel::measure(Kit &kit, float avail_w, float avail_h)
{
	const float iw = max(0.0f, avail_w - this->pad_x * 2.0f);
	const float ih = max(0.0f, avail_h - this->pad_y * 2.0f);
	float w = this->min_w, h = 0.0f;
	for (auto &k : this->kids) {
		if (!k || !k->shown())
			continue;
		k->measure(kit, iw, ih);
		w = max(w, k->r.w);
		h += k->r.h;
	}
	this->r.w = this->grow ? avail_w : this->pad_x * 2.0f + w;
	this->r.h = this->pad_y * 2.0f + h;
	if (this->min_h > 0.0f)
		this->r.h = max(this->r.h, this->min_h);
	if (this->max_h > 0.0f)
		this->r.h = min(this->r.h, this->max_h);
	if (this->min_w > 0.0f)
		this->r.w = max(this->r.w, this->min_w);
	this->r.h = min(this->r.h, avail_h);
	this->r.w = min(this->r.w, avail_w);
}

void
Panel::arrange(Kit &kit, Rect alloc)
{
	if (!shown()) {
		this->r = {};
		return;
	}
	this->r = kit.snap_rect(alloc);
	if (this->max_h > 0.0f && this->r.h > this->max_h)
		this->r.h = this->max_h;
	if (this->min_h > 0.0f && this->r.h < this->min_h)
		this->r.h = this->min_h;
	const Rect in = this->r.inset(this->pad_x, this->pad_y);
	float y = in.y;
	for (auto &k : this->kids) {
		if (!k || !k->shown())
			continue;
		k->measure(kit, in.w, in.h);
		k->arrange(kit, {in.x, y, in.w, k->grows() ? in.h : k->r.h});
		y += k->r.h;
	}
}

void
Panel::paint(Kit &kit) const
{
	if (!shown())
		return;
	if (this->clip)
		kit.list_.push_clip(
			this->r.x, this->r.y, this->r.x + this->r.w, this->r.y + this->r.h);
	switch (this->fill) {
	case Fill::Toolbar:
		kit.list_.add_rect_filled_vgradient(this->r.x, this->r.y,
			this->r.x + this->r.w, this->r.y + this->r.h,
			col(kit.colours_[ColourToolbarTop]),
			col(kit.colours_[ColourToolbarBottom]));
		break;
	case Fill::Tooltip:
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourFrame]));
		break;
	case Fill::Panel:
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourPanel]));
		break;
	case Fill::None:
		break;
	}
	paint_children(kit);
	const float hair = 0.5f / max(kit.dpr_, 0.01f);
	switch (this->stroke) {
	case Stroke::All:
		kit.list_.add_rect_stroke(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourDivider]));
		break;
	case Stroke::Bottom:
		kit.list_.add_line(this->r.x, this->r.y + this->r.h - hair,
			this->r.x + this->r.w, this->r.y + this->r.h - hair,
			col(this->busy ? kit.colours_[ColourBusy]
						   : kit.colours_[ColourDivider]));
		break;
	case Stroke::None:
		break;
	}
	if (this->clip)
		kit.list_.pop_clip();
}

// --- Popup -------------------------------------------------------------------

Popup::Popup()
{
	this->hittable = true;
	this->visible = false;
}

void
Popup::open(Kit &kit, Button *anchor)
{
	kit.close_popups();
	this->parent_popup = nullptr;
	this->opener = anchor;
	if (this->opener) {
		this->opener->active = true;
		this->at = this->opener->r;
	}
	this->visible = true;
	kit.open_popup(this);
	place(kit);
}

void
Popup::open_at(Kit &kit, Rect anchor)
{
	kit.close_popups();
	this->parent_popup = nullptr;
	this->opener = nullptr;
	this->at = anchor;
	this->visible = true;
	kit.open_popup(this);
	place(kit);
}

void
Popup::open_sub(Kit &kit, Popup &owner, Button &anchor)
{
	kit.close_above(&owner);
	this->parent_popup = &owner;
	this->opener = &anchor;
	this->visible = true;
	this->opener->active = true;
	kit.open_popup(this);
	place_sub(kit);
}

void
Popup::paint(Kit &kit) const
{
	if (!this->visible)
		return;
	kit.draw_shadow(this->r);
	Panel::paint(kit);
}

void
Popup::close(Kit &kit)
{
	if (!this->visible)
		return;
	kit.close_above(this);
	this->visible = false;
	this->parent_popup = nullptr;
	if (this->opener) {
		this->opener->active = false;
		this->opener = nullptr;
	}
	auto &ps = kit.popups_;
	ps.erase(remove(ps.begin(), ps.end(), this), ps.end());
	if (ps.empty() && kit.scrim_)
		kit.scrim_->visible = false;
	kit.sync_focus();
}

void
Popup::place(Kit &kit)
{
	if (this->opener)
		this->at = this->opener->r;
	const float cap = kit.host_w_ > 0.0f ? kit.host_w_ : kUnlim;
	measure(kit, cap, kUnlim);
	float x = kit.snap(this->at.x);
	float y = kit.snap(this->at.y + this->at.h);
	if (x + this->r.w > kit.host_w_)
		x = max(0.0f, kit.host_w_ - this->r.w);
	if (x < 0.0f)
		x = 0.0f;
	if (y + this->r.h > kit.host_h_)
		y = max(0.0f, this->at.y - this->r.h);
	if (y + this->r.h > kit.host_h_)
		y = max(0.0f, kit.host_h_ - this->r.h);
	if (y < 0.0f)
		y = 0.0f;
	arrange(kit, {x, y, this->r.w, this->r.h});
}

void
Popup::place_sub(Kit &kit)
{
	const float cap = kit.host_w_ > 0.0f ? kit.host_w_ : kUnlim;
	measure(kit, cap, kUnlim);
	const Popup *owner = this->parent_popup;
	const Widget *anchor = this->opener;
	float x = owner ? kit.snap(owner->r.x + owner->r.w) : 0.0f;
	if (x + this->r.w > kit.host_w_)
		x = owner ? kit.snap(owner->r.x - this->r.w) : 0.0f;
	if (x < 0.0f)
		x = 0.0f;
	float y = anchor ? kit.snap(anchor->r.y) : 0.0f;
	if (y + this->r.h > kit.host_h_)
		y = max(0.0f, kit.host_h_ - this->r.h);
	if (y < 0.0f)
		y = 0.0f;
	arrange(kit, {x, y, this->r.w, this->r.h});
}

// Alt is left to the menu bar's mnemonics, and everything but Escape to
// whatever the popup holds; MenuPopup adds navigation on top of this.
bool
Popup::key(Kit &kit, const Key &ev)
{
	if (ev.mods & unsigned(Qt::AltModifier))
		return false;
	if (ev.key != Qt::Key_Escape)
		return false;
	Button *op = this->opener;
	close(kit);
	if (op)
		kit.set_focus(op, true);
	return true;
}

// --- Dialog ------------------------------------------------------------------

Dialog::Dialog()
{
	this->hittable = true;
	this->visible = false;
	this->fill = Fill::None;
	auto f = make_unique<Panel>();
	f->pad_x = kDialogPad;
	f->pad_y = kDialogPad;
	f->fill = Fill::Panel;
	f->stroke = Stroke::All;
	f->hittable = false;
	f->visible = false;
	this->frame = f.get();

	// The body absorbs whatever height place() clamps away; the footer
	// keeps its own and stays put while the body scrolls under it.
	auto stack = make_unique<Column>();
	stack->grow = true;
	stack->gap = kDialogPad;

	auto body = make_unique<ScrollColumn>();
	body->grow = true;
	body->gap = 8.0f;
	this->body = body.get();
	stack->add_child(std::move(body));

	auto close_btn = make_unique<Button>();
	close_btn->text = QStringLiteral("Close");
	close_btn->pad_x = kDialogPad;
	close_btn->on_click = [this](Kit &kit) { close(kit); };

	auto footer = make_unique<Row>();
	footer->align = Align::End;
	footer->add_child(std::move(close_btn));
	stack->add_child(std::move(footer));

	f->add_child(std::move(stack));
	add_child(std::move(f));
}

void
Dialog::show(Kit &kit, unique_ptr<Widget> content, float min_w)
{
	if (!this->body || !this->frame)
		return;
	kit.forget_tree(this->body);
	this->body->erase_children();
	this->body->add_child(std::move(content));
	this->frame->min_w = min_w;
	Popup::open(kit);
	this->frame->visible = true;
}

void
Dialog::close(Kit &kit)
{
	if (this->frame)
		this->frame->visible = false;
	Popup::close(kit);
}

void
Dialog::place(Kit &kit)
{
	if (!this->frame || !shown()) {
		this->r = {};
		return;
	}
	this->frame->visible = true;
	this->r = {0.0f, 0.0f, kit.host_w_, kit.host_h_};
	// Centred on the window, not on whatever the toolbar left over. The
	// margin is only there to keep the shadow off the edges.
	const float margin = kGlowPts * 2.0f;
	const float max_w = max(1.0f, min(560.0f, kit.host_w_ - margin * 2.0f));
	const float avail_h = max(1.0f, kit.host_h_ - margin * 2.0f);
	this->frame->measure(kit, max_w, avail_h);
	// Taller than that means the body scrolls inside it.
	const float h = min(this->frame->r.h, avail_h);
	const float x = max(0.0f, (kit.host_w_ - this->frame->r.w) * 0.5f);
	const float y = margin + max(0.0f, (avail_h - h) * 0.5f);
	this->frame->arrange(kit, {x, y, this->frame->r.w, h});

	// Button::focusable() wants a laid-out rect, so this cannot happen any
	// earlier; without it Return and Space reach nothing and are eaten.
	bool focused = false;
	for (Widget *w = kit.focus_; w; w = w->parent_) {
		if (w == this) {
			focused = true;
			break;
		}
	}
	if (!focused)
		kit.focus_first(this);
}

void
Dialog::paint(Kit &kit) const
{
	if (!this->visible)
		return;
	// The same wash Hint lays over the window behind it.
	kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, col(kit.colours_[ColourInk], 0.1f));
	if (this->frame && this->frame->visible)
		kit.draw_shadow(this->frame->r);
	Panel::paint(kit);
}

// Only Escape and the Close button dismiss: a press that misses the frame is
// swallowed, and a release is left to whoever claimed the press, so finishing
// a scrollbar drag outside the frame cannot close it.
bool
Dialog::press(Kit &, float, float, Qt::MouseButton)
{
	return true;
}

// Hovering must not reach what the dialog covers, nor drag the keyboard focus
// around and drop its ring, which is what MenuPopup::motion does for items.
bool
Dialog::motion(Kit &, float, float)
{
	return true;
}

// --- Menus -------------------------------------------------------------------

namespace
{

constexpr float kItemGap = 2.0f;
constexpr float kMenuPad = 4.0f;
constexpr float kMenuHoldMs = 500.0f;  // GTK MENU_SHELL_TIMEOUT

unique_ptr<Sep>
hsep()
{
	return make_unique<Sep>();
}

// Where an item's columns land, relative to its own left edge.
struct MenuCols {
	float label_x = 0.0f;
	float accel_x = 0.0f;
	float accel_w = 0.0f;
	float chevron = 0.0f;
	float avail = 0.0f;  // what the label gets before the accelerator
};

MenuCols
menu_cols(const Kit &kit, const MenuItem &m)
{
	MenuCols c;
	c.accel_w = m.accel_col > 0.0f ? m.accel_col : m.accel_width(kit);
	c.chevron = m.sub ? kIconPx : 0.0f;
	c.label_x = kFramePadX + kIconPx + kFramePadX;
	c.accel_x = m.r.w - kFramePadX - c.chevron - c.accel_w;
	c.avail = max(1.0f, c.accel_x - kFramePadX - c.label_x);
	return c;
}

QString
menu_shown(const Kit &kit, const MenuItem &m)
{
	if (m.text.isEmpty())
		return m.text;
	return kit.elide_lines(m.text, menu_cols(kit, m).avail, 1, false);
}

// Right-eliding can cut the mnemonic off, or replace it with an ellipsis.
int
menu_mnemonic(const MenuItem &m, const QString &shown)
{
	if (m.mnemonic < 0 || m.mnemonic >= m.text.size() ||
		m.mnemonic >= shown.size() || shown[m.mnemonic] != m.text[m.mnemonic])
		return -1;
	return m.mnemonic;
}

}  // namespace

void
MenuPopup::focus_item(Kit &kit, Widget *w, bool kbd) const
{
	kit.set_focus(w, kbd);
	if (kbd)
		kit.hot_ = nullptr;
}

void
MenuPopup::reveal(Kit &kit, Widget *w)
{
	auto *item = dynamic_cast<MenuItem *>(w);
	if (item && item->sub) {
		if (!item->sub->visible)
			item->sub->open_sub(kit, *this, *item);
		kit.set_focus(item->sub, false);
		return;
	}
	kit.close_above(this);
	focus_item(kit, w, false);
}

bool
MenuPopup::motion(Kit &kit, float, float)
{
	Widget *w = kit.hot_;
	Widget *item = nullptr;
	for (; w; w = w->parent_) {
		if (w == this)
			break;
		if (dynamic_cast<const Popup *>(w) && w != this)
			return false;
		if (!item && w->focusable())
			item = w;
	}
	if (w != this)
		return false;
	if (item)
		reveal(kit, item);
	return true;
}

bool
MenuPopup::key(Kit &kit, const Key &ev)
{
	if (Popup::key(kit, ev))
		return true;
	if (ev.mods & unsigned(Qt::AltModifier))
		return false;
	if (ev.key == Qt::Key_Up || ev.key == Qt::Key_Down) {
		kit.cycle_focus(this, ev.key == Qt::Key_Up ? -1 : 1);
		focus_item(kit, kit.focus_, true);
		return true;
	}
	if (ev.key == Qt::Key_Right || ev.key == Qt::Key_Return ||
		ev.key == Qt::Key_Enter || ev.key == Qt::Key_Space) {
		if (auto *item = dynamic_cast<MenuItem *>(kit.focus_);
			item && item->sub) {
			item->activate(kit);
			return true;
		}
		if (ev.key == Qt::Key_Right)
			return true;
		return false;
	}
	if (ev.key == Qt::Key_Left) {
		if (this->parent_popup) {
			Button *op = this->opener;
			close(kit);
			if (op)
				kit.set_focus(op, true);
		}
		return true;
	}
	return false;
}

bool
MenuPopup::release(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button != Qt::LeftButton && button != Qt::RightButton)
		return false;
	Widget *hit = hit_at(x, y);
	if (auto *b = dynamic_cast<Button *>(hit); b && hit != this) {
		b->activate(kit);
		if (auto *item = dynamic_cast<MenuItem *>(b);
			item && item->sub && item->sub->visible)
			return true;
		if (this->visible)
			close(kit);
		return true;
	}
	const float elapsed = chrono::duration<float, milli>(
		chrono::steady_clock::now() - kit.popup_at_)
							  .count();
	if (elapsed >= kMenuHoldMs)
		kit.close_popups();
	return true;
}

Overflow::Overflow()
{
	auto column = make_unique<Column>();
	this->col = column.get();
	this->col->gap = kItemGap;
	this->pad_x = kMenuPad;
	this->pad_y = kMenuPad;
	this->fill = Fill::Panel;
	this->stroke = Stroke::All;
	this->hittable = true;
	this->visible = false;
	add_child(std::move(column));
}

void
Overflow::place(Kit &kit)
{
	if (this->fill_items)
		this->fill_items();
	Popup::place(kit);
}

Menu::Menu()
{
	auto c = make_unique<Column>();
	this->col = c.get();
	this->pad_x = kMenuPad;
	this->pad_y = kMenuPad;
	this->fill = Fill::Panel;
	this->stroke = Stroke::All;
	this->hittable = true;
	this->visible = false;
	add_child(std::move(c));
}

MenuItem &
Menu::add_item(const QString &text)
{
	auto item = make_unique<MenuItem>();
	item->text = text;
	MenuItem &ref = *item;
	if (this->col)
		this->col->add_child(std::move(item));
	return ref;
}

MenuItem &
Menu::add_item_with_mnemonic(const QString &text)
{
	auto item = make_unique<MenuItem>();
	item->text = menu_label(text.toStdString().c_str(), &item->mnemonic);
	MenuItem &ref = *item;
	if (this->col)
		this->col->add_child(std::move(item));
	return ref;
}

void
Menu::add_sep()
{
	if (this->col)
		this->col->add_child(hsep());
}

void
Menu::clear()
{
	this->subs_.clear();
	if (this->col)
		this->col->erase_children();
}

void
Menu::build(span<const MenuNode> nodes, const Actor &a)
{
	this->actor = a;
	clear();
	if (!this->col)
		return;
	this->col->grow = false;
	this->min_w = 200.0f;
	for (const MenuNode &node : nodes) {
		if (!node.items.empty()) {
			auto child = make_unique<Menu>();
			child->build(node.items, this->actor);
			auto &item = add_item_with_mnemonic(node.title);
			item.sub = child.get();
			this->subs_.push_back(std::move(child));
			continue;
		}
		if (!node.title && node.action == Action::None) {
			add_sep();
			continue;
		}
		const Action action = node.action;
		const ActionDef &def = action_def(action);
		auto &item = add_item_with_mnemonic(action_label(def, false));
		item.action = action;
		item.accel = accel_label(def);
		item.checkable = (def.flags & ActionToggle) && !def.label[1];
		item.on_click = [this, action](Kit &) {
			if (this->actor.apply)
				this->actor.apply(action);
		};
	}
}

void
Menu::sync()
{
	if (this->col) {
		for (auto &k : this->col->kids) {
			auto *item = dynamic_cast<MenuItem *>(k.get());
			if (!item || item->sub || item->action == Action::None)
				continue;

			const Action action = item->action;
			item->enabled_ =
				!this->actor.enabled || this->actor.enabled(action);
			item->checked = this->actor.checked && this->actor.checked(action);
			const ActionDef &def = action_def(action);
			item->text = menu_label(
				action_label(def, item->checked), &item->mnemonic);
			item->accel = accel_label(def);
			item->checkable = (def.flags & ActionToggle) && !def.label[1];
		}
	}
	for (auto &sub : this->subs_) {
		if (sub)
			sub->sync();
	}
}

void
Menu::measure(Kit &kit, float max_w, float max_h)
{
	if (this->col) {
		float lw = 0.0f;
		float aw = 0.0f;
		for (const auto &k : this->col->kids) {
			auto *item = dynamic_cast<MenuItem *>(k.get());
			if (!item)
				continue;
			lw = max(lw, item->label_width(kit));
			aw = max(aw, item->accel_width(kit));
		}
		for (auto &k : this->col->kids) {
			if (auto *item = dynamic_cast<MenuItem *>(k.get())) {
				item->label_col = lw;
				item->accel_col = aw;
			}
		}
	}
	Panel::measure(kit, max_w, max_h);
}

bool
Menu::key(Kit &kit, const Key &ev)
{
	if (MenuPopup::key(kit, ev))
		return true;
	if (ev.mods)
		return false;
	if (ev.key < Qt::Key_A || ev.key > Qt::Key_Z || !this->col)
		return false;
	const QChar letter = QChar(ev.key).toLower();
	for (const auto &k : this->col->kids) {
		auto *item = dynamic_cast<MenuItem *>(k.get());
		if (!item || item->mnemonic < 0 || item->mnemonic >= item->text.size())
			continue;
		if (item->text[item->mnemonic].toLower() == letter) {
			item->activate(kit);
			return true;
		}
	}
	return false;
}

void
MenuItem::measure(Kit &kit, float, float)
{
	const float lw =
		this->label_col > 0.0f ? this->label_col : label_width(kit);
	const float aw =
		this->accel_col > 0.0f ? this->accel_col : accel_width(kit);
	float width = kFramePadX + kIconPx + kFramePadX + lw + 2 * kFramePadX + aw;
	if (this->sub)
		width += kIconPx;
	width += kFramePadX;
	float ch = kit.text_height(QStringLiteral("Ag"), 0.0f, false);
	ch = max(ch, kIconPx);
	if (!this->text.isEmpty())
		ch = max(ch, kit.text_height(this->text, 0.0f, false));
	if (!this->accel.isEmpty())
		ch = max(ch, kit.text_height(this->accel, 0.0f, false));
	this->r = {
		0, 0, kit.snap_size(width), kit.snap_size(kFramePadY * 2.0f + ch)};
}

void
MenuItem::paint(Kit &kit) const
{
	if (!this->visible)
		return;
	// The selection is kit.focus_ alone: kit.pressed_ is just the capture,
	// and it would stay lit behind the pointer when press-dragging through.
	// A submenu's opener keeps this->active, as focus_ moves into the submenu.
	if (this->enabled_ && (this->active || kit.focus_ == this))
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.colours_[ColourPress]));

	const MenuCols cols = menu_cols(kit, *this);
	const float lead_x = this->r.x + kFramePadX;
	const float label_x = this->r.x + cols.label_x;
	const float accel_x = this->r.x + cols.accel_x;
	const float th =
		this->text.isEmpty() ? 0.0f : kit.text_height(this->text, 0.0f, false);
	const float ty = this->r.y + (this->r.h - th) * 0.5f;
	const float iy = this->r.y + (this->r.h - kIconPx) * 0.5f;
	const Colour label_c =
		col(kit.colours_[ColourInk], this->enabled_ ? 1.0f : 0.5f);

	if (this->checkable && this->checked)
		emit_icon(kit, lead_x, iy, kIconPx, "object-select-symbolic", label_c);
	if (!this->text.isEmpty()) {
		const QString shown = menu_shown(kit, *this);
		emit_text(kit, label_x, ty, shown, label_c, false,
			menu_mnemonic(*this, shown));
	}
	if (!this->accel.isEmpty()) {
		const float tw = kit.text_width(this->accel, false);
		const float ath = kit.text_height(this->accel, 0.0f, false);
		emit_text(kit, accel_x + cols.accel_w - tw,
			this->r.y + (this->r.h - ath) * 0.5f, this->accel,
			col(kit.colours_[ColourInk], 0.5f), false, -1);
	}
	if (this->sub) {
		emit_icon(kit, this->r.x + this->r.w - kFramePadX - cols.chevron, iy,
			kIconPx, "go-next-symbolic",
			col(kit.colours_[ColourInk], this->enabled_ ? 1.0f : 0.375f));
	}
}

void
MenuItem::prepare(Kit &kit)
{
	if (this->sub)
		kit.pack_icon("go-next-symbolic", int(kIconPx * kit.dpr_));
	if (this->checkable)
		kit.pack_icon("object-select-symbolic", int(kIconPx * kit.dpr_));
	if (this->text.isEmpty())
		return;
	cache_text(kit, menu_shown(kit, *this), false, 0.0f);
	if (!this->accel.isEmpty())
		cache_text(kit, this->accel, false, 0.0f);
}

bool
MenuItem::activate(Kit &kit)
{
	if (!this->sub)
		return Button::activate(kit);
	Popup *owner = nullptr;
	for (Widget *w = this->parent_; w; w = w->parent_) {
		if (auto *p = dynamic_cast<Popup *>(w)) {
			owner = p;
			break;
		}
	}
	if (!owner)
		return false;
	this->sub->open_sub(kit, *owner, *this);
	kit.focus_first(this->sub);
	return true;
}

float
MenuItem::label_width(const Kit &kit) const
{
	return kit.text_width(this->text, false);
}

float
MenuItem::accel_width(const Kit &kit) const
{
	return kit.text_width(this->accel, false);
}

// --- ToolbarSlot ------------------------------------------------------------

namespace
{

constexpr float kWinPadX = 4.0f;
constexpr float kWinPadY = 4.0f;

void
sync_overflow_proxy(Widget &source, Widget &proxy)
{
	Widget *parent = proxy.parent_;
	const bool visible = proxy.visible;
	const bool layout_visible = proxy.layout_visible;
	if (auto *button = dynamic_cast<Button *>(&source))
		static_cast<Button &>(proxy) = *button;
	else if (auto *label = dynamic_cast<Label *>(&source))
		static_cast<Label &>(proxy) = *label;
	proxy.parent_ = parent;
	proxy.visible = visible;
	proxy.layout_visible = layout_visible;
}

unique_ptr<Widget>
overflow_proxy(Widget &source)
{
	if (dynamic_cast<Button *>(&source))
		return make_unique<Button>();
	if (dynamic_cast<Label *>(&source))
		return make_unique<Label>();
	if (dynamic_cast<Sep *>(&source))
		return make_unique<Sep>();
	return nullptr;
}

void
add_overflow_proxies(Overflow &overflow, ToolbarSlot *row)
{
	if (!row)
		return;
	const size_t end = row->item_count();
	for (size_t i = 0; i < end; ++i) {
		if (!row->kids[i])
			continue;
		auto proxy = overflow_proxy(*row->kids[i]);
		if (!proxy)
			continue;
		sync_overflow_proxy(*row->kids[i], *proxy);
		proxy->visible = false;
		if (overflow.col) {
			overflow.sources.push_back(row->kids[i].get());
			overflow.col->add_child(std::move(proxy));
		}
	}
}

void
fill_overflow_items(ToolbarSlot &row, Overflow &overflow)
{
	if (!overflow.col)
		return;
	for (auto &item : overflow.col->kids)
		item->visible = false;
	const size_t end = row.item_count();
	size_t a = min(row.split_, end);
	size_t b = end;
	while (a < b && is_sep(row.kids[a].get()))
		++a;
	while (b > a && is_sep(row.kids[b - 1].get()))
		--b;
	for (size_t i = a; i < b; ++i) {
		Widget *source = row.kids[i].get();
		for (size_t j = 0; j < overflow.sources.size(); ++j) {
			if (overflow.sources[j] != source)
				continue;
			Widget &proxy = *overflow.col->kids[j];
			sync_overflow_proxy(*source, proxy);
			proxy.visible = source->visible;
			break;
		}
	}
}
}  // namespace

ToolbarSlot::ToolbarSlot()
{
	auto button = make_unique<Button>();
	button->visible = false;
	button->icon = "disclose-arrow-down-symbolic";
	button->tip_text = "More";
	this->more = button.get();
	Composite::add_child(std::move(button));
}

Widget *
ToolbarSlot::add_item(unique_ptr<Widget> item, size_t at)
{
	return Composite::add_child(std::move(item), min(at, item_count()));
}

size_t
ToolbarSlot::item_count() const
{
	return this->more && !this->kids.empty() ? this->kids.size() - 1 : 0;
}

void
ToolbarSlot::measure(Kit &kit, float max_w, float max_h)
{
	for (size_t i = 0; i < item_count(); ++i) {
		if (this->kids[i])
			this->kids[i]->layout_visible = true;
	}
	this->more->visible = false;
	this->split_ = item_count();
	Row::measure(kit, max_w, max_h);
}

void
ToolbarSlot::arrange(Kit &kit, Rect alloc)
{
	if (!this->visible) {
		this->r = {};
		this->split_ = 0;
		return;
	}
	const Rect in = kit.snap_rect(alloc).inset(this->pad_x, this->pad_y);
	const size_t end = item_count();
	measure(kit, kUnlim, alloc.h);
	const float total = max(0.0f, this->r.w - this->pad_x * 2.0f);

	this->split_ = end;
	this->more->visible = false;
	if (total > in.w) {
		this->more->visible = true;
		this->more->measure(kit, kUnlim, in.h);
		const float budget = max(0.0f, in.w - this->more->r.w - this->gap);
		float used = 0.0f;
		int kept = 0;
		bool full = false;
		this->split_ = 0;
		for (size_t i = 0; i < end; ++i) {
			Widget *item = this->kids[i].get();
			if (!item || !item->shown()) {
				if (!full)
					this->split_ = i + 1;
				continue;
			}
			const float need = item->r.w + (kept ? this->gap : 0.0f);
			if (full || used + need > budget) {
				full = true;
				continue;
			}
			used += need;
			++kept;
			this->split_ = i + 1;
		}
		while (this->split_ > 0 &&
			(!this->kids[this->split_ - 1] ||
				is_sep(this->kids[this->split_ - 1].get())))
			--this->split_;
		this->more->visible = this->split_ < end;
	}
	for (size_t i = this->split_; i < end; ++i) {
		if (this->kids[i])
			this->kids[i]->layout_visible = false;
	}
	Row::arrange(kit, alloc);
}

// --- Toolbar ---------------------------------------------------------------

Toolbar::Toolbar(unique_ptr<ToolbarSlot> left_row,
	unique_ptr<ToolbarSlot> mid_row, unique_ptr<ToolbarSlot> right_row)
{
	this->pad_x = kWinPadX;
	this->pad_y = kWinPadY;
	this->fill = Fill::Toolbar;
	this->stroke = Stroke::Bottom;
	this->hittable = true;

	this->left = left_row.get();
	if (left_row)
		add_child(std::move(left_row));
	this->mid = mid_row.get();
	if (mid_row)
		add_child(std::move(mid_row));
	this->right = right_row.get();
	if (right_row)
		add_child(std::move(right_row));

	this->overflow_owned_ = make_unique<Overflow>();
	this->overflow_owned_->pad_y = kWinPadY;
	this->overflow = this->overflow_owned_.get();
	add_overflow_proxies(*this->overflow, this->left);
	add_overflow_proxies(*this->overflow, this->mid);
	add_overflow_proxies(*this->overflow, this->right);

	this->overflow->fill_items = [this] {
		if (Button *m = this->overflow->opener) {
			if (ToolbarSlot *slot = slot_for_more(m))
				fill_overflow_items(*slot, *this->overflow);
		}
	};

	auto bind_more = [this](Button *m) {
		if (!m)
			return;
		m->activate_on_press = true;
		m->on_click = [this, m](Kit &kit) {
			if (this->overflow->visible && this->overflow->opener == m)
				this->overflow->close(kit);
			else
				this->overflow->open(kit, m);
		};
	};
	bind_more(this->left ? this->left->more : nullptr);
	bind_more(this->mid ? this->mid->more : nullptr);
	bind_more(this->right ? this->right->more : nullptr);
}

void
Toolbar::sync_buttons()
{
	auto apply = [this](Widget *w) {
		auto *btn = dynamic_cast<Button *>(w);
		if (!btn)
			return;
		if (btn->action == Action::None)
			return;
		const ActionDef &d = action_def(btn->action);
		const bool on = this->actor.checked && this->actor.checked(btn->action);
		btn->enabled_ =
			!this->actor.enabled || this->actor.enabled(btn->action);
		btn->active = on && btn->action != Action::SortDir;
		btn->icon = action_icon(d, on);
		btn->tip_text = action_tip(d, on);
		btn->tip_accel = action_accel(d);
	};
	auto walk = [&apply](const ToolbarSlot *row) {
		if (!row)
			return;
		for (auto &k : row->kids)
			apply(k.get());
	};
	walk(this->left);
	walk(this->mid);
	walk(this->right);
	for (ToolbarSlot *slot : {this->left, this->mid, this->right}) {
		if (slot && slot->more)
			slot->more->active = this->overflow && this->overflow->visible &&
				this->overflow->opener == slot->more;
	}
}

void
Toolbar::measure(Kit &kit, float avail_w, float avail_h)
{
	const float ih = max(0.0f, avail_h - this->pad_y * 2.0f);
	float h = 0.0f;
	auto slot = [&](Widget *w) {
		if (!w)
			return;
		w->measure(kit, kUnlim, ih);
		h = max(h, w->r.h);
	};
	slot(this->left);
	slot(this->mid);
	slot(this->right);
	this->r.w = avail_w;
	this->r.h = this->pad_y * 2.0f + h;
	if (avail_h > 0.0f)
		this->r.h = min(this->r.h, avail_h);
}

void
Toolbar::arrange(Kit &kit, Rect alloc)
{
	if (!this->visible) {
		this->r = {};
		return;
	}
	this->r = kit.snap_rect(alloc);
	place_slots(kit);
}

void
Toolbar::place_slots(Kit &kit)
{
	if (this->r.w <= 0.0f)
		return;
	const Rect bar = this->r.inset(this->pad_x, this->pad_y);
	if (bar.w <= 0.0f)
		return;
	const float avail = bar.w;
	const float h = bar.h;
	const float x0 = bar.x;
	const float y0 = bar.y;
	auto nat = [&](Widget *w) {
		if (!w)
			return 0.0f;
		w->measure(kit, kUnlim, h);
		return w->r.w;
	};
	const float lw = nat(this->left);
	const float mw = nat(this->mid);
	const float rw = nat(this->right);
	float mmin = 0.0f;
	if (mw > 0.0f && this->mid && this->mid->more) {
		this->mid->more->measure(kit, kUnlim, h);
		mmin = min(mw, this->mid->more->r.w);
	}
	float left_w = lw;
	float right_w = rw;
	float mid_x;
	if (lw + mw + rw <= avail) {
		mid_x = x0 + clamp((avail - mw) * 0.5f, lw, avail - rw - mw);
	} else {
		const float keep = min(mmin, avail);
		const float rest = max(0.0f, avail - keep);
		left_w = min(lw, rest * 0.5f);
		right_w = min(rw, rest - left_w);
		left_w = min(lw, rest - right_w);
		mid_x = x0 + left_w;
	}
	const float mid_w = x0 + avail - right_w - mid_x;
	if (this->left)
		this->left->arrange(kit, {x0, y0, left_w, h});
	if (this->mid) {
		this->mid->align = Align::Start;
		this->mid->arrange(kit, {mid_x, y0, mid_w, h});
	}
	if (this->right)
		this->right->arrange(kit, {x0 + avail - right_w, y0, right_w, h});
}

ToolbarSlot *
Toolbar::slot_for_more(const Button *more) const
{
	if (!more)
		return nullptr;
	for (ToolbarSlot *slot : {this->left, this->mid, this->right}) {
		if (slot && more == slot->more)
			return slot;
	}
	return nullptr;
}

// --- Titlebar --------------------------------------------------------------

namespace
{

// How far the pointer must travel before a titlebar press becomes a move.
constexpr float kDragPx = 4.0f;

unique_ptr<Button>
make_title_button(Titlebar *bar, Action action, const char *icon)
{
	auto btn = make_unique<Button>();
	btn->action = action;
	btn->icon = icon;
	const ActionDef &d = action_def(action);
	btn->tip_text = action_tip(d, false);
	btn->tip_accel = action_accel(d);
	btn->on_click = [bar, action](Kit &) {
		if (bar->actor.apply)
			bar->actor.apply(action);
	};
	return btn;
}

}  // namespace

Titlebar::Titlebar()
{
	this->pad_x = kWinPadX;
	this->pad_y = kWinPadY;
	this->fill = Fill::Toolbar;
	this->stroke = Stroke::Bottom;
	this->hittable = true;

	auto label = make_unique<Label>();
	label->align = Align::Center;
	label->bold = true;
	this->title = label.get();
	add_child(std::move(label));

	auto min = make_title_button(this, Action::Minimize, "window-minimize");
	this->minimize = min.get();
	add_child(std::move(min));
	auto max = make_title_button(this, Action::Maximize, "window-maximize");
	this->maximize = max.get();
	add_child(std::move(max));
	auto cls = make_title_button(this, Action::CloseWindow, "window-close");
	this->close = cls.get();
	add_child(std::move(cls));
}

void
Titlebar::sync(Kit &kit)
{
	if (this->title)
		this->title->text = this->text;
	if (this->maximize) {
		const ActionDef &d = action_def(Action::Maximize);
		const bool on = kit.maximized_;
		this->maximize->icon = on ? "window-restore" : "window-maximize";
		this->maximize->tip_text = action_tip(d, on);
		this->maximize->active = on;
	}
	auto apply = [this](Button *btn) {
		if (!btn || btn->action == Action::None)
			return;
		btn->enabled_ =
			!this->actor.enabled || this->actor.enabled(btn->action);
	};
	apply(this->minimize);
	apply(this->maximize);
	apply(this->close);
}

// Only the client draws its own decorations, and never over a fullscreen
// window: this is the one widget that decides for itself whether it is there.
void
Titlebar::measure(Kit &kit, float avail_w, float)
{
	this->visible = kit.csd_ && !kit.fullscreen_;
	if (!this->visible) {
		this->r = {};
		return;
	}
	float ih = 0.0f;
	auto slot = [&](Button *b) {
		if (!b)
			return;
		b->measure(kit, kUnlim, kUnlim);
		ih = max(ih, b->r.h);
	};
	slot(this->minimize);
	slot(this->maximize);
	slot(this->close);
	this->r.w = avail_w;
	this->r.h = this->pad_y * 2.0f + ih;
}

void
Titlebar::arrange(Kit &kit, Rect alloc)
{
	if (!this->visible) {
		this->r = {};
		return;
	}
	this->r = kit.snap_rect(alloc);
	const Rect bar = this->r.inset(this->pad_x, this->pad_y);
	float x = bar.x + bar.w;
	auto place = [&](Button *b) {
		if (!b)
			return;
		b->measure(kit, kUnlim, bar.h);
		x -= b->r.w;
		b->arrange(kit, {x, bar.y, b->r.w, bar.h});
	};
	place(this->close);
	place(this->maximize);
	place(this->minimize);
	if (this->title) {
		const float left = bar.x;
		const float right = x;
		const float avail = max(0.0f, right - left);
		this->title->text =
			kit.elide_lines(this->text, avail, 1, this->title->bold);
		this->title->measure(kit, avail, bar.h);
		float tw = min(this->title->r.w, avail);
		float tx = this->r.x + (this->r.w - tw) * 0.5f;
		if (tx < left)
			tx = left;
		if (tx + tw > right)
			tx = max(left, right - tw);
		this->title->arrange(kit, {tx, bar.y, tw, bar.h});
	}
}

void
Titlebar::paint(Kit &kit) const
{
	Panel::paint(kit);
}

void
Titlebar::prepare(Kit &kit)
{
	const int px = max(16, int(lround(double(kIconPx) * double(kit.dpr_))));
	kit.pack_icon("window-minimize", px);
	kit.pack_icon("window-maximize", px);
	kit.pack_icon("window-restore", px);
	kit.pack_icon("window-close", px);
	if (this->title && !this->title->text.isEmpty())
		kit.cache_text(this->title->text, this->title->bold);
	Panel::prepare(kit);
}

bool
Titlebar::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::RightButton) {
		if (!kit.start_menu)
			return false;
		kit.start_menu(x, y);
		return true;
	}
	if (button != Qt::LeftButton)
		return false;
	if (kit.start_resize_at(x, y))
		return true;
	if (!kit.start_move)
		return false;
	// Asking for the move on the press would leave a compositor grab open
	// across the whole double click, and Mutter anchors an unmaximize to the
	// pointer whenever one is: the window would land under the cursor rather
	// than back where it was. Wait for the pointer to travel instead.
	this->drag_x_ = x;
	this->drag_y_ = y;
	this->drag_armed_ = true;
	kit.pressed_ = this;
	return true;
}

bool
Titlebar::release(Kit &, float, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	this->drag_armed_ = false;
	return true;
}

bool
Titlebar::motion(Kit &kit, float x, float y)
{
	if (!this->drag_armed_ || !kit.start_move)
		return false;
	const float dx = x - this->drag_x_;
	const float dy = y - this->drag_y_;
	if (dx * dx + dy * dy < kDragPx * kDragPx)
		return false;
	this->drag_armed_ = false;
	kit.start_move();
	return true;
}

bool
Titlebar::double_click(
	Kit &kit, float x, float y, Qt::MouseButton button, unsigned)
{
	if (button != Qt::LeftButton)
		return false;
	if (dynamic_cast<Button *>(kit.hit(x, y)))
		return false;
	if (this->actor.apply)
		this->actor.apply(Action::Maximize);
	return true;
}

// --- Kit ---------------------------------------------------------------------

Kit::Packed
Kit::pack_bitmap(const QImage &image)
{
	if (image.isNull() || image.width() <= 0 || image.height() <= 0)
		return {};
	QImage src = image;
	if (src.format() != QImage::Format_ARGB32 &&
		src.format() != QImage::Format_ARGB32_Premultiplied)
		src = src.convertToFormat(QImage::Format_ARGB32_Premultiplied);
	const Packed packed = pack_or_grow(*this, src.width(), src.height());
	if (packed.empty())
		return {};
	blit(*this, packed, src, false);
	return packed;
}

void
Kit::cache_text(const QString &text, bool bold)
{
	::dn::cache_text(*this, text, bold, 0.0f);
}

void
Kit::emit_text(float x, float y, const QString &text, Colour colour, bool bold)
{
	::dn::emit_text(*this, x, y, text, colour, bold, -1);
}

void
Kit::pack_icon(const char *name, int px)
{
	if (!name)
		return;
	auto it = this->icons_.find(name);
	if (it != this->icons_.end() && it->second.w == px && it->second.h == px)
		return;
	QImage image = raster_symbolic(name, px);
	if (image.isNull())
		image = raster_window_button(name, px);
	if (image.isNull())
		return;
	const Packed packed = pack_or_grow(*this, image.width(), image.height());
	if (packed.empty())
		return;
	if (it != this->icons_.end())
		this->atlas_.release(it->second);
	blit(*this, packed, image, false);
	this->icons_[name] = packed;
}

void
Kit::draw_icon(float x, float y, float size, const char *name, Colour colour)
{
	emit_icon(*this, x, y, size, name, colour);
}

void
Kit::draw_glow(float ix, float iy, float iw, float ih, Colour col)
{
	if (this->glow_.empty() || iw <= 0.0f || ih <= 0.0f)
		return;
	float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
	this->atlas_.uv(this->glow_, &u0, &v0, &u1, &v1);
	const float aw = float(max(this->atlas_.w, 1));
	const float ah = float(max(this->atlas_.h, 1));
	const float u_in =
		(float(this->glow_.x) + float(this->glow_.w) - 0.5f) / aw;
	const float v_in =
		(float(this->glow_.y) + float(this->glow_.h) - 0.5f) / ah;
	const float ox = ix - kGlowPts;
	const float oy = iy - kGlowPts;
	const float ox1 = ix + iw + kGlowPts;
	const float oy1 = iy + ih + kGlowPts;
	const float ix1 = ix + iw;
	const float iy1 = iy + ih;
	this->list_.add_image(ox, oy, ix, iy, u0, v0, u1, v1, col);
	this->list_.add_image(ix1, oy, ox1, iy, u1, v0, u0, v1, col);
	this->list_.add_image(ox, iy1, ix, oy1, u0, v1, u1, v0, col);
	this->list_.add_image(ix1, iy1, ox1, oy1, u1, v1, u0, v0, col);
	this->list_.add_image(ix, oy, ix1, iy, u_in, v0, u_in, v1, col);
	this->list_.add_image(ix, iy1, ix1, oy1, u_in, v1, u_in, v0, col);
	this->list_.add_image(ox, iy, ix, iy1, u0, v_in, u1, v_in, col);
	this->list_.add_image(ix1, iy, ox1, iy1, u1, v_in, u0, v_in, col);
}

void
Kit::focus_ring(Rect w)
{
	const float hair = 1.0f / max(this->dpr_, 0.01f);

	// Snap the edges rather than the size: snap_size() rounds up, which would
	// push the far edges back out over the widget's own border.
	const Rect g = w.inset(hair, hair);
	this->list_.add_rect_stroke(snap(g.x), snap(g.y), snap(g.x + g.w),
		snap(g.y + g.h), col(this->colours_[ColourInk]));
}

void
Kit::draw_shadow(Rect w)
{
	draw_glow(w.x, w.y, w.w, w.h, {0, 0, 0, 0.25f});
}

bool
Kit::set_dpr(float dpr)
{
	const float next = dpr > 0.0f ? dpr : 1.0f;
	if (abs(next - this->dpr_) < 0.01f)
		return false;
	this->dpr_ = next;
	if (this->inited_)
		rebuild_atlas(*this);
	return true;
}

bool
Kit::font_pixels(unsigned char **out_pixels, int *width, int *height) const
{
	if (!out_pixels || !width || !height || this->atlas_.pixels.empty() ||
		this->atlas_.w <= 0 || this->atlas_.h <= 0)
		return false;
	*out_pixels = (unsigned char *) this->atlas_.pixels.data();
	*width = this->atlas_.w;
	*height = this->atlas_.h;
	return true;
}

bool
Kit::take_atlas_dirty()
{
	return this->atlas_.take_dirty();
}

float
Kit::text_width(const QString &text, bool bold) const
{
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	return float(QFontMetricsF(font).horizontalAdvance(text)) /
		max(this->dpr_, 0.01f);
}

float
Kit::caret_x(const QString &text, int index, bool bold) const
{
	const float dpr = max(this->dpr_, 0.01f);
	if (text.isEmpty())
		return 0.0f;
	const int at = clamp(index, 0, int(text.size()));
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	QTextLayout layout(text, font);
	layout_text(&layout, dpr, 0.0f);
	const QTextLine line = layout.lineForTextPosition(at);
	if (!line.isValid())
		return 0.0f;
	return float(line.cursorToX(at)) / dpr;
}

int
Kit::index_at(const QString &text, float x, bool bold) const
{
	if (text.isEmpty())
		return 0;
	const float dpr = max(this->dpr_, 0.01f);
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	QTextLayout layout(text, font);
	layout_text(&layout, dpr, 0.0f);
	if (layout.lineCount() < 1)
		return 0;
	const QTextLine line = layout.lineAt(0);
	if (!line.isValid())
		return 0;
	return line.xToCursor(qreal(x * dpr));
}

float
Kit::text_ascent(bool bold) const
{
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	return float(QFontMetricsF(font).ascent()) / max(this->dpr_, 0.01f);
}

QString
Kit::elide_lines(
	const QString &text, float wrap_pts, int max_lines, bool bold) const
{
	if (text.isEmpty() || max_lines < 1)
		return text;
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	const float dpr = max(this->dpr_, 0.01f);
	const float wrap_px = (wrap_pts > 0.0f ? wrap_pts : 1.0e8f) * dpr;
	QFontMetricsF fm(font);
	if (max_lines == 1) {
		if (fm.horizontalAdvance(text) <= wrap_px + 1.0f)
			return text;
		return fm.elidedText(text, Qt::ElideRight, wrap_px);
	}
	QTextLayout layout(text, font);
	layout_text(&layout, dpr, wrap_pts);
	if (layout.lineCount() <= max_lines)
		return text;
	const QTextLine last = layout.lineAt(max_lines - 1);
	if (!last.isValid())
		return text;
	const int start = last.textStart();
	return text.left(start) +
		fm.elidedText(text.mid(start), Qt::ElideRight, wrap_px);
}

float
Kit::text_height(const QString &text, float wrap_pts, bool bold) const
{
	const QFont &font = bold ? this->font_bold_px_ : this->font_px_;
	const float dpr = max(this->dpr_, 0.01f);
	if (text.isEmpty())
		return float(QFontMetricsF(font).height()) / dpr;
	QTextLayout layout(text, font);
	layout_text(&layout, dpr, wrap_pts);
	const float h = float(layout.boundingRect().height());
	if (h <= 0.0f)
		return float(QFontMetricsF(font).height()) / dpr;
	return h / dpr;
}

float
Kit::snap(float v) const
{
	const float d = max(this->dpr_, 0.01f);
	return round(v * d) / d;
}

float
Kit::snap_size(float v) const
{
	if (v <= 0.0f)
		return 0.0f;
	const float d = max(this->dpr_, 0.01f);
	return ceil(v * d - 1e-4f) / d;
}

Rect
Kit::snap_rect(Rect r) const
{
	r.x = snap(r.x);
	r.y = snap(r.y);
	r.w = snap_size(r.w);
	r.h = snap_size(r.h);
	return r;
}

namespace
{

void
collect_focusable(Widget *w, vector<Widget *> &out)
{
	if (!w || !w->shown())
		return;
	if (w->focusable())
		out.push_back(w);
	const size_t n = w->child_count();
	for (size_t i = 0; i < n; ++i)
		collect_focusable(w->child(i), out);
}

bool
focus_in_visible_tree(Widget *w, const Widget *root)
{
	if (!w || !root)
		return false;
	if (!w->focusable() && w != root)
		return false;
	for (Widget *p = w; p; p = p->parent_) {
		if (!p->shown())
			return false;
		if (p == root)
			return true;
	}
	return false;
}

Popup *
owning_popup(Widget *w)
{
	for (Widget *p = w; p; p = p->parent_) {
		if (auto *pop = dynamic_cast<Popup *>(p))
			return pop;
	}
	return nullptr;
}

bool
is_popup_opener(const Kit &kit, const Widget *w)
{
	if (!w)
		return false;
	for (Popup *p : kit.popups_) {
		if (p && p->opener == w)
			return true;
	}
	return false;
}

bool
press_targets_popup(const Kit &kit, Widget *w)
{
	return owning_popup(w) || is_popup_opener(kit, w);
}

Popup *
popup_for_hit(const Kit &kit, Widget *h)
{
	if (Popup *p = owning_popup(h))
		return p;
	if (h) {
		for (Popup *p : kit.popups_) {
			if (p && p->opener == h)
				return p;
		}
	}
	return kit.top_popup();
}

}  // namespace

void
Kit::sync_focus()
{
	// Focus can be dropped from under a text field -- a popup opening over
	// it, or the toolbar overflowing it away -- and the input method has to
	// hear about it, or it keeps composing into a field that is gone.
	Widget *was = this->focus_;
	auto done = [this, was] {
		if (this->focus_ != was && this->input_method_changed)
			this->input_method_changed();
	};

	if (!this->popups_.empty()) {
		for (Popup *p : this->popups_) {
			if (focus_in_visible_tree(this->focus_, p))
				return;
		}
		set_focus(nullptr, false);
		done();
		return;
	}
	if (focus_in_visible_tree(this->focus_, this->root_))
		return;
	if (focus_in_visible_tree(this->default_focus_, this->root_)) {
		set_focus(this->default_focus_, false);
		done();
		return;
	}
	set_focus(nullptr, false);
	done();
}

void
Kit::set_focus(Widget *w, bool ring)
{
	this->focus_ = w;
	this->focus_visible_ = ring;
}

Widget *
Kit::focus_scope() const
{
	if (Popup *p = top_popup(); p && p->shown())
		return p;
	for (Widget *w = this->focus_ ? this->focus_ : this->root_; w;
		w = w->parent_) {
		if (w->traps_focus() && w->shown())
			return w;
	}
	return this->root_;
}

void
Kit::cycle_focus(int dir)
{
	cycle_focus(focus_scope(), dir);
}

bool
Kit::cycle_focus(Widget *scope, int dir, bool wrap)
{
	vector<Widget *> items;
	collect_focusable(scope, items);
	if (items.empty())
		return false;
	int i = -1;
	for (int k = 0, n = int(items.size()); k < n; ++k) {
		if (items[size_t(k)] == this->focus_) {
			i = k;
			break;
		}
	}
	const int n = int(items.size());
	Widget *next = this->focus_;
	if (i < 0)
		next = dir >= 0 ? items.front() : items.back();
	else if (wrap)
		next = items[size_t((i + dir + n) % n)];
	else {
		const int j = i + dir;
		if (j >= 0 && j < n)
			next = items[size_t(j)];
	}

	// Tab always means the keyboard, even when it runs into the end of a
	// non-wrapping scope and leaves focus where it was.
	set_focus(next, true);
	return true;
}

void
Kit::focus_first(Widget *scope)
{
	vector<Widget *> items;
	collect_focusable(scope, items);
	set_focus(items.empty() ? nullptr : items.front(), true);
}

bool
Kit::key(const Key &ev)
{
	if (Popup *p = top_popup(); p && p->shown() && p->captures_keys())
		return p->key(*this, ev);
	if (ev.key == Qt::Key_Tab || ev.key == Qt::Key_Backtab) {
		const int dir = (ev.key == Qt::Key_Backtab ||
							(ev.mods & unsigned(Qt::ShiftModifier)))
			? -1
			: 1;
		cycle_focus(dir);
		return true;
	}
	for (Widget *w = this->focus_; w; w = w->parent_) {
		if (w->key(*this, ev))
			return true;
	}
	if (Popup *p = top_popup(); p && p->shown())
		return p->key(*this, ev);
	return false;
}

bool
Kit::text_target(TextTarget &out) const
{
	for (const Widget *w = this->focus_; w; w = w->parent_) {
		if (w->text_target(*this, out))
			return true;
	}
	return false;
}

bool
Kit::input_method(const QString &commit, const QString &preedit, int caret)
{
	for (Widget *w = this->focus_; w; w = w->parent_) {
		if (w->input_method(*this, commit, preedit, caret))
			return true;
	}
	return false;
}

bool
Kit::mouse_press(float x, float y, Qt::MouseButton button, unsigned mods)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	this->mods_ = mods;
	if (button == Qt::LeftButton)
		this->left_down_ = true;
	if (this->root_ && this->root_->r.w <= 0) {
		this->root_->arrange(*this, {0, 0, this->host_w_, this->host_h_});
		relayout_popups();
		sync_focus();
	}
	this->hot_ = hit(x, y);
	// A press tracks the pointer just like a hover does, so that what a menu
	// shows as selected is what the release will activate.
	track_popups(x, y);
	for (Widget *w = this->hot_; w; w = w->parent_) {
		if (w->press(*this, x, y, button))
			return true;
	}
	return false;
}

bool
Kit::mouse_release(float x, float y, Qt::MouseButton button)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	if (button == Qt::LeftButton)
		this->left_down_ = false;
	if (Popup *p = popup_for_hit(*this, hit(x, y));
		p && p->release(*this, x, y, button)) {
		this->pressed_ = nullptr;
		return true;
	}
	if (this->pressed_) {
		const bool consumed = this->pressed_->release(*this, x, y, button);
		this->pressed_ = nullptr;
		return consumed;
	}
	this->pressed_ = nullptr;
	return false;
}

bool
Kit::mouse_motion(float x, float y)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	this->hot_ = hit(x, y);
	for (Widget *w = this->hot_; w; w = w->parent_) {
		if (Scroll *s = w->scrollbar())
			s->reveal();
	}
	tooltip(this->hot_);
	// A popup gets first refusal so press-dragging through a menu tracks
	// hover, but a widget that claims the motion -- a scrollbar being
	// dragged -- keeps it.
	if (this->pressed_) {
		if (!press_targets_popup(*this, this->pressed_))
			return this->pressed_->motion(*this, x, y);
		if (this->pressed_->motion(*this, x, y))
			return true;
	}
	if (track_popups(x, y))
		return true;
	for (Widget *w = this->hot_; w; w = w->parent_) {
		if (w->motion(*this, x, y))
			return true;
	}
	return false;
}

// Let the open popups, innermost first, move their selection to kit.hot_.
bool
Kit::track_popups(float x, float y)
{
	for (auto it = this->popups_.rbegin(); it != this->popups_.rend(); ++it) {
		if (*it && (*it)->motion(*this, x, y))
			return true;
	}
	return false;
}

bool
Kit::mouse_scroll(float x, float y, int delta)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	if (!delta)
		return false;
	Widget *h = hit(x, y);
	// An open popup owns the pointer: dispatch inside it, swallow outside
	// so the page behind never scrolls out from under it.
	if (popup_open() && !owning_popup(h))
		return true;
	for (Widget *w = h; w; w = w->parent_) {
		if (w->scroll(*this, x, y, delta))
			return true;
	}
	return false;
}

bool
Kit::pan(float x, float y, float dx, float dy)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	if (dx == 0.0f && dy == 0.0f)
		return false;
	Widget *h = hit(x, y);
	if (popup_open() && !owning_popup(h))
		return true;
	for (Widget *w = h; w; w = w->parent_) {
		if (w->pan(*this, x, y, dx, dy))
			return true;
	}
	return false;
}

bool
Kit::gesture(float x, float y, float scale_factor, float angle_delta)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	Widget *h = hit(x, y);
	if (popup_open() && !owning_popup(h))
		return true;
	for (Widget *w = h; w; w = w->parent_) {
		if (w->gesture(*this, x, y, scale_factor, angle_delta))
			return true;
	}
	return false;
}

bool
Kit::mouse_double_click(float x, float y, Qt::MouseButton button, unsigned mods)
{
	this->mouse_x_ = x;
	this->mouse_y_ = y;
	if (popup_open())
		return true;
	for (Widget *w = hit(x, y); w; w = w->parent_) {
		if (w->double_click(*this, x, y, button, mods))
			return true;
	}
	return false;
}

void
Kit::init(float dpr)
{
	destroy();
	this->dpr_ = dpr > 0.0f ? dpr : 1.0f;
	rebuild_atlas(*this);
	bake_colours(nullptr, nullptr);
	this->inited_ = true;
}

void
Kit::destroy()
{
	this->inited_ = false;
	this->popups_.clear();
	this->scrim_.reset();
	++this->atlas_epoch_;
	this->icons_.clear();
	this->glyphs_.clear();
	this->fonts_.clear();
	this->atlas_.clear();
	this->glow_ = {};
	this->raw_ = QRawFont();
	this->raw_bold_ = QRawFont();
	this->root_ = nullptr;
	set_focus(nullptr, false);
	this->default_focus_ = nullptr;
	this->hot_ = nullptr;
	this->pressed_ = nullptr;
	this->tooltip_text_.clear();
	this->tooltip_accel_.clear();
	this->tooltip_visible_ = false;
	this->tooltip_anchor_ = nullptr;
}

void
Kit::forget_tree(Widget *tree)
{
	auto forget = [tree](auto &target) {
		for (auto *w = target; w; w = w->parent_) {
			if (w == tree) {
				target = nullptr;
				return true;
			}
		}
		return false;
	};
	forget(this->focus_);
	forget(this->default_focus_);
	forget(this->pressed_);
	const bool forgot_hot = forget(this->hot_);
	if (forget(this->tooltip_anchor_) || forgot_hot)
		this->tooltip_visible_ = false;
}

void
Kit::bake_colours(Cmm *cmm, Profile *target)
{
	if (this->dark_) {
		this->colours_[ColourWell] = bake_grey(cmm, target, 0x20);
		this->colours_[ColourToolbarTop] = bake_grey(cmm, target, 0x34);
		this->colours_[ColourToolbarBottom] = bake_grey(cmm, target, 0x28);
		this->colours_[ColourHover] = bake_grey(cmm, target, 0x34);
		this->colours_[ColourPress] = bake_grey(cmm, target, 0x48);
#ifdef Q_OS_MACOS
		// This is defined by the native window title bottom border.
		this->colours_[ColourDivider] = bake_grey(cmm, target, 0x00);
#else
		this->colours_[ColourDivider] = bake_grey(cmm, target, 0x58);
#endif
		this->colours_[ColourBusy] = bake_rgb(cmm, target, 0xc0, 0x00, 0x00);
		this->colours_[ColourInk] = bake_grey(cmm, target, 0xff);
		this->colours_[ColourFrame] = bake_grey(cmm, target, 0x00);
		this->colours_[ColourPanel] = bake_grey(cmm, target, 0x28);
		this->colours_[ColourHint] = bake_rgb(cmm, target, 0x88, 0x77, 0x00);
	} else {
		this->colours_[ColourWell] = bake_grey(cmm, target, 0xf8);
		this->colours_[ColourToolbarTop] = bake_grey(cmm, target, 0xe8);
		this->colours_[ColourToolbarBottom] = bake_grey(cmm, target, 0xf0);
		this->colours_[ColourHover] = bake_grey(cmm, target, 0xe4);
		this->colours_[ColourPress] = bake_grey(cmm, target, 0xd0);
		this->colours_[ColourDivider] = bake_grey(cmm, target, 0xc0);
		this->colours_[ColourBusy] = bake_rgb(cmm, target, 0x8b, 0x00, 0x00);
		this->colours_[ColourInk] = bake_grey(cmm, target, 0x00);
		this->colours_[ColourFrame] = bake_grey(cmm, target, 0xff);
		this->colours_[ColourPanel] = bake_grey(cmm, target, 0xf0);
		this->colours_[ColourHint] = bake_rgb(cmm, target, 0xff, 0xee, 0x00);
	}
}

void
Kit::tooltip(const Widget *hot)
{
	if (is_popup_opener(*this, hot))
		hot = nullptr;
	if (this->focus_visible_ && this->focus_ &&
		!this->focus_->tip().isEmpty() && this->focus_->tip_anchor().w > 0) {
		this->tooltip_text_ = this->focus_->tip();
		this->tooltip_accel_ = this->focus_->tip_key();
		this->tooltip_anchor_ = this->focus_;
		this->tooltip_visible_ = true;
		return;
	}
	this->tooltip_anchor_ = nullptr;
	const QString tip = hot ? hot->tip() : QString();
	const QString accel = hot ? hot->tip_key() : QString();
	const float dx = this->mouse_x_ - this->hover_x_;
	const float dy = this->mouse_y_ - this->hover_y_;
	const bool moved = dx * dx + dy * dy > kTooltipMovePts * kTooltipMovePts;
	if (tip != this->tooltip_text_ || accel != this->tooltip_accel_ || moved) {
		this->tooltip_text_ = tip;
		this->tooltip_accel_ = accel;
		this->hover_at_ = chrono::steady_clock::now();
		this->hover_x_ = this->mouse_x_;
		this->hover_y_ = this->mouse_y_;
		this->tooltip_visible_ = false;
	}
	if (tip.isEmpty())
		this->tooltip_visible_ = false;
	else {
		const float elapsed = chrono::duration<float, milli>(
			chrono::steady_clock::now() - this->hover_at_)
								  .count();
		if (elapsed >= kTooltipDelayMs)
			this->tooltip_visible_ = true;
	}
}

void
paint_tooltip(Kit &kit)
{
	if (!kit.tooltip_visible_ || kit.tooltip_text_.isEmpty())
		return;
	const QString text = kit.tooltip_text_;
	const QString accel = kit.tooltip_accel_;
	const float gap = accel.isEmpty() ? 0.0f : 8.0f;
	const float aw = accel.isEmpty() ? 0.0f : kit.text_width(accel, false);
	const float tw =
		kit.text_width(text, false) + gap + aw + kTooltipPadX * 2.0f;
	const float th = kit.text_height(text, 0.0f, false) + kFramePadY * 2.0f;
	float tx = kit.mouse_x_ + 16.0f;
	float ty = kit.mouse_y_ + 8.0f;
	Rect a{};
	if (kit.tooltip_anchor_)
		a = kit.tooltip_anchor_->tip_anchor();
	if (a.w > 0) {
		tx = a.x;
		ty = a.y + a.h + 4.0f;
		if (tx + tw + kGlowPts > kit.host_w_)
			tx = max(0.0f, kit.host_w_ - tw - kGlowPts);
		if (ty + th > kit.host_h_)
			ty = max(0.0f, a.y - th - 4.0f);
	} else {
		if (tx + tw + kGlowPts > kit.host_w_)
			tx = max(0.0f, kit.host_w_ - tw - kGlowPts);
		if (ty + th > kit.host_h_)
			ty = max(0.0f, kit.mouse_y_ - th - 4.0f);
	}
	Panel tipn;
	tipn.pad_x = kTooltipPadX;
	tipn.pad_y = kFramePadY;
	tipn.fill = Fill::Tooltip;
	tipn.stroke = Stroke::All;
	auto row = make_unique<Row>();
	row->gap = gap;
	auto lab = make_unique<Label>();
	lab->text = text;
	row->add_child(std::move(lab));
	if (!accel.isEmpty()) {
		auto acc = make_unique<Label>();
		acc->text = accel;
		acc->dim = true;
		row->add_child(std::move(acc));
	}
	tipn.add_child(std::move(row));
	tipn.arrange(kit, {tx, ty, tw, th});
	kit.draw_shadow(tipn.r);
	tipn.paint(kit);
}

namespace
{

struct Scrim : Panel {
	bool press(Kit &kit, float, float, Qt::MouseButton) override;
};

bool
Scrim::press(Kit &kit, float, float, Qt::MouseButton)
{
	kit.close_popups();
	kit.pressed_ = nullptr;
	return true;
}

int
wake_tree(const Widget *w)
{
	if (!w || !w->shown())
		return -1;
	int ms = w->wake_ms();
	const size_t n = w->child_count();
	for (size_t i = 0; i < n; ++i)
		ms = sooner(ms, wake_tree(w->child(i)));
	return ms;
}

}  // namespace

int
Kit::wake_ms() const
{
	int ms = -1;
	if (!this->tooltip_text_.isEmpty() && !this->tooltip_visible_) {
		const float elapsed = chrono::duration<float, milli>(
			chrono::steady_clock::now() - this->hover_at_)
								  .count();
		if (elapsed < kTooltipDelayMs)
			ms = int(ceil(double(kTooltipDelayMs - elapsed)));
	}
	ms = sooner(ms, wake_tree(this->root_));
	// Popups are not in the root tree, and a scrollbar inside one still
	// has to be told when to hide itself.
	for (const Popup *p : this->popups_)
		ms = sooner(ms, wake_tree(p));
	return sooner(ms, wake_tree(this->scrim_.get()));
}

void
ensure_scrim(Kit &kit)
{
	if (kit.scrim_)
		return;
	auto s = make_unique<Scrim>();
	s->hittable = true;
	s->visible = false;
	s->fill = Fill::None;
	kit.scrim_ = std::move(s);
}

void
Kit::open_popup(Popup *p)
{
	if (!p)
		return;
	ensure_scrim(*this);
	for (Popup *q : this->popups_) {
		if (q == p)
			return;
	}
	if (this->popups_.empty())
		this->popup_at_ = chrono::steady_clock::now();
	this->popups_.push_back(p);
	this->scrim_->visible = true;
	this->scrim_->r = {0.0f, 0.0f, this->host_w_, this->host_h_};
	this->tooltip_visible_ = false;
}

void
Kit::close_popups()
{
	while (!this->popups_.empty()) {
		Popup *p = this->popups_.back();
		if (!p) {
			this->popups_.pop_back();
			continue;
		}
		p->close(*this);
		if (!this->popups_.empty() && this->popups_.back() == p)
			this->popups_.pop_back();
	}
	if (this->scrim_)
		this->scrim_->visible = false;
	sync_focus();
}

// Dialogs sit at the bottom of the stack, so this normally closes everything
// or nothing; a menu opened over one through open_sub() would go, and it stay.
void
Kit::close_transient_popups()
{
	for (auto it = this->popups_.rbegin(); it != this->popups_.rend(); ++it) {
		if (*it && !(*it)->transient()) {
			close_above(*it);
			return;
		}
	}
	close_popups();
}

void
Kit::close_above(const Popup *p)
{
	if (!p) {
		close_popups();
		return;
	}
	while (!this->popups_.empty() && this->popups_.back() != p) {
		Popup *top = this->popups_.back();
		if (!top) {
			this->popups_.pop_back();
			continue;
		}
		top->close(*this);
		if (!this->popups_.empty() && this->popups_.back() == top)
			this->popups_.pop_back();
	}
}

bool
Kit::popup_open() const
{
	return !this->popups_.empty();
}

Popup *
Kit::top_popup() const
{
	return this->popups_.empty() ? nullptr : this->popups_.back();
}

Widget *
Kit::hit(float x, float y)
{
	for (auto it = this->popups_.rbegin(); it != this->popups_.rend(); ++it) {
		Popup *p = *it;
		if (!p || !p->shown())
			continue;
		if (Widget *h = p->hit_at(x, y))
			return h;
	}
	for (auto it = this->popups_.rbegin(); it != this->popups_.rend(); ++it) {
		Popup *p = *it;
		if (!p || !p->shown())
			continue;
		if (p->opener && p->opener->shown() && p->opener->r.contains(x, y))
			return p->opener;
	}
	if (this->scrim_ && this->scrim_->visible)
		return this->scrim_.get();
	return this->root_ ? this->root_->hit_at(x, y) : nullptr;
}

Rect
Kit::frame() const
{
	const Rect host = {0.0f, 0.0f, this->host_w_, this->host_h_};
	if (!this->csd_shadow_)
		return host;
	return snap_rect(host.inset(kGlowPts, kGlowPts));
}

// The resize band straddles the frame's edge, and reaches outside it into
// the shadow, which is the only thing a maximised window has none of.
Qt::Edges
Kit::resize_edges(float x, float y) const
{
	if (!this->csd_ || this->fullscreen_ || this->maximized_)
		return {};
	if (x < 0.0f || y < 0.0f || x >= this->host_w_ || y >= this->host_h_)
		return {};
	const Rect f = frame();
	if (this->csd_shadow_ ? f.contains(x, y) : !f.contains(x, y))
		return {};
	const float band = kResizeBorderPts;
	Qt::Edges e;
	if (x < f.x + band)
		e |= Qt::LeftEdge;
	if (x >= f.x + f.w - band)
		e |= Qt::RightEdge;
	if (y < f.y + band)
		e |= Qt::TopEdge;
	if (y >= f.y + f.h - band)
		e |= Qt::BottomEdge;
	return e;
}

bool
Kit::start_resize_at(float x, float y)
{
	const Qt::Edges edges = resize_edges(x, y);
	if (!edges || !this->start_resize)
		return false;
	// A button that reaches into the band keeps its click.
	if (dynamic_cast<Button *>(hit(x, y)))
		return false;
	this->start_resize(edges);
	return true;
}

void
Kit::sync_cursor()
{
	this->cursor_ = Qt::ArrowCursor;
	if (dynamic_cast<Button *>(this->hot_))
		return;
	if (const Qt::Edges edges = resize_edges(this->mouse_x_, this->mouse_y_)) {
		this->cursor_ = resize_cursor(edges);
		return;
	}
	// A child that does not care lets its parent decide.
	for (Widget *w = this->hot_; w; w = w->parent_) {
		if (const Qt::CursorShape shape = w->cursor();
			shape != Qt::ArrowCursor) {
			this->cursor_ = shape;
			return;
		}
	}
}

void
Kit::relayout_popups()
{
	ensure_scrim(*this);
	vector<Popup *> stale;
	for (Popup *p : this->popups_) {
		if (!p)
			continue;
		if (p->opener && !p->opener->shown()) {
			stale.push_back(p);
			continue;
		}
		if (p->parent_popup)
			p->place_sub(*this);
		else
			p->place(*this);
		if (auto *o = dynamic_cast<Overflow *>(p)) {
			bool has = false;
			for (const auto &item : o->col->kids) {
				Widget *k = item.get();
				if (k && k->shown() && !is_sep(k))
					has = true;
			}
			if (!has)
				stale.push_back(p);
		}
	}
	for (Popup *p : stale)
		p->close(*this);
	if (this->scrim_) {
		this->scrim_->visible = !this->popups_.empty();
		if (this->scrim_->visible)
			this->scrim_->r = {0.0f, 0.0f, this->host_w_, this->host_h_};
	}
}

void
Kit::prepare_popups()
{
	if (this->scrim_ && this->scrim_->visible)
		this->scrim_->prepare(*this);
	for (Popup *p : this->popups_) {
		if (p)
			p->prepare(*this);
	}
}

void
Kit::paint()
{
	const float white_u =
		(float(this->white_.x) + 0.5f) / float(max(this->atlas_.w, 1));
	const float white_v =
		(float(this->white_.y) + 0.5f) / float(max(this->atlas_.h, 1));
	this->list_.begin(
		this->host_w_, this->host_h_, this->dpr_, white_u, white_v);
	if (this->csd_shadow_) {
		const Rect f = frame();
		if (f.w > 0.0f && f.h > 0.0f)
			draw_glow(
				f.x, f.y, f.w, f.h, {0, 0, 0, this->active_ ? 0.25f : 0.125f});
	}
	if (this->root_)
		this->root_->paint(*this);
	if (this->scrim_ && this->scrim_->visible)
		this->scrim_->paint(*this);
	for (Popup *p : this->popups_) {
		if (p && p->shown())
			p->paint(*this);
	}
	paint_tooltip(*this);
	this->list_.end();
	if (this->renderer_ && take_atlas_dirty()) {
		unsigned char *pixels = nullptr;
		int width = 0, height = 0;
		if (font_pixels(&pixels, &width, &height))
			this->renderer_->upload_font(pixels, width, height);
	}
}

}  // namespace dn
