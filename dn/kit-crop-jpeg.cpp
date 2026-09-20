//
// kit-crop-jpeg.cpp: lossless JPEG cropper subapplication
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "kit-crop-jpeg.hpp"
#include "kit-files.hpp"
#include "renderer.hpp"
#include "url.hpp"

#include <libdn/gettext.hpp>

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>

#include <algorithm>
#include <cmath>

using namespace std;

namespace dn
{

// --- Loading -----------------------------------------------------------------

Cropper::Cropper(Kit &kit) : kit_(kit)
{
	this->hittable = true;
}

bool
Cropper::load_working(const vector<uint8_t> &data)
{
	dawn::Error error;
	dawn::JpegGrid grid;
	dawn::ImagePtr image;
	if (dawn::jpeg_grid(data, &grid, &error)) {
		dawn::OpenContext ctx;
		ctx.uri = this->jpeg_url_.toEncoded().toStdString();
		ctx.cmm = this->cmm_;
		ctx.screen_profile = this->screen_profile_;
		ctx.first_frame_only = true;
		image = dawn::open_from_data(data, ctx, &error);
	}
	this->message_ = error.message;
	this->message_dismissed_ = false;
	if (!image)
		return false;

	this->image_ = std::move(image);
	this->grid_ = grid;
	this->exif_ = dawn::orientation_or_0(this->image_->orientation);
	if (this->kit_.renderer_) {
		const auto &im = *this->image_;
		this->kit_.renderer_->set_image(
			im.width, im.height, im.data.data(), im.stride);
	}
	return true;
}

void
Cropper::open(const QUrl &url)
{
	this->jpeg_url_ = url;
	this->file_.clear();
	this->image_.reset();
	this->grid_ = {};
	this->message_.clear();
	this->message_dismissed_ = false;
	this->drag_ = Drag::None;
	this->zoom_ = 1;
	this->pan_x_ = this->pan_y_ = 0;
	if (this->kit_.renderer_)
		this->kit_.renderer_->clear_image();
	if (!url.isEmpty()) {
		dawn::Error error;
		if (!dawn::read_file(
				url_to_path(url).toStdString(), &this->file_, &error))
			this->message_ = error.message;
		else if (!load_working(this->file_))
			this->file_.clear();
	}
	reset_region();
}

void
Cropper::screen_changed(
	const ScreenState &state, bool changed, bool force_reload)
{
	this->cmm_ = state.cmm;
	this->screen_profile_ = state.profile;
	if (!this->file_.empty() && (changed || force_reload))
		load_working(this->file_);
}

// --- Geometry and input ------------------------------------------------------

Size
Cropper::measure_content(Kit &, int max_w, int max_h)
{
	return {max_w, max_h};
}

void
Cropper::arrange_content(Kit &, Rect alloc)
{
	this->r = alloc;
}

void
Cropper::origin(double *x, double *y) const
{
	uint32_t w, h;
	dawn::orientation_display_size(
		this->grid_.width, this->grid_.height, this->exif_, &w, &h);
	*x = round(
		this->r.x + this->r.w * .5 - (w * .5 + this->pan_x_) * this->zoom_);
	*y = round(
		this->r.y + this->r.h * .5 - (h * .5 + this->pan_y_) * this->zoom_);
}

void
Cropper::zoom_at(int zoom, float x, float y)
{
	zoom = clamp(zoom, 1, 16);
	const float delta = 1.f / float(this->zoom_) - 1.f / float(zoom);
	this->pan_x_ += (x - float(this->r.x) - float(this->r.w) * .5f) * delta;
	this->pan_y_ += (y - float(this->r.y) - float(this->r.h) * .5f) * delta;
	this->zoom_ = zoom;
}

void
Cropper::reset_region()
{
	this->left_ = this->top_ = 0;
	this->right_ = this->grid_.width;
	this->bottom_ = this->grid_.height;
}

Qt::CursorShape
Cropper::cursor() const
{
	if (!this->image_)
		return Qt::ArrowCursor;
	if (this->drag_ == Drag::Pan)
		return Qt::ClosedHandCursor;
	return Qt::CrossCursor;
}

bool
Cropper::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (!this->image_)
		return false;

