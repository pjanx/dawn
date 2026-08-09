//
// browser.cpp: directory browser (fiv-style masonry thumbs)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "browser.hpp"

#include "action.hpp"
#include "chrome.hpp"
#include "renderer.hpp"
#include "thumb-scaler.hpp"
#include "thumbnail-cache.hpp"
#include "thumbnailer.hpp"
#include "xdg.hpp"

#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QKeyEvent>
#include <QStandardPaths>
#include <QUrl>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

using namespace std;

namespace dn
{

namespace
{

constexpr float kWinPadX = 4.0f;
constexpr float kWinPadY = 2.0f;
constexpr float kItemGap = 2.0f;
constexpr float kGridPad = 8.0f;
constexpr float kGlow = 8.0f;
constexpr float kGlowAlpha = 0.375f;
constexpr float kBorder = 2.0f;
constexpr float kThumbGap = 1.0f;
constexpr int kCapLines = 2;
constexpr float kCapPad = 4.0f;
constexpr float kCheck = 40.0f;
constexpr float kPrefetchRows = 2.0f;
constexpr const char *kMoreIcon = "disclose-arrow-down-symbolic";
constexpr const char *kPendingIcon = "dots-horizontal-symbolic";
constexpr const char *kMissingIcon = "image-missing-symbolic";

constexpr int kThumbSizes[] = {128, 256, 512, 1024};
constexpr int kThumbSizeN = 4;
// Wide thumbnail box is 2× row height (512×256 at the default size).
constexpr int kThumbWide = 2;
constexpr size_t kThumbRamBudget = 2ull << 30;

enum class Slot : uint8_t { Left, Middle, Right };
enum class Kind : uint8_t { Icon, Text, Sep };

struct Spec {
	Kind kind;
	Slot slot;
	Action action;
};

constexpr Spec kItems[] = {
	{Kind::Icon, Slot::Left, Action::Sidebar},
	{Kind::Icon, Slot::Left, Action::Back},
	{Kind::Icon, Slot::Left, Action::Forward},
	{Kind::Icon, Slot::Left, Action::Reload},
	{Kind::Sep, Slot::Left, Action::None},
	{Kind::Icon, Slot::Left, Action::DirPrev},
	{Kind::Icon, Slot::Left, Action::DirParent},
	{Kind::Icon, Slot::Left, Action::DirNext},
	{Kind::Sep, Slot::Left, Action::None},

	// I don't know if these should be Slot::Middle.
	{Kind::Icon, Slot::Left, Action::ThumbMinus},
	{Kind::Icon, Slot::Left, Action::ThumbPlus},
	{Kind::Sep, Slot::Left, Action::None},
	{Kind::Icon, Slot::Left, Action::ViewTile},
	{Kind::Icon, Slot::Left, Action::ViewGrid},
	// TODO: {Kind::Icon, Slot::Left, Action::ViewList},
	{Kind::Sep, Slot::Left, Action::None},
	{Kind::Icon, Slot::Left, Action::Filenames},
	{Kind::Icon, Slot::Left, Action::Filter},
	{Kind::Sep, Slot::Left, Action::None},
	{Kind::Icon, Slot::Left, Action::SortDir},
	{Kind::Text, Slot::Left, Action::SortName},
	{Kind::Text, Slot::Left, Action::SortTime},

	{Kind::Sep, Slot::Right, Action::None},
	{Kind::Icon, Slot::Right, Action::DarkMode},
	{Kind::Icon, Slot::Right, Action::Fullscreen},
};

bool apply_action(Browser &b, Action action);

struct ThumbJob {
	uint64_t gen = 0;
	Thumbnailer::Priority priority = Thumbnailer::Priority::Dimensions;
	string path;
	int64_t mtime = 0;
	uint64_t size = 0;
	int thumb_size = 0;
	float dpr = 1.0f;
	int atlas_max = 0;
	vector<uint8_t> screen_icc;
	bool skip_cache = false;
};

enum class GpuPurpose : uint8_t {
	Display,
	CacheDisplay,
	CacheOnly,
	DisplayInterim,
	DisplayRefit,
};

struct ThumbUpdate {
	uint32_t geometry_w = 0;
	uint32_t geometry_h = 0;
	uint32_t ram_w = 0;
	uint32_t ram_h = 0;
	vector<uint16_t> ram;
	dn::ImagePtr image;
	dn::Orientation orientation = dn::Orientation::Rotate0;
	dn::Transfer transfer = dn::Transfer::Srgb;
	bool failed = true;
	bool gpu_pending = false;
	bool interim = false;
	bool cache_bypass = false;
	bool regeneration = false;
	GpuPurpose gpu_purpose = GpuPurpose::Display;
	int tier = 0;
};

struct FinishJob {
	uint64_t gen = 0;
	Thumbnailer::Priority priority = Thumbnailer::Priority::Maintenance;
	string path;
	int64_t mtime = 0;
	uint64_t size = 0;
	uint32_t image_w = 0, image_h = 0;
	uint32_t width = 0, height = 0;
	shared_ptr<const vector<uint16_t>> pixels;
	int tier = 0;
	int thumb_size = 0;
	float dpr = 1.0f;
	int atlas_max = 0;
	vector<uint8_t> screen_icc;
};

int
thumb_size_index(int size)
{
	for (int i = 0; i < kThumbSizeN; ++i) {
		if (kThumbSizes[i] == size)
			return i;
	}
	return 1;
}

bool
hidden_name(const string &name)
{
	return !name.empty() && name[0] == '.';
}

bool
is_image_ext(const QString &name)
{
	static const vector<QString> globs =
		extract_mime_globs(supported_media_types());
	const QString lower = QFileInfo(name).fileName().toLower();
	for (const QString &glob : globs) {
		if (QDir::match(glob, lower))
			return true;
	}
	return false;
}

shared_ptr<Profile>
profile_from_icc(Cmm &cmm, const vector<uint8_t> &icc)
{
	if (!icc.empty()) {
		if (auto profile = cmm.get_profile(icc))
			return profile;
	}
	return cmm.get_profile_sRGB();
}

vector<uint8_t>
screen_icc_bytes(const Browser &b)
{
	if (!b.screen_profile_)
		return {};
	return b.screen_profile_->to_bytes();
}

int
thumb_atlas_max(const Browser &b)
{
	if (b.kit_.renderer_)
		return b.kit_.renderer_->thumb_atlas_max();
	return Sheet::kSize;
}

void
thumb_dest_params(uint32_t gw, uint32_t gh, int thumb_size, float dpr,
	int atlas_max, uint32_t *out_w, uint32_t *out_h)
{
	if (!out_w || !out_h)
		return;
	if (!gw || !gh) {
		*out_w = 1;
		*out_h = 1;
		return;
	}
	const float d = dpr > 0.0f ? dpr : 1.0f;
	float cap_h = float(thumb_size) * d;
	if (cap_h < 1.0f)
		cap_h = 1.0f;
	const float cap_w = float(kThumbWide) * cap_h;
	const float s = min(cap_w / float(gw), cap_h / float(gh));
	int w = max(1, int(lround(double(gw) * double(s))));
	int h = max(1, int(lround(double(gh) * double(s))));
	const int atlas = max(1, atlas_max);
	if (w > atlas || h > atlas) {
		float s2 = 1.0f;
		if (w > 0)
			s2 = min(s2, float(atlas) / float(w));
		if (h > 0)
			s2 = min(s2, float(atlas) / float(h));
		w = max(1, int(lround(double(w) * double(s2))));
		h = max(1, int(lround(double(h) * double(s2))));
		if (w > atlas)
			w = atlas;
		if (h > atlas)
			h = atlas;
	}
	*out_w = uint32_t(w);
	*out_h = uint32_t(h);
}

void
thumb_dest(const Browser &b, uint32_t gw, uint32_t gh, uint32_t *out_w,
	uint32_t *out_h)
{
	const float dpr = b.kit_.dpr_ > 0.0f ? b.kit_.dpr_ : 1.0f;
	thumb_dest_params(
		gw, gh, b.thumb_size_, dpr, thumb_atlas_max(b), out_w, out_h);
}

float
label_h(const Browser &b)
{
	if (!b.show_names_)
		return 0.0f;
	return b.kit_.text_height(QStringLiteral("Ag\nAg"), 0.0f, false) + kCapPad;
}

float
chrome()
{
	return kGlow + kBorder;
}

QString
caption_name(const string &name)
{
	QString s = QString::fromStdString(name);
	s.replace(QLatin1Char('.'), QString(QChar(0x200B)) + QLatin1Char('.'));
	return s;
}

float
row_h(const Browser &b)
{
	return float(b.thumb_size_) + 2.0f * chrome() + label_h(b);
}

bool
thumb_in_band(const Browser &b, const Browser::File &f, float pad)
{
	if (f.cell.w <= 0.0f)
		return false;
	return f.cell.y + f.cell.h >= b.r.y - pad &&
		f.cell.y <= b.r.y + b.r.h + pad;
}

void
request_render(const Browser &b)
{
	if (b.kit_.request_render)
		b.kit_.request_render();
}

// --- Widgets -----------------------------------------------------------------

bool
shift_enter(int key, unsigned mods)
{
	if (mods != unsigned(Qt::ShiftModifier))
		return false;
	return key == Qt::Key_Return || key == Qt::Key_Enter;
}

void
open_new_window(const Browser &b, const string &path)
{
	if (path.empty() || !b.page_ || !b.page_->host ||
		!b.page_->host->new_window)
		return;
	b.page_->host->new_window(path);
}

void
show_file_context(
	const Browser &b, Kit &kit, const string &path, Rect anchor, bool kbd)
{
	if (!b.page_ || !b.page_->context || path.empty())
		return;
	b.page_->context->show(kit, QString::fromStdString(path), anchor, kbd);
}

bool
show_cursor_context(Browser &b, Kit &kit)
{
	if (b.cursor_ >= 0 && b.cursor_ < int(b.files_.size())) {
		show_file_context(b, kit, b.files_[size_t(b.cursor_)].path,
			b.files_[size_t(b.cursor_)].tile, true);
		return true;
	}
	if (b.dir_path_.isEmpty())
		return false;
	show_file_context(
		b, kit, b.dir_path_.toStdString(), {b.r.x, b.r.y, 0, 0}, true);
	return true;
}

struct SideRow : Button {
	string path;
	Browser *browser = nullptr;
	void measure(Kit &, float max_w, float) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, int key, unsigned mods) override;
};

void
SideRow::measure(Kit &, float max_w, float)
{
	this->r = {0, 0, max_w, kButtonH + kWinPadY * 2.0f};
}

bool
SideRow::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::RightButton) {
		if (this->browser)
			show_file_context(
				*this->browser, kit, this->path, {x, y, 0, 0}, false);
		return true;
	}
	if (button == Qt::MiddleButton) {
		kit.pressed_ = this;
		return true;
	}
	return Button::press(kit, x, y, button);
}

