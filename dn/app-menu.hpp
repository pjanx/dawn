//
// app-menu.hpp: application menu and information overlay widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "kit.hpp"

#include <QUrl>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace dn
{

struct MenuItem;

// A popup navigated like a menu: hovering moves the focus, arrows walk the
// items, and a click anywhere else in the stack dismisses it.
struct MenuPopup : Popup {
	void focus_item(Kit &kit, Widget *w, bool kbd) const;
	void reveal(Kit &kit, Widget *w);
	bool motion(Kit &kit, float x, float y) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

struct Overflow : MenuPopup {
	Column *col = nullptr;
	std::vector<Widget *> sources;
	std::function<void()> fill_items;

	Overflow();
	void place(Kit &kit) override;
};

struct Menu : MenuPopup {
	Column *col = nullptr;
	Actor actor;
	std::vector<std::unique_ptr<Menu>> subs_;

	Menu();
	void build(std::span<const MenuNode> nodes, const Actor &actor);
	void sync();
	MenuItem &add_item(const QString &text);
	void add_sep();
	void clear();
	void measure(Kit &kit, float max_w, float max_h) override;
	bool key(Kit &kit, int key, unsigned mods) override;
};

struct MenuItem : Button {
	QString accel;
	// FIXME: This is an index into a label, it should at least be a unichar.
	int mnemonic = -1;
	Menu *sub = nullptr;
	bool checked = false;
	bool checkable = false;
	float label_col = 0;
	float accel_col = 0;

	void measure(Kit &kit, float max_w, float max_h) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool activate(Kit &kit) override;
	float label_width(const Kit &kit) const;
	float accel_width(const Kit &kit) const;
};

struct ContextMenu : Menu {
	std::function<void(const QUrl &url)> on_new_window;
	std::function<void(const QUrl &url)> on_trash;

	void show(Kit &kit, const QUrl &url, Rect anchor, bool kbd);

private:
	void fill_items(const QUrl &url);
};

void dialog_about(Kit &kit, Dialog &dialog);
void dialog_shortcuts(Kit &kit, Dialog &dialog, std::span<const MenuNode> tree,
	std::span<const Action> keys);

}  // namespace dn