	switch (button) {
	case Qt::LeftButton:
		this->drag_ = Drag::Origin;
		break;
	case Qt::RightButton:
		this->drag_ = Drag::Corner;
		break;
	case Qt::MiddleButton:
		this->drag_ = Drag::Pan;
		break;
	default:
		return false;
	}
	kit.pressed_ = this;
	this->drag_x_ = x;
	this->drag_y_ = y;
	return motion(kit, x, y);
}

bool
Cropper::release(Kit &, float, float, Qt::MouseButton button)
{
	if ((button == Qt::LeftButton && this->drag_ == Drag::Origin) ||
		(button == Qt::RightButton && this->drag_ == Drag::Corner) ||
		(button == Qt::MiddleButton && this->drag_ == Drag::Pan)) {
		this->drag_ = Drag::None;
		return true;
	}
	return false;
}

bool
Cropper::motion(Kit &kit, float x, float y)
{
	if (!this->image_ || this->drag_ == Drag::None)
		return false;

	if (this->drag_ == Drag::Pan) {
		pan(kit, x, y, float(x - this->drag_x_), float(y - this->drag_y_));
	} else {
		double ox, oy, sx, sy;
		origin(&ox, &oy);

		// Use pixel centres so that a mirrored edge still selects the pixel
		// under it.
		dawn::orientation_map_display_to_source(this->exif_, this->grid_.width,
			this->grid_.height, floor((x - ox) / this->zoom_) + .5,
			floor((y - oy) / this->zoom_) + .5, &sx, &sy);

		// Clamp before conversion, and leave at least one pixel at either edge.
		uint32_t px = uint32_t(clamp(sx, 0., double(this->grid_.width - 1)));
		uint32_t py = uint32_t(clamp(sy, 0., double(this->grid_.height - 1)));
		if (this->drag_ == Drag::Origin) {
			this->left_ = min(px, this->right_ - 1) / this->grid_.mcu_width *
				this->grid_.mcu_width;
			this->top_ = min(py, this->bottom_ - 1) / this->grid_.mcu_height *
				this->grid_.mcu_height;
		} else {
			this->right_ = clamp(px + 1, this->left_ + 1, this->grid_.width);
			this->bottom_ = clamp(py + 1, this->top_ + 1, this->grid_.height);
		}
	}
	this->drag_x_ = x;
	this->drag_y_ = y;
	return true;
}

bool
Cropper::scroll(Kit &, float x, float y, int delta)
{
	if (!this->image_ || !delta)
		return false;

	zoom_at(this->zoom_ + (delta > 0 ? 1 : -1), x, y);
	return true;
}

bool
Cropper::pan(Kit &, float, float, float dx, float dy)
{
	if (!this->image_)
		return false;

	this->pan_x_ -= dx / float(this->zoom_);
	this->pan_y_ -= dy / float(this->zoom_);
	return true;
}

bool
Cropper::key(Kit &, const Key &ev)
{
	if (!this->image_ || ev.mods || ev.key < Qt::Key_1 || ev.key > Qt::Key_9)
		return false;

	zoom_at(ev.key - Qt::Key_0, float(this->r.x) + float(this->r.w) * .5f,
		float(this->r.y) + float(this->r.h) * .5f);
	return true;
}

// --- Turns -------------------------------------------------------------------

static dawn::Orientation
stored_op(dawn::Orientation exif, Action action)
{
	const bool mirrors = exif == dawn::Orientation::Mirror0 ||
		exif == dawn::Orientation::Mirror180 ||
		exif == dawn::Orientation::Mirror270 ||
		exif == dawn::Orientation::Mirror90;
	if (action == Action::RotateLeft)
		return mirrors ? dawn::Orientation::Rotate90
					   : dawn::Orientation::Rotate270;
	if (action == Action::RotateRight)
		return mirrors ? dawn::Orientation::Rotate270
					   : dawn::Orientation::Rotate90;
	return int(exif) >= 5 ? dawn::Orientation::Mirror180
						  : dawn::Orientation::Mirror0;
}