bool
SideRow::release(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::MiddleButton) {
		if (kit.pressed_ != this)
			return false;
		if (kit.hit(x, y) == this && this->browser && !this->path.empty()) {
			if (Page *page = this->browser->page_;
				page && page->host && page->host->new_window)
				page->host->new_window(this->path);
		}
		return true;
	}
	return Button::release(kit, x, y, button);
}

bool
SideRow::key(Kit &kit, int key, unsigned mods)
{
	if (context_key(key, mods)) {
		if (this->browser)
			show_file_context(*this->browser, kit, this->path, this->r, true);
		return true;
	}
	if (shift_enter(key, mods)) {
		if (this->browser)
			open_new_window(*this->browser, this->path);
		return true;
	}
	return Button::key(kit, key, mods);
}

void layout_grid(Browser &b, Rect area);

// --- GPU thumbnail input -----------------------------------------------------

void
copy_bgra16(const Image &src, uint16_t *dst, uint32_t dw, uint32_t dh)
{
	const size_t packed = size_t(dw) * kBytesPerPixel;
	for (uint32_t y = 0; y < dh; ++y)
		memcpy(dst + size_t(y) * dw * 4, row_u16(src, y), packed);
}

ThumbUpdate
make_thumb(shared_ptr<Cmm> cmm, const vector<uint8_t> &icc, const string &path,
	int64_t mtime, uint64_t size, int thumb_size, float dpr, int atlas_max,
	bool skip_cache)
{
	ThumbUpdate result;
	result.regeneration = skip_cache;
	if (!cmm)
		return result;
	const int tier = thumbnail_tier_for_height(
		max(1, int(ceil(double(thumb_size) * double(dpr)))));
	shared_ptr<Profile> screen = profile_from_icc(*cmm, icc);
	const ThumbnailSource source =
		thumbnail_source(QString::fromStdString(path), mtime, size);
	if (!skip_cache) {
		ThumbnailHit hit =
			thumbnail_cache_lookup(source, tier, cmm, screen.get());
		if (!hit.pixels.empty()) {
			ImagePtr image = image_new(hit.width, hit.height);
			if (!image)
				return result;
			memcpy(image->data.data(), hit.pixels.data(),
				hit.pixels.size() * sizeof(uint16_t));
			result.geometry_w = hit.image_width ? hit.image_width : hit.width;
			result.geometry_h =
				hit.image_height ? hit.image_height : hit.height;
			result.image = std::move(image);
			result.orientation = Orientation::Rotate0;
			result.transfer = profile_transfer(screen.get());
			result.interim = hit.interim;
			result.cache_bypass = hit.interim;
			result.tier = tier;
			result.failed = false;
			return result;
		}
	}
	OpenContext ctx;
	ctx.uri = QUrl::fromLocalFile(QString::fromStdString(path))
				  .toLocalFile()
				  .toStdString();
	ctx.cmm = cmm;
	const bool cacheable = !thumbnail_cache_root().isEmpty() &&
		!thumbnail_cache_contains(QString::fromStdString(path));
	ctx.screen_profile = cacheable ? cmm->get_profile_display_p3() : screen;
	ctx.first_frame_only = true;
	ctx.screen_dpi = 96;
	Error error;
	ImagePtr image = open(ctx, &error);
	if (!image || !image->width || !image->height)
		return result;
	const Orientation ori = orientation_or_0(image->orientation);
	orientation_display_size(image->width, image->height, ori,
		&result.geometry_w, &result.geometry_h);
	if (image->render && result.geometry_w && result.geometry_h) {
		uint32_t ow = 1, oh = 1;
		if (cacheable) {
			const int h = thumbnail_tier_height(tier);
			thumb_dest_params(result.geometry_w, result.geometry_h, h, 1.0f,
				h * kThumbWide, &ow, &oh);
		} else {
			thumb_dest_params(result.geometry_w, result.geometry_h, thumb_size,
				dpr, atlas_max, &ow, &oh);
		}
		const double scale = min(double(ow) / double(result.geometry_w),
			double(oh) / double(result.geometry_h));
		if (ImagePtr raster = image->render->render(
				cmm.get(), ctx.screen_profile.get(), scale))
			image = std::move(raster);
		else
			return {};
	}
	if (!image || !image->width || !image->height)
		return {};
	result.image = std::move(image);
	result.orientation = ori;
	result.transfer = profile_transfer(ctx.screen_profile.get());
	result.gpu_purpose =
		cacheable ? GpuPurpose::CacheDisplay : GpuPurpose::Display;
	result.tier = tier;
	result.failed = false;
	return result;
}

// --- Global execution --------------------------------------------------------

void apply_thumb(Browser &b, uint64_t gen, string path, ThumbUpdate update);
void apply_thumb_gpu(Browser &b, uint64_t gen, GpuPurpose purpose, int tier,
	ThumbScaler::Result result);
void enqueue_thumbs(Browser &b);

shared_ptr<Cmm>
worker_cmm()
{
	thread_local shared_ptr<Cmm> cmm = make_shared<Cmm>();
	return cmm;
}

bool
queue_gpu(Thumbnailer &thumbnailer, Thumbnailer::Client client,
	Browser *browser, uint64_t gen, Thumbnailer::Priority priority,
	GpuPurpose purpose, int tier, ThumbScaler::Job job, bool keyed = true)
{
	const string key = keyed ? job.path : string{};
	return thumbnailer.submit_gpu(
		client, gen, priority, std::move(job),
		[browser, gen, purpose, tier](ThumbScaler::Result result) mutable {
			apply_thumb_gpu(*browser, gen, purpose, tier, std::move(result));
		},
		key);
}

Thumbnailer::Completion
display_thumb(Thumbnailer &thumbnailer, Thumbnailer::Client client,
	Browser *browser, FinishJob job)
{
	auto cmm = worker_cmm();
	ThumbUpdate update;
	update.geometry_w = job.image_w;
	update.geometry_h = job.image_h;
	update.regeneration = true;
	shared_ptr<Profile> p3 = cmm->get_profile_display_p3();
	shared_ptr<Profile> screen = profile_from_icc(*cmm, job.screen_icc);
	vector<uint16_t> display = job.pixels ? *job.pixels : vector<uint16_t>{};
	if (p3 && screen && !display.empty() &&
		cmm->transform_bgra16(reinterpret_cast<uint8_t *>(display.data()),
			job.width, job.height, p3.get(), screen.get(), true, true)) {
		update.transfer = profile_transfer(screen.get());
		uint32_t ow = 1, oh = 1;
		thumb_dest_params(job.image_w, job.image_h, job.thumb_size, job.dpr,
			job.atlas_max, &ow, &oh);
		if (job.width == ow && job.height == oh) {
			update.ram = std::move(display);
			update.ram_w = ow;
			update.ram_h = oh;
			update.failed = false;
		} else {
			ThumbScaler::Job gpu;
			gpu.pixels = display.data();
			gpu.stride = size_t(job.width) * kBytesPerPixel;
			gpu.src_w = job.width;
			gpu.src_h = job.height;
			gpu.out_w = ow;
			gpu.out_h = oh;
			gpu.orientation = Orientation::Rotate0;
			gpu.transfer = update.transfer;
			gpu.path = job.path;
			update.gpu_pending = queue_gpu(thumbnailer, client, browser,
				job.gen, job.priority, GpuPurpose::Display, 0, std::move(gpu));
			update.failed = !update.gpu_pending;
		}
	}
	return [browser, gen = job.gen, path = std::move(job.path),
			   update = std::move(update)]() mutable {
		apply_thumb(*browser, gen, std::move(path), std::move(update));
	};
}

Thumbnailer::Completion
cache_thumb(Thumbnailer &thumbnailer, Thumbnailer::Client client,
	Browser *browser, FinishJob job)
{
	if (!job.pixels || job.pixels->empty())
		return {};
	const ThumbnailSource source =
		thumbnail_source(QString::fromStdString(job.path), job.mtime, job.size);
	QString error;
	if (!thumbnail_cache_write(source, job.tier, job.pixels->data(), job.width,
			job.height, job.image_w, job.image_h, &error) &&
		!error.isEmpty())
		fprintf(stderr, "%s: %s\n", job.path.c_str(), qUtf8Printable(error));

	if (job.tier > 0) {
		uint32_t ow = 1, oh = 1;
		const int next = job.tier - 1;
		const int h = thumbnail_tier_height(next);
		thumb_dest_params(
			job.image_w, job.image_h, h, 1.0f, h * kThumbWide, &ow, &oh);
		ThumbScaler::Job gpu;
		gpu.pixels = job.pixels->data();
		gpu.stride = size_t(job.width) * kBytesPerPixel;
		gpu.src_w = job.width;
		gpu.src_h = job.height;
		gpu.out_w = ow;
		gpu.out_h = oh;
		gpu.orientation = Orientation::Rotate0;
		gpu.transfer = Transfer::Srgb;
		gpu.path = job.path;
		(void) queue_gpu(thumbnailer, client, browser, job.gen,
			Thumbnailer::Priority::Maintenance, GpuPurpose::CacheOnly, next,
			std::move(gpu), false);
	}
	return {};
}

Thumbnailer::Completion
load_thumb(Thumbnailer &thumbnailer, Thumbnailer::Client client,
	Browser *browser, ThumbJob job)
{
	auto cmm = worker_cmm();
	ThumbUpdate update = make_thumb(cmm, job.screen_icc, job.path, job.mtime,
		job.size, job.thumb_size, job.dpr, job.atlas_max, job.skip_cache);
	if (!update.failed && update.image && update.image->width &&
		update.image->height) {
		uint32_t ow = 1, oh = 1;
		if (update.gpu_purpose == GpuPurpose::CacheDisplay) {
			const int h = thumbnail_tier_height(update.tier);
			thumb_dest_params(update.geometry_w, update.geometry_h, h, 1.0f,
				h * kThumbWide, &ow, &oh);
		} else {
			thumb_dest_params(update.geometry_w, update.geometry_h,
				job.thumb_size, job.dpr, job.atlas_max, &ow, &oh);
		}
		const Image &src = *update.image;
		const bool one_to_one = update.gpu_purpose == GpuPurpose::Display &&
			update.orientation == Orientation::Rotate0 && src.width == ow &&
			src.height == oh;
		if (one_to_one) {
			update.ram.resize(size_t(ow) * oh * 4);
			copy_bgra16(src, update.ram.data(), ow, oh);
			update.ram_w = ow;
			update.ram_h = oh;
		} else {
			ThumbScaler::Job gpu;
			gpu.pixels = reinterpret_cast<const uint16_t *>(src.data.data());
			gpu.stride = src.stride;
			gpu.src_w = src.width;
			gpu.src_h = src.height;
			gpu.out_w = ow;
			gpu.out_h = oh;
			gpu.orientation = update.orientation;
			gpu.transfer = update.transfer;
			gpu.path = job.path;
			const GpuPurpose purpose =
				update.gpu_purpose == GpuPurpose::Display && update.interim
				? GpuPurpose::DisplayInterim
				: update.gpu_purpose;
			update.gpu_pending = queue_gpu(thumbnailer, client, browser,
				job.gen, job.priority, purpose, update.tier, std::move(gpu));
			update.failed = !update.gpu_pending;
		}
		update.image.reset();
	}
	return [browser, gen = job.gen, path = std::move(job.path),
			   update = std::move(update)]() mutable {
		apply_thumb(*browser, gen, std::move(path), std::move(update));
	};
}

