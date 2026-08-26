//
// chrome.cpp: toolbar, sidebar, and page chrome widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "chrome.hpp"

#include <QKeyEvent>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

constexpr float kWinPadX = 4.0f;
constexpr float kWinPadY = 2.0f;

// How far the pointer must travel before a titlebar press becomes a move.
constexpr float kDragPx = 4.0f;

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

Qt::Edges
resize_edges(const Kit &kit, Rect window, Rect frame, float x, float y)
{
	if (!kit.csd_ || kit.fullscreen_ || kit.maximized_)
		return {};
	if (!window.contains(x, y))
		return {};
	if (kit.csd_shadow_ ? frame.contains(x, y) : !frame.contains(x, y))
		return {};
	const float band = kResizeBorderPts;
	Qt::Edges e;
	if (x < frame.x + band)
		e |= Qt::LeftEdge;
	if (x >= frame.x + frame.w - band)
		e |= Qt::RightEdge;
	if (y < frame.y + band)
		e |= Qt::TopEdge;
	if (y >= frame.y + frame.h - band)
		e |= Qt::BottomEdge;
	return e;
}

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

// --- ToolbarSlot ------------------------------------------------------------

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

#if !defined(Q_OS_MACOS)
	if (this->left) {
		auto app = make_unique<Button>();
		app->icon = "open-menu-symbolic";
		app->tip_text = "Menu";
		this->app_menu_button = app.get();
		this->left->add_item(make_unique<Sep>(), 0);
		this->left->add_item(std::move(app), 0);
	}
#endif

	this->overflow_owned_ = make_unique<Overflow>();
	this->overflow_owned_->pad_y = kWinPadY;
	this->overflow = this->overflow_owned_.get();
	add_overflow_proxies(*this->overflow, this->left);
	add_overflow_proxies(*this->overflow, this->mid);
	add_overflow_proxies(*this->overflow, this->right);

	this->app_menu_owned_ = make_unique<Menu>();
	this->app_menu = this->app_menu_owned_.get();

	this->overflow->fill_items = [this] {
		if (Button *m = this->overflow->opener) {
			if (ToolbarSlot *slot = slot_for_more(m))
				fill_overflow_items(*slot, *this->overflow);
		}
	};

	if (this->app_menu_button) {
		this->app_menu_button->activate_on_press = true;
		this->app_menu_button->on_click = [this](Kit &kit) {
			open_app_menu(kit, false);
		};
	}
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

unique_ptr<Overflow>
Toolbar::take_overflow()
{
	return std::move(this->overflow_owned_);
}

unique_ptr<Menu>
Toolbar::take_app_menu()
{
	return std::move(this->app_menu_owned_);
}

