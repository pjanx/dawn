//
// kit-viewer.cpp: image view (document, overlay chrome, view input)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "kit-viewer.hpp"

#include "action.hpp"
#include "kit-chrome.hpp"
#include "kit-cie-diagram.hpp"
#include "kit.hpp"
#include "renderer.hpp"
#include "url.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QKeyEvent>
#include <QMimeData>
#include <QUrl>
#include <QtLogging>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{

constexpr float kWinPadX = 4.f;
constexpr float kWinPadY = 2.f;
constexpr float kItemGap = 2.f;
constexpr float kZoomStep = 1.25f;
constexpr float kZoomDragPts = 40.f;
constexpr float kKeyboardPan = 50.f;
constexpr float kRotateMinR = 8.f;
constexpr float kScaleMin = 0.0125f;
constexpr float kScaleMax = 64.f;
constexpr float kAngleFast = 1e-5f;
constexpr const char *kMoreIcon = "disclose-arrow-down-symbolic";

// The loader below is a local-filesystem reader, and keys its jobs by path.
static string
viewer_local_path(const Viewer &v)
{
	return url_to_path(v.url_).toStdString();
}

enum class Slot : uint8_t { Left, Middle, Right };
enum class Kind : uint8_t { Icon, Scale, Sep };

namespace
{

struct Spec {
	Kind kind;
	Slot slot;
	Action action;
};

}  // namespace

constexpr Spec kItems[] = {
	{Kind::Icon, Slot::Left, Action::Browse},
	{Kind::Icon, Slot::Left, Action::PrevFile},
	{Kind::Icon, Slot::Left, Action::NextFile},
	{Kind::Icon, Slot::Left, Action::Reload},
	{Kind::Sep, Slot::Left, Action::None},

	{Kind::Icon, Slot::Middle, Action::PageFirst},
	{Kind::Icon, Slot::Middle, Action::PagePrevious},
	{Kind::Icon, Slot::Middle, Action::PageNext},
	{Kind::Icon, Slot::Middle, Action::PageLast},
	{Kind::Sep, Slot::Middle, Action::None},
	{Kind::Icon, Slot::Middle, Action::FrameFirst},
	{Kind::Icon, Slot::Middle, Action::FramePrevious},
	{Kind::Icon, Slot::Middle, Action::PlayPause},
	{Kind::Icon, Slot::Middle, Action::FrameNext},
	{Kind::Sep, Slot::Middle, Action::None},
	{Kind::Icon, Slot::Middle, Action::Lock},
	{Kind::Icon, Slot::Middle, Action::Fixate},
	{Kind::Icon, Slot::Middle, Action::ZoomOut},
	{Kind::Scale, Slot::Middle, Action::ZoomLevel},
	{Kind::Icon, Slot::Middle, Action::ZoomIn},
	{Kind::Icon, Slot::Middle, Action::Zoom1},
	{Kind::Icon, Slot::Middle, Action::Fit},
	{Kind::Sep, Slot::Middle, Action::None},
	{Kind::Icon, Slot::Middle, Action::ColorManagement},
	{Kind::Icon, Slot::Middle, Action::Smooth},
	{Kind::Icon, Slot::Middle, Action::Checkerboard},
	{Kind::Sep, Slot::Middle, Action::None},
#if 0
	{Kind::Icon, Slot::Middle, Action::None},
	{Kind::Icon, Slot::Middle, Action::None},
#endif
	{Kind::Icon, Slot::Middle, Action::Information},
	{Kind::Sep, Slot::Middle, Action::None},
	{Kind::Icon, Slot::Middle, Action::RotateLeft},
	{Kind::Icon, Slot::Middle, Action::Mirror},
	{Kind::Icon, Slot::Middle, Action::RotateRight},

	{Kind::Sep, Slot::Right, Action::None},
	{Kind::Icon, Slot::Right, Action::DarkMode},
	{Kind::Icon, Slot::Right, Action::Fullscreen},
};

static bool
spec_enabled(const Viewer &v, Action action)
{
	switch (action) {
	case Action::None:
		return false;
	case Action::PageFirst:
	case Action::PagePrevious:
		return v.current_ && v.current_ != v.image_;
	case Action::PageNext:
	case Action::PageLast:
		return v.current_ && v.current_->page_next;
	case Action::FrameFirst:
	case Action::FramePrevious:
	case Action::PlayPause:
	case Action::FrameNext:
		return v.current_ && v.current_->frame_next;
	case Action::Copy:
		return !v.url_.isEmpty() ||
			(v.frame_ && !v.frame_->data.empty() && v.frame_->width &&
				v.frame_->height);
	case Action::Reload:
		return !v.url_.isEmpty();
	case Action::Trash:
		return QFileInfo(url_to_path(v.url_)).isFile();
	case Action::Smooth:
		return !(v.current_ && v.current_->render);
	default:
		return true;
	}
}

static bool
spec_active(const Viewer &v, Action action)
{
	switch (action) {
	case Action::Fit:
		return v.scale_to_fit_;
	case Action::Checkerboard:
		return v.checkerboard_;
	case Action::BlendLinearLight:
		return v.blend_linear_light_;
	case Action::ColorManagement:
		return v.enable_cms_;
	case Action::Smooth:
		return v.filter_;
	case Action::Information:
		return v.page_ && v.page_->sidebar_open;
	case Action::Fixate:
		return v.fixate_;
	case Action::Lock:
		return v.view_locked_;
	case Action::PlayPause:
		return v.playing_;
	case Action::Fullscreen:
		return v.kit_.fullscreen_;
	case Action::DarkMode:
		return v.kit_.dark_;
	default:
		return false;
	}
}

static void
sync_scale_label(Viewer &v)
{
	char scale_buf[16];
	snprintf(scale_buf, sizeof(scale_buf), "%.f%%", double(v.scale_ * 100.f));
	v.scale_text_ = QString::fromUtf8(scale_buf);
	if (!v.scale_label_)
		return;

	const float scale_slot =
		max(v.kit_.text_width(QStringLiteral("100%"), false),
			v.kit_.text_width(v.scale_text_, false));
	v.scale_label_->min_w = scale_slot / v.kit_.dpr_;
	v.scale_label_->text = v.scale_text_;
}

static unique_ptr<Widget>
make_item(Viewer &v, const Spec &spec)
{
	if (spec.kind == Kind::Sep)
		return make_unique<Sep>();
	if (spec.kind == Kind::Scale) {
		auto n = make_unique<Label>();
		n->hittable = true;
		n->align = Align::Center;
		n->tip_text = action_tip(action_def(Action::ZoomLevel), false);
		n->tip_accel = action_accel(action_def(Action::ZoomLevel));
		v.scale_label_ = n.get();

		sync_scale_label(v);
		return n;
	}
	auto n = make_unique<Button>();
	n->flat = true;
	n->focus_on_press = false;
	const Action action = spec.action;
	const ActionDef &d = action_def(action);
	const bool on = spec_active(v, action);
	n->action = action;
	n->icon = action_icon(d, on);
	n->enabled_ = spec_enabled(v, action);
	n->active = on;
	n->tip_text = action_tip(d, on);
	n->tip_accel = action_accel(d);
	n->on_click = [&v, action](Kit &) {
		if (v.page_ && v.page_->actor.apply)
			v.page_->actor.apply(action);
	};
	return n;
}

static unique_ptr<ToolbarSlot>
make_slot_row(Viewer &v, Slot slot)
{
	auto row = make_unique<ToolbarSlot>();
	row->gap = kItemGap;
	for (const Spec &spec : kItems) {
		if (spec.slot == slot)
			row->add_item(make_item(v, spec));
	}
	return row;
}

static unique_ptr<Row>
meta_row(
	const QString &lab, const QString &value, float label_w, Label *&value_out)
{
	auto row = make_unique<Row>();
	row->gap = 4.f;
	auto k = make_unique<Label>();
	k->min_w = label_w;
	k->text = lab;
	k->bold = true;
	auto v = make_unique<Label>();
	v->grow = true;
	v->text = value;
	v->wrap = true;
	value_out = v.get();
	row->add_child(std::move(k));
	row->add_child(std::move(v));
	return row;
}

static QString
dim_text(uint32_t v)
{
	if (!v)
		return QStringLiteral("-");
	char buf[16];
	snprintf(buf, sizeof(buf), "%u", v);
	return QString::fromUtf8(buf);
}

// FIXME: This is fucking stupid.
constexpr int kInfoFixedKids = 6 + DAWN_WITH_JPEG_QS;

static void
fill_info_texts(Viewer &v, const dawn::Image *im)
{
	if (!v.info_ || v.info_text_src_ == im)
		return;

	auto &list = *v.info_;
	v.info_text_src_ = im;
	list.scroll_.offset = 0;
	if (int(list.kids.size()) > kInfoFixedKids)
		list.erase_children(size_t(kInfoFixedKids));
	if (!im || im->text.empty())
		return;

	vector<pair<string, string>> rows(im->text.begin(), im->text.end());
	sort(rows.begin(), rows.end());
	for (const auto &kv : rows) {
		auto key = make_unique<Label>();
		key->bold = true;
		key->wrap = true;
		key->text = QString::fromUtf8(kv.first.data(), int(kv.first.size()));
		auto val = make_unique<Label>();
		val->wrap = true;
		val->text = QString::fromUtf8(kv.second.data(), int(kv.second.size()));
		list.add_child(std::move(key));
		list.add_child(std::move(val));
	}
}

