//
// kit-chrome.cpp: non-generic widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "assoc.hpp"
#include "kit-chrome.hpp"
#include "kit-browser.hpp"
#include "url.hpp"

#include <QFileInfo>
#include <QKeyEvent>
#include <QtGlobal>

#include <algorithm>
#include <functional>
#include <iterator>
#include <memory>
#include <span>
#include <vector>

using namespace std;

namespace dn
{

// --- Sidebar -----------------------------------------------------------------

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
Sidebar::key(Kit &kit, const Key &ev)
{
	if (ev.mods)
		return false;
	if (ev.key != Qt::Key_Up && ev.key != Qt::Key_Down)
		return false;
	const int dir = ev.key == Qt::Key_Up ? -1 : 1;
	return kit.cycle_focus(this, dir, false);
}

// --- Context menu ------------------------------------------------------------

void
ContextMenu::fill_items(const QUrl &url)
{
	clear();
	this->min_w = 200.0f;

	// Open With and Move to Trash are filesystem operations on a real file.
	const QString path = url_to_path(url);
	const Handler def = default_for(path);
	const vector<Handler> rec = recommended_for(path);
	const vector<Handler> fall = fallback_for(path);

	auto apps = make_unique<Menu>();
	apps->min_w = this->min_w;
	bool apps_sep = false;
	auto add_apps = [&](span<const Handler> group) {
		bool added = false;
		for (const Handler &app : group) {
			if (app.id.isEmpty())
				continue;
			if (apps_sep && !added)
				apps->add_sep();
			added = true;
			auto &item = apps->add_item(app.name.isEmpty() ? app.id : app.name);
			item.on_click = [app, path](Kit &) {
				launch(app, path);
				set_last_used(app, path);
			};
		}
		if (added)
			apps_sep = true;
	};

	add_apps({&def, 1});
	add_apps(rec);
	add_apps(fall);

	auto &new_win = add_item_with_mnemonic("Open in New _Window");
	new_win.on_click = [this, url](Kit &) {
		if (this->on_new_window)
			this->on_new_window(url);
	};
	if (apps_sep) {
		add_item_with_mnemonic("Open _With").sub = apps.get();
		this->subs_.push_back(std::move(apps));
	}
	if (QFileInfo(path).isDir() && this->on_bookmarked &&
		this->on_toggle_bookmark) {
		add_sep();
		auto &bookmark = add_item_with_mnemonic(this->on_bookmarked(url)
				? "Remove from _Bookmarks"
				: "Add to _Bookmarks");
		bookmark.on_click = [this, url](Kit &) {
			if (this->on_toggle_bookmark)
				this->on_toggle_bookmark(url);
		};
	}
	if (QFileInfo(path).isFile() && this->on_trash) {
		add_sep();
		auto &trash = add_item_with_mnemonic("Move to _Trash");
		trash.accel = action_accel(action_def(Action::Trash));
		trash.on_click = [this, url](Kit &) {
			if (this->on_trash)
				this->on_trash(url);
		};
	}
}

void
ContextMenu::show(Kit &kit, const QUrl &url, Rect anchor, bool kbd)
{
	kit.forget_tree(this);
	fill_items(url);
	bool any = false;
	if (this->col) {
		for (const auto &k : this->col->kids) {
			if (k && !is_sep(k.get())) {
				any = true;
				break;
			}
		}
	}
	if (!any) {
		if (this->visible)
			close(kit);
		return;
	}
	open_at(kit, anchor);
	if (kbd)
		kit.focus_first(this);
}

// --- Dialogs -----------------------------------------------------------------

namespace
{

unique_ptr<Label>
dialog_label(const QString &text, bool bold = false, bool wrap = false)
{
	auto label = make_unique<Label>();
	label->text = text;
	label->bold = bold;
	label->wrap = wrap;
	return label;
}

QString
shortcut_accel(const ActionDef &def)
{
	if (def.accel)
		return QString::fromUtf8(def.accel);
	QString s;
	for (const Accel &a : def.keys) {
		const QString part = accel_label(a);
		if (part.isEmpty())
			continue;
		if (!s.isEmpty())
			s += QLatin1String(", ");
		s += part;
	}
	return s;
}

bool
has_shortcut(const ActionDef &def)
{
	return !shortcut_accel(def).isEmpty();
}

void
for_leaves(span<const MenuNode> nodes, auto &&fn)
{
	for (const MenuNode &n : nodes) {
		if (!n.items.empty())
			for_leaves(n.items, fn);
		else if (n.action != Action::None)
			fn(n.action);
	}
}

unique_ptr<Row>
shortcut_row(const ActionDef &def, float accel_w)
{
	auto row = make_unique<Row>();
	row->gap = 8.0f;
	auto accel = dialog_label(shortcut_accel(def));
	accel->min_w = accel_w;
	accel->dim = true;
	auto name = dialog_label(menu_label(def.label[0], nullptr));
	row->add_child(std::move(accel));
	row->add_child(std::move(name));
	return row;
}

}  // namespace

// The body takes its natural height, never stretched: it only knows to
// scroll when what it holds is taller than it is.
void
dialog_about(Kit &kit, Dialog &dialog)
{
	auto col = make_unique<Column>();
	col->gap = 8.0f;
	col->add_child(dialog_label(QStringLiteral(DAWN_NAME), true));
	col->add_child(
		dialog_label(QStringLiteral("Colour-managed image browser and viewer."),
			false, true));
	dialog.show(kit, std::move(col), 360.0f);
}

void
dialog_shortcuts(Kit &kit, Dialog &dialog, span<const MenuNode> tree,
	span<const Action> keys)
{
	bool seen[size_t(Action::Count)] = {};
	float accel_w = 120.0f;
	auto consider = [&](Action action) {
		const ActionDef &def = action_def(action);
		if (!has_shortcut(def))
			return;
		accel_w = max(accel_w, kit.text_width(shortcut_accel(def), false));
	};
	for_leaves(tree, consider);
	auto consider_other = [&](Action action) {
		const ActionDef &def = action_def(action);
		if ((def.flags & ActionInMenu) || !has_shortcut(def))
			return;
		consider(action);
	};
	bool viewer = false;
	for (Action action : keys) {
		if (action == Action::ZoomIn || action == Action::FitWidth) {
			viewer = true;
			break;
		}
	}
	for (Action action : window_keys())
		consider_other(action);
	for (Action action : keys)
		consider_other(action);
	consider_other(Action::Cancel);
	if (viewer)
		consider_other(Action::ZoomLevel);

	auto col = make_unique<Column>();
	col->gap = 2.0f;
	col->add_child(dialog_label(QStringLiteral("Keyboard Shortcuts"), true));
	for (const MenuNode &section : tree) {
		if (section.items.empty())
			continue;
		bool any = false;
		for_leaves(section.items, [&](Action action) {
			if (has_shortcut(action_def(action)))
				any = true;
		});
		if (!any)
			continue;
		col->add_child(dialog_label(menu_label(section.title, nullptr), true));
		for_leaves(section.items, [&](Action action) {
			const ActionDef &def = action_def(action);
			if (!has_shortcut(def))
				return;
			seen[size_t(action)] = true;
			col->add_child(shortcut_row(def, accel_w));
		});
	}
	bool other = false;
	auto emit_other = [&](Action action) {
		const size_t i = size_t(action);
		if (i >= size(seen) || seen[i])
			return;
		const ActionDef &def = action_def(action);
		if ((def.flags & ActionInMenu) || !has_shortcut(def))
			return;
		if (!other) {
			col->add_child(dialog_label(QStringLiteral("Other"), true));
			other = true;
		}
		seen[i] = true;
		col->add_child(shortcut_row(def, accel_w));
	};
	for (Action action : window_keys())
		emit_other(action);
	for (Action action : keys)
		emit_other(action);
	emit_other(Action::Cancel);
	if (viewer)
		emit_other(Action::ZoomLevel);
	dialog.show(kit, std::move(col), 520.0f);
}

// --- Hint -------------------------------------------------------------------

namespace
{

constexpr char kChars[] = "SADFJKLEWCMPGH";
constexpr int kNChars = size(kChars) - 1;
constexpr float kChipPadX = 4.0f;
constexpr float kChipPadY = 2.0f;

Colour
col(const Colour &c, float alpha = 1.0f)
{
	return {c.r, c.g, c.b, c.a * alpha};
}

Rect
intersection(Rect a, Rect b)
{
	const float x = max(a.x, b.x);
	const float y = max(a.y, b.y);
	return {x, y, max(0.0f, min(a.x + a.w, b.x + b.w) - x),
		max(0.0f, min(a.y + a.h, b.y + b.h) - y)};
}

Rect
visible_rect(const Widget *w, Rect host)
{
	if (!w || !w->shown() || w->r.w <= 0.0f || w->r.h <= 0.0f)
		return {};
	Rect visible = intersection(w->r, host);
	for (const Widget *p = w->parent_; p; p = p->parent_) {
		if (!p->shown())
			return {};
		if (p->clips_children())
			visible = intersection(visible, p->r);
	}
	return visible;
}

// Anything the keyboard can reach is worth a hint, so this asks focusable()
// rather than testing for a type: a widget opts in by being reachable at all.
// The three exceptions are containers that are focusable as a whole -- one
// chip over the entire browser well would say nothing useful, and its files
// get their own targets below.
void
collect_targets(Widget *w, Rect host, vector<Widget *> &out)
{
	if (!w || !w->shown())
		return;
	if (dynamic_cast<Popup *>(w) || dynamic_cast<Browser *>(w))
		return;
	if (w->focusable() && visible_rect(w, host).w > 0.0f)
		out.push_back(w);
	const size_t n = w->child_count();
	for (size_t i = 0; i < n; ++i)
		collect_targets(w->child(i), host, out);
}

QString
label_at(int i, int len)
{
	QString s(len, QLatin1Char('A'));
	int x = i;
	for (int k = len - 1; k >= 0; --k) {
		s[k] = QLatin1Char(kChars[x % kNChars]);
		x /= kNChars;
	}
	return s;
}

int
label_len(int n)
{
	if (n <= 0)
		return 1;
	int len = 1;
	int cap = kNChars;
	while (cap < n && len < 8) {
		++len;
		cap *= kNChars;
	}
	return len;
}

bool
modifier_only(int key)
{
	return key == Qt::Key_Shift || key == Qt::Key_Control ||
		key == Qt::Key_Alt || key == Qt::Key_Meta || key == Qt::Key_AltGr;
}

}  // namespace

Hint::Hint()
{
	this->hittable = true;
	this->visible = false;
	this->fill = Fill::None;
}

void
Hint::open(Kit &kit)
{
	this->typed_.clear();
	Popup::open(kit);
	collect();
	assign_labels();
	place(kit);
}

void
Hint::close(Kit &kit)
{
	this->typed_.clear();
	this->targets_.clear();
	Popup::close(kit);
}

void
Hint::place(Kit &kit)
{
	this->visible = true;
	this->r = {0.0f, 0.0f, kit.host_w_, kit.host_h_};
	refresh_rects();
	layout_chips(kit);
}

void
Hint::prepare(Kit &kit)
{
	for (const Target &t : this->targets_) {
		if (matches(t))
			kit.cache_text(t.label, true);
	}
}

void
Hint::paint(Kit &kit) const
{
	if (!shown())
		return;
	kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
		this->r.y + this->r.h, col(kit.colours_[ColourInk], 0.1f));
	const float th = kit.text_height(QStringLiteral("Ag"), 0.0f, true);
	for (const Target &t : this->targets_) {
		if (!matches(t) || t.chip.w <= 0.0f)
			continue;
		const Rect c = t.chip;
		kit.list_.add_rect_filled(
			c.x, c.y, c.x + c.w, c.y + c.h, kit.colours_[ColourHint]);
		kit.list_.add_rect_stroke(
			c.x, c.y, c.x + c.w, c.y + c.h, kit.colours_[ColourInk]);
		const QString rest = t.label.mid(this->typed_.size());
		float tx = c.x + kChipPadX;
		const float ty = c.y + max(0.0f, (c.h - th) * 0.5f);
		if (!this->typed_.isEmpty()) {
			kit.emit_text(tx, ty, this->typed_,
				col(kit.colours_[ColourInk], 0.25f), true);
			tx += kit.text_width(this->typed_, true);
		}
		if (!rest.isEmpty())
			kit.emit_text(tx, ty, rest, kit.colours_[ColourInk], true);
	}
}

