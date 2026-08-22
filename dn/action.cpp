//
// action.cpp: shared action table (labels, keys, menus)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "action.hpp"

#include <QByteArray>
#include <QClipboard>
#include <QDataStream>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIODevice>
#include <QKeySequence>
#include <QList>
#include <QMimeData>
#include <QUrl>

#include <cstdio>

using namespace std;

namespace dn
{
namespace
{

constexpr uint8_t kMenu = ActionInMenu;
constexpr uint8_t kToggle = ActionInMenu | ActionToggle;
constexpr unsigned kCtrl = unsigned(Qt::ControlModifier);
constexpr unsigned kAlt = unsigned(Qt::AltModifier);
constexpr unsigned kShift = unsigned(Qt::ShiftModifier);

// clang-format off
constexpr ActionDef kDefs[] = {
	{},
	{kMenu, {"_New Window"}, {}, {{Qt::Key_N, kCtrl}}, {}},
	{kMenu, {"_Close Window"}, {}, {{Qt::Key_W, kCtrl}, {Qt::Key_Q}}, {}},
	{0, {"_Minimise"}, {}, {}, {}},
	{kToggle, {"_Maximise", "Res_tore"}, {}, {}, {}},
	{kMenu, {"_Quit"}, {}, {{Qt::Key_Q, kCtrl}}, {}},
	{kToggle, {"_Fullscreen", "E_xit Fullscreen"},
		{"view-fullscreen-symbolic", "view-restore-symbolic"},
		{{Qt::Key_F11}}, {}},
	{kToggle, {"_Dark Mode"}, {"dark-mode-symbolic"}, {{Qt::Key_D}}, {}},
	{kMenu, {"_Hint"}, {}, {{Qt::Key_F}}, {}},
	{kMenu, {"_Back in History"}, {"curved-arrow-left-symbolic"},
		{{Qt::Key_Left, kAlt}, {Qt::Key_Backspace}}, {}},
	{kMenu, {"_Forward in History"}, {"curved-arrow-right-symbolic"},
		{{Qt::Key_Right, kAlt}}, {}},
	{kMenu, {"_Contents"}, {}, {{Qt::Key_F1}}, {}},
	{kMenu, {"_About"}, {}, {}, {}},
	{kMenu, {"_Keyboard Shortcuts"}, {}, {{Qt::Key_Question, kCtrl}}, {}},
	{0, {"_Menu"}, {}, {{Qt::Key_F10}}, {}},
	// Mostly documentation only.
	{0, {"_Context Menu"}, {}, {{Qt::Key_Menu}, {Qt::Key_F10, kShift}}, {}},
	{0, {"_Cancel"}, {}, {{Qt::Key_Escape}}, {}},
	{0, {"_Next Pane"}, {}, {{Qt::Key_F6}}, {}},
	{0, {"_Previous Pane"}, {}, {{Qt::Key_F6, kShift}}, {}},
	{kToggle, {"Show _Sidebar"}, {"sidebar-left-symbolic"},
		{{Qt::Key_F9}}, {}},
	{kMenu, {"_Previous Directory in Tree"}, {"go-previous-symbolic"},
		{{Qt::Key_BracketLeft}}, {}},
	{kMenu, {"_Next Directory in Tree"}, {"go-next-symbolic"},
		{{Qt::Key_BracketRight}}, {}},
	{kMenu, {"Parent _Directory"}, {"go-up-symbolic"},
		{{Qt::Key_Up, kAlt}}, {}},
	{kMenu, {"_Home"}, {}, {{Qt::Key_Home, kAlt}}, {}},
	{kMenu, {"S_maller Thumbnails"}, {"minus-framed-symbolic"},
		{{Qt::Key_Minus}, {Qt::Key_Minus, kCtrl}}, {}},
	{kMenu, {"_Larger Thumbnails"}, {"plus-framed-symbolic"},
		{{Qt::Key_Plus}, {Qt::Key_Plus, kCtrl}}, {}},
	{kToggle, {"Tiled _View"}, {"blocks-symbolic"},
		{{Qt::Key_1}, {Qt::Key_1, kCtrl}}, {}},
	{kToggle, {"_Grid View"}, {"view-grid-symbolic"},
		{{Qt::Key_2}, {Qt::Key_2, kCtrl}}, {}},
	{kToggle, {"L_ist View"}, {"view-list-symbolic"},
		{{Qt::Key_3}, {Qt::Key_3, kCtrl}}, {}},
	{kToggle, {"Sho_w Filenames"}, {"font-symbolic"},
		{{Qt::Key_T}, {Qt::Key_T, kCtrl}}, {}},
	{kToggle, {"Hide _Unsupported Files"}, {"filter-symbolic"},
		{{Qt::Key_H}, {Qt::Key_H, kCtrl}}, {}},
	{kToggle, {"Sort Des_cending", "Sort As_cending"},
		{"view-sort-descending-symbolic", "view-sort-ascending-symbolic"},
		{{Qt::Key_C}}, {}},
	{kToggle, {"Sort by _Name"}, {}, {{Qt::Key_1, kCtrl | kAlt}}, {}},
	{kToggle, {"Sort by _Time"}, {}, {{Qt::Key_2, kCtrl | kAlt}}, {}},
	{0, {"_Open"}, {}, {{Qt::Key_Return}, {Qt::Key_Enter}}, {}},
	{kMenu, {"_Browse"}, {"blocks-symbolic"},
		{{Qt::Key_Return}, {Qt::Key_Enter}}, {}},
	{kMenu, {"_Previous File"}, {"go-previous-symbolic"},
		{{Qt::Key_Left}, {Qt::Key_Up}, {Qt::Key_PageUp}}, {}},
	{kMenu, {"_Next File"}, {"go-next-symbolic"},
		{{Qt::Key_Right}, {Qt::Key_Down}, {Qt::Key_PageDown}}, {}},
	{kMenu, {"Zoom _In"}, {"plus-framed-symbolic"}, {{Qt::Key_Plus}}, {}},
	{kMenu, {"Zoom _Out"}, {"minus-framed-symbolic"}, {{Qt::Key_Minus}}, {}},
	{kMenu, {"O_riginal Size"}, {"one-framed-symbolic"},
		{{Qt::Key_0, kCtrl}}, {}},
	{0, {"Zoom _Level"}, {}, {}, "1-9"},
	{kToggle, {"_Scale to Fit"}, {"zoom-fit-symbolic"}, {{Qt::Key_X}}, {}},
	{0, {"Fit _Width"}, {}, {{Qt::Key_W}}, {}},
	{0, {"Fit _Height"}, {}, {{Qt::Key_H}}, {}},
	{kToggle, {"_Lock View"},
		{"padlock-open-symbolic", "padlock-closed-symbolic"}, {}, {}},
	{kToggle, {"_Keep Zoom and Position"}, {"pin2-symbolic"},
		{{Qt::Key_K}}, {}},
	{kToggle, {"_Colour Management"}, {"color-symbolic"}, {{Qt::Key_C}}, {}},
	{kToggle, {"S_mooth Scaling"}, {"blend-tool-symbolic"}, {{Qt::Key_I}}, {}},
	{kToggle, {"Highlight _Transparency"},
		{"transparent-background-symbolic"}, {{Qt::Key_T}}, {}},
	{kMenu, {"Rotate _Left"}, {"rotate-acw-symbolic"}, {{Qt::Key_Less}}, {}},
	{kMenu, {"_Mirror"}, {"flip-h-symbolic"}, {{Qt::Key_Equal}}, {}},
	{kMenu, {"Rotate _Right"}, {"rotate-cw-symbolic"},
		{{Qt::Key_Greater}}, {}},
	{kToggle, {"Show I_nformation"}, {"info-outline-symbolic"},
		{{Qt::Key_Return, kAlt}, {Qt::Key_Enter, kAlt}}, {}},
	{kMenu, {"_First Page"}, {"go-top-symbolic"}, {}, {}},
	{kMenu, {"Pr_evious Page"}, {"go-up-symbolic"},
		{{Qt::Key_BracketLeft}}, {}},
	{kMenu, {"_Next Page"}, {"go-down-symbolic"},
		{{Qt::Key_BracketRight}}, {}},
	{kMenu, {"La_st Page"}, {"go-bottom-symbolic"}, {}, {}},
	{kMenu, {"Re_wind"}, {"media-skip-backward-symbolic"}, {}, {}},
	{kMenu, {"Pre_vious Frame"}, {"media-seek-backward-symbolic"},
		{{Qt::Key_BraceLeft}}, {}},
	{kToggle, {"_Play", "_Pause"},
		{"media-playback-start-symbolic", "media-playback-pause-symbolic"},
		{{Qt::Key_Space}}, {}},
	{kMenu, {"Ne_xt Frame"}, {"media-seek-forward-symbolic"},
		{{Qt::Key_BraceRight}}, {}},
	{0, {"_Copy"}, {}, {{Qt::Key_C, kCtrl}, {Qt::Key_Insert, kCtrl}}, {}},
	{0, {"Move to _Trash"}, {}, {{Qt::Key_Delete}}, {}},
	{kMenu, {"_Reload"}, {"arrows-circle-symbolic"},
		{{Qt::Key_F5}, {Qt::Key_R}, {Qt::Key_R, kCtrl}}, {}},
};
// clang-format on

static_assert(size(kDefs) == size_t(Action::Count));

// Shift is frequently just the means of typing a punctuation character:
// Ctrl+? arrives as Ctrl+Shift+? where the question mark sits above the
// slash.  Letters, digits and named keys stay strict.
constexpr bool
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

constexpr Action
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
	Action::Help,
	Action::About,
	Action::Shortcuts,
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
const MenuNode kFileMenu{"_File", {
	Action::NewWindow,
	Action::CloseWindow,
	{},
	Action::Reload,
	{},
	Action::Quit,
}};

const MenuNode kHelpMenu{"_Help", {
	Action::Help,
	Action::Shortcuts,
	Action::About,
}};

const MenuNode kBrowserMenu[] = {
	kFileMenu,
	{"_Go", {
		Action::Back,
		Action::Forward,
		{},
		Action::DirPrev,
		Action::DirNext,
		Action::DirParent,
		Action::DirHome,
	}},
	{"_View", {
		Action::Sidebar,
		{},
		Action::ThumbPlus,
		Action::ThumbMinus,
		{},
		Action::ViewTile,
		Action::ViewGrid,
		// TODO: Action::ViewList,
		{},
		Action::Filenames,
		Action::Filter,
		{},
		Action::SortDir,
		Action::SortName,
		Action::SortTime,
		{},
		Action::Hint,
		Action::DarkMode,
		Action::Fullscreen,
	}},
	kHelpMenu,
};

const MenuNode kViewerMenu[] = {
	kFileMenu,
	{"_Go", {
		Action::Back,
		Action::Forward,
		{},
		Action::Browse,
		Action::PrevFile,
		Action::NextFile,
	}},
	{"_View", {
		Action::Information,
		{},
		Action::ZoomIn,
		Action::ZoomOut,
		Action::Zoom1,
		Action::Fit,
		{},
		Action::Lock,
		Action::Fixate,
		{},
		Action::ColorManagement,
		Action::Smooth,
		Action::Checkerboard,
		{},
		Action::Hint,
		Action::DarkMode,
		Action::Fullscreen,
	}},
	{"_Image", {
		Action::RotateLeft,
		Action::Mirror,
		Action::RotateRight,
		{},
		Action::PageFirst,
		Action::PagePrevious,
		Action::PageNext,
		Action::PageLast,
		{},
		Action::FrameFirst,
		Action::FramePrevious,
		Action::PlayPause,
		Action::FrameNext,
	}},
	kHelpMenu,
};
// clang-format on

}  // namespace

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
accel_label(const Accel &a)
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
	return accel_label(def.keys[0]);
}