namespace
{

struct OpenJob {
	uint64_t epoch = 0;
	Viewer::OpenKey key;
	string uri;
	shared_ptr<const vector<uint8_t>> screen_icc;
	shared_ptr<const vector<string>> loaders;
	int dpi = 96;
	bool enable_cms = true;
};

struct OpenLoad {
	uint64_t epoch = 0;
	Viewer::OpenKey key;
	dawn::ImagePtr image;
	string message;
};

struct ScaleJob {
	uint64_t gen = 0;
	dawn::ImagePtr page;
	float scale = 1.f;
	shared_ptr<const vector<uint8_t>> screen_icc;
	bool enable_cms = true;
};

}  // namespace

static string
join_load_text(const vector<string> &warnings, const dawn::Error &error,
	bool no_image, bool empty_image)
{
	vector<string> parts = warnings;
	if (error) {
		if (!error.message.empty())
			parts.push_back(error.message);
		else
			parts.push_back("cannot load");
	}
	if (no_image && parts.empty())
		parts.push_back("cannot load");
	else if (empty_image && parts.empty())
		parts.push_back("empty image");
	string out;
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i)
			out += '\n';
		out += parts[i];
	}
	return out;
}

static shared_ptr<dawn::Profile>
profile_from_icc(
	dawn::Cmm &cmm, const shared_ptr<const vector<uint8_t>> &icc)
{
	if (icc && !icc->empty()) {
		if (auto profile = cmm.get_profile(*icc))
			return profile;
	}
	return cmm.get_profile_sRGB();
}

static bool apply_action(Viewer &v, Action action);
static void reload_open(Viewer &v);

struct Viewer::Worker {
	struct ActiveOpen {
		uint64_t epoch = 0;
		Viewer::OpenKey key;

		bool operator==(const ActiveOpen &) const = default;
	};
	mutex mu;
	condition_variable cv;
	bool stop = false;
	uint64_t epoch = 1;
	array<string, 3> desired;
	bool current_ready = false;
	bool detached = false;
	optional<OpenJob> pending_open;
	optional<ScaleJob> pending_scale;
	vector<OpenJob> pending_preloads;
	optional<ActiveOpen> active_open;
	vector<ActiveOpen> active_preloads;
	thread foreground;
	array<thread, 2> preloads;
};

static void
pack_toolbar_icons(Viewer &v)
{
	const int px = v.kit_.icon_px();
	for (const Spec &spec : kItems) {
		const ActionDef &d = action_def(spec.action);
		v.kit_.pack_icon(action_icon(d, false), px);
		if (d.icon[1])
			v.kit_.pack_icon(d.icon[1], px);
	}
	v.kit_.pack_icon(kMoreIcon, px);
	v.kit_.pack_icon("open-menu-symbolic", px);
	v.kit_.pack_icon("x-symbolic", px);
}

static bool
set_dpr(Viewer &v, float dpr)
{
	if (!v.kit_.set_dpr(dpr))
		return false;
	pack_toolbar_icons(v);
	return true;
}

static void
set_message(Viewer &v, string text)
{
	v.message_ = std::move(text);
	v.message_dismissed_ = false;
}

static void
request_render(Viewer &v)
{
	if (v.kit_.request_render)
		v.kit_.request_render();
}

static void
post_gui(const Viewer &v, function<void()> fn)
{
	if (v.kit_.post)
		v.kit_.post(std::move(fn));
}

static void
upload_frame(const Viewer &v, const dawn::Image &image)
{
	if (!v.kit_.renderer_)
		return;
	v.kit_.renderer_->set_image(
		image.width, image.height, image.data.data(), image.stride);
}

constexpr int64_t kBrowserDelayFloorMs = 11;
constexpr int64_t kBrowserDelayBumpMs = 100;

static int64_t
display_delay_ms(int64_t duration)
{
	if (duration < 0)
		return duration;
	if (duration < kBrowserDelayFloorMs)
		return kBrowserDelayBumpMs;
	return duration;
}

static void
set_frame(Viewer &v, dawn::ImagePtr frame)
{
	if (!frame || frame.get() == v.frame_.get())
		return;
	v.frame_ = std::move(frame);
	v.image_width_ = v.frame_->width;
	v.image_height_ = v.frame_->height;
	if (v.kit_.renderer_)
		upload_frame(v, *v.frame_);
}

static void
stop_playback(Viewer &v)
{
	v.playing_ = false;
}

static bool
advance_frame(Viewer &v)
{
	if (!v.frame_)
		return false;
	if (v.frame_->frame_next) {
		set_frame(v, v.frame_->frame_next);
		return true;
	}
	if (v.remaining_loops_ && !--v.remaining_loops_)
		return false;
	set_frame(v, v.current_);
	return true;
}

static void
start_playback(Viewer &v)
{
	stop_playback(v);
	if (!v.image_ || !v.current_ || !v.current_->frame_next)
		return;
	v.frame_at_ = chrono::steady_clock::now();
	if (!v.remaining_loops_) {
		v.remaining_loops_ = v.current_->loops;
		if (v.remaining_loops_ && v.frame_ && !v.frame_->frame_next)
			set_frame(v, v.current_);
	}
	v.playing_ = true;
}

static void
animate(Viewer &v)
{
	if (!v.playing_ || !v.frame_)
		return;
	const int64_t duration = display_delay_ms(v.frame_->frame_duration);
	if (duration < 0) {
		stop_playback(v);
		return;
	}
	const auto now = chrono::steady_clock::now();
	const auto then = v.frame_at_ + chrono::milliseconds(duration);
	if (then > now)
		return;
	if (!advance_frame(v)) {
		stop_playback(v);
		return;
	}
	v.frame_at_ = then;
	const int64_t next = display_delay_ms(v.frame_->frame_duration);
	if (next >= 0 && v.frame_at_ + chrono::milliseconds(next) < now)
		v.frame_at_ = now;
}

static void
display_size(const Viewer &v, uint32_t *width, uint32_t *height)
{
	orientation_display_size(
		v.image_width_, v.image_height_, v.orientation_, width, height);
}

// The well is already in device pixels, like every other widget rect.
static float
well_w_px(const Viewer &v)
{
	return float(max(0, v.r.w));
}

static float
well_h_px(const Viewer &v)
{
	return float(max(0, v.r.h));
}

static float
well_cx(const Viewer &v)
{
	return float(v.r.x) + float(v.r.w) * 0.5f;
}

static float
well_cy(const Viewer &v)
{
	return float(v.r.y) + float(v.r.h) * 0.5f;
}

static void
get_slack_xy(Viewer &v, float &slack_x, float &slack_y)
{
	slack_x = 0.;
	slack_y = 0.;

	const float well_w = well_w_px(v);
	const float well_h = well_h_px(v);
	if (well_w <= 0.f || well_h <= 0.f)
		return;

	uint32_t dw = 0, dh = 0;
	display_size(v, &dw, &dh);
	if (!dw || !dh)
		return;

	slack_x = 0.5f * float(dw) - well_w / (2.f * v.scale_);
	slack_y = 0.5f * float(dh) - well_h / (2.f * v.scale_);
}

static void
clamp_view(Viewer &v)
{
	if (!v.view_locked_ || v.scale_ <= 0.f)
		return;

	float slack_x = 0, slack_y = 0;
	get_slack_xy(v, slack_x, slack_y);
	v.pan_x_ = slack_x <= 0.f ? 0 : clamp(v.pan_x_, -slack_x, slack_x);
	v.pan_y_ = slack_y <= 0.f ? 0 : clamp(v.pan_y_, -slack_y, slack_y);
}

static void
stop_worker(Viewer &v)
{
	if (!v.worker_)
		return;
	++v.load_epoch_;
	{
		lock_guard<mutex> lock(v.worker_->mu);
		v.worker_->stop = true;
		v.worker_->epoch = v.load_epoch_;
		v.worker_->detached = false;
		v.worker_->desired = {};
		v.worker_->pending_open.reset();
		v.worker_->pending_scale.reset();
		v.worker_->pending_preloads.clear();
	}
	v.worker_->cv.notify_all();
	if (v.worker_->foreground.joinable())
		v.worker_->foreground.join();
	for (thread &preload : v.worker_->preloads) {
		if (preload.joinable())
			preload.join();
	}
	v.worker_.reset();
}

static unique_ptr<Toolbar>
make_toolbar(Viewer &v)
{
	auto left = make_slot_row(v, Slot::Left);
	auto mid = make_slot_row(v, Slot::Middle);
	auto right = make_slot_row(v, Slot::Right);
	right->align = Align::End;
	return make_unique<Toolbar>(
		std::move(left), std::move(mid), std::move(right));
}

