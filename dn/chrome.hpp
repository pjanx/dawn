//
// chrome.hpp: toolbar, sidebar, and page chrome widgets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "hint.hpp"
#include "kit.hpp"

#include <QUrl>

#include <functional>
#include <memory>
#include <span>

namespace dn
{

struct HostActions {
	std::function<void(Action)> apply;
	std::function<bool(Action)> enabled;
	std::function<void(QUrl url)> activate;
	std::function<void(QUrl url)> new_window;
	std::function<void(QUrl url)> trash;
	std::function<void(QUrl url)> launch_exiftool;
};

// The menu that a right click on a file opens: what this application knows
// how to do with it, plus whatever the desktop can open it with.
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
	Dialog *dialog = nullptr;
	Hint *hint = nullptr;
	ContextMenu *context = nullptr;
	Menu *app_menu = nullptr;
	Button *app_menu_button = nullptr;
	Actor actor;
	const HostActions *host = nullptr;
	std::span<const MenuNode> menu_tree = {};
	std::span<const Action> keys = {};

	enum class Side : uint8_t { None, Left, Right };
	Side sidebar_side = Side::None;
	float sidebar_w = 192;
	bool sidebar_open = true;

	Page(std::unique_ptr<Toolbar> tb, std::unique_ptr<Sidebar> sb, Side side,
		std::unique_ptr<Widget> body);
	void set_banner(std::unique_ptr<Widget> w);
	void open_app_menu(Kit &kit, bool kbd);
	void sync_app_menu();
	[[nodiscard]] bool app_menu_open() const
	{
		return this->app_menu && this->app_menu->visible;
	}

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	std::size_t child_count() const override;
	Widget *child(std::size_t i) const override;

private:
	std::unique_ptr<Widget> banner_owned_;
	std::unique_ptr<Menu> app_menu_owned_;
	std::unique_ptr<Dialog> dialog_owned_;
	std::unique_ptr<Hint> hint_owned_;
	std::unique_ptr<ContextMenu> context_owned_;
	Rect well_{};
};

Actor chain_actor(const HostActions &host, std::function<bool(Action)> apply,
	std::function<bool(Action)> enabled, std::function<bool(Action)> checked);

}  // namespace dn