QString
menu_label(const char *label)
{
	if (!label)
		return {};
	QString s = QString::fromUtf8(label);
	s.remove(QLatin1Char('_'));
	return s;
}

int
mnemonic_index(const char *label)
{
	if (!label)
		return -1;
	const QString s = QString::fromUtf8(label);
	int i = 0;
	for (int p = 0; p < s.size(); ++p) {
		if (s[p] == QLatin1Char('_'))
			return p + 1 < s.size() ? i : -1;
		++i;
	}
	return -1;
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
	return menu_label(action_label(def, checked));
}

QString
action_accel(const ActionDef &def)
{
	return accel_label(def);
}

span<const MenuNode>
browser_menu()
{
	return kBrowserMenu;
}

span<const MenuNode>
viewer_menu()
{
	return kViewerMenu;
}

span<const Action>
window_keys()
{
	return kWindowKeys;
}

span<const Action>
browser_keys()
{
	return kBrowserKeys;
}

span<const Action>
viewer_keys()
{
	return kViewerKeys;
}

void
copy_files(QMimeData *mime, span<const QString> abs_paths, bool cut)
{
	if (!mime)
		return;
	QList<QUrl> urls;
	urls.reserve(int(abs_paths.size()));
	for (const QString &path : abs_paths)
		urls.append(QUrl::fromLocalFile(path));
	mime->setUrls(urls);
#ifdef Q_OS_WIN
	QByteArray effect;
	QDataStream ds(&effect, QIODevice::WriteOnly);
	ds.setByteOrder(QDataStream::LittleEndian);
	ds << quint32(cut ? 2u : 1u);
	mime->setData(
		QStringLiteral(
			"application/x-qt-windows-mime;value=\"Preferred DropEffect\""),
		effect);
#endif
#if defined(Q_OS_UNIX) && !defined(Q_OS_MACOS)
	QByteArray gnome;
	gnome += cut ? "cut" : "copy";
	for (const QUrl &url : urls) {
		gnome += '\n';
		gnome += url.toEncoded();
	}
	mime->setData(QByteArrayLiteral("x-special/gnome-copied-files"), gnome);
	mime->setData(QByteArrayLiteral("application/x-kde-cutselection"),
		cut ? QByteArrayLiteral("1") : QByteArrayLiteral("0"));
#endif
}

void
copy_files(span<const QString> abs_paths, bool cut)
{
	auto *mime = new QMimeData;
	copy_files(mime, abs_paths, cut);
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
	fprintf(stderr, "%s: %s\n", qUtf8Printable(abs_path),
		qUtf8Printable(file.errorString()));
	return false;
}

}  // namespace dn