void
reset_thumb_atlas(Browser &b)
{
	for (Browser::File &f : b.files_)
		f.gpu = {};
	b.sheet_.clear();
	b.sheet_.grow(Sheet::kSize);
	if (b.kit_.renderer_)
		b.kit_.renderer_->reset_thumbs();
}

void
clear_gpu(Browser &b)
{
	for (Browser::File &f : b.files_) {
		if (!f.gpu.empty()) {
			b.sheet_.release(f.gpu);
			f.gpu = {};
		}
	}
}

void
invalidate_thumbs(Browser &b)
{
	++b.thumb_gen_;
	b.thumbnailer_.set_epoch(b.thumbnail_client_, b.thumb_gen_);
	b.thumb_inflight_.clear();
	reset_thumb_atlas(b);
	for (Browser::File &f : b.files_) {
		vector<uint16_t>().swap(f.ram);
		f.ram_w = f.ram_h = 0;
		f.ram_interim = false;
		f.ram_pending = false;
		f.cache_bypass = false;
		f.regen_failed = false;
		f.failed = false;
	}
}

size_t
ram_bytes(const Browser::File &f)
{
	return f.ram.capacity() * sizeof(uint16_t);
}

void
trim_ram(Browser &b)
{
	size_t total = 0;
	for (const Browser::File &f : b.files_)
		total += ram_bytes(f);
	if (total <= kThumbRamBudget)
		return;
	const float pad = row_h(b) * kPrefetchRows;
	const float mid = b.r.y + b.r.h * 0.5f;
	vector<int> idx;
	for (int i = 0; i < int(b.files_.size()); ++i) {
		const Browser::File &f = b.files_[size_t(i)];
		if (f.ram.empty() || thumb_in_band(b, f, pad))
			continue;
		idx.push_back(i);
	}
	sort(idx.begin(), idx.end(), [&](int a, int bidx) {
		const Rect &ca = b.files_[size_t(a)].cell;
		const Rect &cb = b.files_[size_t(bidx)].cell;
		const float da = abs(ca.y + ca.h * 0.5f - mid);
		const float db = abs(cb.y + cb.h * 0.5f - mid);
		return da > db;
	});
	for (int i : idx) {
		if (total <= kThumbRamBudget)
			break;
		Browser::File &f = b.files_[size_t(i)];
		total -= ram_bytes(f);
		vector<uint16_t>().swap(f.ram);
		f.ram_w = f.ram_h = 0;
		f.ram_interim = false;
		f.cache_bypass = false;
	}
}

bool
try_grow_sheet(Browser &b)
{
	const int cap = thumb_atlas_max(b);
	if (b.sheet_.w >= cap)
		return false;
	b.sheet_.grow(min(cap, b.sheet_.w * 2));
	return true;
}

bool
push_gpu(Browser &b, Browser::File &f, const Sheet::Packed &slot)
{
	Renderer *r = b.kit_.renderer_;
	if (!r)
		return false;
	bool recreated = false;
	if (!r->upload_thumb(f.ram.data(), f.ram_w, f.ram_h, slot.x, slot.y,
			b.sheet_.w, &recreated))
		return false;
	if (!recreated)
		return true;
	for (Browser::File &o : b.files_) {
		if (o.gpu.empty() || o.ram.empty())
			continue;
		if (!r->upload_thumb(
				o.ram.data(), o.ram_w, o.ram_h, o.gpu.x, o.gpu.y, b.sheet_.w))
			return false;
	}
	return true;
}

void
try_upload(Browser &b, Browser::File &f)
{
	if (f.ram.empty() || f.ram_w <= 0 || f.ram_h <= 0 || !f.gpu.empty())
		return;
	Sheet::Packed slot = b.sheet_.alloc(f.ram_w, f.ram_h);
	while (slot.empty() && try_grow_sheet(b))
		slot = b.sheet_.alloc(f.ram_w, f.ram_h);
	if (slot.empty()) {
		const float pad = row_h(b) * kPrefetchRows;
		for (Browser::File &o : b.files_) {
			if (o.gpu.empty() || thumb_in_band(b, o, pad))
				continue;
			b.sheet_.release(o.gpu);
			o.gpu = {};
		}
		slot = b.sheet_.alloc(f.ram_w, f.ram_h);
	}
	if (slot.empty()) {
		for (Browser::File &o : b.files_) {
			if (o.gpu.empty() || thumb_in_band(b, o, 0.0f))
				continue;
			b.sheet_.release(o.gpu);
			o.gpu = {};
		}
		slot = b.sheet_.alloc(f.ram_w, f.ram_h);
	}
	if (slot.empty())
		return;
	if (!push_gpu(b, f, slot)) {
		b.sheet_.release(slot);
		return;
	}
	f.gpu = slot;
}

void
apply_thumb_gpu(Browser &b, uint64_t gen, GpuPurpose purpose, int tier,
	ThumbScaler::Result res)
{
	if (gen != b.thumb_gen_ || res.path.empty())
		return;
	for (Browser::File &f : b.files_) {
		if (f.path != res.path)
			continue;
		Thumbnailer::Priority display_priority =
			Thumbnailer::Priority::Maintenance;
		if (auto active = b.thumb_inflight_.find(f.path);
			active != b.thumb_inflight_.end())
			display_priority = active->second;
		if (purpose == GpuPurpose::CacheDisplay ||
			purpose == GpuPurpose::CacheOnly) {
			if (res.failed || !res.out_w || !res.out_h || res.data.empty()) {
				if (purpose == GpuPurpose::CacheDisplay) {
					f.ram_pending = false;
					if (!f.ram.empty() && f.ram_interim)
						f.regen_failed = true;
					else
						f.failed = true;
					b.thumb_inflight_.erase(f.path);
				}
				break;
			}
			FinishJob cache;
			cache.gen = gen;
			cache.priority = Thumbnailer::Priority::Maintenance;
			cache.path = f.path;
			cache.mtime = f.mtime;
			cache.size = f.size;
			cache.image_w = f.image_w;
			cache.image_h = f.image_h;
			cache.width = res.out_w;
			cache.height = res.out_h;
			cache.pixels =
				make_shared<const vector<uint16_t>>(std::move(res.data));
			cache.tier = tier;
			cache.thumb_size = b.thumb_size_;
			cache.dpr = b.kit_.dpr_ > 0.0f ? b.kit_.dpr_ : 1.0f;
			cache.atlas_max = thumb_atlas_max(b);
			cache.screen_icc = screen_icc_bytes(b);
			Thumbnailer *thumbnailer = &b.thumbnailer_;
			const auto client = b.thumbnail_client_;
			Browser *browser = &b;
			if (purpose == GpuPurpose::CacheDisplay) {
				FinishJob display = cache;
				display.priority = display_priority;
				if (!thumbnailer->submit(
						client, gen, display_priority,
						[thumbnailer, client, browser,
							display = std::move(display)]() mutable {
							return display_thumb(*thumbnailer, client, browser,
								std::move(display));
						},
						f.path)) {
					f.ram_pending = false;
					f.failed = f.ram.empty();
					b.thumb_inflight_.erase(f.path);
				}
			}
			(void) thumbnailer->submit(client, gen,
				Thumbnailer::Priority::Maintenance,
				[thumbnailer, client, browser,
					cache = std::move(cache)]() mutable {
					return cache_thumb(
						*thumbnailer, client, browser, std::move(cache));
				});
			break;
		}

		f.ram_pending = false;
		b.thumb_inflight_.erase(f.path);
		if (res.failed) {
			if (purpose == GpuPurpose::DisplayRefit)
				f.regen_failed = false;
			else if (!f.ram.empty() && f.ram_interim)
				f.regen_failed = true;
			else
				f.failed = true;
			break;
		}
		if (!res.out_w || !res.out_h || res.data.empty())
			break;
		if (!f.gpu.empty())
			b.sheet_.release(f.gpu);
		f.gpu = {};
		f.ram = std::move(res.data);
		f.ram_w = int(res.out_w);
		f.ram_h = int(res.out_h);
		f.ram_interim = purpose == GpuPurpose::DisplayInterim ||
			purpose == GpuPurpose::DisplayRefit;
		f.cache_bypass = purpose == GpuPurpose::DisplayInterim;
		f.regen_failed = false;
		f.failed = false;
		if (thumb_in_band(b, f, row_h(b) * kPrefetchRows))
			try_upload(b, f);
		break;
	}
	trim_ram(b);
	request_render(b);
}

void
sync_thumbs(Browser &b)
{
	const float pad = row_h(b) * kPrefetchRows;
	for (Browser::File &f : b.files_) {
		if (f.gpu.empty() || thumb_in_band(b, f, pad))
			continue;
		b.sheet_.release(f.gpu);
		f.gpu = {};
	}
	for (Browser::File &f : b.files_) {
		if (!thumb_in_band(b, f, pad))
			continue;
		try_upload(b, f);
	}
	trim_ram(b);
	enqueue_thumbs(b);
}

void
apply_thumb(Browser &b, uint64_t gen, string path, ThumbUpdate update)
{
	if (!update.gpu_pending)
		b.thumb_inflight_.erase(path);
	if (gen != b.thumb_gen_)
		return;
	for (Browser::File &f : b.files_) {
		if (f.path != path)
			continue;
		if (update.failed && update.regeneration && !f.ram.empty() &&
			f.ram_interim) {
			f.ram_pending = false;
			f.regen_failed = true;
			break;
		}
		f.failed = update.failed;
		if (update.failed) {
			f.image_w = 0;
			f.image_h = 0;
			b.size_cache_.erase(f.path);
		} else {
			f.image_w = update.geometry_w;
			f.image_h = update.geometry_h;
			b.size_cache_[f.path] = {
				f.mtime, f.size, update.geometry_w, update.geometry_h};
		}
		if (update.failed || !update.ram.empty() || update.gpu_pending) {
			if (!update.gpu_pending && !f.gpu.empty()) {
				b.sheet_.release(f.gpu);
				f.gpu = {};
			}
		}
		if (update.failed) {
			vector<uint16_t>().swap(f.ram);
			f.ram_w = f.ram_h = 0;
			f.ram_interim = false;
			f.ram_pending = false;
			f.cache_bypass = false;
		} else if (update.gpu_pending) {
			f.ram_pending = true;
			f.transfer = update.transfer;
			if (!update.regeneration)
				f.ram_interim = update.interim;
			if (!update.regeneration)
				f.cache_bypass = update.cache_bypass;
		} else if (!update.ram.empty() && update.ram_w && update.ram_h) {
			f.ram = std::move(update.ram);
			f.ram_w = int(update.ram_w);
			f.ram_h = int(update.ram_h);
			f.ram_interim = update.interim;
			f.cache_bypass = update.cache_bypass;
			f.ram_pending = false;
			f.regen_failed = false;
			f.transfer = update.transfer;
			if (thumb_in_band(b, f, row_h(b) * kPrefetchRows))
				try_upload(b, f);
			trim_ram(b);
		}
		break;
	}
	request_render(b);
}