static unique_ptr<Panel>
make_error(Viewer &v)
{
	auto row = make_unique<Row>();
	row->gap = kItemGap;
	row->pad_x = kWinPadX;
	row->pad_y = kWinPadY;
	row->grow = true;
	auto lab = make_unique<Label>();
	lab->grow = true;
	lab->wrap = true;
	lab->valign = Align::Center;
	auto dismiss = make_unique<Button>();
	dismiss->flat = true;
	dismiss->tip_text = "Dismiss";
	dismiss->icon = "x-symbolic";
	dismiss->on_click = [&v](Kit &) { v.message_dismissed_ = true; };
	v.error_label_ = lab.get();
	row->add_child(std::move(lab));
	row->add_child(std::move(dismiss));
	auto err = make_unique<Panel>();
	err->fill = Fill::Panel;
	err->stroke = Stroke::Bottom;
	err->grow = true;
	err->hittable = true;
	err->clip = true;
	err->visible = false;
	err->add_child(std::move(row));
	v.error_ = err.get();
	return err;
}

static unique_ptr<Sidebar>
make_sidebar(Viewer &v)
{
	// FIXME: This needs proper layouting.
	const float label_w = v.kit_.text_width(QStringLiteral("Height:"), true)
		/ v.kit_.dpr_;

	auto col = make_unique<ScrollColumn>();
	v.info_ = col.get();
	col->gap = kItemGap;
	col->pad_x = kWinPadX * 2.f;
	col->pad_y = kWinPadX * 2.f;
	col->grow = true;

	col->add_child(meta_row(QStringLiteral("Name:"), QStringLiteral("-"),
		label_w, v.name_label_));
	col->add_child(meta_row(QStringLiteral("Loader:"), QStringLiteral("-"),
		label_w, v.loader_label_));
	col->add_child(meta_row(QStringLiteral("Width:"), QStringLiteral("-"),
		label_w, v.width_label_));
	col->add_child(meta_row(QStringLiteral("Height:"), QStringLiteral("-"),
		label_w, v.height_label_));

#if DAWN_WITH_JPEG_QS
	// QuantSmooth processing can take extremely long,
	// so it's not elligible as a regular loader.
	auto jpegqs = make_unique<Checkbox>();
	jpegqs->text = QStringLiteral("Enable JPEG Quant Smooth");
	jpegqs->enabled_ = false;
	jpegqs->on_click = [&v](Kit &) {
		if (!v.jpeg_quant_smooth_)
			return;

		v.enhance_jpeg_ = v.jpeg_quant_smooth_->checked;
		if (!v.url_.isEmpty()) {
			v.restore_view_ = {true, v.scale_, v.pan_x_, v.pan_y_,
				v.orientation_, v.angle_, v.view_locked_};
			reload_open(v);
		}
	};
	v.jpeg_quant_smooth_ = jpegqs.get();
	col->add_child(std::move(jpegqs));
#endif

	auto exiftool = make_unique<Button>();
	exiftool->text = QStringLiteral("Launch ExifTool");
	exiftool->on_click = [&v](Kit &) {
		if (v.page_ && v.page_->host && v.page_->host->launch_exiftool)
			v.page_->host->launch_exiftool(v.url_);
	};
	v.exiftool_button_ = exiftool.get();
	col->add_child(std::move(exiftool));

	auto cie = make_unique<CieDiagram>();
	v.cie_ = cie.get();
	col->add_child(std::move(cie));

	auto side = make_unique<Sidebar>(std::move(col));
	side->min_w = kInfoSidebarPts;
	side->visible = false;
	return side;
}

static void
sync_ui(Viewer &v, Page &ui)
{
	if (ui.toolbar)
		ui.toolbar->sync_buttons();
	ui.sync_app_menu();
	if (v.error_) {
		v.error_->visible = !v.message_.empty() && !v.message_dismissed_;
		v.error_->max_h = v.kit_.host_h_ * 0.4f;
		if (v.error_->visible && v.error_label_)
			v.error_label_->text =
				QStringLiteral("Error: ") + QString::fromStdString(v.message_);
	}
	if (v.info_)
		fill_info_texts(v, v.current_ ? v.current_.get() : v.image_.get());
	if (v.exiftool_button_)
		v.exiftool_button_->enabled_ = !v.url_.isEmpty();
	if (v.jpeg_quant_smooth_) {
		v.jpeg_quant_smooth_->checked = v.enhance_jpeg_;
		v.jpeg_quant_smooth_->enabled_ = v.image_ && v.image_->loader &&
			string_view(v.image_->loader) == "libjpeg-turbo";
	}
	if (ui.sidebar_open && v.info_) {
		const char *basename = v.basename_.c_str();
		const QString name = (basename && basename[0])
			? QString::fromUtf8(basename)
			: QStringLiteral("-");
		v.name_label_->text = name;
		v.loader_label_->text = v.image_->loader
			? QString::fromUtf8(v.image_->loader)
			: QStringLiteral("-");
		v.width_label_->text = dim_text(v.image_width_);
		v.height_label_->text = dim_text(v.image_height_);
		if (v.cie_) {
			const dawn::Image *im =
				v.current_ ? v.current_.get() : v.image_.get();
			const dawn::Profile *src = im && im->effective_profile
				? im->effective_profile.get()
				: nullptr;
			const bool assumed = im && im->profile_assumed;
			dawn::Chromaticities img = profile_chromaticities(src);
			bool image_dashed = assumed || !src || !img.have_primaries;
			shared_ptr<dawn::Profile> srgb;
			if (!img.have_primaries) {
				auto cmm = v.cmm_ ? v.cmm_ : dawn::Cmm::get_default();
				srgb = cmm->get_profile_sRGB();
				img = profile_chromaticities(srgb.get());
				image_dashed = true;
			}
			v.cie_->image = img;
			v.cie_->image_dashed = image_dashed;
			v.cie_->screen = profile_chromaticities(v.screen_profile_.get());
			v.cie_->show_screen = v.cie_->screen.have_primaries;
			v.cie_->screen_dashed = v.screen_profile_fallback_;
		}
	}
}

static void
fit_to_well(Viewer &v)
{
	uint32_t disp_w = 0, disp_h = 0;
	display_size(v, &disp_w, &disp_h);
	if (!disp_w || !disp_h)
		return;

	const float content_w = well_w_px(v);
	const float content_h = well_h_px(v);
	if (content_w <= 0.f || content_h <= 0.f)
		return;

	const float fit =
		min({content_w / float(disp_w), content_h / float(disp_h), 1.f});
	v.scale_ = clamp(fit, kScaleMin, kScaleMax);
	v.pan_x_ = 0;
	v.pan_y_ = 0;
	v.angle_ = 0;
}

static void
clear_image(Viewer &v)
{
	stop_playback(v);
	v.remaining_loops_ = 0;
	v.image_.reset();
	v.current_.reset();
	v.frame_.reset();
	v.page_scaled_.reset();
	v.image_width_ = 0;
	v.image_height_ = 0;
	v.vector_scale_ = 0;
	v.restore_view_.valid = false;
	if (v.kit_.renderer_)
		v.kit_.renderer_->clear_image();
}

static void
apply_open(Viewer &v, uint64_t gen, dawn::ImagePtr image, string message)
{
	if (gen != v.open_gen_)
		return;
	v.opening_ = false;
	v.open_done_ = true;
	set_message(v, message);
	if (!message.empty())
		qWarning("%s: %s", qUtf8Printable(v.url_.toString(QUrl::PrettyDecoded)),
			message.c_str());
	if (!image || !image->width || !image->height) {
		clear_image(v);
		request_render(v);
		return;
	}
	v.image_ = std::move(image);
	v.current_ = v.image_;
	v.frame_ = v.current_;
	v.page_scaled_.reset();
	v.image_width_ = v.frame_->width;
	v.image_height_ = v.frame_->height;
	if (v.restore_view_.valid) {
		v.scale_ = v.restore_view_.scale;
		v.pan_x_ = v.restore_view_.pan_x;
		v.pan_y_ = v.restore_view_.pan_y;
		v.orientation_ = v.restore_view_.orientation;
		v.angle_ = v.restore_view_.angle;
		v.view_locked_ = v.restore_view_.view_locked;
		v.restore_view_.valid = false;
	} else {
		v.orientation_ = orientation_or_0(v.current_->orientation);
		if (!v.fixate_) {
			v.scale_to_fit_ = true;
			v.pan_x_ = 0;
			v.pan_y_ = 0;
			v.angle_ = 0;
		}
	}
	if (v.kit_.renderer_)
		upload_frame(v, *v.frame_);
	v.vector_scale_ = v.current_->render ? 1.f : 0.f;
	v.remaining_loops_ = 0;
	start_playback(v);
	request_render(v);
}

static void
apply_scale(Viewer &v, uint64_t gen, dawn::ImagePtr image, float scale)
{
	if (!v.kit_.renderer_ || gen != v.scale_gen_)
		return;
	v.scale_job_pending_ = false;
	if (!image || !image->width || !image->height) {
		v.scale_failed_ = true;
		v.scale_failed_target_ = scale;
		request_render(v);
		return;
	}
	v.page_scaled_ = std::move(image);
	upload_frame(v, *v.page_scaled_);
	v.vector_scale_ = scale;
	request_render(v);
}