void
Cropper::turn(Action action)
{
	// TODO(p): Not sure if it's necessary to change the /data/ every time we
	// rotate the image.
	const auto op = stored_op(this->exif_, action);
	dawn::Error error;
	auto data = dawn::jpeg_transform(this->file_, op, 0, 0, 0, 0, &error);
	if (data.empty()) {
		this->message_ = error.message;
		this->message_dismissed_ = false;
		return;
	}
	if (!load_working(data))
		return;

	uint32_t w, h;
	dawn::orientation_display_size(
		this->grid_.width, this->grid_.height, op, &w, &h);

	// Map the selection through the same perfect turn as the stored image.
	double x0, y0, x1, y1;
	dawn::orientation_map_source_to_display(
		op, w, h, this->left_, this->top_, &x0, &y0);
	dawn::orientation_map_source_to_display(
		op, w, h, this->right_, this->bottom_, &x1, &y1);

	this->file_ = std::move(data);
	this->right_ = uint32_t(clamp(max(x0, x1), 1., double(this->grid_.width)));
	this->bottom_ =
		uint32_t(clamp(max(y0, y1), 1., double(this->grid_.height)));
	this->left_ = uint32_t(clamp(min(x0, x1), 0., double(this->right_ - 1))) /
		this->grid_.mcu_width * this->grid_.mcu_width;
	this->top_ = uint32_t(clamp(min(y0, y1), 0., double(this->bottom_ - 1))) /
		this->grid_.mcu_height * this->grid_.mcu_height;
	this->pan_x_ = this->pan_y_ = 0;
}

// --- Saving and actions ------------------------------------------------------

constexpr FileType kJpegTypes[] = {
	{N_("JPEG image (*.jpg, *.jpeg)"), "*.jpg;*.jpeg;*.jpe;*.jfif", ".jpg"},
	{N_("All files"), "*", nullptr},
};

// Overwriting is the dialog's question to ask, not this one's.
QString
Cropper::save(const QString &input)
{
	const QString path =
		url_to_path(url_from_user_input(input, QDir::currentPath()));
	if (path.isEmpty())
		return QString::fromUtf8(_("Not a local path"));

	dawn::Error error;
	auto data = dawn::jpeg_transform(this->file_, dawn::Orientation::Rotate0,
		this->left_, this->top_, this->right_ - this->left_,
		this->bottom_ - this->top_, &error);
	if (data.empty())
		return QString::fromStdString(error.message);

	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly) ||
		file.write(reinterpret_cast<const char *>(data.data()),
			qint64(data.size())) != qint64(data.size()) ||
		!file.commit())
		return file.errorString();
	return {};
}

bool
Cropper::enabled(Action action) const
{
	switch (action) {
	case Action::SaveAs:
		return this->image_ && this->left_ < this->right_ &&
			this->top_ < this->bottom_;
	case Action::ZoomOut:
		return this->image_ && this->zoom_ > 1;
	case Action::ZoomIn:
		return this->image_ && this->zoom_ < 16;
	case Action::RotateLeft:
	case Action::Mirror:
	case Action::RotateRight: {
		if (!this->image_)
			return false;

		const auto op = stored_op(this->exif_, action);
		// These four stored operations are all the three screen verbs need.
		if (op == dawn::Orientation::Rotate90 ||
			op == dawn::Orientation::Mirror180)
			return this->grid_.height % this->grid_.mcu_height == 0;
		return this->grid_.width % this->grid_.mcu_width == 0;
	}
	case Action::CropReset:
	case Action::CropRegion:
	case Action::ZoomLevel:
	case Action::Zoom1:
	case Action::Reload:
		return bool(this->image_);
	default:
		return true;
	}
}