void
enqueue_thumbs(Browser &b)
{
	if (!b.thumbnail_client_)
		return;
	const float pad = row_h(b) * kPrefetchRows;
	vector<int> vis, pre, sizes;
	for (int i = 0; i < int(b.files_.size()); ++i) {
		const Browser::File &f = b.files_[size_t(i)];
		const bool visible = thumb_in_band(b, f, 0.0f);
		const bool prefetched = !visible && thumb_in_band(b, f, pad);
		// The viewport defines the current priority exactly.  In particular,
		// demote work from the old viewport before adding new visible jobs.
		if (auto active = b.thumb_inflight_.find(f.path);
			active != b.thumb_inflight_.end()) {
			const Thumbnailer::Priority desired = visible
				? Thumbnailer::Priority::Visible
				: prefetched ? Thumbnailer::Priority::Prefetch
							 : Thumbnailer::Priority::Dimensions;
			if (desired != active->second &&
				b.thumbnailer_.reprioritize(
					b.thumbnail_client_, b.thumb_gen_, desired, f.path))
				active->second = desired;
			continue;
		}
		if (f.failed || f.ram_pending || (f.regen_failed && f.ram_interim) ||
			(!f.ram.empty() && !f.ram_interim))
			continue;
		if (visible)
			vis.push_back(i);
		else if (prefetched)
			pre.push_back(i);
		else if (!f.image_w || !f.image_h)
			sizes.push_back(i);
	}
	ThumbJob proto;
	proto.gen = b.thumb_gen_;
	proto.thumb_size = b.thumb_size_;
	proto.dpr = b.kit_.dpr_ > 0.0f ? b.kit_.dpr_ : 1.0f;
	proto.atlas_max = thumb_atlas_max(b);
	proto.screen_icc = screen_icc_bytes(b);
	auto push = [&](const vector<int> &idx, Thumbnailer::Priority priority) {
		for (int i : idx) {
			const Browser::File &f = b.files_[size_t(i)];
			ThumbJob job = proto;
			job.priority = priority;
			job.path = f.path;
			job.mtime = f.mtime;
			job.size = f.size;
			job.skip_cache = f.cache_bypass;
			Thumbnailer *thumbnailer = &b.thumbnailer_;
			const auto client = b.thumbnail_client_;
			Browser *browser = &b;
			if (thumbnailer->submit(
					client, job.gen, priority,
					[thumbnailer, client, browser,
						job = std::move(job)]() mutable {
						return load_thumb(
							*thumbnailer, client, browser, std::move(job));
					},
					f.path))
				b.thumb_inflight_.emplace(f.path, priority);
		}
	};
	push(vis, Thumbnailer::Priority::Visible);
	push(pre, Thumbnailer::Priority::Prefetch);
	size_t dimensions = 0;
	for (const auto &[path, priority] : b.thumb_inflight_) {
		(void) path;
		if (priority == Thumbnailer::Priority::Dimensions)
			dimensions++;
	}
	const size_t limit = b.thumbnailer_.background_limit();
	if (dimensions < limit) {
		const size_t room = limit - dimensions;
		if (sizes.size() > room)
			sizes.resize(room);
		push(sizes, Thumbnailer::Priority::Dimensions);
	}
}

// --- Thumbnail layout --------------------------------------------------------

enum class CursorDir : uint8_t { Left, Right, Up, Down };

int
find_cursor_row(const Browser &b)
{
	if (b.cursor_ < 0)
		return -1;
	for (int i = 0; i < int(b.rows_.size()); ++i) {
		const Browser::GridRow &row = b.rows_[size_t(i)];
		if (b.cursor_ >= row.first && b.cursor_ < row.first + row.count)
			return i;
	}
	return -1;
}

void
clear_cursor(Browser &b)
{
	b.cursor_ = -1;
	b.cursor_x_ = 0;
	b.cursor_x_dirty_ = false;
}

void
remember_cursor_x(Browser &b)
{
	if (b.cursor_ < 0 || b.cursor_ >= int(b.files_.size()))
		return;
	const Rect &c = b.files_[size_t(b.cursor_)].cell;
	if (c.w <= 0.0f) {
		b.cursor_x_dirty_ = true;
		return;
	}
	b.cursor_x_ = c.x + c.w * 0.5f;
	b.cursor_x_dirty_ = false;
}

void
remember_cursor_x_at(Browser &b, float x)
{
	if (b.cursor_ < 0 || b.cursor_ >= int(b.files_.size()))
		return;
	const Rect &c = b.files_[size_t(b.cursor_)].cell;
	if (c.w <= 0.0f) {
		b.cursor_x_dirty_ = true;
		return;
	}
	b.cursor_x_ = clamp(x, c.x, c.x + c.w);
	b.cursor_x_dirty_ = false;
}

void
select_closest(Browser &b, const Browser::GridRow &row, float target_x)
{
	float closest = 1e30f;
	for (int i = 0; i < row.count; ++i) {
		const int fi = row.first + i;
		if (fi < 0 || fi >= int(b.files_.size()))
			break;
		const Rect &cell = b.files_[size_t(fi)].cell;
		const float d = abs(cell.x + cell.w * 0.5f - target_x);
		if (d > closest)
			break;
		b.cursor_ = fi;
		closest = d;
	}
}

void
scroll_to_row(Browser &b, const Browser::GridRow &row)
{
	const float vis = max(0.0f, b.r.h - 2.0f * kGridPad);
	if (row.y < b.scroll_.offset)
		b.scroll_.offset = row.y;
	else if (row.y + row.h > b.scroll_.offset + vis)
		b.scroll_.offset = max(0.0f, row.y + row.h - vis);
	b.scroll_.offset =
		b.kit_.snap(clamp(b.scroll_.offset, 0.0f, b.scroll_.max_offset()));
}

void
page_scroll(Browser &b, int dir)
{
	const float vis = max(0.0f, b.r.h);
	const float rh = row_h(b);
	const float step = vis > rh ? vis - rh : vis;
	b.scroll_.offset = b.kit_.snap(clamp(
		b.scroll_.offset + float(dir) * step, 0.0f, b.scroll_.max_offset()));
	request_render(b);
}

void
move_cursor(Browser &b, CursorDir dir)
{
	if (b.rows_.empty())
		return;
	if (b.cursor_ < 0) {
		int row_i = 0;
		if (dir == CursorDir::Right || dir == CursorDir::Down) {
			b.cursor_ = b.rows_.front().first;
		} else {
			row_i = int(b.rows_.size()) - 1;
			const Browser::GridRow &row = b.rows_.back();
			b.cursor_ = row.first + row.count - 1;
		}
		remember_cursor_x(b);
		scroll_to_row(b, b.rows_[size_t(row_i)]);
		request_render(b);
		return;
	}
	int row_i = find_cursor_row(b);
	if (row_i < 0)
		return;
	const Browser::GridRow &cur = b.rows_[size_t(row_i)];
	const int col_i = b.cursor_ - cur.first;
	switch (dir) {
	case CursorDir::Left:
		if (col_i > 0)
			b.cursor_ = cur.first + col_i - 1;
		else if (row_i > 0) {
			const Browser::GridRow &prev = b.rows_[size_t(row_i - 1)];
			b.cursor_ = prev.first + prev.count - 1;
		}
		remember_cursor_x(b);
		break;
	case CursorDir::Right:
		if (col_i + 1 < cur.count)
			b.cursor_ = cur.first + col_i + 1;
		else if (row_i + 1 < int(b.rows_.size()))
			b.cursor_ = b.rows_[size_t(row_i + 1)].first;
		remember_cursor_x(b);
		break;
	case CursorDir::Up:
		if (row_i > 0)
			select_closest(b, b.rows_[size_t(row_i - 1)], b.cursor_x_);
		break;
	case CursorDir::Down:
		if (row_i + 1 < int(b.rows_.size()))
			select_closest(b, b.rows_[size_t(row_i + 1)], b.cursor_x_);
		break;
	}
	row_i = find_cursor_row(b);
	if (row_i >= 0)
		scroll_to_row(b, b.rows_[size_t(row_i)]);
	request_render(b);
}

void
move_cursor_home(Browser &b)
{
	if (b.rows_.empty())
		return;
	const Browser::GridRow &row = b.rows_.front();
	b.cursor_ = row.first;
	remember_cursor_x(b);
	scroll_to_row(b, row);
	request_render(b);
}

void
move_cursor_end(Browser &b)
{
	if (b.rows_.empty())
		return;
	const Browser::GridRow &row = b.rows_.back();
	b.cursor_ = row.first + row.count - 1;
	remember_cursor_x(b);
	scroll_to_row(b, row);
	request_render(b);
}