bool
Hint::key(Kit &kit, const Key &ev)
{
	const unsigned extra = ev.mods &
		unsigned(Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier);
	if (modifier_only(ev.key) && extra == 0)
		return true;
	if (ev.key == Qt::Key_Escape) {
		close(kit);
		return true;
	}
	if (ev.key == Qt::Key_Backspace && extra == 0) {
		if (!this->typed_.isEmpty())
			this->typed_.chop(1);
		return true;
	}
	if (extra == 0 && ev.key >= Qt::Key_A && ev.key <= Qt::Key_Z) {
		const QChar ch = QLatin1Char(char('A' + (ev.key - Qt::Key_A)));
		const QString next = this->typed_ + ch;
		bool any = false;
		const Target *exact = nullptr;
		int exact_n = 0;
		for (const Target &t : this->targets_) {
			if (!t.label.startsWith(next))
				continue;
			any = true;
			if (t.label == next) {
				exact = &t;
				++exact_n;
			}
		}
		if (!any)
			return true;
		this->typed_ = next;
		if (exact_n == 1 && exact)
			fire(kit, *exact);
		return true;
	}
	close(kit);
	return true;
}

bool
Hint::press(Kit &kit, float x, float y, Qt::MouseButton button)
{
	if (button != Qt::LeftButton) {
		close(kit);
		kit.pressed_ = nullptr;
		return true;
	}
	for (const Target &t : this->targets_) {
		if (!matches(t))
			continue;
		if (t.chip.contains(x, y) || t.at.contains(x, y)) {
			fire(kit, t);
			kit.pressed_ = nullptr;
			return true;
		}
	}
	close(kit);
	kit.pressed_ = nullptr;
	return true;
}