static void
apply_open_result(Viewer &v, OpenLoad result)
{
	if (result.epoch != v.load_epoch_)
		return;
	const string current = viewer_local_path(v);
	if (!v.detached_ && result.key.path != current &&
		result.key.path != v.previous_path_ &&
		result.key.path != v.next_path_)
		return;
	auto found = find_if(v.open_cache_.begin(), v.open_cache_.end(),
		[&](const Viewer::CachedOpen &entry) {
			return entry.key == result.key;
		});
	if (found == v.open_cache_.end()) {
		v.open_cache_.push_back(
			{std::move(result.key), std::move(result.image),
				std::move(result.message)});
		found = prev(v.open_cache_.end());
	} else {
		found->image = std::move(result.image);
		found->message = std::move(result.message);
	}
	Viewer::CachedOpen *cached = &*found;
	if (!v.detached_ && cached->key ==
		Viewer::OpenKey{current, v.enhance_jpeg_})
		apply_open(v, v.open_gen_, cached->image, cached->message);
	if (v.open_cache_.size() > 3)
		v.open_cache_.erase(v.open_cache_.begin());
}

static OpenLoad
decode_open(const OpenJob &open, const shared_ptr<dawn::Cmm> &cmm)
{
	OpenLoad result;
	result.epoch = open.epoch;
	result.key = open.key;
	dawn::OpenContext ctx;
	ctx.uri = open.uri;
	ctx.cmm = cmm;
	ctx.screen_profile =
		open.enable_cms ? profile_from_icc(*cmm, open.screen_icc) : nullptr;
	ctx.screen_dpi = open.dpi;
	ctx.enhance = open.key.enhance;
	if (open.loaders)
		ctx.loaders = *open.loaders;
	vector<string> warnings;
	ctx.warnings = &warnings;
	dawn::Error error;
	QFile file(QString::fromStdString(open.key.path));
	if (!file.open(QIODevice::ReadOnly)) {
		result.message =
			open.key.path + ": " + file.errorString().toStdString();
		return result;
	}
	const QByteArray bytes = file.readAll();
	if (file.error() != QFileDevice::NoError) {
		result.message =
			open.key.path + ": " + file.errorString().toStdString();
		return result;
	}
	const auto *data = reinterpret_cast<const uint8_t *>(bytes.constData());
	result.image = open_from_data(
		span<const uint8_t>(data, size_t(bytes.size())), ctx, &error);
	const bool no_image = !result.image;
	const bool empty_image =
		result.image && (!result.image->width || !result.image->height);
	if (empty_image)
		result.image.reset();
	result.message = join_load_text(warnings, error, no_image, empty_image);
	return result;
}

static bool
desired(const Viewer::Worker &worker, const OpenJob &job)
{
	return job.epoch == worker.epoch &&
		find(worker.desired.begin(), worker.desired.end(), job.key.path) !=
		worker.desired.end();
}

static bool
finish_decode(Viewer &v, const OpenJob &open, bool preload)
{
	bool accept = false;
	{
		lock_guard lock(v.worker_->mu);
		const Viewer::Worker::ActiveOpen identity{open.epoch, open.key};
		if (preload) {
			auto &active = v.worker_->active_preloads;
			if (auto found = find(active.begin(), active.end(), identity);
				found != active.end())
				active.erase(found);
		} else if (v.worker_->active_open == identity) {
			v.worker_->active_open.reset();
		}
		accept = open.epoch == v.worker_->epoch &&
			(v.worker_->detached || desired(*v.worker_, open));
		if (accept && open.key.path == v.worker_->desired[0])
			v.worker_->current_ready = true;
	}
	v.worker_->cv.notify_all();
	return accept;
}

static void
post_open_result(Viewer &v, OpenLoad result)
{
	post_gui(v, [&v, result = std::move(result)]() mutable {
		apply_open_result(v, std::move(result));
	});
}

static void
worker_loop(Viewer &v, bool foreground)
{
	auto cmm = make_shared<dawn::Cmm>();
	for (;;) {
		OpenJob open;
		ScaleJob scale;
		bool have_open = false;
		bool have_scale = false;
		{
			unique_lock lock(v.worker_->mu);
			v.worker_->cv.wait(lock, [&] {
				if (v.worker_->stop)
					return true;
				if (foreground)
					return v.worker_->pending_open || v.worker_->pending_scale;
				return v.worker_->current_ready &&
					!v.worker_->pending_preloads.empty();
			});
			if (v.worker_->stop)
				return;
			if (!foreground) {
				open = std::move(v.worker_->pending_preloads.back());
				v.worker_->pending_preloads.pop_back();
				v.worker_->active_preloads.emplace_back(
					Viewer::Worker::ActiveOpen{open.epoch, open.key});
				have_open = true;
			} else if (v.worker_->pending_open) {
				open = std::move(*v.worker_->pending_open);
				v.worker_->pending_open.reset();
				v.worker_->active_open =
					Viewer::Worker::ActiveOpen{open.epoch, open.key};
				have_open = true;
			} else {
				scale = std::move(*v.worker_->pending_scale);
				v.worker_->pending_scale.reset();
				have_scale = true;
			}
		}
		if (have_open) {
			OpenLoad result = decode_open(open, cmm);
			if (finish_decode(v, open, !foreground))
				post_open_result(v, std::move(result));
		} else if (have_scale) {
			dawn::ImagePtr image;
			if (scale.page && scale.page->render) {
				shared_ptr<dawn::Profile> profile;
				if (scale.enable_cms)
					profile = profile_from_icc(*cmm, scale.screen_icc);
				image = scale.page->render->render(
					cmm.get(), profile.get(), double(scale.scale));
			}
			const uint64_t gen = scale.gen;
			const float job_scale = scale.scale;
			post_gui(v, [&v, gen, image, job_scale]() {
				apply_scale(v, gen, image, job_scale);
			});
		}
	}
}

static void
start_worker(Viewer &v)
{
	if (v.worker_ && v.worker_->foreground.joinable())
		return;
	v.worker_ = make_unique<Viewer::Worker>();
	v.worker_->epoch = v.load_epoch_;
	v.worker_->foreground = thread([&v] { worker_loop(v, true); });
	for (thread &preload : v.worker_->preloads)
		preload = thread([&v] { worker_loop(v, false); });
}

static OpenJob
make_open_job(const Viewer &v, Viewer::OpenKey key)
{
	OpenJob job;
	job.epoch = v.load_epoch_;
	job.key = std::move(key);
	job.uri = path_to_url(QString::fromStdString(job.key.path))
			  .toEncoded()
			  .toStdString();
	job.dpi = 96;
	job.enable_cms = v.enable_cms_;
	job.screen_icc = v.enable_cms_ ? v.screen_icc_ : nullptr;
	job.loaders = v.loaders_;
	return job;
}

static Viewer::CachedOpen *
find_cached(Viewer &v, const Viewer::OpenKey &key)
{
	for (auto &entry : v.open_cache_) {
		if (entry.key == key)
			return &entry;
	}
	return nullptr;
}

static bool
job_active(
	const Viewer::Worker &worker, uint64_t epoch, const Viewer::OpenKey &key)
{
	const Viewer::Worker::ActiveOpen active{epoch, key};
	if (worker.active_open == active)
		return true;
	return find(worker.active_preloads.begin(), worker.active_preloads.end(),
			   active) != worker.active_preloads.end();
}

static void
start_open(Viewer &v, bool invalidate)
{
	if (!v.worker_ || v.url_.isEmpty())
		return;
	if (invalidate) {
		++v.load_epoch_;
		v.open_cache_.clear();
	}
	++v.open_gen_;
	++v.scale_gen_;
	v.opening_ = true;
	v.open_done_ = false;
	v.detached_ = false;
	v.scale_job_pending_ = false;
	v.scale_failed_ = false;
	const string path = viewer_local_path(v);
	const Viewer::OpenKey key{path, v.enhance_jpeg_};
	Viewer::CachedOpen *cached =
		invalidate ? nullptr : find_cached(v, key);
	{
		lock_guard lock(v.worker_->mu);
		v.worker_->epoch = v.load_epoch_;
		v.worker_->detached = false;
		v.worker_->desired[0] = path;
		v.worker_->current_ready = cached;
		v.worker_->pending_preloads.erase(
			remove_if(v.worker_->pending_preloads.begin(),
				v.worker_->pending_preloads.end(),
				[&](const OpenJob &job) { return job.key.path == path; }),
			v.worker_->pending_preloads.end());
		if (invalidate)
			v.worker_->pending_preloads.clear();
		if (cached || job_active(*v.worker_, v.load_epoch_, key))
			v.worker_->pending_open.reset();
		else
			v.worker_->pending_open = make_open_job(v, key);
		v.worker_->pending_scale.reset();
	}
	v.worker_->cv.notify_all();
	if (cached)
		apply_open(v, v.open_gen_, cached->image, cached->message);
	request_render(v);
}