void
layout_grid(Browser &b, Rect area)
{
	const Rect inner = area.inset(kGridPad, kGridPad);
	const float th = float(b.thumb_size_);
	const float ch = chrome();
	const float avail = inner.w;
	const float dpr = max(b.kit_.dpr_, 0.01f);
	const bool grid = b.view_ == BrowserView::Grid;
	struct Item {
		int i;
		float w;
		float h;
	};
	vector<Item> row;
	float row_w = 0.0f;
	float y = 0.0f;
	b.rows_.clear();

	auto flush = [&]() {
		if (row.empty())
			return;
		float band = grid ? th : 0.0f;
		float cap_band = 0.0f;
		for (const Item &it : row) {
			band = max(band, it.h);
			cap_band = max(cap_band, b.files_[size_t(it.i)].cap.h);
		}
		const float rh = band + 2.0f * ch + cap_band;
		const float extra = max(0.0f, avail - row_w) * 0.5f;
		float x = b.kit_.snap(inner.x + extra);
		for (const Item &it : row) {
			Browser::File &f = b.files_[size_t(it.i)];
			const float reserved_w = grid ? th : it.w;
			const float ow = reserved_w + 2.0f * ch;
			f.tile = b.kit_.snap_rect({x + ch + (reserved_w - it.w) * 0.5f,
				inner.y + y - b.scroll_.offset + ch + (band - it.h) * 0.5f,
				it.w, it.h});
			f.cell =
				b.kit_.snap_rect({x, inner.y + y - b.scroll_.offset, ow, rh});
			f.cap.x = f.cell.x;
			f.cap.y = b.kit_.snap(f.cell.y + band + 2.0f * ch);
			x = f.cell.x + f.cell.w + kThumbGap;
		}
		b.rows_.push_back({row.front().i, int(row.size()), y, rh});
		y += rh + kThumbGap;
		row.clear();
		row_w = 0.0f;
	};

	for (int i = 0; i < int(b.files_.size()); ++i) {
		Browser::File &f = b.files_[size_t(i)];
		float tw = th;
		float ih = th;
		if (f.image_w && f.image_h) {
			uint32_t fit_w = 1, fit_h = 1;
			thumb_dest(b, f.image_w, f.image_h, &fit_w, &fit_h);
			tw = float(fit_w) / dpr;
			ih = float(fit_h) / dpr;
		}
		const float cap_w = float(grid ? 1 : kThumbWide) * th;
		float fit = 1.0f;
		if (tw > 0.0f)
			fit = min(fit, cap_w / tw);
		if (ih > 0.0f)
			fit = min(fit, th / ih);
		tw = max(1.0f, b.kit_.snap(tw * fit));
		ih = max(1.0f, b.kit_.snap(ih * fit));
		const float ow = (grid ? th : tw) + 2.0f * ch;
		if (!b.show_names_) {
			f.cap = {};
			f.cap_text.clear();
		} else if (f.cap.w != ow) {
			f.cap_text =
				b.kit_.elide_lines(caption_name(f.name), ow, kCapLines, false);
			f.cap = {0.0f, 0.0f, ow,
				b.kit_.text_height(f.cap_text, ow, false) + kCapPad};
		}
		if (!row.empty() && row_w + kThumbGap + ow > avail + 0.01f)
			flush();
		if (!row.empty())
			row_w += kThumbGap;
		row.push_back({i, tw, ih});
		row_w += ow;
	}
	flush();
	if (y > 0.0f)
		y -= kThumbGap;
	if (b.cursor_ >= int(b.files_.size()))
		clear_cursor(b);
	if (b.cursor_ < 0 || b.cursor_ >= int(b.files_.size())) {
		b.layout_cursor_ = -1;
		b.layout_cell_x_ = 0;
		b.layout_w_ = 0;
	} else {
		const Rect &cell = b.files_[size_t(b.cursor_)].cell;
		const float cx = cell.x + cell.w * 0.5f;
		if (b.cursor_x_dirty_ || abs(area.w - b.layout_w_) > 0.5f ||
			(b.cursor_ == b.layout_cursor_ &&
				abs(cx - b.layout_cell_x_) > 0.5f))
			remember_cursor_x(b);
		b.layout_cursor_ = b.cursor_;
		b.layout_cell_x_ = cx;
		b.layout_w_ = area.w;
	}
	const float prev = b.scroll_.offset;
	b.scroll_.set_metrics(y + kGridPad * 2.0f, area.h);
	b.scroll_.offset = b.kit_.snap(b.scroll_.offset);
	b.scroll_.clamp();
	if (b.scroll_.offset != prev) {
		const float dy = prev - b.scroll_.offset;
		for (Browser::File &f : b.files_) {
			f.tile.y += dy;
			f.cell.y += dy;
			f.cap.y += dy;
			f.tile = b.kit_.snap_rect(f.tile);
			f.cell = b.kit_.snap_rect(f.cell);
		}
	}
}

void
draw_checker(Kit &kit, const Rect &tile)
{
	if (tile.w <= 0.0f || tile.h <= 0.0f)
		return;
	kit.list_.push_clip(tile.x, tile.y, tile.x + tile.w, tile.y + tile.h);
	const Colour bg = kit.toolbar_bottom_;
	const Colour fg = kit.well_;
	kit.list_.add_rect_filled(
		tile.x, tile.y, tile.x + tile.w, tile.y + tile.h, bg);
	const int nx = max(1, int(ceil(double(tile.w / kCheck))));
	const int ny = max(1, int(ceil(double(tile.h / kCheck))));
	for (int j = 0; j < ny; ++j) {
		for (int i = 0; i < nx; ++i) {
			if (((i + j) & 1) == 0)
				continue;
			const float x0 = tile.x + float(i) * kCheck;
			const float y0 = tile.y + float(j) * kCheck;
			kit.list_.add_rect_filled(x0, y0, x0 + kCheck, y0 + kCheck, fg);
		}
	}
	kit.list_.pop_clip();
}

int
hit_file(const Browser &b, float x, float y)
{
	for (int i = 0; i < int(b.files_.size()); ++i) {
		if (b.files_[size_t(i)].tile.contains(x, y))
			return i;
	}
	return -1;
}

bool
same_path(const filesystem::path &a, const filesystem::path &b)
{
	error_code ec;
	if (filesystem::equivalent(a, b, ec) && !ec)
		return true;
	return a == b;
}

filesystem::path
without_trailing_sep(filesystem::path p)
{
	while (p.filename().empty()) {
		const filesystem::path parent = p.parent_path();
		if (parent.empty() || parent == p)
			break;
		p = parent;
	}
	return p;
}

string
dir_basename(const filesystem::path &dir)
{
	const filesystem::path p = without_trailing_sep(dir);
	string s = p.filename().string();
	if (s.empty() || s == ".")
		s = p.string();
	return s.empty() ? string("/") : s;
}

QString
abs_dir(const QString &path)
{
	const QFileInfo info(path);
	const QString dir =
		info.isDir() ? info.absoluteFilePath() : info.absolutePath();
	return QDir(dir).absolutePath();
}

int
browse_cmp(const BrowseSetup &setup, const QString &name_a, int64_t mtime_a,
	const QString &name_b, int64_t mtime_b)
{
	int cmp = 0;
	if (setup.sort == SortField::Time) {
		if (mtime_a < mtime_b)
			cmp = -1;
		else if (mtime_a > mtime_b)
			cmp = 1;
	}
	if (cmp == 0)
		cmp = name_a.localeAwareCompare(name_b);
	return setup.sort_desc ? -cmp : cmp;
}

int64_t
path_mtime_ms(const string &path)
{
	return QFileInfo(QString::fromStdString(path))
		.lastModified()
		.toMSecsSinceEpoch();
}

struct DirEnt {
	string path;
	string name;
	int64_t mtime = 0;
};

bool
dir_ent_less(const BrowseSetup &setup, const DirEnt &a, const DirEnt &c)
{
	return browse_cmp(setup, QString::fromStdString(a.name), a.mtime,
		QString::fromStdString(c.name), c.mtime) < 0;
}

vector<string>
list_subdirs(const filesystem::path &dir, const BrowseSetup &setup)
{
	vector<DirEnt> kids;
	error_code ec;
	for (const auto &ent : filesystem::directory_iterator(dir, ec)) {
		if (ec)
			break;
		error_code fec;
		const string name = ent.path().filename().string();
		if (setup.filter_files && hidden_name(name))
			continue;
		if (!ent.is_directory(fec) || fec)
			continue;
		DirEnt kid;
		kid.path = ent.path().string();
		kid.name = name;
		kid.mtime = path_mtime_ms(kid.path);
		kids.push_back(std::move(kid));
	}
	sort(kids.begin(), kids.end(), [&](const DirEnt &a, const DirEnt &c) {
		return dir_ent_less(setup, a, c);
	});
	vector<string> out;
	out.reserve(kids.size());
	for (const DirEnt &kid : kids)
		out.push_back(kid.path);
	return out;
}

int
index_of_dir(const vector<string> &dirs, const filesystem::path &self)
{
	for (int i = 0; i < int(dirs.size()); ++i) {
		if (same_path(dirs[size_t(i)], self))
			return i;
	}
	return -1;
}

string
parent_dir(const filesystem::path &dir)
{
	const filesystem::path p = without_trailing_sep(dir);
	const filesystem::path parent = p.parent_path();
	if (parent.empty() || parent == p)
		return {};
	return parent.string();
}

string
last_deep_subdir(
	const string &dir, unordered_set<string> *seen, const BrowseSetup &setup)
{
	unordered_set<string> local;
	if (!seen)
		seen = &local;
	error_code ec;
	string key = filesystem::weakly_canonical(dir, ec).string();
	if (key.empty())
		key = dir;
	if (!seen->insert(key).second)
		return dir;
	const vector<string> kids = list_subdirs(dir, setup);
	if (kids.empty())
		return dir;
	return last_deep_subdir(kids.back(), seen, setup);
}

string
next_dir_within_parents(const filesystem::path &dir, const BrowseSetup &setup)
{
	const string parent = parent_dir(dir);
	if (parent.empty())
		return {};
	const vector<string> sibs = list_subdirs(parent, setup);
	const int i = index_of_dir(sibs, dir);
	if (i >= 0 && i + 1 < int(sibs.size()))
		return sibs[size_t(i + 1)];
	return next_dir_within_parents(parent, setup);
}

string
tree_prev_dir(const filesystem::path &dir, const BrowseSetup &setup)
{
	const string parent = parent_dir(dir);
	if (parent.empty())
		return {};
	const vector<string> sibs = list_subdirs(parent, setup);
	const int i = index_of_dir(sibs, dir);
	if (i > 0)
		return last_deep_subdir(sibs[size_t(i - 1)], nullptr, setup);
	return parent;
}

string
tree_next_dir(const filesystem::path &dir, const BrowseSetup &setup)
{
	const vector<string> kids = list_subdirs(dir, setup);
	if (!kids.empty())
		return kids.front();
	return next_dir_within_parents(dir, setup);
}

void
push_place(Browser &b, const filesystem::path &root, string path,
	const char *name, const char *icon)
{
	Browser::DirRow row;
	row.path = std::move(path);
	row.name = name;
	row.icon = icon;
	row.current = same_path(root, row.path);
	b.side_dirs_.push_back(std::move(row));
}

// --- Scan --------------------------------------------------------------------

