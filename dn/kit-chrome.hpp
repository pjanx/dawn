//
// kit-chrome.hpp: non-generic widgets
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "kit.hpp"

#include <QUrl>

#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace dn
{

struct HostActions {
	std::function<void(Action)> apply;
	std::function<bool(Action)> enabled;
	std::function<void(QUrl url)> activate;
	std::function<void(QUrl url)> new_window;
	std::function<void(QUrl url)> trash;
	std::function<void(QUrl url)> launch_exiftool;
	std::function<std::vector<std::string>()> bookmarks;
	std::function<bool(const QUrl &url)> bookmarked;
	std::function<void(QUrl url)> toggle_bookmark;
};

// The menu that a right click on a file opens: what this application knows
// how to do with it, plus whatever the desktop can open it with.
struct ContextMenu : Menu {
	std::function<void(const QUrl &url)> on_new_window;
	std::function<void(const QUrl &url)> on_trash;
	std::function<bool(const QUrl &url)> on_bookmarked;
	std::function<void(const QUrl &url)> on_toggle_bookmark;

	void show(Kit &kit, const QUrl &url, Rect anchor, bool kbd);

private:
	void fill_items(Kit &kit, const QUrl &url);
};

// The thumbnail sizes the browser has, smallest first.  One table: what the
// settings dialog offers must not drift from what the browser can show, nor
// from what Settings::load() will accept back off disk.
constexpr int kThumbSizes[] = {128, 256, 512, 1024};

// What the settings dialog edits: a copy it owns, handed back whole on Save.
// Deliberately not Settings itself -- the dialog has no business knowing
// where any of this is kept, or what listens for it to change.
struct SettingsDraft {
	int thumbnail_size = 256;
	bool show_filenames = true;
	QString icc_profile_path;
	bool disable_dithering = false;

	struct Loader {
		QString name;
		QString formats;
		bool enabled = true;
	};

	std::vector<Loader> loaders;
};

void dialog_about(Kit &kit, Dialog &dialog);
void dialog_shortcuts(Kit &kit, Dialog &dialog, std::span<const MenuNode> tree,
	std::span<const Action> keys);
void dialog_location(Kit &kit, Dialog &dialog,
	std::function<void(const QString &)> on_open);

// Takes the draft by value: the dialog edits its own copy, and Save is the
// only way anything gets back out.  An empty loader list is filled in with
// the placeholder one.
void dialog_settings(Kit &kit, Dialog &dialog, SettingsDraft draft,
	std::function<void(const SettingsDraft &)> on_save);

struct Sidebar : Panel {
	Widget *content = nullptr;

	explicit Sidebar(std::unique_ptr<Widget> child);
	bool key(Kit &kit, const Key &ev) override;
};

struct Browser;
struct Page;

struct Hint : Popup {
	Page *page = nullptr;

	Hint();
	void open(Kit &kit);
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void prepare(Kit &kit) override;
	void paint(Kit &kit) const override;
	bool captures_keys() const override { return true; }
	bool key(Kit &kit, const Key &ev) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;

private:
	struct Target {
		QString label;
		Rect at{};
		Rect chip{};
		Widget *widget = nullptr;
		// TODO(p): Unwanted dependency.
		Browser *browser = nullptr;
		int file_i = -1;
	};

	std::vector<Target> targets_;
	QString typed_;

	void collect();
	void assign_labels();
	void refresh_rects();
	void layout_chips(const Kit &kit);
	[[nodiscard]] bool matches(const Target &t) const;
	void fire(Kit &kit, Target t);
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

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	bool key(Kit &kit, const Key &ev) override;
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