static void
schedule_preloads(Viewer &v)
{
	const string current = viewer_local_path(v);
	auto wanted = [&](const string &path) {
		return !path.empty() && path != current &&
			(path == v.previous_path_ || path == v.next_path_);
	};
	v.open_cache_.erase(remove_if(v.open_cache_.begin(), v.open_cache_.end(),
							[&](const Viewer::CachedOpen &entry) {
								return entry.key.path != current &&
									!wanted(entry.key.path);
							}),
		v.open_cache_.end());
	if (!v.worker_)
		return;
	vector<string> missing;
	for (const string *path : {&v.previous_path_, &v.next_path_}) {
		const Viewer::OpenKey key{*path, false};
		if (!wanted(*path) ||
			find(missing.begin(), missing.end(), *path) != missing.end() ||
			find_cached(v, key))
			continue;
		missing.push_back(*path);
	}
	v.detached_ = false;
	{
		lock_guard lock(v.worker_->mu);
		v.worker_->detached = false;
		v.worker_->desired = {current, v.previous_path_, v.next_path_};
		if (!v.opening_ && !v.url_.isEmpty())
			v.worker_->current_ready = true;
		auto desired_job = [&](const OpenJob &job) {
			return job.epoch == v.load_epoch_ && wanted(job.key.path);
		};
		v.worker_->pending_preloads.erase(
			remove_if(v.worker_->pending_preloads.begin(),
				v.worker_->pending_preloads.end(),
				[&](const OpenJob &job) { return !desired_job(job); }),
			v.worker_->pending_preloads.end());
		for (const string &path : missing) {
			const Viewer::OpenKey key{path, false};
			const bool pending = any_of(v.worker_->pending_preloads.begin(),
				v.worker_->pending_preloads.end(), [&](const OpenJob &job) {
					return job.epoch == v.load_epoch_ && job.key == key;
				});
			if (pending || job_active(*v.worker_, v.load_epoch_, key) ||
				(v.worker_->pending_open &&
					v.worker_->pending_open->epoch == v.load_epoch_ &&
					v.worker_->pending_open->key == key))
				continue;
			v.worker_->pending_preloads.push_back(make_open_job(v, key));
		}
	}
	v.worker_->cv.notify_all();
}

static void
reload_open(Viewer &v)
{
	start_open(v, true);
	schedule_preloads(v);
}

static void
post_scale(Viewer &v)
{
	if (!v.worker_ || !v.current_ || !v.current_->render)
		return;
	++v.scale_gen_;
	v.scale_job_pending_ = true;
	v.scale_job_target_ = v.scale_;
	v.scale_failed_ = false;
	ScaleJob job;
	job.gen = v.scale_gen_;
	job.page = v.current_;
	job.scale = v.scale_;
	job.enable_cms = v.enable_cms_;
	job.screen_icc = v.enable_cms_ ? v.screen_icc_ : nullptr;
	{
		lock_guard<mutex> lock(v.worker_->mu);
		v.worker_->pending_scale = std::move(job);
	}
	v.worker_->cv.notify_all();
}

static void
ensure_vector_frame(Viewer &v)
{
	if (!v.current_ || !v.current_->render)
		return;

	if (v.scale_ == 1.f) {
		if (v.scale_job_pending_) {
			++v.scale_gen_;
			v.scale_job_pending_ = false;
			if (v.worker_) {
				lock_guard<mutex> lock(v.worker_->mu);
				v.worker_->pending_scale.reset();
			}
		}
		v.scale_failed_ = false;
		v.page_scaled_.reset();
		if (v.vector_scale_ != 1.f) {
			upload_frame(v, *v.current_);
			v.vector_scale_ = 1.f;
		}
		return;
	}

	if (v.page_scaled_ && v.vector_scale_ == v.scale_)
		return;
	if (v.scale_job_pending_ && v.scale_job_target_ == v.scale_)
		return;
	if (v.scale_failed_ && v.scale_failed_target_ == v.scale_)
		return;
	post_scale(v);
}

static void
set_scale(Viewer &v, float scale)
{
	v.scale_to_fit_ = false;
	v.scale_ = clamp(scale, kScaleMin, kScaleMax);
	request_render(v);
}

static void
fit_width_if_larger(Viewer &v)
{
	uint32_t disp_w = 0, disp_h = 0;
	display_size(v, &disp_w, &disp_h);
	const float content_w = well_w_px(v);
	if (!disp_w || content_w <= 0.f)
		return;
	if (ceil(double(disp_w) * double(v.scale_)) > double(content_w))
		set_scale(v, content_w / float(disp_w));
}

static void
fit_height_if_larger(Viewer &v)
{
	uint32_t disp_w = 0, disp_h = 0;
	display_size(v, &disp_w, &disp_h);
	const float content_h = well_h_px(v);
	if (!disp_h || content_h <= 0.f)
		return;
	if (ceil(double(disp_h) * double(v.scale_)) > double(content_h))
		set_scale(v, content_h / float(disp_h));
}

static void
toggle_cms(Viewer &v)
{
	v.enable_cms_ = !v.enable_cms_;
	if (!v.url_.isEmpty()) {
		v.restore_view_ = {true, v.scale_, v.pan_x_, v.pan_y_, v.orientation_,
			v.angle_, v.view_locked_};
		reload_open(v);
	}
	request_render(v);
}

static void
toggle_filter(Viewer &v)
{
	if (v.current_ && v.current_->render)
		return;
	v.filter_ = !v.filter_;
	if (v.kit_.renderer_)
		v.kit_.renderer_->set_filter(v.filter_);
	request_render(v);
}

static float
wrap_angle(float a)
{
	while (a > numbers::pi_v<float>)
		a -= 2.f * numbers::pi_v<float>;
	while (a < -numbers::pi_v<float>)
		a += 2.f * numbers::pi_v<float>;
	return a;
}

struct Vec {
	float x = 0;
	float y = 0;
};

static Vec
turn(float angle, Vec p)
{
	const float c = cosf(angle);
	const float s = sinf(angle);
	return {c * p.x - s * p.y, s * p.x + c * p.y};
}

// The renderer's view transform, in image pixels measured from the image
// centre: screen = well centre + turn(angle) * (image - pan) * scale.
// Screen is device pixels now, so no scale factor enters here beyond the
// zoom itself.  Pan lives in the unturned image frame; see scale-2d.frag.
static Vec
view_to_image(const Viewer &v, Vec p)
{
	const float k = v.scale_ > 0.f ? 1.f / v.scale_ : 0.f;
	const Vec q =
		turn(-v.angle_, {(p.x - well_cx(v)) * k, (p.y - well_cy(v)) * k});
	return {q.x + v.pan_x_, q.y + v.pan_y_};
}

static Vec
image_to_view(const Viewer &v, Vec p)
{
	const float k = v.scale_;
	const Vec q = turn(v.angle_, {(p.x - v.pan_x_) * k, (p.y - v.pan_y_) * k});
	return {q.x + well_cx(v), q.y + well_cy(v)};
}

// Rescale and turn the view, leaving the image point under `at` under it.
// That point may be outside the image.
static void
transform_at(Viewer &v, float new_scale, float new_angle, Vec at)
{
	v.scale_to_fit_ = false;
	new_scale = clamp(new_scale, kScaleMin, kScaleMax);
	const bool hold = v.scale_ > 0.f && new_scale > 0.f;
	const Vec was = hold ? view_to_image(v, at) : Vec{};
	v.scale_ = new_scale;
	v.angle_ = wrap_angle(new_angle);
	if (hold) {
		const Vec now = view_to_image(v, at);
		v.pan_x_ += was.x - now.x;
		v.pan_y_ += was.y - now.y;
	}
	request_render(v);
}

static void
zoom_at(Viewer &v, float factor, Vec at)
{
	transform_at(v, v.scale_ * factor, v.angle_, at);
}

// Keyboard zoom has no pointer, so it holds the middle of the view.
static void
zoom_to(Viewer &v, float new_scale)
{
	transform_at(v, new_scale, v.angle_, {well_cx(v), well_cy(v)});
}

static Rect
image_dest_rect(const Viewer &v)
{
	uint32_t dw = 0, dh = 0;
	display_size(v, &dw, &dh);
	if (v.scale_ <= 0.f || !dw || !dh)
		return {};
	const Vec c = image_to_view(v, {});
	// Bounding box of the turned rectangle.
	const float k = v.scale_;
	const float ca = fabs(cosf(v.angle_));
	const float sa = fabs(sinf(v.angle_));
	const float w = (ca * float(dw) + sa * float(dh)) * k;
	const float h = (sa * float(dw) + ca * float(dh)) * k;
	// Continuous all the way here -- zoom and rotation are analogue -- and
	// rounded only as it becomes a rect to hit-test against.
	return {int(lround(c.x - w * 0.5f)), int(lround(c.y - h * 0.5f)),
		int(lround(w)), int(lround(h))};
}

