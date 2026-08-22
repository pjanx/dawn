//
// chrome.hpp: toolbar, sidebar, and page chrome widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "app-menu.hpp"
#include "hint.hpp"
#include "kit.hpp"
#include "types.hpp"

#include <functional>
#include <memory>
#include <span>
#include <string>

namespace dn
{

struct HostActions {
	std::function<void(Action)> apply;
	std::function<bool(Action)> enabled;
	std::function<void(std::string path)> activate;
	std::function<void(std::string path)> new_window;
	std::function<void(std::string path)> trash;
	std::function<void(QString path)> launch_exiftool;
};

struct ToolbarSlot : Row {
	Button *more = nullptr;
	std::size_t split_ = 0;

	ToolbarSlot();
	Widget *add_item(
		std::unique_ptr<Widget> item, std::size_t at = std::size_t(-1));
	[[nodiscard]] std::size_t item_count() const;
	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;

private:
	using Composite::add_child;
	using Composite::erase_children;
};

struct Toolbar : Panel {
	Actor actor;
	Button *app_menu_button = nullptr;
	ToolbarSlot *left = nullptr;
	ToolbarSlot *mid = nullptr;
	ToolbarSlot *right = nullptr;
	Overflow *overflow = nullptr;
	Menu *app_menu = nullptr;

	Toolbar(std::unique_ptr<ToolbarSlot> left_row,
		std::unique_ptr<ToolbarSlot> mid_row,
		std::unique_ptr<ToolbarSlot> right_row);
	[[nodiscard]] std::unique_ptr<Overflow> take_overflow();
	[[nodiscard]] std::unique_ptr<Menu> take_app_menu();
	void open_app_menu(Kit &kit, bool kbd);
	[[nodiscard]] bool app_menu_open() const
	{
		return this->app_menu && this->app_menu->visible;
	}
	void sync_buttons();

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;

private:
	std::unique_ptr<Overflow> overflow_owned_;
	std::unique_ptr<Menu> app_menu_owned_;
	void place_slots(Kit &kit);
	ToolbarSlot *slot_for_more(const Button *more) const;
};

struct Titlebar : Panel {
	Label *title = nullptr;
	Button *minimize = nullptr;
	Button *maximize = nullptr;
	Button *close = nullptr;
	Actor actor;
	QString text;

	Titlebar();
	void sync(Kit &kit);

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
};

struct Sidebar : Panel {
	Widget *content = nullptr;

	explicit Sidebar(std::unique_ptr<Widget> child);
	bool key(Kit &kit, int key, unsigned mods) override;
};

struct Page : Composite {
	Titlebar *titlebar = nullptr;
	Toolbar *toolbar = nullptr;
	Sidebar *sidebar = nullptr;
	Splitter *splitter = nullptr;
	Widget *content = nullptr;
	Widget *banner = nullptr;
	Modal *modal = nullptr;
	Hint *hint = nullptr;
	ContextMenu *context = nullptr;
	Actor actor;
	const HostActions *host = nullptr;
	std::span<const MenuNode> menu_tree = {};
	std::span<const Action> keys = {};

	enum class Side : uint8_t { None, Left, Right };
	Side side = Side::None;
	float sidebar_w = 192;
	bool sidebar_open = true;

	Page(std::unique_ptr<Toolbar> tb, std::unique_ptr<Sidebar> sb, Side side,
		std::unique_ptr<Widget> body);
	void set_banner(std::unique_ptr<Widget> w);

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	std::size_t child_count() const override;
	Widget *child(std::size_t i) const override;
	void apply_csd_cursor(Kit &kit);
	bool start_csd_resize(Kit &kit, float x, float y);

	[[nodiscard]] float toolbar_h() const { return this->well_.y - this->r.y; }
	[[nodiscard]] Rect well() const { return this->well_; }
	[[nodiscard]] Rect frame() const { return this->frame_; }

private:
	std::unique_ptr<Widget> banner_owned_;
	std::unique_ptr<Overflow> overflow_owned_;
	std::unique_ptr<Menu> app_menu_owned_;
	std::unique_ptr<Modal> modal_owned_;
	std::unique_ptr<Hint> hint_owned_;
	std::unique_ptr<ContextMenu> context_owned_;
	Rect well_{};
	Rect frame_{};
};

Actor chain_actor(const HostActions &host, std::function<bool(Action)> apply,
	std::function<bool(Action)> enabled, std::function<bool(Action)> checked);

}  // namespace dn
