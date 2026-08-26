//
// app-menu.cpp: application menu and information overlay widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "action.hpp"
#include "app-menu.hpp"
#include "assoc.hpp"
#include "chrome.hpp"
#include "url.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QKeyEvent>

#include <algorithm>
#include <chrono>
#include <iterator>
#include <memory>

using namespace std;

namespace dn
{
namespace
{

constexpr float kItemGap = 2.0f;
constexpr float kMenuPad = 4.0f;
constexpr float kDialogPad = 16.0f;
constexpr float kMenuHoldMs = 500.0f;  // GTK MENU_SHELL_TIMEOUT

unique_ptr<Sep>
hsep()
{
	return make_unique<Sep>();
}

Colour
col(const Colour &c, float alpha = 1.0f)
{
	return {c.r, c.g, c.b, c.a * alpha};
}

void
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
	auto name = dialog_label(menu_label(def.label[0]));
	row->add_child(std::move(accel));
	row->add_child(std::move(name));
	return row;
}

}  // namespace

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

void
Popup::focus_item(Kit &kit, Widget *w, bool kbd) const
{
	kit.focus_ = w;
	kit.focus_visible_ = kbd;
	if (kbd)
		kit.hot_ = nullptr;
}

void
Popup::reveal(Kit &kit, Widget *w)
{
	auto *item = dynamic_cast<MenuItem *>(w);
	if (item && item->sub) {
		if (!item->sub->visible)
			item->sub->open_sub(kit, *this, *item);
		kit.focus_ = item->sub;
		kit.focus_visible_ = false;
		return;
	}
	kit.close_above(this);
	focus_item(kit, w, false);
}

void
Popup::select_first(Kit &kit)
{
	kit.focus_first(this);
}

bool
Popup::motion(Kit &kit, float, float)
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
Popup::key(Kit &kit, int key, unsigned mods)
{
	if (mods & unsigned(Qt::AltModifier))
		return false;
	if (key == Qt::Key_Escape) {
		Button *op = this->opener;
		close(kit);
		if (op) {
			kit.focus_ = op;
			kit.focus_visible_ = true;
		}
		return true;
	}
	if (key == Qt::Key_Up || key == Qt::Key_Down) {
		kit.cycle_focus(this, key == Qt::Key_Up ? -1 : 1);
		focus_item(kit, kit.focus_, true);
		return true;
	}
	if (key == Qt::Key_Right || key == Qt::Key_Return || key == Qt::Key_Enter ||
		key == Qt::Key_Space) {
		if (auto *item = dynamic_cast<MenuItem *>(kit.focus_);
			item && item->sub) {
			item->activate(kit);
			return true;
		}
		if (key == Qt::Key_Right)
			return true;
		return false;
	}
	if (key == Qt::Key_Left) {
		if (this->parent_popup) {
			Button *op = this->opener;
			close(kit);
			if (op) {
				kit.focus_ = op;
				kit.focus_visible_ = true;
			}
		}
		return true;
	}
	return false;
}

bool
Popup::release(Kit &kit, float x, float y, Qt::MouseButton button)
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
			auto &item = add_item(menu_label(node.title));
			item.mnemonic = mnemonic_index(node.title);
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
		auto &item = add_item(menu_label(action_label(def, false)));
		item.action = action;
		item.accel = accel_label(def);
		item.mnemonic = mnemonic_index(def.label[0]);
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
			const char *label = action_label(def, item->checked);
			item->text = menu_label(label);
			item->mnemonic = mnemonic_index(label);
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
Menu::key(Kit &kit, int key, unsigned mods)
{
	if (Popup::key(kit, key, mods))
		return true;
	if (mods)
		return false;
	if (key < Qt::Key_A || key > Qt::Key_Z || !this->col)
		return false;
	const QChar letter = QChar(key).toLower();
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
	this->r = {0, 0, kit.snap_size(width),
		kit.snap_size(kFramePadY * 2.0f + ch)};
}