static Rect
context_anchor(const Viewer &v)
{
	const Rect dest = image_dest_rect(v);
	const int x0 = max(dest.x, v.r.x);
	const int y0 = max(dest.y, v.r.y);
	const int x1 = min(dest.x + dest.w, v.r.x + v.r.w);
	const int y1 = min(dest.y + dest.h, v.r.y + v.r.h);
	if (x1 <= x0 || y1 <= y0)
		return {int(lround(well_cx(v))), int(lround(well_cy(v))), 0, 0};
	return {x0, y0, 0, 0};
}

static bool
show_view_context(const Viewer &v, Kit &kit)
{
	if (v.url_.isEmpty() || !v.image_)
		return false;
	if (v.page_ && v.page_->context)
		v.page_->context->show(kit, v.url_, context_anchor(v), true);
	return true;
}

static void
set_scale_to_fit(Viewer &v, bool enabled)
{
	if (enabled)
		v.fixate_ = false;
	if (v.scale_to_fit_ == enabled)
		return;
	v.scale_to_fit_ = enabled;
	if (v.scale_to_fit_)
		fit_to_well(v);
	request_render(v);
}

enum class SnapDir : uint8_t { Left, Right, Mirror };

static dawn::Orientation
orientation_flip_v(dawn::Orientation orientation)
{
	return orientation_mirror(
		orientation_rotate_left(orientation_rotate_left(orientation)));
}

static void
set_orientation(Viewer &v, dawn::Orientation next)
{
	next = orientation_or_0(next);
	if (next == v.orientation_)
		return;
	if (v.scale_to_fit_) {
		v.orientation_ = next;
		fit_to_well(v);
		request_render(v);
		return;
	}
	uint32_t old_w = 0, old_h = 0;
	display_size(v, &old_w, &old_h);
	double sx = 0, sy = 0;
	orientation_map_display_to_source(v.orientation_, v.image_width_,
		v.image_height_, double(old_w) * 0.5 + double(v.pan_x_),
		double(old_h) * 0.5 + double(v.pan_y_), &sx, &sy);
	v.orientation_ = next;
	uint32_t new_w = 0, new_h = 0;
	display_size(v, &new_w, &new_h);
	double dx = 0, dy = 0;
	orientation_map_source_to_display(
		v.orientation_, v.image_width_, v.image_height_, sx, sy, &dx, &dy);
	v.pan_x_ = float(dx - double(new_w) * 0.5);
	v.pan_y_ = float(dy - double(new_h) * 0.5);
	request_render(v);
}

static void
snap_view(Viewer &v, SnapDir dir)
{
	const float a = v.angle_;
	if (dir == SnapDir::Mirror) {
		const float c = cosf(a);
		const float s = sinf(a);
		const dawn::Orientation next = (fabs(c) >= fabs(s))
			? orientation_mirror(v.orientation_)
			: orientation_flip_v(v.orientation_);
		set_orientation(v, next);
		return;
	}
	// Leftover on this key's side: drop it (1° left + < → 0).
	// Otherwise take the 90° step (180°+1° + > → 270°).
	if (dir == SnapDir::Left) {
		if (a <= kAngleFast)
			set_orientation(v, orientation_rotate_left(v.orientation_));
	} else if (a >= -kAngleFast)
		set_orientation(v, orientation_rotate_right(v.orientation_));
	v.angle_ = 0;
	request_render(v);
}

// A locked view has no free angle, so a drag turns it in quarter steps,
// each taken as the drag passes the halfway point.
static void
rotate_locked(Viewer &v, float delta)
{
	constexpr float step = numbers::pi_v<float> * 0.5f;
	v.drag_angle_ += delta;
	while (v.drag_angle_ >= step * 0.5f) {
		set_orientation(v, orientation_rotate_right(v.orientation_));
		v.drag_angle_ -= step;
	}
	while (v.drag_angle_ <= -step * 0.5f) {
		set_orientation(v, orientation_rotate_left(v.orientation_));
		v.drag_angle_ += step;
	}
}

static void
pan_by(Viewer &v, double dx_points, double dy_points)
{
	if (v.scale_ <= 0.f)
		return;

	const float k = v.kit_.dpr_ / v.scale_;
	const Vec u = turn(-v.angle_, {float(dx_points) * k, float(dy_points) * k});
	v.pan_x_ -= u.x;
	v.pan_y_ -= u.y;
	request_render(v);
}

void
snap_pan_to_pixels(float *pan, float disp, float vp, float scale)
{
	if (!pan || scale <= 0.f || disp <= 0.f || vp <= 0.f)
		return;

	const float origin = 0.5f * vp - scale * (0.5f * disp + *pan);
	*pan += (origin - round(origin)) / scale;
}

static void
cancel_scale(Viewer &v)
{
	++v.scale_gen_;
	v.scale_job_pending_ = false;
	v.scale_failed_ = false;
	v.page_scaled_.reset();
	if (v.worker_) {
		lock_guard<mutex> lock(v.worker_->mu);
		v.worker_->pending_scale.reset();
	}
}

static void
frame_step(Viewer &v, int step)
{
	if (!v.frame_)
		return;
	stop_playback(v);
	if (step > 0) {
		(void) advance_frame(v);
		request_render(v);
		return;
	}
	if (step == 0) {
		set_frame(v, v.current_);
		v.remaining_loops_ = 0;
		request_render(v);
		return;
	}
	if (dawn::ImagePtr prev = v.frame_->frame_previous.lock()) {
		set_frame(v, std::move(prev));
		request_render(v);
		return;
	}
	dawn::ImagePtr last = v.current_;
	if (!last)
		return;
	while (last->frame_next)
		last = last->frame_next;
	set_frame(v, std::move(last));
	v.remaining_loops_ = 0;
	request_render(v);
}

static void
switch_page(Viewer &v, dawn::ImagePtr page)
{
	if (!page || page.get() == v.current_.get())
		return;
	v.current_ = std::move(page);
	v.frame_ = v.current_;
	v.image_width_ = v.frame_->width;
	v.image_height_ = v.frame_->height;
	v.orientation_ = orientation_or_0(v.current_->orientation);
	v.angle_ = 0;
	cancel_scale(v);
	if (v.kit_.renderer_)
		upload_frame(v, *v.frame_);
	v.vector_scale_ = v.current_->render ? 1.f : 0.f;
	v.remaining_loops_ = 0;
	start_playback(v);
	request_render(v);
}

static void
page_step(Viewer &v, int step)
{
	if (!v.current_)
		return;
	if (step < 0) {
		if (dawn::ImagePtr prev = v.current_->page_previous.lock())
			switch_page(v, std::move(prev));
		return;
	}
	if (v.current_->page_next)
		switch_page(v, v.current_->page_next);
}

static void
page_last(Viewer &v)
{
	dawn::ImagePtr p = v.current_ ? v.current_ : v.image_;
	if (!p)
		return;
	while (p->page_next)
		p = p->page_next;
	switch_page(v, std::move(p));
}

static void
copy_image(QMimeData *mime, const dawn::Image &im)
{
	if (!mime || im.data.empty() || !im.width || !im.height)
		return;
	const uint32_t w = im.width;
	const uint32_t h = im.height;
	vector<uint8_t> bgra(size_t(w) * h * 4);
	for (uint32_t y = 0; y < h; ++y) {
		const uint16_t *s = row_u16(im, y);
		uint8_t *d = bgra.data() + size_t(y) * w * 4;
		for (uint32_t x = 0; x < w; ++x) {
			d[0] = uint8_t(s[0] >> 8);
			d[1] = uint8_t(s[1] >> 8);
			d[2] = uint8_t(s[2] >> 8);
			d[3] = uint8_t(s[3] >> 8);
			s += 4;
			d += 4;
		}
	}
	dawn::unpremultiply_bgra8(bgra.data(), w, h, size_t(w) * 4);
	QImage image(int(w), int(h), QImage::Format_ARGB32);
	for (uint32_t y = 0; y < h; ++y) {
		const uint8_t *s = bgra.data() + size_t(y) * w * 4;
		auto *d = reinterpret_cast<QRgb *>(image.scanLine(int(y)));
		for (uint32_t x = 0; x < w; ++x) {
			d[x] = qRgba(s[2], s[1], s[0], s[3]);
			s += 4;
		}
	}
	mime->setImageData(image);
}

static void
copy_frame(const Viewer &v)
{
	auto *mime = new QMimeData;
	if (v.frame_)
		copy_image(mime, *v.frame_);
	if (!v.url_.isEmpty()) {
		const QUrl files[] = {v.url_};
		copy_files(mime, files);
	}
	QGuiApplication::clipboard()->setMimeData(mime);
}