void
scan_dir(Browser &b)
{
	string keep;
	if (b.cursor_ >= 0 && b.cursor_ < int(b.files_.size()))
		keep = b.files_[size_t(b.cursor_)].path;
	vector<Browser::File> old = std::move(b.files_);
	b.files_.clear();
	b.side_dirs_.clear();
	b.places_dirty_ = true;
	if (b.dir_path_.isEmpty()) {
		clear_cursor(b);
		return;
	}

	const filesystem::path root(b.dir_path_.toStdString());
	error_code ec;
	vector<Browser::File> files;
	vector<DirEnt> children;
	for (const auto &ent : filesystem::directory_iterator(root, ec)) {
		if (ec)
			break;
		error_code fec;
		const string name = ent.path().filename().string();
		if (b.setup_.filter_files && hidden_name(name))
			continue;
		if (ent.is_directory(fec) && !fec) {
			DirEnt kid;
			kid.path = ent.path().string();
			kid.name = name;
			kid.mtime = path_mtime_ms(kid.path);
			children.push_back(std::move(kid));
			continue;
		}
		if (!ent.is_regular_file(fec) || fec)
			continue;
		if (b.setup_.filter_files &&
			!is_image_ext(QString::fromStdString(name)))
			continue;
		Browser::File f;
		f.path = ent.path().string();
		f.name = name;
		QFileInfo info(QString::fromStdString(f.path));
		f.mtime = info.lastModified().toMSecsSinceEpoch();
		f.size = uint64_t(max<qint64>(0, info.size()));
		auto cached = b.size_cache_.find(f.path);
		if (cached != b.size_cache_.end()) {
			if (cached->second.mtime == f.mtime &&
				cached->second.size == f.size) {
				f.image_w = cached->second.w;
				f.image_h = cached->second.h;
			} else {
				b.size_cache_.erase(cached);
			}
		}
		files.push_back(std::move(f));
	}

	sort(files.begin(), files.end(),
		[&](const Browser::File &a, const Browser::File &bfile) {
			return browse_cmp(b.setup_, QString::fromStdString(a.name),
				a.mtime, QString::fromStdString(bfile.name),
				bfile.mtime) < 0;
		});

	for (Browser::File &f : files) {
		for (Browser::File &o : old) {
			if (o.path != f.path)
				continue;
			if (o.mtime != f.mtime || o.size != f.size)
				break;
			f.image_w = o.image_w;
			f.image_h = o.image_h;
			f.ram = std::move(o.ram);
			f.ram_w = o.ram_w;
			f.ram_h = o.ram_h;
			f.ram_interim = o.ram_interim;
			f.ram_pending = o.ram_pending;
			f.cache_bypass = o.cache_bypass;
			f.regen_failed = o.regen_failed;
			f.transfer = o.transfer;
			f.gpu = o.gpu;
			o.gpu = {};
			f.failed = o.failed;
			break;
		}
	}
	for (Browser::File &o : old) {
		if (!o.gpu.empty())
			b.sheet_.release(o.gpu);
	}
	b.files_ = std::move(files);
	clear_cursor(b);
	if (!keep.empty()) {
		for (int i = 0; i < int(b.files_.size()); ++i) {
			if (b.files_[size_t(i)].path == keep) {
				b.cursor_ = i;
				remember_cursor_x(b);
				break;
			}
		}
	}

	push_place(b, root, "/", "Computer", "computer-symbolic");
	push_place(
		b, root, QDir::homePath().toStdString(), "Home", "go-home-symbolic");
	{
		const QString pictures =
			QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);
		const QFileInfo info(pictures);
		if (info.isDir()) {
			const string name = info.fileName().toStdString();
			if (!name.empty())
				push_place(b, root, pictures.toStdString(), name.c_str(),
					"image-symbolic");
		}
	}
	b.side_dirs_.push_back({});

	vector<filesystem::path> ancestors;
	filesystem::path cur = without_trailing_sep(root);
	for (;;) {
		filesystem::path parent = cur.parent_path();
		if (parent.empty() || parent == cur)
			break;
		ancestors.push_back(parent);
		cur = parent;
		if (ancestors.size() > 64)
			break;
	}
	reverse(ancestors.begin(), ancestors.end());
	for (const filesystem::path &p : ancestors) {
		Browser::DirRow row;
		row.path = p.string();
		row.name = dir_basename(p);
		row.icon = "go-up-symbolic";
		b.side_dirs_.push_back(std::move(row));
	}
	{
		Browser::DirRow row;
		row.path = without_trailing_sep(root).string();
		row.name = dir_basename(root);
		row.icon = "dot-large-symbolic";
		row.current = true;
		b.side_dirs_.push_back(std::move(row));
	}
	sort(children.begin(), children.end(),
		[&](const DirEnt &a, const DirEnt &c) {
			return dir_ent_less(b.setup_, a, c);
		});
	for (const DirEnt &kid : children) {
		Browser::DirRow row;
		row.path = kid.path;
		row.name = dir_basename(kid.path);
		row.icon = "go-down-symbolic";
		b.side_dirs_.push_back(std::move(row));
	}
}

float
places_scroll(const Browser &b)
{
	return b.places_ ? b.places_->scroll_.offset : 0;
}

void
push_hist(vector<Browser::HistEntry> &st, const Browser &b)
{
	if (b.dir_path_.isEmpty())
		return;
	st.push_back({b.dir_path_, places_scroll(b)});
}

void
open_directory(
	Browser &b, const QString &path, bool record = true, float side_scroll = 0)
{
	const QString dir = abs_dir(path);
	if (dir == b.dir_path_)
		return;
	if (record) {
		b.hist_forward_.clear();
		push_hist(b.hist_back_, b);
	}
	b.dir_path_ = dir;
	b.size_cache_.clear();
	++b.thumb_gen_;
	b.thumbnailer_.set_epoch(b.thumbnail_client_, b.thumb_gen_);
	b.thumb_inflight_.clear();
	reset_thumb_atlas(b);
	b.scroll_.offset = 0;
	if (b.places_)
		b.places_->scroll_.offset = side_scroll;
	scan_dir(b);
	enqueue_thumbs(b);
	request_render(b);
}

void
set_thumb_size(Browser &b, int size)
{
	if (size == b.thumb_size_)
		return;
	b.thumb_size_ = size;
	++b.thumb_gen_;
	b.thumbnailer_.set_epoch(b.thumbnail_client_, b.thumb_gen_);
	b.thumb_inflight_.clear();
	clear_gpu(b);
	for (Browser::File &f : b.files_) {
		f.ram_pending = false;
		if (f.ram.empty() || f.ram_w <= 0 || f.ram_h <= 0 || !f.image_w ||
			!f.image_h)
			continue;
		uint32_t ow = 1, oh = 1;
		thumb_dest(b, f.image_w, f.image_h, &ow, &oh);
		f.ram_interim = true;
		f.cache_bypass = false;
		f.regen_failed = false;
		f.failed = false;
		if (uint32_t(f.ram_w) != ow || uint32_t(f.ram_h) != oh) {
			ThumbScaler::Job job;
			job.pixels = f.ram.data();
			job.stride = size_t(f.ram_w) * kBytesPerPixel;
			job.src_w = uint32_t(f.ram_w);
			job.src_h = uint32_t(f.ram_h);
			job.out_w = ow;
			job.out_h = oh;
			job.orientation = Orientation::Rotate0;
			job.transfer = f.transfer;
			job.path = f.path;
			f.ram_pending = queue_gpu(b.thumbnailer_, b.thumbnail_client_, &b,
				b.thumb_gen_, Thumbnailer::Priority::Visible,
				GpuPurpose::DisplayRefit, 0, std::move(job));
		}
	}
	enqueue_thumbs(b);
	request_render(b);
}

void
set_view(Browser &b, BrowserView view)
{
	if (view == b.view_)
		return;
	b.view_ = view;
	request_render(b);
}

// --- Toolbar -----------------------------------------------------------------

void
pack_standin_icons(Browser &b)
{
	const float dpr = b.kit_.dpr_ > 0.0f ? b.kit_.dpr_ : 1.0f;
	const int px =
		max(1, int(lround(double(b.thumb_size_) * 0.5 * double(dpr))));
	b.kit_.pack_icon(kPendingIcon, px);
	b.kit_.pack_icon(kMissingIcon, px);
}

void
pack_toolbar_icons(Browser &b)
{
	const int px = max(16, int(lround(kIconPx * b.kit_.dpr_)));
	for (const Spec &spec : kItems) {
		const ActionDef &d = action_def(spec.action);
		b.kit_.pack_icon(action_icon(d, false), px);
		if (d.icon[1])
			b.kit_.pack_icon(d.icon[1], px);
	}
	b.kit_.pack_icon(kMoreIcon, px);
	b.kit_.pack_icon("go-up-symbolic", px);
	b.kit_.pack_icon("go-down-symbolic", px);
	b.kit_.pack_icon("dot-large-symbolic", px);
	b.kit_.pack_icon("computer-symbolic", px);
	b.kit_.pack_icon("go-home-symbolic", px);
	b.kit_.pack_icon("image-symbolic", px);
	b.kit_.pack_icon("open-menu-symbolic", px);
}

bool
set_dpr(Browser &b, float dpr)
{
	const float prev = b.kit_.dpr_;
	if (!b.kit_.set_dpr(dpr))
		return false;
	pack_toolbar_icons(b);
	if (abs(prev - b.kit_.dpr_) >= 0.01f && !b.files_.empty()) {
		invalidate_thumbs(b);
		enqueue_thumbs(b);
		for (Browser::File &f : b.files_) {
			f.cap = {};
			f.cap_text.clear();
		}
	}
	return true;
}

bool
spec_enabled(const Browser &b, Action action)
{
	const int idx = thumb_size_index(b.thumb_size_);
	switch (action) {
	case Action::DirPrev:
		return !b.dir_path_.isEmpty() &&
			!parent_dir(b.dir_path_.toStdString()).empty();
	case Action::DirNext:
		return !b.dir_path_.isEmpty() &&
			!tree_next_dir(b.dir_path_.toStdString(), b.setup_).empty();
	case Action::DirParent: {
		if (b.dir_path_.isEmpty())
			return false;
		const QFileInfo info(b.dir_path_);
		const QString parent = info.dir().absolutePath();
		return !parent.isEmpty() && parent != b.dir_path_;
	}
	case Action::ThumbPlus:
		return idx + 1 < kThumbSizeN;
	case Action::ThumbMinus:
		return idx > 0;
	case Action::ViewList:
		return false;
	case Action::Reload:
		return !b.dir_path_.isEmpty();
	case Action::Copy:
		return b.cursor_ >= 0 && b.cursor_ < int(b.files_.size());
	case Action::Trash:
		return b.cursor_ >= 0 && b.cursor_ < int(b.files_.size()) &&
			QFileInfo(QString::fromStdString(b.files_[size_t(b.cursor_)].path))
				.isFile();
	default:
		return true;
	}
}

bool
spec_active(const Browser &b, Action action)
{
	switch (action) {
	case Action::Sidebar:
		return b.page_ ? b.page_->sidebar_open : true;
	case Action::Filenames:
		return b.show_names_;
	case Action::Filter:
		return b.setup_.filter_files;
	case Action::SortName:
		return b.setup_.sort == SortField::Name;
	case Action::SortTime:
		return b.setup_.sort == SortField::Time;
	case Action::ViewTile:
		return b.view_ == BrowserView::Tile;
	case Action::ViewGrid:
		return b.view_ == BrowserView::Grid;
	case Action::SortDir:
		return b.setup_.sort_desc;
	case Action::Fullscreen:
		return b.kit_.fullscreen_;
	case Action::DarkMode:
		return b.kit_.dark_;
	default:
		return false;
	}
}

unique_ptr<Widget>
make_item(Browser &b, const Spec &spec)
{
	if (spec.kind == Kind::Sep)
		return make_unique<Sep>();
	auto n = make_unique<Button>();
	const Action action = spec.action;
	const ActionDef &d = action_def(action);
	const bool on = spec_active(b, action);
	n->action = action;
	if (spec.kind == Kind::Text) {
		n->pad_x = 2.0f;
		n->text = action == Action::SortTime ? QStringLiteral("Time")
											 : QStringLiteral("Name");
	}
	n->icon = spec.kind == Kind::Text ? nullptr : action_icon(d, on);
	n->enabled_ = spec_enabled(b, action);
	n->active = on;
	n->tip_text = action_tip(d, on);
	n->tip_accel = action_accel(d);
	n->on_click = [&b, action](Kit &) {
		if (b.page_ && b.page_->actor.apply)
			b.page_->actor.apply(action);
	};
	return n;
}