void
MenuItem::paint(Kit &kit) const
{
	if (!this->visible)
		return;
	const bool pressed = kit.left_down_ && kit.pressed_ == this;
	if (this->enabled_ && (pressed || this->active || kit.focus_ == this))
		kit.list_.add_rect_filled(this->r.x, this->r.y, this->r.x + this->r.w,
			this->r.y + this->r.h, col(kit.press_));

	const float pad = kFramePadX;
	const float aw =
		this->accel_col > 0.0f ? this->accel_col : accel_width(kit);
	const float chevron = this->sub ? kIconPx : 0.0f;
	const float lead_x = this->r.x + pad;
	const float label_x = lead_x + kIconPx + pad;
	const float accel_x = this->r.x + this->r.w - pad - chevron - aw;
	const float th = this->text.isEmpty()
		? 0.0f
		: kit.text_height(this->text, 0.0f, false);
	const float ty = this->r.y + (this->r.h - th) * 0.5f;
	const float iy = this->r.y + (this->r.h - kIconPx) * 0.5f;
	const Colour label_c = col(kit.ink_, this->enabled_ ? 1.0f : 0.5f);

	if (this->checkable && this->checked)
		emit_icon(kit, lead_x, iy, kIconPx, "object-select-symbolic", label_c);
	if (!this->text.isEmpty()) {
		const float avail = max(1.0f, accel_x - pad - label_x);
		const QString shown = kit.elide_lines(this->text, avail, 1, false);
		kit.emit_text(label_x, ty, shown, label_c, false);
		if (this->mnemonic >= 0 && this->mnemonic < shown.size() &&
			this->mnemonic < this->text.size() &&
			shown[this->mnemonic] == this->text[this->mnemonic]) {
			const QString left = shown.left(this->mnemonic);
			const QString ch = shown.mid(this->mnemonic, 1);
			const float x0 = label_x + kit.text_width(left, false);
			const float x1 = x0 + kit.text_width(ch, false);
			const float uy = kit.snap(ty + kit.text_ascent(false) + 1.0f);
			kit.list_.add_line(x0, uy, x1, uy, label_c);
		}
	}
	if (!this->accel.isEmpty()) {
		const float tw = kit.text_width(this->accel, false);
		const float ath = kit.text_height(this->accel, 0.0f, false);
		kit.emit_text(accel_x + aw - tw,
			this->r.y + (this->r.h - ath) * 0.5f, this->accel,
			col(kit.ink_, 0.5f), false);
	}
	if (this->sub) {
		emit_icon(kit, this->r.x + this->r.w - pad - chevron, iy, kIconPx,
			"go-next-symbolic", col(kit.ink_, this->enabled_ ? 1.0f : 0.375f));
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
	const float pad = kFramePadX;
	const float aw =
		this->accel_col > 0.0f ? this->accel_col : accel_width(kit);
	const float chev = this->sub ? kIconPx : 0.0f;
	const float label_x = pad + kIconPx + pad;
	const float accel_x = this->r.w - pad - chev - aw;
	const float avail = max(1.0f, accel_x - pad - label_x);
	kit.cache_text(kit.elide_lines(this->text, avail, 1, false), false);
	if (!this->accel.isEmpty())
		kit.cache_text(this->accel, false);
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

	auto &new_win = add_item(menu_label("Open in New _Window"));
	new_win.mnemonic = mnemonic_index("Open in New _Window");
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
		auto &trash = add_item(menu_label("Move to _Trash"));
		trash.mnemonic = mnemonic_index("Move to _Trash");
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
		select_first(kit);
}

Modal::Modal()
{
	this->hittable = true;
	this->visible = false;
	this->fill = Fill::None;
	auto d = make_unique<Panel>();
	d->pad_x = kDialogPad;
	d->pad_y = kDialogPad;
	d->fill = Fill::Panel;
	d->stroke = Stroke::All;
	d->hittable = false;
	d->visible = false;
	this->dialog = d.get();
	add_child(std::move(d));
}

void
Modal::fill_dialog(Kit &kit)
{
	if (!this->dialog)
		return;
	kit.forget_tree(this->dialog);
	this->dialog->erase_children();
	if (this->kind == AppOverlay::None)
		return;
	auto col = make_unique<Column>();
	col->gap = 8.0f;
	if (this->kind == AppOverlay::About) {
		QString name = QStringLiteral(DAWN_NAME);
		col->add_child(dialog_label(name, true));
		col->add_child(dialog_label(
			QStringLiteral("Colour-managed image browser and viewer."), false,
			true));
	} else {
		bool seen[size_t(Action::Count)] = {};
		float accel_w = 120.0f;
		auto consider = [&](Action action) {
			const ActionDef &def = action_def(action);
			if (!has_shortcut(def))
				return;
			accel_w = max(accel_w, kit.text_width(shortcut_accel(def), false));
		};
		for_leaves(this->tree, consider);
		auto consider_other = [&](Action action) {
			const ActionDef &def = action_def(action);
			if ((def.flags & ActionInMenu) || !has_shortcut(def))
				return;
			consider(action);
		};
		bool viewer = false;
		for (Action action : this->keys) {
			if (action == Action::ZoomIn || action == Action::FitWidth) {
				viewer = true;
				break;
			}
		}
		for (Action action : window_keys())
			consider_other(action);
		for (Action action : this->keys)
			consider_other(action);
		consider_other(Action::Cancel);
		if (viewer)
			consider_other(Action::ZoomLevel);

		col->gap = 2.0f;
		col->add_child(
			dialog_label(QStringLiteral("Keyboard Shortcuts"), true));
		for (const MenuNode &section : this->tree) {
			if (section.items.empty())
				continue;
			bool any = false;
			for_leaves(section.items, [&](Action action) {
				if (has_shortcut(action_def(action)))
					any = true;
			});
			if (!any)
				continue;
			col->add_child(dialog_label(menu_label(section.title), true));
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
		for (Action action : this->keys)
			emit_other(action);
		emit_other(Action::Cancel);
		if (viewer)
			emit_other(Action::ZoomLevel);
	}
	col->add_child(dialog_label(
		QStringLiteral("Click or press Escape to dismiss."), false, true));
	this->dialog->min_w = this->kind == AppOverlay::About ? 360.0f : 520.0f;
	this->dialog->add_child(std::move(col));
}

void
Modal::set_kind(Kit &kit, AppOverlay overlay)
{
	if (overlay == AppOverlay::None) {
		this->kind = AppOverlay::None;
		fill_dialog(kit);
		close(kit);
		return;
	}
	if (this->kind == overlay && this->visible)
		return;
	this->kind = overlay;
	fill_dialog(kit);
	open(kit);
}

void
Modal::open(Kit &kit)
{
	Popup::open(kit);
	if (this->dialog)
		this->dialog->visible = true;
}

void
Modal::close(Kit &kit)
{
	this->kind = AppOverlay::None;
	if (this->dialog)
		this->dialog->visible = false;
	Popup::close(kit);
}

void
Modal::place(Kit &kit)
{
	if (this->kind == AppOverlay::None || !this->dialog) {
		this->r = {};
		return;
	}
	this->visible = true;
	this->dialog->visible = true;
	this->r = {0.0f, 0.0f, kit.host_w_, kit.host_h_};
	float top = 0.0f;
	if (auto *f = dynamic_cast<Page *>(kit.root_))
		top = f->toolbar_h();
	const float max_w = max(1.0f, min(560.0f, kit.host_w_ - 32.0f));
	const float well_h = max(1.0f, kit.host_h_ - top);
	this->dialog->measure(kit, max_w, well_h);
	const float x = max(0.0f, (kit.host_w_ - this->dialog->r.w) * 0.5f);
	const float y = top + max(0.0f, (well_h - this->dialog->r.h) * 0.5f);
	this->dialog->arrange(kit, {x, y, this->dialog->r.w, this->dialog->r.h});
}

void
Modal::paint(Kit &kit) const
{
	if (!this->visible)
		return;
	if (this->dialog && this->dialog->visible)
		kit.draw_shadow(this->dialog->r);
	Panel::paint(kit);
}

bool
Modal::press(Kit &kit, float, float, Qt::MouseButton button)
{
	if (button != Qt::LeftButton)
		return false;
	close(kit);
	kit.pressed_ = nullptr;
	return true;
}

}  // namespace dn