bool
Cropper::apply(Action action)
{
	if (!enabled(action))
		return true;

	switch (action) {
	case Action::Reload:
		open(this->jpeg_url_);
		break;
	case Action::CropReset:
		reset_region();
		break;
	case Action::RotateLeft:
	case Action::Mirror:
	case Action::RotateRight:
		turn(action);
		break;
	case Action::Zoom1:
	case Action::ZoomIn:
	case Action::ZoomOut:
		zoom_at(action == Action::Zoom1
				? 1
				: this->zoom_ + (action == Action::ZoomIn ? 1 : -1),
			float(this->r.x) + float(this->r.w) * .5f,
			float(this->r.y) + float(this->r.h) * .5f);
		break;
	case Action::Open: {
		FileDialogSetup chooser;
		chooser.directory =
			QFileInfo(url_to_path(this->jpeg_url_)).absolutePath();
		chooser.types = kJpegTypes;
		chooser.on_accept = [this](Kit &, const QString &path, int) {
			open(path_to_url(path));
			return QString();
		};
		dialog_files(this->kit_, std::move(chooser));
		break;
	}
	case Action::SaveAs: {
		const QFileInfo source(url_to_path(this->jpeg_url_));
		FileDialogSetup chooser;
		chooser.save = true;
		chooser.directory = source.absolutePath();
		// TRANSLATORS: Inserted into a filename when saving a cropped JPEG,
		// between the base name and the extension, dots included.
		chooser.name = source.completeBaseName() +
			QString::fromUtf8(_(".crop.")) + source.suffix();
		chooser.types = kJpegTypes;
		chooser.on_accept = [this](Kit &, const QString &path, int) {
			return save(path);
		};
		dialog_files(this->kit_, std::move(chooser));
		break;
	}
	default:
		return false;
	}
	return true;
}

// --- Painting ----------------------------------------------------------------

void
Cropper::paint(Kit &kit) const
{
	kit.clip_to(this->r);
	if (!this->image_) {
		Label hint;
		hint.text = QString::fromUtf8(_("Open a JPEG file")) +
			QStringLiteral(" — ") + action_accel(action_def(Action::Open));
		hint.dim = true;
		hint.align = Align::Center;
		hint.r = this->r;
		hint.paint(kit);
		kit.clip_pop();
		return;
	}

	double ox, oy;
	origin(&ox, &oy);
	if (kit.renderer_) {
		uint32_t w, h;
		dawn::orientation_display_size(
			this->grid_.width, this->grid_.height, this->exif_, &w, &h);

		auto &renderer = *kit.renderer_;
		auto vp = renderer.extent();
		renderer.set_filter(false);
		renderer.set_checkerboard(false);
		renderer.set_view(float(this->zoom_),
			float((vp.width * .5 - ox) / this->zoom_ - w * .5),
			float((vp.height * .5 - oy) / this->zoom_ - h * .5), this->exif_,
			0);
	}

	double x0, y0, x1, y1;
	dawn::orientation_map_source_to_display(this->exif_, this->grid_.width,
		this->grid_.height, this->left_, this->top_, &x0, &y0);
	dawn::orientation_map_source_to_display(this->exif_, this->grid_.width,
		this->grid_.height, this->right_, this->bottom_, &x1, &y1);

	Box box{int(ox + min(x0, x1) * this->zoom_),
		int(oy + min(y0, y1) * this->zoom_),
		int(ox + max(x0, x1) * this->zoom_),
		int(oy + max(y0, y1) * this->zoom_)};
	const int l = this->r.x, t = this->r.y;
	const int r = l + this->r.w, b = t + this->r.h;
	const int cl = clamp(box.x0, l, r), cr = clamp(box.x1, l, r);
	const int ct = clamp(box.y0, t, b), cb = clamp(box.y1, t, b);
	for (Box shade : {Box{l, t, r, ct}, Box{l, cb, r, b}, Box{l, ct, cl, cb},
			 Box{cr, ct, r, cb}})
		kit.list_.add_rect_filled(shade, {0, 0, 0, .5f});
	kit.list_.add_rect_stroke(
		{box.x0 - 1, box.y0 - 1, box.x1 + 1, box.y1 + 1}, {0, 0, 0, 1}, 1);
	kit.list_.add_rect_stroke(box, {1, 1, 1, 1}, 1);
	kit.clip_pop();
}