unique_ptr<ToolbarSlot>
make_slot_row(Browser &b, Slot slot)
{
	auto row = make_unique<ToolbarSlot>();
	row->gap = kItemGap;
	for (const Spec &spec : kItems) {
		if (spec.slot == slot)
			row->add_item(make_item(b, spec));
	}
	return row;
}

unique_ptr<Toolbar>
make_toolbar(Browser &b)
{
	auto left = make_slot_row(b, Slot::Left);
	auto mid = make_slot_row(b, Slot::Middle);
	auto right = make_slot_row(b, Slot::Right);
	right->align = Align::End;
	return make_unique<Toolbar>(
		std::move(left), std::move(mid), std::move(right));
}

unique_ptr<Sidebar>
make_sidebar(Browser &b)
{
	auto list = make_unique<ScrollColumn>();
	b.places_ = list.get();
	list->follow_focus = true;
	list->gap = 0.0f;
	list->grow = true;
	auto side = make_unique<Sidebar>(std::move(list));
	side->min_w = kBrowseSidebarPts;
	return side;
}

void
fill_places(Browser &b)
{
	auto *list = b.places_;
	if (!list)
		return;
	string restore_path;
	for (const auto &item : b.place_items_) {
		if (item.button == b.kit_.focus_) {
			restore_path = item.path;
			b.kit_.focus_ = nullptr;
			break;
		}
	}
	b.kit_.forget_tree(list);
	b.place_items_.clear();
	list->erase_children();
	for (int i = 0; i < int(b.side_dirs_.size()); ++i) {
		const Browser::DirRow &d = b.side_dirs_[size_t(i)];
		if (d.path.empty()) {
			list->add_child(make_unique<Sep>());
			b.place_items_.push_back({});
			continue;
		}
		auto row = make_unique<SideRow>();
		SideRow *item = row.get();
		row->path = d.path;
		row->browser = &b;
		row->pad_x = kWinPadX;
		row->icon = d.icon;
		row->text = QString::fromStdString(d.name);
		row->tip_text = {};
		row->active = d.current;
		const string path = d.path;
		row->on_click = [&b, path](Kit &) {
			if (!path.empty())
				open_directory(b, QString::fromStdString(path));
		};
		list->add_child(std::move(row));
		b.place_items_.push_back({item, d.path});
	}
	if (!restore_path.empty()) {
		b.kit_.focus_ = nullptr;
		for (size_t i = b.place_items_.size(); i--; ) {
			const auto &item = b.place_items_.at(i);
			if (item.path == restore_path) {
				b.kit_.focus_ = item.button;
				break;
			}
		}
	}
	b.places_dirty_ = false;
}

void
sync_ui(Browser &b, Page &ui)
{
	if (ui.toolbar)
		ui.toolbar->sync_buttons();
	if (b.places_dirty_)
		fill_places(b);
	else {
		const size_t n = min(b.place_items_.size(), b.side_dirs_.size());
		for (size_t i = 0; i < n; ++i) {
			if (Button *button = b.place_items_[i].button)
				button->active = b.side_dirs_[i].current;
		}
	}
}

void
layout(Browser &b, Page &ui)
{
	b.kit_.root_ = &ui;
	sync_ui(b, ui);
	ui.arrange(b.kit_, {0.0f, 0.0f, b.kit_.host_w_, b.kit_.host_h_});
	sync_thumbs(b);
	b.kit_.relayout_popups();
	b.kit_.sync_focus();
	ui.prepare(b.kit_);
	b.kit_.prepare_popups();
}

void
draw(Browser &b, Page &ui)
{
	if (!b.kit_.inited_)
		return;
	layout(b, ui);
	b.kit_.hot_ = b.kit_.hit(b.kit_.mouse_x_, b.kit_.mouse_y_);
	b.kit_.tooltip(b.kit_.hot_);
	b.kit_.paint();
}

bool
apply_action(Browser &b, Action action)
{
	switch (action) {
	case Action::Sidebar:
		if (b.page_)
			b.page_->sidebar_open = !b.page_->sidebar_open;
		request_render(b);
		return true;
	case Action::DirPrev: {
		if (b.dir_path_.isEmpty())
			return true;
		const string p =
			tree_prev_dir(b.dir_path_.toStdString(), b.setup_);
		if (!p.empty())
			open_directory(b, QString::fromStdString(p));
		return true;
	}
	case Action::DirNext: {
		if (b.dir_path_.isEmpty())
			return true;
		const string p =
			tree_next_dir(b.dir_path_.toStdString(), b.setup_);
		if (!p.empty())
			open_directory(b, QString::fromStdString(p));
		return true;
	}
	case Action::DirParent: {
		const QFileInfo info(b.dir_path_);
		const QString parent = info.dir().absolutePath();
		if (!parent.isEmpty() && parent != b.dir_path_)
			open_directory(b, parent);
		return true;
	}
	case Action::DirHome:
		open_directory(b, QDir::homePath());
		return true;
	case Action::ThumbPlus: {
		const int idx = thumb_size_index(b.thumb_size_);
		if (idx + 1 < kThumbSizeN)
			set_thumb_size(b, kThumbSizes[idx + 1]);
		return true;
	}
	case Action::ThumbMinus: {
		const int idx = thumb_size_index(b.thumb_size_);
		if (idx > 0)
			set_thumb_size(b, kThumbSizes[idx - 1]);
		return true;
	}
	case Action::ViewTile:
		set_view(b, BrowserView::Tile);
		return true;
	case Action::ViewGrid:
		set_view(b, BrowserView::Grid);
		return true;
	case Action::ViewList:
		return true;
	case Action::Filenames:
		b.show_names_ = !b.show_names_;
		request_render(b);
		return true;
	case Action::Filter:
		b.setup_.filter_files = !b.setup_.filter_files;
		scan_dir(b);
		enqueue_thumbs(b);
		request_render(b);
		return true;
	case Action::SortDir:
		b.setup_.sort_desc = !b.setup_.sort_desc;
		scan_dir(b);
		request_render(b);
		return true;
	case Action::SortName:
		b.setup_.sort = SortField::Name;
		scan_dir(b);
		request_render(b);
		return true;
	case Action::SortTime:
		b.setup_.sort = SortField::Time;
		scan_dir(b);
		request_render(b);
		return true;
	case Action::Activate:
		if (b.cursor_ >= 0 && b.cursor_ < int(b.files_.size()) && b.page_ &&
			b.page_->host && b.page_->host->activate)
			b.page_->host->activate(b.files_[size_t(b.cursor_)].path);
		return true;
	case Action::Reload:
		if (!b.dir_path_.isEmpty()) {
			scan_dir(b);
			enqueue_thumbs(b);
			request_render(b);
		}
		return true;
	case Action::Copy:
		if (b.cursor_ >= 0 && b.cursor_ < int(b.files_.size())) {
			const QString files[] = {
				QString::fromStdString(b.files_[size_t(b.cursor_)].path)};
			copy_files(files);
		}
		return true;
	case Action::Trash:
		if (b.cursor_ >= 0 && b.cursor_ < int(b.files_.size()) && b.page_ &&
			b.page_->host && b.page_->host->trash)
			b.page_->host->trash(b.files_[size_t(b.cursor_)].path);
		return true;
	default:
		return false;
	}
}

Actor
make_actor(Browser &b, const HostActions &host)
{
	return chain_actor(
		host, [&b](Action a) { return apply_action(b, a); },
		[&b](Action a) { return spec_enabled(b, a); },
		[&b](Action a) { return spec_active(b, a); });
}

}  // namespace

// --- Browser -----------------------------------------------------------------

Browser::Browser(Kit &kit, Thumbnailer &thumbnailer)
	: kit_(kit), thumbnailer_(thumbnailer)
{
	this->hittable = true;
}

Browser::~Browser()
{
	destroy();
}

void
Browser::init()
{
	pack_toolbar_icons(*this);
	this->thumbnail_client_ = this->thumbnailer_.add_client(
		this->thumb_gen_, [this] { request_render(*this); });
}

void
Browser::measure(Kit &, float max_w, float max_h)
{
	this->r = {0, 0, max_w, max_h};
}

void
Browser::arrange(Kit &kit, Rect alloc)
{
	this->r = kit.snap_rect(alloc);
	layout_grid(*this, this->r);
}

bool
Browser::focusable() const
{
	return shown() && this->r.w > 0 && this->r.h > 0;
}

QString
Browser::tip() const
{
	if (this->show_names_)
		return {};
	const int i = hit_file(*this, this->kit_.mouse_x_, this->kit_.mouse_y_);
	if (i < 0)
		return {};
	return QString::fromStdString(this->files_[size_t(i)].name);
}

void
Browser::select_file(const string &path)
{
	clear_cursor(*this);
	if (!path.empty()) {
		for (int i = 0; i < int(this->files_.size()); ++i) {
			if (this->files_[size_t(i)].path == path) {
				this->cursor_ = i;
				remember_cursor_x(*this);
				// This won't quite work if the browser
				// is not currently visible.
				if (const int ri = find_cursor_row(*this); ri >= 0)
					scroll_to_row(*this, this->rows_[size_t(ri)]);
				break;
			}
		}
	}
	request_render(*this);
}

void
Browser::file_gone(const string &path)
{
	const bool was_cursor = this->cursor_ >= 0 &&
		this->cursor_ < int(this->files_.size()) &&
		this->files_[size_t(this->cursor_)].path == path;
	string next;
	if (was_cursor) {
		const int i = this->cursor_;
		if (i + 1 < int(this->files_.size()))
			next = this->files_[size_t(i + 1)].path;
		else if (i > 0)
			next = this->files_[size_t(i - 1)].path;
	}
	scan_dir(*this);
	enqueue_thumbs(*this);
	if (was_cursor && !next.empty())
		select_file(next);
	else
		request_render(*this);
}

void
Browser::prepare(Kit &kit)
{
	pack_standin_icons(*this);
	if (!this->show_names_)
		return;
	for (const File &f : this->files_) {
		if (f.cell.w <= 0.0f || f.cap.h <= 0.0f)
			continue;
		Label lab;
		lab.text = f.cap_text;
		lab.wrap = true;
		lab.pad_y = kCapPad * 0.5f;
		lab.r = f.cap;
		lab.prepare(kit);
	}
}

