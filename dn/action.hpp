//
// action.hpp: shared action table (labels, keys, menus)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QString>
#include <QUrl>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <span>
#include <vector>

class QMimeData;

namespace dn
{

enum class Action : uint8_t {
	None,
	// window
	NewWindow,
	CloseWindow,
	Minimize,
	Maximize,
	Quit,
	Fullscreen,
	DarkMode,
	Hint,
	Back,
	Forward,
	Help,
	About,
	Shortcuts,
	Menu,
	Context,
	Cancel,
	NextPane,
	PrevPane,
	// browser
	Sidebar,
	DirPrev,
	DirNext,
	DirParent,
	DirHome,
	ThumbMinus,
	ThumbPlus,
	ViewTile,
	ViewGrid,
	ViewList,
	Filenames,
	Filter,
	SortDir,
	SortName,
	SortTime,
	Activate,
	// viewer
	Browse,
	PrevFile,
	NextFile,
	ZoomIn,
	ZoomOut,
	Zoom1,
	ZoomLevel,
	Fit,
	FitWidth,
	FitHeight,
	Lock,
	Fixate,
	ColorManagement,
	Smooth,
	Checkerboard,
	RotateLeft,
	Mirror,
	RotateRight,
	Information,
	PageFirst,
	PagePrevious,
	PageNext,
	PageLast,
	FrameFirst,
	FramePrevious,
	PlayPause,
	FrameNext,
	Copy,
	Trash,
	Reload,
	Count,
};

enum : uint8_t { ActionInMenu = 1, ActionToggle = 2 };

struct Accel { uint32_t key = 0; uint32_t mods = 0; };

struct ActionDef {
	uint8_t flags = 0;
	const char *label[2] = {};
	const char *icon[2] = {};
	Accel keys[3] = {};
	const char *accel = {};
};

struct MenuNode {
	const char *title = nullptr;
	Action action = Action::None;
	std::vector<MenuNode> items = {};
	MenuNode() = default;
	MenuNode(Action a) : action(a) {}
	MenuNode(const char *t, std::initializer_list<MenuNode> xs)
		: title(t), items(xs) {}
};

struct Actor {
	std::function<void(Action)> apply;
	std::function<bool(Action)> enabled;
	std::function<bool(Action)> checked;
};

const ActionDef &action_def(Action);
Action match_key(std::span<const Action> scope, int key, unsigned mods);
QString accel_label(const Accel &);
QString accel_label(const ActionDef &);
QString menu_label(const char *label);
int mnemonic_index(const char *label);
const char *action_label(const ActionDef &, bool checked);
const char *action_icon(const ActionDef &, bool checked);
QString action_tip(const ActionDef &, bool checked);
QString action_accel(const ActionDef &);

std::span<const MenuNode> browser_menu();
std::span<const MenuNode> viewer_menu();
std::span<const Action> window_keys();
std::span<const Action> browser_keys();
std::span<const Action> viewer_keys();

void copy_files(QMimeData *mime, std::span<const QUrl> urls, bool cut = false);
void copy_files(std::span<const QUrl> urls, bool cut = false);
bool move_to_trash(const QString &abs_path);

}  // namespace dn