static bool
apply_action(Viewer &v, Action action)
{
	switch (action) {
	case Action::ZoomIn:
		zoom_to(v, v.scale_ * kZoomStep);
		return true;
	case Action::ZoomOut:
		zoom_to(v, v.scale_ / kZoomStep);
		return true;
	case Action::Zoom1:
		zoom_to(v, 1.f);
		return true;
	case Action::Fit:
		set_scale_to_fit(v, !v.scale_to_fit_);
		return true;
	case Action::FitWidth:
		fit_width_if_larger(v);
		return true;
	case Action::FitHeight:
		fit_height_if_larger(v);
		return true;
	case Action::Lock:
		v.view_locked_ = !v.view_locked_;
		if (v.view_locked_)
			v.angle_ = 0;
		else
			set_scale_to_fit(v, false);
		request_render(v);
		return true;
	case Action::Fixate:
		v.fixate_ = !v.fixate_;
		if (v.fixate_)
			set_scale_to_fit(v, false);
		request_render(v);
		return true;
	case Action::ColorManagement:
		toggle_cms(v);
		return true;
	case Action::Smooth:
		toggle_filter(v);
		return true;
	case Action::Checkerboard:
		v.checkerboard_ = !v.checkerboard_;
		if (v.kit_.renderer_)
			v.kit_.renderer_->set_checkerboard(v.checkerboard_);
		request_render(v);
		return true;
	case Action::BlendLinearLight:
		v.blend_linear_light_ = !v.blend_linear_light_;
		if (v.kit_.renderer_)
			v.kit_.renderer_->set_blend_linear_light(v.blend_linear_light_);
		request_render(v);
		return true;
	case Action::RotateLeft:
		snap_view(v, SnapDir::Left);
		return true;
	case Action::Mirror:
		snap_view(v, SnapDir::Mirror);
		return true;
	case Action::RotateRight:
		snap_view(v, SnapDir::Right);
		return true;
	case Action::Information:
		if (v.page_)
			v.page_->sidebar_open = !v.page_->sidebar_open;
		request_render(v);
		return true;
	case Action::PageFirst:
		switch_page(v, v.image_);
		return true;
	case Action::PagePrevious:
		page_step(v, -1);
		return true;
	case Action::PageNext:
		page_step(v, +1);
		return true;
	case Action::PageLast:
		page_last(v);
		return true;
	case Action::FrameFirst:
		frame_step(v, 0);
		return true;
	case Action::FramePrevious:
		frame_step(v, -1);
		return true;
	case Action::FrameNext:
		frame_step(v, +1);
		return true;
	case Action::PlayPause:
		if (v.playing_)
			stop_playback(v);
		else
			start_playback(v);
		request_render(v);
		return true;
	case Action::Copy:
		copy_frame(v);
		return true;
	case Action::Reload:
		reload_open(v);
		return true;
	case Action::Trash:
		if (!v.url_.isEmpty() && v.page_ && v.page_->host &&
			v.page_->host->trash)
			v.page_->host->trash(v.url_);
		return true;
	default:
		return false;
	}
}

static void
apply_view(const Viewer &v)
{
	if (!v.kit_.renderer_)
		return;
	Renderer &renderer = *v.kit_.renderer_;
	const bool have_scaled = v.page_scaled_ && v.page_scaled_->width &&
		v.page_scaled_->height && v.vector_scale_ > 0.f;
	const float gpu_scale = have_scaled ? v.scale_ / v.vector_scale_ : v.scale_;
	float pan_x = v.pan_x_;
	float pan_y = v.pan_y_;
	if (have_scaled && v.current_ && v.current_->width && v.current_->height) {
		uint32_t src_dw = 0, src_dh = 0, dst_dw = 0, dst_dh = 0;
		orientation_display_size(v.current_->width, v.current_->height,
			v.orientation_, &src_dw, &src_dh);
		orientation_display_size(v.page_scaled_->width, v.page_scaled_->height,
			v.orientation_, &dst_dw, &dst_dh);
		if (src_dw && src_dh) {
			pan_x *= float(dst_dw) / float(src_dw);
			pan_y *= float(dst_dh) / float(src_dh);
		}
	}
	if (gpu_scale > 0.f) {
		const Extent vp = renderer.extent();
		// The shader turns the image about the viewport centre; this offset
		// moves that to the well centre, so it takes the image frame too.
		const float ox = float(vp.width) * 0.5f - well_cx(v);
		const float oy = float(vp.height) * 0.5f - well_cy(v);
		const Vec o = turn(-v.angle_, {ox / gpu_scale, oy / gpu_scale});
		pan_x += o.x;
		pan_y += o.y;
		if (fabs(v.angle_) < kAngleFast &&
			fabs(double(gpu_scale) - round(double(gpu_scale))) < 1.0e-5) {
			uint32_t dw = 0, dh = 0;
			if (have_scaled)
				orientation_display_size(v.page_scaled_->width,
					v.page_scaled_->height, v.orientation_, &dw, &dh);
			else
				display_size(v, &dw, &dh);
			snap_pan_to_pixels(&pan_x, float(dw), float(vp.width), gpu_scale);
			snap_pan_to_pixels(&pan_y, float(dh), float(vp.height), gpu_scale);
		}
	}
	renderer.set_filter(v.filter_);
	renderer.set_checkerboard(v.checkerboard_);
	renderer.set_blend_linear_light(v.blend_linear_light_);
	renderer.set_transfer(v.enable_cms_
			? profile_transfer(v.screen_profile_.get())
			: dawn::Transfer::Srgb);
	renderer.set_view(gpu_scale, pan_x, pan_y, v.orientation_, v.angle_);
}

static Actor
make_actor(Viewer &v, const HostActions &host)
{
	return chain_actor(
		host, [&v](Action a) { return apply_action(v, a); },
		[&v](Action a) { return spec_enabled(v, a); },
		[&v](Action a) { return spec_active(v, a); });
}

Viewer::Viewer(Kit &kit) : kit_(kit)
{
	this->hittable = true;
}

Viewer::~Viewer()
{
	destroy();
}

void
Viewer::init()
{
	this->scale_text_.clear();
	pack_toolbar_icons(*this);
	start_worker(*this);
}

void
Viewer::measure(Kit &, int max_w, int max_h)
{
	this->r = {0, 0, max_w, max_h};
}

void
Viewer::arrange(Kit &kit, Rect alloc)
{
	this->r = alloc;
	if (this->scale_to_fit_)
		fit_to_well(*this);
	clamp_view(*this);
}

bool
Viewer::focusable() const
{
	return shown() && this->r.w > 0 && this->r.h > 0;
}

void
Viewer::prepare(Kit &)
{
	ensure_vector_frame(*this);
}

void
Viewer::paint(Kit &) const
{
	apply_view(*this);
}

unique_ptr<Page>
make_viewer_page(Kit &kit, const HostActions &host, Viewer **out)
{
	auto viewer = make_unique<Viewer>(kit);
	Viewer *v = viewer.get();
	v->init();
	auto toolbar = make_toolbar(*v);
	auto err = make_error(*v);
	auto side = make_sidebar(*v);
	auto page = make_unique<Page>(std::move(toolbar), std::move(side),
		Page::Side::Right, std::move(viewer));
	page->host = &host;
	if (page->context) {
		page->context->on_new_window = host.new_window;
		page->context->on_trash = host.trash;
	}
	page->set_banner(std::move(err));
	page->menu_tree = viewer_menu();
	page->keys = viewer_keys();
	page->actor = make_actor(*v, host);
	if (page->titlebar)
		page->titlebar->actor = page->actor;
	if (page->toolbar)
		page->toolbar->actor = page->actor;
	if (page->app_menu)
		page->app_menu->build(kit, page->menu_tree, page->actor);
	v->page_ = page.get();
	if (out)
		*out = v;
	return page;
}

void
Viewer::set_host(float width_pts, float height_pts, float dpr)
{
	// The platform speaks logical points; everything past here is pixels,
	// so the scale has to be current before the conversion.
	if (!set_dpr(*this, dpr))
		pack_toolbar_icons(*this);
	this->kit_.host_w_ = this->kit_.px(width_pts);
	this->kit_.host_h_ = this->kit_.px(height_pts);
}

void
Viewer::destroy()
{
	stop_worker(*this);
	this->scale_text_.clear();
	this->opening_ = false;
	this->open_done_ = false;
	this->image_.reset();
	this->current_.reset();
	this->frame_.reset();
	this->page_scaled_.reset();
	this->open_cache_.clear();
	this->previous_path_.clear();
	this->next_path_.clear();
	this->playing_ = false;
	this->remaining_loops_ = 0;
	this->scale_label_ = nullptr;
	this->error_ = nullptr;
	this->error_label_ = nullptr;
	this->info_ = nullptr;
	this->name_label_ = nullptr;
	this->loader_label_ = nullptr;
	this->width_label_ = nullptr;
	this->height_label_ = nullptr;
	this->jpeg_quant_smooth_ = nullptr;
	this->exiftool_button_ = nullptr;
	this->cie_ = nullptr;
	this->info_text_src_ = nullptr;
	this->page_ = nullptr;
}

void
Viewer::open(const QUrl &url)
{
	if (url == this->url_ && this->image_ && this->image_->width &&
		this->image_->height) {
		this->opening_ = false;
		this->open_done_ = true;
		if (this->kit_.request_render)
			this->kit_.request_render();
		return;
	}
	this->enhance_jpeg_ = false;
	this->url_ = url;
	this->basename_ = url_basename(url).toStdString();
	start_open(*this, false);
}