void
Cropper::update(Kit &kit)
{
	this->error_->set_visible(
		!this->message_.empty() && !this->message_dismissed_);
	this->error_label_->set_text(QString::fromStdString(this->message_));

	this->scale_label_->set_text(
		QString::number(this->zoom_ * 100) + QLatin1Char('%'));

	this->region_label_->set_text(QStringLiteral("(%1, %2) × (%3, %4)")
			.arg(this->left_)
			.arg(this->top_)
			.arg(this->right_ - this->left_)
			.arg(this->bottom_ - this->top_));

	int digit_width = 0;
	for (int i = 0; i < 10; i++)
		digit_width =
			max(digit_width, kit.text_width(QString::number(i), false));

	// Reserve the widest digit for every position, independent of the
	// selection.
	const int digits = int(max(QString::number(this->grid_.width).size(),
		QString::number(this->grid_.height).size()));
	const float width = kit.pts(4 * digits * digit_width +
		kit.text_width(QStringLiteral("(, ) × (, )"), false));
	if (this->region_label_->min_w != width) {
		this->region_label_->min_w = width;
		this->region_label_->invalidate_measure();
	}
	this->scale_label_->min_w =
		kit.pts(kit.text_width(QStringLiteral("1600%"), false));
}

constexpr ToolbarSpec kItems[] = {
	{Slot::Left, Action::Reload},
	{Slot::Left, Action::SaveAs},
	{Slot::Left, Action::None},

	{Slot::Middle, Action::ZoomOut},
	{Slot::Middle, Action::ZoomLevel},
	{Slot::Middle, Action::ZoomIn},
	{Slot::Middle, Action::Zoom1},
	{Slot::Middle, Action::None},
	{Slot::Middle, Action::CropRegion},
	{Slot::Middle, Action::CropReset},
	{Slot::Middle, Action::None},
	{Slot::Middle, Action::RotateLeft},
	{Slot::Middle, Action::Mirror},
	{Slot::Middle, Action::RotateRight},

	{Slot::Right, Action::None},
	{Slot::Right, Action::DarkMode},
	{Slot::Right, Action::Fullscreen},
};

unique_ptr<Page>
make_crop_jpeg_page(Kit &kit, const HostActions &host, Cropper **out)
{
	auto content = make_unique<Cropper>(kit);
	Cropper *c = content.get();
	if (out)
		*out = c;

	PageSetup setup;
	setup.mode = Mode::CropJpeg;
	setup.content = std::move(content);
	setup.toolbar = make_toolbar(
		kItems, [c](const ToolbarSpec &spec) -> unique_ptr<Widget> {
			if (spec.action == Action::CropReset) {
				auto button = make_unique<Button>();
				button->flat = true;
				button->focus_on_press = false;
				button->action = spec.action;
				button->text = action_tip(action_def(spec.action), false);
				return button;
			}
			if (spec.action != Action::CropRegion &&
				spec.action != Action::ZoomLevel)
				return {};

			auto label = make_unique<Label>();
			label->hittable = true;
			label->align = Align::Center;
			label->tip_text = action_tip(action_def(spec.action), false);
			label->tip_accel = action_accel(action_def(spec.action));
			if (spec.action == Action::CropRegion)
				c->region_label_ = label.get();
			if (spec.action == Action::ZoomLevel)
				c->scale_label_ = label.get();
			return label;
		});
	setup.actor = chain_actor(
		host, [c](Action a) { return c->apply(a); },
		[c](Action a) { return c->enabled(a); },
		[&kit](Action a) {
			if (a == Action::DarkMode)
				return kit.dark_;
			if (a == Action::Fullscreen)
				return kit.fullscreen_;
			return false;
		});

	auto page = make_page(kit, host, std::move(setup));
	auto banner = make_banner(
		&c->error_label_, [c](Kit &) { c->message_dismissed_ = true; });
	c->error_ = banner.get();
	page->set_banner(kit, std::move(banner));
	return page;
}

}  // namespace dn