bool
Hint::release(Kit &, float, float, Qt::MouseButton)
{
	return true;
}

bool
Hint::motion(Kit &, float, float)
{
	return true;
}

void
Hint::collect()
{
	this->targets_.clear();
	if (!this->page)
		return;
	Rect host = this->page->r;
	if (host.w <= 0.0f || host.h <= 0.0f)
		host = {0.0f, 0.0f, 1.0e8f, 1.0e8f};
	vector<Widget *> widgets;
	collect_targets(this->page, host, widgets);
	for (Widget *w : widgets) {
		Target t;
		t.widget = w;
		t.at = visible_rect(w, host);
		this->targets_.push_back(t);
	}
	auto *browser = dynamic_cast<Browser *>(this->page->content);
	if (!browser)
		return;
	const Rect well = browser->r;
	for (int i = 0; i < int(browser->files_.size()); ++i) {
		const Browser::File &f = browser->files_[size_t(i)];
		if (f.tile.w <= 0.0f || f.tile.h <= 0.0f)
			continue;
		const Rect visible = intersection(f.tile, well);
		if (visible.w <= 0.0f || visible.h <= 0.0f)
			continue;
		Target t;
		t.browser = browser;
		t.file_i = i;
		t.at = visible;
		this->targets_.push_back(t);
	}
}