void
Toolbar::open_app_menu(Kit &kit, bool kbd)
{
	if (!this->app_menu)
		return;
	if (this->app_menu->visible) {
		this->app_menu->close(kit);
		return;
	}
	if (!this->app_menu_button)
		return;
	Button *anchor = this->app_menu_button;
	if (!anchor->shown() && this->left && this->left->more->shown())
		anchor = this->left->more;
	this->app_menu->open(kit, anchor);
	if (kbd)
		kit.focus_first(this->app_menu);
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
	if (this->app_menu_button)
		this->app_menu_button->active = app_menu_open();
	if (this->app_menu)
		this->app_menu->sync();
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

void
Titlebar::measure(Kit &kit, float avail_w, float)
{
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
	if (Page *page = dynamic_cast<Page *>(this->parent_)) {
		const Qt::Edges edges =
			resize_edges(kit, page->r, page->frame(), x, y);
		if (edges && kit.start_resize) {
			kit.start_resize(edges);
			return true;
		}
	}
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

Sidebar::Sidebar(unique_ptr<Widget> child)
{
	this->content = child.get();
	this->fill = Fill::Panel;
	this->hittable = true;
	this->clip = true;
	if (child)
		add_child(std::move(child));
}

bool
Sidebar::key(Kit &kit, int key, unsigned mods)
{
	if (mods)
		return false;
	if (key != Qt::Key_Up && key != Qt::Key_Down)
		return false;
	const int dir = key == Qt::Key_Up ? -1 : 1;
	return kit.cycle_focus(this, dir, false);
}

// --- Page -------------------------------------------------------------------

namespace
{

constexpr float kMinWell = 80.0f;
constexpr float kMinSide = 120.0f;
constexpr float kSplitW = 8.0f;

}  // namespace

Page::Page(unique_ptr<Toolbar> tb, unique_ptr<Sidebar> sb, Side s,
	unique_ptr<Widget> body)
	: sidebar_side(s)
{
	auto title = make_unique<Titlebar>();
	this->titlebar = title.get();
	add_child(std::move(title));
	this->toolbar = tb.get();
	add_child(std::move(tb));
	this->sidebar = sb.get();
	add_child(std::move(sb));
	if (this->sidebar) {
		auto split = make_unique<Splitter>();
		split->min_w = kSplitW;
		split->hittable = true;
		this->splitter = split.get();
		this->splitter->on_drag = [this](float mx) {
			if (!this->sidebar_open || this->sidebar_side == Side::None)
				return;
			const float max_side = max(kMinSide, this->frame_.w - kMinWell);
			if (this->sidebar_side == Side::Right)
				this->sidebar_w = clamp(
					this->frame_.x + this->frame_.w - mx, kMinSide, max_side);
			else
				this->sidebar_w =
					clamp(mx - this->frame_.x, kMinSide, max_side);
		};
		add_child(std::move(split));
	}
	this->content = body.get();
	add_child(std::move(body));
	if (this->toolbar) {
		this->overflow_owned_ = this->toolbar->take_overflow();
		this->app_menu_owned_ = this->toolbar->take_app_menu();
	}
	this->dialog_owned_ = make_unique<Dialog>();
	this->dialog = this->dialog_owned_.get();
	this->hint_owned_ = make_unique<Hint>();
	this->hint = this->hint_owned_.get();
	this->hint->page = this;
	this->context_owned_ = make_unique<ContextMenu>();
	this->context = this->context_owned_.get();
	if (this->sidebar) {
		if (this->sidebar->min_w > 0.0f)
			this->sidebar_w = this->sidebar->min_w;
		this->sidebar_open = this->sidebar->visible;
	} else {
		this->sidebar_open = false;
		this->sidebar_side = Side::None;
	}
}

void
Page::set_banner(unique_ptr<Widget> w)
{
	if (this->banner)
		this->banner->parent_ = nullptr;
	this->banner = w.get();
	if (this->banner)
		this->banner->parent_ = this;
	this->banner_owned_ = std::move(w);
}

void
Page::measure(Kit &, float max_w, float max_h)
{
	this->r = {0.0f, 0.0f, max_w, max_h};
}

void
Page::arrange(Kit &kit, Rect alloc)
{
	if (!this->visible) {
		this->r = {};
		this->well_ = {};
		return;
	}
	this->r = kit.snap_rect(alloc);
	this->frame_ = this->r;
	if (kit.csd_shadow_)
		this->frame_ = kit.snap_rect(this->r.inset(kGlowPts, kGlowPts));
	if (this->titlebar)
		this->titlebar->visible = kit.csd_ && !kit.fullscreen_;
	float y = this->frame_.y;
	if (this->titlebar && this->titlebar->visible) {
		this->titlebar->measure(kit, this->frame_.w, this->frame_.h);
		this->titlebar->arrange(
			kit, {this->frame_.x, y, this->frame_.w, this->titlebar->r.h});
		y += this->titlebar->r.h;
	} else if (this->titlebar) {
		this->titlebar->r = {};
	}
	if (this->toolbar && this->toolbar->visible) {
		this->toolbar->measure(kit, this->frame_.w, this->frame_.h);
		this->toolbar->arrange(
			kit, {this->frame_.x, y, this->frame_.w, this->toolbar->r.h});
		y += this->toolbar->r.h;
	}
	if (this->banner && this->banner->visible) {
		const float rest = max(0.0f, this->frame_.y + this->frame_.h - y);
		this->banner->measure(kit, this->frame_.w, rest);
		this->banner->arrange(
			kit, {this->frame_.x, y, this->frame_.w, this->banner->r.h});
		y += this->banner->r.h;
	}
	const float body_y = y;
	const float body_h = max(0.0f, this->frame_.y + this->frame_.h - body_y);
	float side_w = 0.0f;
	if (this->sidebar) {
		this->sidebar->visible =
			this->sidebar_open && this->sidebar_side != Side::None && body_h > 0.0f;
		if (this->sidebar->visible) {
			side_w = max(0.0f, this->sidebar_w);
			this->sidebar->min_w = side_w;
			if (this->sidebar_side == Side::Left)
				this->sidebar->arrange(
					kit, {this->frame_.x, body_y, side_w, body_h});
			else
				this->sidebar->arrange(kit, {this->frame_.x + this->frame_.w -
						side_w,
					body_y, side_w, body_h});
		} else {
			this->sidebar->r = {};
		}
	}
	this->well_ = {this->frame_.x, body_y, this->frame_.w, body_h};
	if (this->sidebar && this->sidebar->visible) {
		if (this->sidebar_side == Side::Left)
			this->well_.x += side_w;
		this->well_.w = max(0.0f, this->well_.w - side_w);
	}
	if (this->content && this->content->visible)
		this->content->arrange(kit, this->well_);
	kit.default_focus_ = this->content;
	if (this->splitter) {
		this->splitter->visible = this->sidebar && this->sidebar->visible;
		if (this->splitter->visible) {
			const float sw =
				this->splitter->min_w > 0.0f ? this->splitter->min_w : kSplitW;
			float sx = this->sidebar_side == Side::Right
				? this->well_.x + this->well_.w - sw * 0.5f
				: this->well_.x - sw * 0.5f;
			this->splitter->arrange(kit, {sx, body_y, sw, body_h});
		} else {
			this->splitter->r = {};
		}
	}
}

bool
Page::key(Kit &kit, int key, unsigned mods)
{
	constexpr Action pane[] = {Action::NextPane, Action::PrevPane};
	const Action a = match_key(pane, key, mods);
	if (a == Action::None) {
		const Action mode = match_key(this->keys, key, mods);
		if (mode == Action::None)
			return false;
		if (this->actor.apply)
			this->actor.apply(mode);
		return true;
	}
	Widget *here = nullptr;
	for (Widget *w = kit.focus_; w; w = w->parent_) {
		if (w == this->toolbar || w == this->sidebar || w == this->content) {
			here = w;
			break;
		}
	}
	Widget *panes[3];
	int n = 0, i = 0;
	for (size_t c = 0; c < child_count(); ++c) {
		Widget *k = child(c);
		if (!k || !k->visible ||
			(k != this->toolbar && k != this->sidebar && k != this->content))
			continue;
		if (k == here)
			i = n;
		panes[n++] = k;
	}
	if (!n)
		return false;
	const int dir = a == Action::PrevPane ? -1 : 1;
	kit.focus_first(panes[(i + dir + n) % n]);
	return true;
}

void
Page::paint(Kit &kit) const
{
	if (!shown())
		return;
	if (kit.csd_shadow_ && this->frame_.w > 0.0f && this->frame_.h > 0.0f)
		kit.draw_glow(this->frame_.x, this->frame_.y, this->frame_.w,
			this->frame_.h, {0, 0, 0, kit.active_ ? 0.25f : 0.125f});
	Widget::paint(kit);
}

bool
Page::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	const Qt::Edges edges = resize_edges(kit, this->r, this->frame_, x, y);
	if (edges && kit.start_resize) {
		kit.start_resize(edges);
		return true;
	}
	return false;
}

void
Page::apply_csd_cursor(Kit &kit)
{
	kit.cursor_ = Qt::ArrowCursor;
	if (!kit.csd_ || kit.fullscreen_ || kit.maximized_)
		return;
	if (dynamic_cast<Button *>(kit.hot_))
		return;
	const Qt::Edges edges =
		resize_edges(kit, this->r, this->frame_, kit.mouse_x_, kit.mouse_y_);
	if (edges)
		kit.cursor_ = resize_cursor(edges);
}

bool
Page::start_csd_resize(Kit &kit, float x, float y)
{
	if (dynamic_cast<Button *>(kit.hit(x, y)))
		return false;
	const Qt::Edges edges = resize_edges(kit, this->r, this->frame_, x, y);
	if (!edges || !kit.start_resize)
		return false;
	kit.start_resize(edges);
	return true;
}

size_t
Page::child_count() const
{
	return 6;
}

Widget *
Page::child(size_t i) const
{
	const bool right = this->sidebar_side == Side::Right;
	switch (i) {
	case 0:
		return this->titlebar;
	case 1:
		return this->toolbar;
	case 2:
		return this->banner;
	case 3:
		return right ? this->content : this->sidebar;
	case 4:
		return right ? this->sidebar : this->content;
	case 5:
		return this->splitter;
	default:
		return nullptr;
	}
}

Actor
chain_actor(const HostActions &host, function<bool(Action)> apply,
	function<bool(Action)> enabled, function<bool(Action)> checked)
{
	auto can = [&host, enabled](Action action) {
		if (action == Action::Back || action == Action::Forward)
			return host.enabled && host.enabled(action);
		return !enabled || enabled(action);
	};
	Actor actor;
	actor.apply = [&host, apply, can](Action action) {
		if (!can(action))
			return;
		if (apply && apply(action))
			return;
		if (host.apply)
			host.apply(action);
	};
	actor.enabled = std::move(can);
	actor.checked = std::move(checked);
	return actor;
}

}  // namespace dn