void
Browser::paint(Kit &kit) const
{
	if (kit.renderer_)
		kit.renderer_->set_view(1.0f, 0.0f, 0.0f, Orientation::Rotate0);
	kit.list_.push_clip(
		this->r.x, this->r.y, this->r.x + this->r.w, this->r.y + this->r.h);
	kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, kit.well_);
	const float th = float(this->thumb_size_);
	const Colour ink = kit.ink_;
	const Colour glow_idle = {ink.r, ink.g, ink.b, ink.a * kGlowAlpha};
	const Colour frame = kit.frame_;
	for (int i = 0; i < int(this->files_.size()); ++i) {
		const File &f = this->files_[size_t(i)];
		if (f.cell.w <= 0.0f)
			continue;
		if (f.cell.y + f.cell.h < this->r.y || f.cell.y > this->r.y + this->r.h)
			continue;
		const float tw = f.tile.w > 0.0f ? f.tile.w : th;
		const float thp = f.tile.h > 0.0f ? f.tile.h : th;
		const float tx = f.tile.x;
		const float ty = f.tile.y;
		const bool focused =
			kit.focus_ == this && this->cursor_ >= 0 && i == this->cursor_;
		if (!f.gpu.empty()) {
			kit.draw_glow(tx - kBorder, ty - kBorder, tw + 2.0f * kBorder,
				thp + 2.0f * kBorder, focused ? ink : glow_idle);
			draw_checker(kit, {tx, ty, tw, thp});
			kit.list_.add_rect_stroke(tx - 1.0f, ty - 1.0f, tx + tw + 1.0f,
				ty + thp + 1.0f, frame, kBorder);
			float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
			this->sheet_.uv(f.gpu, &u0, &v0, &u1, &v1);
			kit.list_.add_thumb(tx, ty, tx + tw, ty + thp, u0, v0, u1, v1);
		} else {
			kit.list_.add_rect_filled(
				tx, ty, tx + tw, ty + thp, focused ? kit.press_ : kit.hover_);
			const float sz = min(tw, thp) * 0.5f;
			kit.draw_icon(tx + (tw - sz) * 0.5f, ty + (thp - sz) * 0.5f, sz,
				f.failed ? kMissingIcon : kPendingIcon, ink);
		}
		if (this->show_names_ && f.cap.h > 0.0f) {
			kit.list_.push_clip(
				f.cap.x, f.cap.y, f.cap.x + f.cap.w, f.cap.y + f.cap.h);
			Label lab;
			lab.text = f.cap_text;
			lab.align = Align::Center;
			lab.wrap = true;
			lab.pad_y = kCapPad * 0.5f;
			lab.r = f.cap;
			lab.paint(kit);
			kit.list_.pop_clip();
		}
	}
	this->scroll_.paint(kit, this->r);
	kit.list_.pop_clip();
}

bool
Browser::thumbs_busy() const
{
	return this->thumbnailer_.busy(this->thumbnail_client_);
}

unique_ptr<Page>
make_browser_page(
	Kit &kit, const HostActions &host, Thumbnailer &thumbnailer, Browser **out)
{
	auto browser = make_unique<Browser>(kit, thumbnailer);
	Browser *b = browser.get();
	b->init();
	auto toolbar = make_toolbar(*b);
	auto sidebar = make_sidebar(*b);
	auto page = make_unique<Page>(std::move(toolbar), std::move(sidebar),
		Page::Side::Left, std::move(browser));
	page->host = &host;
	if (page->context) {
		page->context->on_new_window = [nw = host.new_window](
										   const QString &path) {
			if (nw)
				nw(path.toStdString());
		};
		page->context->on_trash = [t = host.trash](const QString &path) {
			if (t)
				t(path.toStdString());
		};
	}
	page->menu_tree = browser_menu();
	page->keys = browser_keys();
	page->modal->tree = page->menu_tree;
	page->modal->keys = page->keys;
	page->actor = make_actor(*b, host);
	if (page->toolbar)
		page->toolbar->actor = page->actor;
	if (page->toolbar && page->toolbar->app_menu)
		page->toolbar->app_menu->build(page->menu_tree, page->actor);
	b->places_dirty_ = true;
	b->page_ = page.get();
	if (out)
		*out = b;
	return page;
}

void
Browser::destroy()
{
	if (this->thumbnail_client_) {
		this->thumbnailer_.remove_client(this->thumbnail_client_);
		this->thumbnail_client_ = 0;
	}
	this->thumb_inflight_.clear();
	clear_gpu(*this);
	this->files_.clear();
	this->side_dirs_.clear();
	this->places_ = nullptr;
	this->place_items_.clear();
	this->page_ = nullptr;
}

void
Browser::set_host(float width_pts, float height_pts, float dpr)
{
	this->kit_.host_w_ = width_pts;
	this->kit_.host_h_ = height_pts;
	if (!set_dpr(*this, dpr))
		pack_toolbar_icons(*this);
}

void
Browser::open_dir(const QString &path, bool record)
{
	open_directory(*this, path, record);
}

bool
Browser::hist_back()
{
	if (this->hist_back_.empty())
		return false;
	push_hist(this->hist_forward_, *this);
	const HistEntry e = this->hist_back_.back();
	this->hist_back_.pop_back();
	open_directory(*this, e.path, false, e.side_scroll);
	return true;
}

bool
Browser::hist_forward()
{
	if (this->hist_forward_.empty())
		return false;
	push_hist(this->hist_back_, *this);
	const HistEntry e = this->hist_forward_.back();
	this->hist_forward_.pop_back();
	open_directory(*this, e.path, false, e.side_scroll);
	return true;
}

void
Browser::hist_clear_forward()
{
	this->hist_forward_.clear();
}

bool
Browser::hist_can_back() const
{
	return !this->hist_back_.empty();
}

bool
Browser::hist_can_forward() const
{
	return !this->hist_forward_.empty();
}

void
Browser::set_screen_profile(shared_ptr<Cmm> cmm, shared_ptr<Profile> profile)
{
	const bool reload_thumbs =
		!profiles_equal(this->screen_profile_.get(), profile.get());
	this->cmm_ = std::move(cmm);
	this->screen_profile_ = std::move(profile);
	this->kit_.bake_colours(this->cmm_.get(), this->screen_profile_.get());
	if (this->kit_.renderer_) {
		const Colour well = this->kit_.well_;
		const Colour tile = this->kit_.toolbar_bottom_;
		this->kit_.renderer_->set_well_colour(well.r, well.g, well.b);
		this->kit_.renderer_->set_checker_colour(tile.r, tile.g, tile.b);
		this->kit_.renderer_->set_transfer(
			profile_transfer(this->screen_profile_.get()));
	}
	if (reload_thumbs) {
		invalidate_thumbs(*this);
		enqueue_thumbs(*this);
	}
	if (this->kit_.request_render)
		this->kit_.request_render();
}

void
Browser::present(Page &ui)
{
	draw(*this, ui);
}

bool
Browser::key(Kit &kit, int key, unsigned mods)
{
	if (context_key(key, mods))
		return show_cursor_context(*this, kit);
	if (shift_enter(key, mods)) {
		if (this->cursor_ >= 0 && this->cursor_ < int(this->files_.size()))
			open_new_window(*this, this->files_[size_t(this->cursor_)].path);
		return true;
	}
	if (key == Qt::Key_Escape && mods == 0 && this->cursor_ >= 0) {
		clear_cursor(*this);
		request_render(*this);
		return true;
	}
	if (mods == 0) {
		switch (key) {
		case Qt::Key_Left:
			move_cursor(*this, CursorDir::Left);
			return true;
		case Qt::Key_Right:
			move_cursor(*this, CursorDir::Right);
			return true;
		case Qt::Key_Up:
			move_cursor(*this, CursorDir::Up);
			return true;
		case Qt::Key_Down:
			move_cursor(*this, CursorDir::Down);
			return true;
		case Qt::Key_Home:
			move_cursor_home(*this);
			return true;
		case Qt::Key_End:
			move_cursor_end(*this);
			return true;
		case Qt::Key_PageUp:
			page_scroll(*this, -1);
			return true;
		case Qt::Key_PageDown:
			page_scroll(*this, 1);
			return true;
		}
	}
	return false;
}

bool
Browser::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::RightButton) {
		kit.focus_ = this;
		const int i = hit_file(*this, x, y);
		if (i >= 0) {
			this->cursor_ = i;
			remember_cursor_x_at(*this, x);
			show_file_context(
				*this, kit, this->files_[size_t(i)].path, {x, y, 0, 0}, false);
			return true;
		}
		if (this->dir_path_.isEmpty())
			return false;
		show_file_context(
			*this, kit, this->dir_path_.toStdString(), {x, y, 0, 0}, false);
		return true;
	}
	if (button == Qt::MiddleButton) {
		const int i = hit_file(*this, x, y);
		if (i < 0)
			return false;
		kit.focus_ = this;
		this->mid_file_ = i;
		kit.pressed_ = this;
		return true;
	}
	if (button != Qt::LeftButton)
		return false;
	if (this->scroll_.press(x, y, button, this->r)) {
		kit.focus_ = this;
		kit.pressed_ = this;
		return true;
	}
	kit.focus_ = this;
	if (hit_file(*this, x, y) < 0 && this->cursor_ >= 0) {
		clear_cursor(*this);
		request_render(*this);
	}
	kit.pressed_ = this;
	return true;
}

void
activate_hit(Browser &b, float x, float y)
{
	const int i = hit_file(b, x, y);
	if (i < 0 || i >= int(b.files_.size()))
		return;
	b.cursor_ = i;
	remember_cursor_x_at(b, x);
	if (b.page_ && b.page_->host && b.page_->host->activate)
		b.page_->host->activate(b.files_[size_t(i)].path);
}

bool
Browser::release(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button == Qt::MiddleButton) {
		if (kit.pressed_ != this)
			return false;
		const int i = hit_file(*this, x, y);
		if (i >= 0 && i == this->mid_file_ && this->page_ &&
			this->page_->host && this->page_->host->new_window)
			this->page_->host->new_window(this->files_[size_t(i)].path);
		this->mid_file_ = -1;
		return true;
	}
	if (button != Qt::LeftButton)
		return false;
	if (kit.pressed_ != this)
		return false;
	if (this->scroll_.release(button))
		return true;
	Widget *hit = kit.root_ ? kit.root_->hit_at(x, y) : hit_at(x, y);
	if (hit != this)
		return false;
	activate_hit(*this, x, y);
	return true;
}

bool
Browser::motion(Kit &, float, float y)
{
	if (this->scroll_.dragging)
		return this->scroll_.motion(y, this->r);
	this->scroll_.reveal();
	return false;
}

bool
Browser::double_click(Kit &, float x, float y, Qt::MouseButton button, unsigned)
{
	if (button != Qt::LeftButton)
		return false;
	activate_hit(*this, x, y);
	return true;
}

bool
Browser::scroll(Kit &, float, float, int delta)
{
	this->scroll_.wheel(delta, row_h(*this));
	this->scroll_.offset = this->kit_.snap(this->scroll_.offset);
	this->scroll_.clamp();
	return true;
}

bool
Browser::pan(Kit &, float, float, float, float dy)
{
	this->scroll_.pan(dy);
	this->scroll_.offset = this->kit_.snap(this->scroll_.offset);
	this->scroll_.clamp();
	return true;
}

int
Browser::wake_ms() const
{
	int ms = this->scroll_.wake_ms();
	if (this->thumbs_busy())
		ms = ms < 0 ? 0 : min(ms, 0);
	return ms;
}

}  // namespace dn