void
Hint::assign_labels()
{
	const int n = int(this->targets_.size());
	const int len = label_len(n);
	for (int i = 0; i < n; ++i)
		this->targets_[size_t(i)].label = label_at(i, len);
}

void
Hint::refresh_rects()
{
	vector<Target> keep;
	keep.reserve(this->targets_.size());
	for (Target &t : this->targets_) {
		if (t.widget) {
			const Rect visible = visible_rect(t.widget, this->page->r);
			if (!t.widget->focusable() || visible.w <= 0.0f ||
				visible.h <= 0.0f)
				continue;
			t.at = visible;
			keep.push_back(t);
			continue;
		}
		if (!t.browser || t.file_i < 0 ||
			t.file_i >= int(t.browser->files_.size()))
			continue;
		const Browser::File &f = t.browser->files_[size_t(t.file_i)];
		const Rect visible = intersection(f.tile, t.browser->r);
		if (visible.w <= 0.0f || visible.h <= 0.0f)
			continue;
		t.at = visible;
		keep.push_back(t);
	}
	this->targets_ = std::move(keep);
}

void
Hint::layout_chips(const Kit &kit)
{
	const float th = kit.text_height(QStringLiteral("Ag"), 0.0f, true);
	const float ch = th + kChipPadY * 2.0f;
	for (Target &t : this->targets_) {
		const float tw = kit.text_width(t.label, true);
		const float cw = tw + kChipPadX * 2.0f;
		t.chip = kit.snap_rect({t.at.x, t.at.y, cw, ch});
	}
}

bool
Hint::matches(const Target &t) const
{
	return t.label.startsWith(this->typed_);
}