void
Viewer::set_preload_urls(const QUrl &previous, const QUrl &next)
{
	this->previous_path_ = url_to_path(previous).toStdString();
	this->next_path_ = url_to_path(next).toStdString();
	const string current = viewer_local_path(*this);
	if (this->previous_path_ == current)
		this->previous_path_.clear();
	if (this->next_path_ == current)
		this->next_path_.clear();
	schedule_preloads(*this);
}

void
Viewer::cancel_loads()
{
	const bool discard_current = this->opening_;
	++this->open_gen_;
	++this->scale_gen_;
	this->opening_ = false;
	this->open_done_ = false;
	this->detached_ = true;
	this->scale_job_pending_ = false;
	this->scale_failed_ = false;
	this->previous_path_.clear();
	this->next_path_.clear();
	if (this->worker_) {
		lock_guard lock(this->worker_->mu);
		this->worker_->detached = true;
		this->worker_->desired = {};
		this->worker_->current_ready = false;
		this->worker_->pending_open.reset();
		this->worker_->pending_scale.reset();
		this->worker_->pending_preloads.clear();
		this->worker_->cv.notify_all();
	}
	if (!discard_current)
		return;
	clear_image(*this);
	this->url_.clear();
	this->basename_.clear();
	this->message_.clear();
	this->message_dismissed_ = false;
	request_render(*this);
}

bool
Viewer::has_view() const
{
	return !this->opening_ && !this->url_.isEmpty();
}

bool
Viewer::consume_open_done()
{
	if (!this->open_done_)
		return false;
	this->open_done_ = false;
	return true;
}

void
Viewer::set_screen_profile(
	shared_ptr<dawn::Cmm> cmm, shared_ptr<dawn::Profile> profile, bool fallback)
{
	auto screen_icc = profile
		? make_shared<const vector<uint8_t>>(profile->to_bytes())
		: nullptr;
	const bool changed = bool(this->screen_icc_) != bool(screen_icc) ||
		(this->screen_icc_ && *this->screen_icc_ != *screen_icc);
	const bool reload =
		this->enable_cms_ && !this->url_.isEmpty() && changed;
	this->cmm_ = std::move(cmm);
	this->screen_icc_ = std::move(screen_icc);
	this->screen_profile_ = std::move(profile);
	this->screen_profile_fallback_ = fallback;
	this->kit_.bake_colours(this->cmm_.get(), this->screen_profile_.get());
	if (this->kit_.renderer_)
		this->kit_.renderer_->set_transfer(this->enable_cms_
				? profile_transfer(this->screen_profile_.get())
				: dawn::Transfer::Srgb);
	if (reload) {
		this->restore_view_ = {true, this->scale_, this->pan_x_, this->pan_y_,
			this->orientation_, this->angle_, this->view_locked_};
		reload_open(*this);
	}
}

void
Viewer::present(Page &ui)
{
	animate(*this);
	if (!this->kit_.inited_)
		return;
	sync_ui(*this, ui);
	// The label reserves its width when measured, so settle it beforehand;
	// only fit-to-well, which needs the well, still has to follow layout.
	sync_scale_label(*this);
	this->kit_.frame_ui(ui, [this] { sync_scale_label(*this); });
}

int
Viewer::wake_ms() const
{
	if (!this->playing_ || !this->frame_)
		return -1;
	const int64_t duration = display_delay_ms(this->frame_->frame_duration);
	if (duration < 0)
		return -1;
	const float elapsed = chrono::duration<float, milli>(
		chrono::steady_clock::now() - this->frame_at_)
							  .count();
	if (elapsed >= float(duration))
		return 0;
	return int(ceil(double(duration) - double(elapsed)));
}

bool
Viewer::key(Kit &kit, const Key &ev)
{
	if (context_key(ev.key, ev.mods))
		return show_view_context(*this, kit);

	switch (ev.mods) {
	case unsigned(Qt::NoModifier):
		if (ev.key >= Qt::Key_1 && ev.key <= Qt::Key_9) {
			set_scale(*this, float(ev.key - Qt::Key_0));
			return true;
		}
		break;
	case unsigned(Qt::ControlModifier): {
		float slack_x = 0, slack_y = 0;
		get_slack_xy(*this, slack_x, slack_y);
		switch (ev.key) {
		case Qt::Key_Up:
			if (slack_y > 0.f)
				this->pan_y_ = -slack_y;
			return true;
		case Qt::Key_Right:
			if (slack_x > 0.f)
				this->pan_x_ = +slack_x;
			return true;
		case Qt::Key_Down:
			if (slack_y > 0.f)
				this->pan_y_ = +slack_y;
			return true;
		case Qt::Key_Left:
			if (slack_x > 0.f)
				this->pan_x_ = -slack_x;
			return true;
		}
		break;
	}
	case unsigned(Qt::ShiftModifier):
		// Other modifiers are taken, and bare arrows iterate files.
		switch (ev.key) {
		case Qt::Key_Up:
			pan_by(*this, 0., +kKeyboardPan);
			return true;
		case Qt::Key_Right:
			pan_by(*this, -kKeyboardPan, 0);
			return true;
		case Qt::Key_Down:
			pan_by(*this, 0., -kKeyboardPan);
			return true;
		case Qt::Key_Left:
			pan_by(*this, +kKeyboardPan, 0);
			return true;
		}
		break;
	}
	return false;
}

bool
Viewer::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::RightButton) {
		if (this->url_.isEmpty() || !this->image_)
			return false;
		const Rect dest = image_dest_rect(*this);
		if (!dest.contains(x, y))
			return false;
		if (this->page_ && this->page_->context)
			this->page_->context->show(
				kit, this->url_, {int(x), int(y), 0, 0}, false);
		return true;
	}
	if (button != Qt::LeftButton && button != Qt::MiddleButton)
		return false;
	kit.pressed_ = this;
	if (button == Qt::MiddleButton) {
		if (kit.mods_ == unsigned(Qt::ControlModifier))
			this->drag_ = Drag::Zoom;
		else if (kit.mods_ == unsigned(Qt::AltModifier))
			this->drag_ = Drag::Rotate;
		else
			this->drag_ = Drag::Pan;
	} else
		this->drag_ = Drag::Pan;
	// Hold one screen point for the whole gesture, so the image point under
	// it cannot walk with the cursor.
	this->drag_pivot_x_ = x;
	this->drag_pivot_y_ = y;
	if (this->drag_ == Drag::Rotate) {
		this->drag_pivot_x_ = well_cx(*this);
		this->drag_pivot_y_ = well_cy(*this);
	}
	this->drag_angle_ = 0;
	this->drag_x_ = double(x);
	this->drag_y_ = double(y);
	return true;
}

bool
Viewer::release(Kit &, float, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton && button != Qt::MiddleButton)
		return false;
	this->drag_ = Drag::None;
	return true;
}

bool
Viewer::motion(Kit &, float x, float y)
{
	if (this->drag_ == Drag::None)
		return false;
	const float x0 = float(this->drag_x_);
	const float y0 = float(this->drag_y_);
	if (this->drag_ == Drag::Pan)
		pan_by(*this, double(x) - this->drag_x_, double(y) - this->drag_y_);
	else if (this->drag_ == Drag::Zoom) {
		const float factor = pow(kZoomStep, -(y - y0) / kZoomDragPts);
		zoom_at(*this, factor, {this->drag_pivot_x_, this->drag_pivot_y_});
	} else {
		const Vec p = {this->drag_pivot_x_, this->drag_pivot_y_};
		const float r0 = hypot(x0 - p.x, y0 - p.y);
		const float r1 = hypot(x - p.x, y - p.y);
		const float min_r = float(this->kit_.px(kRotateMinR));
		if (r0 >= min_r && r1 >= min_r) {
			const float d =
				wrap_angle(atan2(y - p.y, x - p.x) - atan2(y0 - p.y, x0 - p.x));
			if (this->view_locked_)
				rotate_locked(*this, d);
			else
				transform_at(*this, this->scale_, this->angle_ + d, p);
		}
	}
	this->drag_x_ = double(x);
	this->drag_y_ = double(y);
	return true;
}

bool
Viewer::double_click(Kit &, float, float, Qt::MouseButton button, unsigned mods)
{
	if (button != Qt::LeftButton || mods)
		return false;
	this->drag_ = Drag::None;
	if (this->page_ && this->page_->actor.apply)
		this->page_->actor.apply(Action::Fullscreen);
	return true;
}

bool
Viewer::scroll(Kit &, float x, float y, int delta)
{
	zoom_at(*this, delta > 0 ? kZoomStep : 1.f / kZoomStep, {x, y});
	return true;
}

bool
Viewer::pan(Kit &, float, float, float dx, float dy)
{
	pan_by(*this, double(dx), double(dy));
	return true;
}

bool
Viewer::gesture(Kit &, float x, float y, float scale_factor, float angle_delta)
{
	if (this->view_locked_)
		angle_delta = 0;
	if (scale_factor == 1.f && angle_delta == 0.f)
		return true;
	transform_at(
		*this, this->scale_ * scale_factor, this->angle_ + angle_delta, {x, y});
	return true;
}

}  // namespace dn
