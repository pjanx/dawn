//
// app-menu.hpp: application menu and information overlay widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "kit.hpp"
#include "types.hpp"

#include <QUrl>

#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace dn
{

struct MenuItem;

struct Popup : Panel {
	Button *opener = nullptr;
	Popup *parent_popup = nullptr;
	Rect at{};

	Popup();
	void open(Kit &kit, Button *anchor = nullptr);
	void open_at(Kit &kit, Rect anchor);
	void open_sub(Kit &kit, Popup &owner, Button &anchor);
	virtual void close(Kit &kit);
	virtual void place(Kit &kit);
	void place_sub(Kit &kit);
	void focus_item(Kit &kit, Widget *w, bool kbd) const;
	void reveal(Kit &kit, Widget *w);
	void select_first(Kit &kit);
	bool traps_focus() const override { return true; }
	virtual bool captures_keys() const { return false; }
	void paint(Kit &kit) const override;
	bool motion(Kit &kit, float x, float y) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

struct Overflow : Popup {
	Column *col = nullptr;
	std::vector<Widget *> sources;
	std::function<void()> fill_items;

	Overflow();
	void place(Kit &kit) override;
};

struct Menu : Popup {
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

struct Modal : Popup {
	Panel *dialog = nullptr;
	AppOverlay kind = AppOverlay::None;
	std::span<const MenuNode> tree = {};
	std::span<const Action> keys = {};

	Modal();
	void set_kind(Kit &kit, AppOverlay overlay);
	void open(Kit &kit);
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void paint(Kit &kit) const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;

private:
	void fill_dialog(Kit &kit);
};

}  // namespace dn