void
Hint::fire(Kit &kit, Target t)
{
	Widget *widget = t.widget;
	Browser *browser = t.browser;
	const int file_i = t.file_i;
	close(kit);
	if (widget) {
		// Whatever this widget calls its default action; one that has none
		// takes the keyboard instead, which is what hinting a field is for.
		// Both are re-checked because a target collected when the overlay
		// opened may have been disabled or hidden since.
		if (!widget->activate(kit) && widget->focusable()) {
			kit.set_focus(widget, true);
		}
		return;
	}
	if (!browser || file_i < 0 || file_i >= int(browser->files_.size()))
		return;
	const QUrl url = browser->file_url(file_i);
	browser->select_file(url);
	if (browser->page_ && browser->page_->host &&
		browser->page_->host->activate)
		browser->page_->host->activate(url);
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
		this->splitter->on_drag = [this](Kit &kit, float mx) {
			if (!this->sidebar_open || this->sidebar_side == Side::None)
				return;
			const Rect frame = kit.frame();
			const float max_side = max(kMinSide, frame.w - kMinWell);
			if (this->sidebar_side == Side::Right)
				this->sidebar_w =
					clamp(frame.x + frame.w - mx, kMinSide, max_side);
			else
				this->sidebar_w = clamp(mx - frame.x, kMinSide, max_side);
		};
		add_child(std::move(split));
	}
	this->content = body.get();
	add_child(std::move(body));

	// macOS has a real menu bar for this; everywhere else it is a button
	// at the far end of the toolbar.
	this->app_menu_owned_ = make_unique<Menu>();
	this->app_menu = this->app_menu_owned_.get();
#if !defined(Q_OS_MACOS)
	if (this->toolbar && this->toolbar->left) {
		auto app = make_unique<Button>();
		app->icon = "open-menu-symbolic";
		app->tip_text = "Menu";
		app->activate_on_press = true;
		app->on_click = [this](Kit &kit) { open_app_menu(kit, false); };
		this->app_menu_button = app.get();
		this->toolbar->left->add_item(make_unique<Sep>(), 0);
		this->toolbar->left->add_item(std::move(app), 0);
	}
#endif
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
Page::open_app_menu(Kit &kit, bool kbd)
{
	if (!this->app_menu)
		return;
	if (this->app_menu->visible) {
		this->app_menu->close(kit);
		return;
	}
	if (!this->app_menu_button)
		return;
	// Too narrow a window packs the button away into the overflow, which
	// then anchors the menu instead.
	Button *anchor = this->app_menu_button;
	if (!anchor->shown() && this->toolbar && this->toolbar->left &&
		this->toolbar->left->more->shown())
		anchor = this->toolbar->left->more;
	this->app_menu->open(kit, anchor);
	if (kbd)
		kit.focus_first(this->app_menu);
}

void
Page::sync_app_menu()
{
	if (this->app_menu_button)
		this->app_menu_button->active = app_menu_open();
	if (this->app_menu)
		this->app_menu->sync();
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
	// The window may only fill the frame within its surface, the rest
	// belonging to the shadow that a client-side decorated window casts.
	const Rect frame = kit.frame();
	float y = frame.y;
	if (this->titlebar) {
		this->titlebar->measure(kit, frame.w, frame.h);
		if (this->titlebar->visible) {
			this->titlebar->arrange(
				kit, {frame.x, y, frame.w, this->titlebar->r.h});
			y += this->titlebar->r.h;
		}
	}
	if (this->toolbar && this->toolbar->visible) {
		this->toolbar->measure(kit, frame.w, frame.h);
		this->toolbar->arrange(kit, {frame.x, y, frame.w, this->toolbar->r.h});
		y += this->toolbar->r.h;
	}
	if (this->banner && this->banner->visible) {
		const float rest = max(0.0f, frame.y + frame.h - y);
		this->banner->measure(kit, frame.w, rest);
		this->banner->arrange(kit, {frame.x, y, frame.w, this->banner->r.h});
		y += this->banner->r.h;
	}
	const float body_y = y;
	const float body_h = max(0.0f, frame.y + frame.h - body_y);
	float side_w = 0.0f;
	if (this->sidebar) {
		this->sidebar->visible =
			this->sidebar_open && this->sidebar_side != Side::None && body_h > 0.0f;
		if (this->sidebar->visible) {
			side_w = max(0.0f, this->sidebar_w);
			this->sidebar->min_w = side_w;
			if (this->sidebar_side == Side::Left)
				this->sidebar->arrange(kit, {frame.x, body_y, side_w, body_h});
			else
				this->sidebar->arrange(
					kit, {frame.x + frame.w - side_w, body_y, side_w, body_h});
		} else {
			this->sidebar->r = {};
		}
	}
	this->well_ = {frame.x, body_y, frame.w, body_h};
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
Page::key(Kit &kit, const Key &ev)
{
	constexpr Action pane[] = {Action::NextPane, Action::PrevPane};
	const Action a = match_key(pane, ev.key, ev.mods);
	if (a == Action::None) {
		const Action mode = match_key(this->keys, ev.key, ev.mods);
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
