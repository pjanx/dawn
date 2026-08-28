//
// kit-cie-diagram.cpp: CIE 1931 xy sidebar widget
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "kit-cie-diagram.hpp"

#include "cmf-cie1931-2deg-1nm.h"

#include <QColor>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QString>

#include <algorithm>
#include <cmath>

using namespace std;

namespace dn
{
namespace
{

constexpr float kXMax = 0.8f;
constexpr float kYMax = 0.9f;
// XXX: I guess we hardcode it and I guess we shouldn't.
constexpr float kD65x = 0.3127f;
constexpr float kD65y = 0.3290f;
constexpr int kRasterW = 256;
constexpr int kRasterH = 288;
constexpr float kCapGap = 4.0f;
constexpr Colour kMidGreyCol{188 / 255.0f, 188 / 255.0f, 188 / 255.0f, 1.0f};
constexpr Colour kBlackCol{0.0f, 0.0f, 0.0f, 1.0f};
constexpr Colour kWhiteCol{1.0f, 1.0f, 1.0f, 1.0f};

const QString kSourceLab = QStringLiteral("Source");
const QString kTargetLab = QStringLiteral("Target");

float
caption_h(const Kit &kit)
{
	return kCapGap + kit.text_height(kSourceLab, 0.0f, false) + 4.;
}

Rect
plot_rect(Rect r, float dpr)
{
	if (r.w <= 0.0f || r.h <= 0.0f || dpr <= 0.0f)
		return {};
	const float aspect = kXMax / kYMax;
	const float nw = float(kRasterW) / dpr;
	const float nh = float(kRasterH) / dpr;
	float w = min(r.w, nw);
	float h = w / aspect;
	if (h > r.h || h > nh) {
		h = min(r.h, nh);
		w = h * aspect;
	}
	return {r.x + (r.w - w) * 0.5f, r.y, w, h};
}

QPointF
xy_to_px(double x, double y, int w, int h)
{
	return {x / double(kXMax) * w, (1.0 - y / double(kYMax)) * h};
}

QPainterPath
locus_path(int w, int h)
{
	QPainterPath path;
	if (kCmfN <= 0)
		return path;
	const auto xy = [](int i) {
		const double s = kCmf[i][0] + kCmf[i][1] + kCmf[i][2];
		if (s <= 0.0)
			return QPointF{};
		return QPointF{kCmf[i][0] / s, kCmf[i][1] / s};
	};
	QPointF p0 = xy(0);
	path.moveTo(xy_to_px(p0.x(), p0.y(), w, h));
	for (int i = 1; i < kCmfN; ++i) {
		QPointF p = xy(i);
		path.lineTo(xy_to_px(p.x(), p.y(), w, h));
	}
	path.closeSubpath();
	return path;
}

int
srgb_encode8(double u)
{
	if (u <= 0.0)
		return 0;
	if (u >= 1.0)
		return 255;
	if (u <= 0.0031308)
		return int(12.92 * u * 255.0 + 0.5);
	return int((1.055 * pow(u, 1.0 / 2.4) - 0.055) * 255.0 + 0.5);
}

QRgb
xy_srgb(double x, double y)
{
	if (y < 1e-8)
		return qRgba(0, 0, 0, 255);
	double rl =
		3.2404542 * (x / y) - 1.5371385 - 0.4985314 * ((1.0 - x - y) / y);
	double gl =
		-0.9692660 * (x / y) + 1.8760108 + 0.0415560 * ((1.0 - x - y) / y);
	double bl =
		0.0556434 * (x / y) - 0.2040259 + 1.0572252 * ((1.0 - x - y) / y);
	if (rl < 0.0)
		rl = 0.0;
	if (gl < 0.0)
		gl = 0.0;
	if (bl < 0.0)
		bl = 0.0;
	double m = rl;
	if (gl > m)
		m = gl;
	if (bl > m)
		m = bl;
	if (m > 0.0) {
		rl /= m;
		gl /= m;
		bl /= m;
	}
	return qRgba(srgb_encode8(rl), srgb_encode8(gl), srgb_encode8(bl), 255);
}

void
stroke_poly(
	QPainter &p, const QPen &pen, const dawn::Chromaticities &c, int w, int h)
{
	if (!c.have_primaries || c.n < 2)
		return;
	QPainterPath path;
	path.moveTo(xy_to_px(c.x[0], c.y[0], w, h));
	for (int i = 1; i < c.n; ++i)
		path.lineTo(xy_to_px(c.x[i], c.y[i], w, h));
	path.closeSubpath();
	p.strokePath(path, pen);
}

QImage
raster_diagram(int w, int h, const dawn::Chromaticities &image,
	const dawn::Chromaticities &screen, bool show_screen, bool screen_dashed,
	bool image_dashed)
{
	QImage img(w, h, QImage::Format_ARGB32_Premultiplied);
	img.fill(Qt::transparent);
	if (w <= 0 || h <= 0)
		return img;

	QImage mask(w, h, QImage::Format_ARGB32_Premultiplied);
	mask.fill(Qt::transparent);
	{
		QPainter mp(&mask);
		mp.setRenderHint(QPainter::Antialiasing);
		mp.fillPath(locus_path(w, h), Qt::white);
	}

	for (int py = 0; py < h; ++py) {
		auto *dst = (QRgb *) img.scanLine(py);
		const auto *ms = (const QRgb *) mask.constScanLine(py);
		const double y = double(kYMax) * (1.0 - (py + 0.5) / h);
		for (int px = 0; px < w; ++px) {
			if (!qAlpha(ms[px]))
				continue;
			const double x = double(kXMax) * (px + 0.5) / w;
			dst[px] = xy_srgb(x, y);
		}
	}

	QPainter p(&img);
	p.setRenderHint(QPainter::Antialiasing);
	p.setCompositionMode(QPainter::CompositionMode_DestinationIn);
	p.drawImage(0, 0, mask);
	p.setCompositionMode(QPainter::CompositionMode_SourceOver);

	p.strokePath(locus_path(w, h),
		QPen(QColor(0, 0, 0, 180), 1.25, Qt::SolidLine, Qt::RoundCap,
			Qt::RoundJoin));
	if (show_screen) {
		QPen pen(QColor(255, 255, 255), 1.8,
			screen_dashed ? Qt::CustomDashLine : Qt::SolidLine, Qt::RoundCap,
			Qt::RoundJoin);
		if (screen_dashed) {
			pen.setDashPattern({4, 4});
			pen.setDashOffset(4);
		}
		stroke_poly(p, pen, screen, w, h);
	}
	{
		QPen pen(QColor(0, 0, 0), 1.8,
			image_dashed ? Qt::CustomDashLine : Qt::SolidLine, Qt::RoundCap,
			Qt::RoundJoin);
		if (image_dashed)
			pen.setDashPattern({4, 4});
		stroke_poly(p, pen, image, w, h);
	}

	const QPointF wp = xy_to_px(kD65x, kD65y, w, h);
	p.setPen(QPen(QColor(0, 0, 0), 1.4, Qt::SolidLine, Qt::RoundCap));
	p.drawLine(wp + QPointF(-7, 0), wp + QPointF(7, 0));
	p.drawLine(wp + QPointF(0, -7), wp + QPointF(0, 7));
	return img;
}

bool
same_chroma(const dawn::Chromaticities &a, const dawn::Chromaticities &b)
{
	if (a.model != b.model || a.have_white != b.have_white ||
		a.have_primaries != b.have_primaries || a.n != b.n)
		return false;
	if (a.have_white && (a.wx != b.wx || a.wy != b.wy))
		return false;
	for (int i = 0; i < a.n; ++i) {
		if (a.x[i] != b.x[i] || a.y[i] != b.y[i])
			return false;
	}
	return true;
}

}  // namespace

void
CieDiagram::measure(Kit &kit, float max_w, float max_h)
{
	const float cap = caption_h(kit);
	const float plot_h = max(0.0f, max_h - cap);
	const Rect fit = plot_rect({0, 0, max_w, plot_h}, kit.dpr_);
	const float labs = kit.text_width(kSourceLab, false) + 8. +
		kit.text_width(kTargetLab, false);
	this->r.w = max(fit.w, labs);
	this->r.h = fit.h + cap;
}

void
CieDiagram::arrange(Kit &kit, Rect alloc)
{
	this->r = kit.snap_rect(alloc);
}

void
CieDiagram::prepare(Kit &kit)
{
	kit.cache_text(kSourceLab, false);
	kit.cache_text(kTargetLab, false);

	const float cap = caption_h(kit);
	const Rect plot =
		plot_rect({this->r.x, this->r.y, this->r.w, max(0.0f, this->r.h - cap)},
			kit.dpr_);
	if (plot.w < 8.0f || plot.h < 8.0f)
		return;

	const bool epoch_ok =
		this->epoch_ == kit.atlas_epoch_ && !this->slot_.empty();
	const bool chroma_ok = this->packed_show_screen_ == this->show_screen &&
		this->packed_screen_dashed_ == this->screen_dashed &&
		this->packed_image_dashed_ == this->image_dashed &&
		same_chroma(this->packed_image_, this->image) &&
		same_chroma(this->packed_screen_, this->screen);
	if (epoch_ok && chroma_ok)
		return;

	if (epoch_ok)
		kit.atlas_.release(this->slot_);
	this->slot_ = kit.pack_bitmap(
		raster_diagram(kRasterW, kRasterH, this->image, this->screen,
			this->show_screen, this->screen_dashed, this->image_dashed));
	if (this->slot_.empty())
		return;
	this->epoch_ = kit.atlas_epoch_;
	this->packed_image_ = this->image;
	this->packed_screen_ = this->screen;
	this->packed_show_screen_ = this->show_screen;
	this->packed_screen_dashed_ = this->screen_dashed;
	this->packed_image_dashed_ = this->image_dashed;
}

void
CieDiagram::paint(Kit &kit) const
{
	const float cap = caption_h(kit);
	const Rect plot =
		plot_rect({this->r.x, this->r.y, this->r.w, max(0.0f, this->r.h - cap)},
			kit.dpr_);
	const float x0 = plot.x > 0.0f ? plot.x : this->r.x;
	const float cap_y0 = plot.h >= 8.0f ? plot.y + plot.h : this->r.y;
	const float y = cap_y0 + kCapGap;
	const float th = kit.text_height(kSourceLab, 0.0f, true);
	const float cap_w = plot.w >= 8.0f ? plot.w : this->r.w;
	kit.list_.add_rect_filled(x0, y, x0 + cap_w, y + th, kMidGreyCol);
	if (plot.w >= 8.0f && plot.h >= 8.0f && !this->slot_.empty()) {
		float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
		kit.atlas_.uv(this->slot_, &u0, &v0, &u1, &v1);
		kit.list_.add_image(plot.x, plot.y, plot.x + plot.w, plot.y + plot.h,
			u0, v0, u1, v1, {1, 1, 1, 1});
	}

	const float cx = x0 + cap_w / 2;
	const float widthS = kit.text_width(kSourceLab, true);
	kit.emit_text(cx - 4. - widthS, y, kSourceLab, kBlackCol, true);
	kit.emit_text(cx + 4., y, kTargetLab, kWhiteCol, true);
}

}  // namespace dn
