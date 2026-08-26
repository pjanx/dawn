//
// kit-chrome.cpp: non-generic widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "assoc.hpp"
#include "kit-chrome.hpp"
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
Sidebar::key(Kit &kit, int key, unsigned mods)
{
	if (mods)
		return false;
	if (key != Qt::Key_Up && key != Qt::Key_Down)
		return false;
	const int dir = key == Qt::Key_Up ? -1 : 1;
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

	auto add_app = [&](const Handler &app) {
		if (app.id.isEmpty())
			return;
		auto &item = add_item(app.name.isEmpty() ? app.id : app.name);
		item.mnemonic = -1;
		item.on_click = [app, path](Kit &) {
			launch(app, path);
			set_last_used(app, path);
		};
	};

	bool need_sep = false;
	auto flush_sep = [&] {
		if (!need_sep)
			return;
		add_sep();
		need_sep = false;
	};

	auto &new_win = add_item_with_mnemonic("Open in New _Window");
	new_win.on_click = [this, url](Kit &) {
		if (this->on_new_window)
			this->on_new_window(url);
	};
	need_sep = true;
	if (!def.id.isEmpty()) {
		flush_sep();
		add_app(def);
		need_sep = true;
	}
	if (!rec.empty()) {
		flush_sep();
		for (const Handler &app : rec)
			add_app(app);
		need_sep = true;
	}
	if (!fall.empty()) {
		flush_sep();
		for (const Handler &app : fall)
			add_app(app);
		need_sep = true;
	}
	if (QFileInfo(path).isFile() && this->on_trash) {
		flush_sep();
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
