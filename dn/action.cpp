//
// action.cpp: shared action table (labels, keys, menus)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "action.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QKeySequence>
#include <QList>
#include <QMimeData>
#include <QUrl>
#include <QtLogging>

using namespace std;

namespace dn
{

constexpr uint8_t kMenu = ActionInMenu;
constexpr uint8_t kToggle = ActionInMenu | ActionToggle;
constexpr unsigned kCtrl = unsigned(Qt::ControlModifier);
constexpr unsigned kAlt = unsigned(Qt::AltModifier);
constexpr unsigned kShift = unsigned(Qt::ShiftModifier);
[[maybe_unused]] constexpr unsigned kMeta = unsigned(Qt::MetaModifier);

// clang-format off
constexpr ActionDef kDefs[] = {
	{},
	// TRANSLATORS: The underscore in a label marks the mnemonic, the letter
	// that follows it.  Every label in this file works that way; keep one,
	// on a letter that no other entry of the same menu has taken.
	{kMenu, {N_("_New Window")}, {}, {{Qt::Key_N, kCtrl}}, {}},
	{kMenu, {N_("_Close Window")}, {}, {{Qt::Key_W, kCtrl}, {Qt::Key_Q}}, {}},
	{0, {N_("_Minimise")}, {}, {}, {}},
	{kToggle, {N_("_Maximise"), N_("Res_tore")}, {}, {}, {}},
	{kMenu, {N_("_Quit")}, {}, {{Qt::Key_Q, kCtrl}}, {}},
	{kToggle, {N_("Enter _Full Screen"), N_("Exit _Full Screen")},
		{"view-fullscreen-symbolic", "view-restore-symbolic"}, {
#ifdef Q_OS_MACOS
		{Qt::Key_F, kCtrl | kMeta},
#else
		{Qt::Key_F11},
#endif
		}, {}},
	{kToggle, {N_("_Dark Mode")}, {"dark-mode-symbolic"}, {{Qt::Key_D}}, {}},
	// TRANSLATORS: Labels every clickable thing with a letter to type.
	{kMenu, {N_("_Hint")}, {}, {{Qt::Key_F}}, {}},
	{kMenu, {N_("_Back in History")}, {"curved-arrow-left-symbolic"}, {
#ifdef Q_OS_MACOS
		{Qt::Key_BracketLeft, kCtrl},
#else
		{Qt::Key_Left, kAlt},
#endif
		{Qt::Key_Backspace}}, {}},
	{kMenu, {N_("_Forward in History")}, {"curved-arrow-right-symbolic"}, {
#ifdef Q_OS_MACOS
		{Qt::Key_BracketRight, kCtrl},
#else
		{Qt::Key_Right, kAlt},
#endif
		}, {}},
	{kMenu, {N_("_Location...")}, {}, {
#ifdef Q_OS_MACOS
		{Qt::Key_G, kCtrl | kShift},
#else
		{Qt::Key_L, kCtrl},
#endif
		}, {}},
	{kMenu, {N_("_Contents")}, {}, {
#ifdef Q_OS_MACOS
		{Qt::Key_Question, kCtrl},
#else
		{Qt::Key_F1},
#endif
		}, {}},
	{kMenu, {N_("_About")}, {}, {}, {}},
	{kMenu, {N_("_Keyboard Shortcuts")}, {}, {
#ifndef Q_OS_MACOS
		{Qt::Key_Question, kCtrl},
#endif
		}, {}},
	{kMenu, {N_("_Settings...")}, {}, {{Qt::Key_Comma, kCtrl}}, {}},
	// TODO(p): Skip on macOS entirely, as it uses the global menu.
	{0, {N_("_Menu")}, {}, {{Qt::Key_F10}}, {}},
	// The context menu action is mostly for documentation only.
	{0, {N_("_Context Menu")}, {}, {{Qt::Key_Menu}, {Qt::Key_F10, kShift}}, {}},
	{0, {N_("_Cancel")}, {}, {{Qt::Key_Escape}}, {}},
	{0, {N_("_Next Pane")}, {}, {{Qt::Key_F6}}, {}},
	{0, {N_("_Previous Pane")}, {}, {{Qt::Key_F6, kShift}}, {}},

	{kToggle, {N_("Show _Sidebar")}, {"sidebar-left-symbolic"},
		{{Qt::Key_F9}}, {}},
	{kMenu, {N_("_Previous Directory in Tree")}, {"go-previous-symbolic"},
		{{Qt::Key_BracketLeft}}, {}},
	{kMenu, {N_("_Next Directory in Tree")}, {"go-next-symbolic"},
		{{Qt::Key_BracketRight}}, {}},
	{kMenu, {N_("Parent _Directory")}, {"go-up-symbolic"}, {
#ifdef Q_OS_MACOS
		{Qt::Key_Up, kCtrl},
#else
		{Qt::Key_Up, kAlt},
#endif
		}, {}},
	{kMenu, {N_("_Home")}, {}, {
#ifdef Q_OS_MACOS
		{Qt::Key_H, kCtrl | kShift},
#else
		{Qt::Key_Home, kAlt},
#endif
		}, {}},
	{kMenu, {N_("S_maller Thumbnails")}, {"minus-framed-symbolic"},
		{{Qt::Key_Minus}, {Qt::Key_Minus, kCtrl}}, {}},
	{kMenu, {N_("_Larger Thumbnails")}, {"plus-framed-symbolic"},
		{{Qt::Key_Plus}, {Qt::Key_Plus, kCtrl}}, {}},
	{kToggle, {N_("Tiled _View")}, {"blocks-symbolic"},
		{{Qt::Key_1}, {Qt::Key_1, kCtrl}}, {}},
	{kToggle, {N_("_Grid View")}, {"view-grid-symbolic"},
		{{Qt::Key_2}, {Qt::Key_2, kCtrl}}, {}},
	{kToggle, {N_("L_ist View")}, {"view-list-symbolic"},
		{{Qt::Key_3}, {Qt::Key_3, kCtrl}}, {}},
	{kToggle, {N_("Sho_w Filenames")}, {"font-symbolic"},
		{{Qt::Key_T}, {Qt::Key_T, kCtrl}}, {}},
	{kToggle, {N_("Hide _Unsupported Files")}, {"filter-symbolic"},
		{{Qt::Key_H}, {Qt::Key_H, kCtrl}}, {}},
	{kToggle, {N_("Sort Des_cending"), N_("Sort As_cending")},
		{"view-sort-descending-symbolic", "view-sort-ascending-symbolic"},
		{{Qt::Key_C}}, {}},
	{kToggle, {N_("Sort by _Name")}, {}, {{Qt::Key_1, kCtrl | kAlt}}, {}},
	{kToggle, {N_("Sort by _Time")}, {}, {{Qt::Key_2, kCtrl | kAlt}}, {}},
	{kMenu, {N_("_Filter")}, {}, {{Qt::Key_F, kCtrl}, {Qt::Key_Slash}}, {}},
	{0, {N_("_Open")}, {}, {
		// In Finder, Return nonsensically renames items.
		{Qt::Key_Return}, {Qt::Key_Enter},
#ifdef Q_OS_MACOS
		{Qt::Key_Down, kCtrl},
#endif
		}, {}},

	{kMenu, {N_("_Browse")}, {"blocks-symbolic"},
		{{Qt::Key_Return}, {Qt::Key_Enter}}, {}},
	{kMenu, {N_("_Previous File")}, {"go-previous-symbolic"},
		{{Qt::Key_Left}, {Qt::Key_Up}, {Qt::Key_PageUp}}, {}},
	{kMenu, {N_("_Next File")}, {"go-next-symbolic"},
		{{Qt::Key_Right}, {Qt::Key_Down}, {Qt::Key_PageDown}}, {}},
	{kMenu, {N_("Zoom _In")}, {"plus-framed-symbolic"},
		{{Qt::Key_Plus}, {Qt::Key_Plus, kCtrl}}, {}},
	{kMenu, {N_("Zoom _Out")}, {"minus-framed-symbolic"},
		{{Qt::Key_Minus}, {Qt::Key_Minus, kCtrl}}, {}},
	{kMenu, {N_("O_riginal Size")}, {"one-framed-symbolic"},
		{{Qt::Key_0, kCtrl}}, {}},
	// TRANSLATORS: The toolbar's zoom readout; "1-9" beside it is the range
	// of digit keys that set it, and is not a label.
	{0, {N_("Zoom _Level")}, {}, {}, "1-9"},
	{kToggle, {N_("_Scale to Fit")}, {"zoom-fit-symbolic"}, {{Qt::Key_X}}, {}},
	{0, {N_("Fit to _Width")}, {}, {{Qt::Key_W}}, {}},
	{0, {N_("Fit to H_eight")}, {}, {{Qt::Key_H}}, {}},
	{kToggle, {N_("_Lock View")},
		{"padlock-open-symbolic", "padlock-closed-symbolic"},
		{{Qt::Key_L}}, {}},
	// TRANSLATORS: Keeps the current zoom and scroll position across images.
	{kToggle, {N_("_Keep Zoom and Position")}, {"pin2-symbolic"},
		{{Qt::Key_K}}, {}},
	{kToggle, {N_("_Colour Management")}, {"color-symbolic"}, {{Qt::Key_C}}, {}},
	{kToggle, {N_("S_mooth Scaling")}, {"blend-tool-symbolic"}, {{Qt::Key_I}}, {}},
	{kToggle, {N_("Highlight _Transparency")},
		{"transparent-background-symbolic"}, {{Qt::Key_T}}, {}},
	{kToggle, {N_("Blend in Linear Light")}, {}, {}, {}},
	{kMenu, {N_("Rotate _Left")}, {"rotate-acw-symbolic"}, {{Qt::Key_Less}}, {}},
	// TRANSLATORS: A verb: flips the image horizontally.
	{kMenu, {N_("_Mirror")}, {"flip-h-symbolic"}, {{Qt::Key_Equal}}, {}},
	{kMenu, {N_("Rotate _Right")}, {"rotate-cw-symbolic"},
		{{Qt::Key_Greater}}, {}},
	{kToggle, {N_("Show I_nformation")}, {"info-outline-symbolic"},
		{{Qt::Key_Return, kAlt}, {Qt::Key_Enter, kAlt}}, {}},
	{kMenu, {N_("_First Page")}, {"go-top-symbolic"}, {}, {}},
	{kMenu, {N_("Pr_evious Page")}, {"go-up-symbolic"},
		{{Qt::Key_BracketLeft}}, {}},
	{kMenu, {N_("_Next Page")}, {"go-down-symbolic"},
		{{Qt::Key_BracketRight}}, {}},
	{kMenu, {N_("La_st Page")}, {"go-bottom-symbolic"}, {}, {}},
	{kMenu, {N_("Re_wind")}, {"media-skip-backward-symbolic"}, {}, {}},
	{kMenu, {N_("Pre_vious Frame")}, {"media-seek-backward-symbolic"},
		{{Qt::Key_BraceLeft}}, {}},
	{kToggle, {N_("_Play"), N_("_Pause")},
		{"media-playback-start-symbolic", "media-playback-pause-symbolic"},
		{{Qt::Key_Space}}, {}},
	{kMenu, {N_("Ne_xt Frame")}, {"media-seek-forward-symbolic"},
		{{Qt::Key_BraceRight}}, {}},
	{0, {N_("_Copy")}, {}, {{Qt::Key_C, kCtrl}, {Qt::Key_Insert, kCtrl}}, {}},
	{0, {N_("Move to _Trash")}, {}, {{Qt::Key_Delete}}, {}},
	{kMenu, {N_("_Reload")}, {"arrows-circle-symbolic"},
		{{Qt::Key_F5}, {Qt::Key_R}, {Qt::Key_R, kCtrl}}, {}},
};
// clang-format on

static_assert(size(kDefs) == size_t(Action::Count));

// Shift is frequently just the means of typing a punctuation character:
// Ctrl+? arrives as Ctrl+Shift+? where the question mark sits above the
// slash.  Letters, digits and named keys stay strict.
static constexpr bool
shift_is_incidental(uint32_t key)
{
	if (key <= uint32_t(Qt::Key_Space) || key >= 0x7f)
		return false;
	return !(key >= uint32_t(Qt::Key_0) && key <= uint32_t(Qt::Key_9)) &&
		!(key >= uint32_t(Qt::Key_A) && key <= uint32_t(Qt::Key_Z));
}

static_assert(shift_is_incidental(uint32_t(Qt::Key_Question)));
static_assert(!shift_is_incidental(uint32_t(Qt::Key_A)));
static_assert(!shift_is_incidental(uint32_t(Qt::Key_Return)));

static constexpr Action
match_exact(span<const Action> scope, uint32_t key, uint32_t mods)
{
	for (Action action : scope) {
		const size_t i = size_t(action);
		if (i >= size(kDefs))
			continue;
		for (const Accel &a : kDefs[i].keys) {
			if (a.key && a.key == key && a.mods == mods)
				return action;
		}
	}
	return Action::None;
}

constexpr Action kWindowKeys[] = {
	Action::NewWindow,
	Action::CloseWindow,
	Action::Quit,
	Action::Fullscreen,
	Action::DarkMode,
	Action::Hint,
	Action::Back,
	Action::Forward,
	Action::Location,
	Action::Help,
	Action::About,
	Action::Shortcuts,
	Action::Settings,
	Action::Menu,
	Action::NextPane,
	Action::PrevPane,
	Action::Reload,
};

constexpr Action kBrowserKeys[] = {
	Action::Sidebar,
	Action::DirPrev,
	Action::DirNext,
	Action::DirParent,
	Action::DirHome,
	Action::ThumbMinus,
	Action::ThumbPlus,
	Action::ViewTile,
	Action::ViewGrid,
	// TODO: Action::ViewList,
	Action::Filenames,
	Action::Filter,
	Action::SortDir,
	Action::SortName,
	Action::SortTime,
	Action::Search,
	Action::Activate,
	Action::Copy,
	Action::Trash,
	Action::Context,
};

constexpr Action kViewerKeys[] = {
	Action::Browse,
	Action::PrevFile,
	Action::NextFile,
	Action::ZoomIn,
	Action::ZoomOut,
	Action::Zoom1,
	Action::Fit,
	Action::FitWidth,
	Action::FitHeight,
	Action::Lock,
	Action::Fixate,
	Action::ColorManagement,
	Action::Smooth,
	Action::Checkerboard,
	Action::RotateLeft,
	Action::Mirror,
	Action::RotateRight,
	Action::Information,
	Action::PageFirst,
	Action::PagePrevious,
	Action::PageNext,
	Action::PageLast,
	Action::FrameFirst,
	Action::FramePrevious,
	Action::PlayPause,
	Action::FrameNext,
	Action::Copy,
	Action::Trash,
	Action::Context,
};

// clang-format off
const MenuNode kFileMenu = MenuNode::group(N_("_File"), {
	MenuNode::item(Action::NewWindow),
	MenuNode::item(Action::CloseWindow),
	{},
	MenuNode::item(Action::Reload),
	{},
	MenuNode::item(Action::Settings),
	{},
	MenuNode::item(Action::Quit),
});

const MenuNode kHelpMenu = MenuNode::group(N_("_Help"), {
	MenuNode::item(Action::Help),
	MenuNode::item(Action::Shortcuts),
	MenuNode::item(Action::About),
});

const MenuNode kBrowserMenu[] = {
	kFileMenu,
	MenuNode::group(N_("_Go"), {
		MenuNode::item(Action::Back),
		MenuNode::item(Action::Forward),
		MenuNode::item(Action::Location),
		{},
		MenuNode::item(Action::DirPrev),
		MenuNode::item(Action::DirNext),
		MenuNode::item(Action::DirParent),
		MenuNode::item(Action::DirHome),
	}),
	MenuNode::group(N_("_View"), {
		MenuNode::item(Action::Sidebar),
		{},
		MenuNode::item(Action::ThumbPlus),
		MenuNode::item(Action::ThumbMinus),
		{},
		MenuNode::item(Action::ViewTile),
		MenuNode::item(Action::ViewGrid),
		// TODO: Action::ViewList,
		{},
		MenuNode::item(Action::Filenames),
		MenuNode::item(Action::Filter),
		{},
		MenuNode::item(Action::SortDir),
		MenuNode::item(Action::SortName),
		MenuNode::item(Action::SortTime),
		{},
		MenuNode::item(Action::Search),
		MenuNode::item(Action::Hint),
		MenuNode::item(Action::DarkMode),
		MenuNode::item(Action::Fullscreen),
	}),
	kHelpMenu,
};

const MenuNode kViewerMenu[] = {
	kFileMenu,
	MenuNode::group(N_("_Go"), {
		MenuNode::item(Action::Back),
		MenuNode::item(Action::Forward),
		MenuNode::item(Action::Location),
		{},
		MenuNode::item(Action::Browse),
		MenuNode::item(Action::PrevFile),
		MenuNode::item(Action::NextFile),
	}),
	MenuNode::group(N_("_View"), {
		MenuNode::item(Action::Information),
		{},
		MenuNode::item(Action::ZoomIn),
		MenuNode::item(Action::ZoomOut),
		MenuNode::item(Action::Zoom1),
		MenuNode::item(Action::Fit),
		MenuNode::item(Action::FitWidth),
		MenuNode::item(Action::FitHeight),
		{},
		MenuNode::item(Action::Lock),
		MenuNode::item(Action::Fixate),
		{},
		MenuNode::item(Action::ColorManagement),
		MenuNode::item(Action::Smooth),
		MenuNode::item(Action::Checkerboard),
		MenuNode::item(Action::BlendLinearLight),
		{},
		MenuNode::item(Action::Hint),
		MenuNode::item(Action::DarkMode),
		MenuNode::item(Action::Fullscreen),
	}),
	MenuNode::group(N_("_Image"), {
		MenuNode::item(Action::RotateLeft),
		MenuNode::item(Action::Mirror),
		MenuNode::item(Action::RotateRight),
		{},
		MenuNode::item(Action::PageFirst),
		MenuNode::item(Action::PagePrevious),
		MenuNode::item(Action::PageNext),
		MenuNode::item(Action::PageLast),
		{},
		MenuNode::item(Action::FrameFirst),
		MenuNode::item(Action::FramePrevious),
		MenuNode::item(Action::PlayPause),
		MenuNode::item(Action::FrameNext),
	}),
	kHelpMenu,
};
// clang-format on

MenuNode
MenuNode::item(Action action)
{
	MenuNode node;
	node.action = action;
	return node;
}

MenuNode
MenuNode::group(const char *title, initializer_list<MenuNode> items)
{
	MenuNode node;
	node.title = title;
	node.items = items;
	return node;
}

const ActionDef &
action_def(Action action)
{
	const size_t i = size_t(action);
	if (i >= size(kDefs))
		return kDefs[0];
	return kDefs[i];
}

Action
match_key(span<const Action> scope, int key, unsigned mods)
{
	const uint32_t k = uint32_t(key);
	const uint32_t m = uint32_t(mods);
	if (Action a = match_exact(scope, k, m); a != Action::None)
		return a;
	if ((m & uint32_t(Qt::ShiftModifier)) && shift_is_incidental(k))
		return match_exact(scope, k, m & ~uint32_t(Qt::ShiftModifier));
	return Action::None;
}

QString
accel_key_label(const Accel &a)
{
	if (!a.key)
		return {};

	QString s = QKeySequence(int(a.mods) | int(a.key))
					.toString(QKeySequence::NativeText);
	s.replace(QLatin1Char('-'), QChar(0x2212));
	return s;
}

QString
accel_label(const ActionDef &def)
{
	if (def.accel) {
		QString s = QString::fromUtf8(def.accel);
		s.replace(QLatin1Char('-'), QChar(0x2212));
		return s;
	}
	return accel_key_label(def.keys[0]);
}

// Where source strings become what the user reads, and so where they get
// translated; call sites mark their literals with N_().  An empty msgid
// would only ever return the catalogue's own header.
QString
menu_label(const char *label, int *mnemonic_index)
{
	if (mnemonic_index)
		*mnemonic_index = -1;
	if (!label || !*label)
		return {};

	QString s = QString::fromUtf8(_(label));
	// On macOS, the Alt/Option key modifies characters, so mnemonics are
	// unusable for window navigation.  The remaining thing mnemonics could do
	// is act as unmodified accelerators in menus, though AppKit menu navigation
	// is instead done using type-ahead (which resets after about a second).
#ifndef Q_OS_MACOS
	for (int i = 0; i + 1 < s.size(); i++) {
		if (s[i] == QLatin1Char('_')) {
			if (mnemonic_index)
				*mnemonic_index = i;
			break;
		}
	}
#endif
	s.remove(QLatin1Char('_'));
	return s;
}

const char *
action_label(const ActionDef &def, bool checked)
{
	if (checked && (def.flags & ActionToggle) && def.label[1])
		return def.label[1];
	return def.label[0];
}

const char *
action_icon(const ActionDef &def, bool checked)
{
	if (checked && (def.flags & ActionToggle) && def.icon[1])
		return def.icon[1];
	return def.icon[0];
}

QString
action_tip(const ActionDef &def, bool checked)
{
	return menu_label(action_label(def, checked), nullptr);
}

QString
action_accel(const ActionDef &def)
{
	return accel_label(def);
}

static const MenuNode kCropJpegMenu[] = {kFileMenu, kHelpMenu};
static const MenuNode kCommanderMenu[] = {kFileMenu, kHelpMenu};

static constexpr ModeDef kModes[] = {
	// TRANSLATORS: The application's name, in window titles.  Transliterate
	// it if that is what your script does with foreign names; do not
	// translate the word.
	{"view", N_("Dawn"), kViewerMenu, kViewerKeys},
	{"browse", N_("Dawn"), kBrowserMenu, kBrowserKeys},
	{"cropjpeg", N_("Dawn JPEG Cropper"), kCropJpegMenu, {}},
	{"commander", N_("Dawn Commander"), kCommanderMenu, {}},
};
static_assert(size(kModes) == size_t(Mode::Count));

span<const ModeDef>
modes()
{
	return kModes;
}

const ModeDef &
mode_def(Mode mode)
{
	return kModes[size_t(mode)];
}

optional<Mode>
parse_mode(string_view name)
{
	for (size_t i = 0; i < size(kModes); i++)
		if (name == kModes[i].name)
			return Mode(i);
	return {};
}

span<const Action>
window_keys()
{
	return kWindowKeys;
}

void
set_file_mime_data(QMimeData *mime, span<const QUrl> urls, bool cut)
{
	if (!mime)
		return;

	mime->setUrls(QList<QUrl>(urls.begin(), urls.end()));
#ifdef Q_OS_WIN
	// A little-endian DWORD: DROPEFFECT_MOVE or DROPEFFECT_COPY.
	mime->setData(
		QStringLiteral(
			"application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
		cut ? QByteArrayLiteral("\x02\x00\x00\x00")
			: QByteArrayLiteral("\x01\x00\x00\x00"));
#endif
#if defined Q_OS_UNIX && !defined Q_OS_MACOS
	QByteArray gnome;
	gnome += cut ? "cut" : "copy";
	for (const QUrl &url : urls) {
		gnome += '\n';
		gnome += url.toEncoded();
	}
	mime->setData(QByteArrayLiteral("x-special/gnome-copied-files"), gnome);
	mime->setData(
		QByteArrayLiteral("application/x-kde-cutselection"), cut ? "1" : "0");
#endif
}

void
copy_files(span<const QUrl> urls, bool cut)
{
	auto *mime = new QMimeData;
	set_file_mime_data(mime, urls, cut);
	QGuiApplication::clipboard()->setMimeData(mime);
}

bool
move_to_trash(const QString &abs_path)
{
	if (abs_path.isEmpty() || !QFileInfo(abs_path).isFile())
		return false;

	QFile file(abs_path);
	if (file.moveToTrash())
		return true;

	qWarning(
		"%s: %s", qUtf8Printable(abs_path), qUtf8Printable(file.errorString()));
	return false;
}

}  // namespace dn
