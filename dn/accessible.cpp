//
// accessible.cpp: platform accessibility over the Kit widget tree
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "accessible.hpp"

#if !DN_WITH_ACCESSIBILITY

// Qt was built without accessibility, or without its AT-SPI bridge (*nix).
// Nothing here can work, and nothing that calls it needs to know that.

namespace dn
{

void
accessible_init()
{
}

void
accessible_changed(Window *, Change, Widget *)
{
}

void
accessible_retire_page(Window *)
{
}

void
accessible_forget_window(Window *)
{
}

void
accessible_renamed(Window *)
{
}

void
accessible_activated(Window *)
{
}

}  // namespace dn

#else

#include "app.hpp"
#include "kit-browser.hpp"
#include "kit-chrome.hpp"
#include "kit-files.hpp"
#include "kit-viewer.hpp"
#include "url.hpp"
#include "window.hpp"

#include <QAccessible>
#include <QAccessibleInterface>
#include <QAccessibleObject>
#include <QByteArray>
#include <QGuiApplication>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <QVariant>
#include <QWindow>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{

// --- Semantic traversal ------------------------------------------------------

// What clipping everything here is measured against: the frame, not the whole
// surface.  Under client-side decorations a Dialog spans the surface, shadow
// and all, and reporting that as its extents would put its origin outside the
// window a client has just been told it is in.
static Rect
host_rect(const Kit &kit)
{
	return kit.frame();
}

// The hint overlay is a keyboard shortcut drawn over the page, not a control:
// every chip it puts up stands for something that is exposed already.
// The tooltip and the scrim are nobody's children, and need no such rule.
static bool
suppressed(const Widget *w)
{
	return dynamic_cast<const Hint *>(w);
}

// Packing is not semantics.  What survives is either a container a client
// navigates by--a toolbar, a menu, a dialog--or a control in its own
// right; the rows, columns and panels that merely hold them are not.
// Subclasses are tested before their bases, which is the whole reason this
// is one list rather than a virtual asked of every widget.
static bool
flattened(const Widget *w)
{
	if (dynamic_cast<const Popup *>(w) || dynamic_cast<const Toolbar *>(w) ||
		dynamic_cast<const Titlebar *>(w) || dynamic_cast<const Sidebar *>(w) ||
		dynamic_cast<const FileRows *>(w))
		return false;
	// The page is the client area itself, and has an interface of its own.
	return dynamic_cast<const Page *>(w) ||
		dynamic_cast<const Container *>(w) || dynamic_cast<const Panel *>(w) ||
		dynamic_cast<const Sep *>(w) || dynamic_cast<const Splitter *>(w);
}

// Layout membership, not ownership claims: an overflowed toolbar item lives
// in the popup's Flow while the popup is up, and in nobody's children while
// it is shut.  Walking child() follows the first, and shown() drops the
// second, so neither turns into a second copy of the same control.
static void
semantic_children(const Widget *w, vector<Widget *> &out)
{
	for (const auto &child : w->children()) {
		Widget *k = child.get();
		if (!k || !k->shown() || suppressed(k))
			continue;

		if (flattened(k))
			semantic_children(k, out);
		else
			out.push_back(k);
	}
}

// The inverse of the above, so that parent, child and indexOfChild cannot
// disagree.  Null means the page, or a popup: neither is a widget's parent.
static Widget *
semantic_parent(const Widget *w)
{
	for (Widget *p = w->parent_; p; p = p->parent_)
		if (!flattened(p))
			return p;
	return nullptr;
}

static void
open_popups(const Kit &kit, vector<Widget *> &out)
{
	for (Popup *p : kit.popups_)
		if (p && p->shown() && !suppressed(p))
			out.push_back(p);
}

// How far down the tree, counting whatever owns a widget rather than what is
// exposed: strictly greater for a child than for its parent, which is all a
// bottom-up teardown order needs of it.
static int
widget_depth(const Widget *w)
{
	int depth = 0;
	for (const Widget *p = w->parent_; p; p = p->parent_)
		depth++;
	return depth;
}

static bool
effectively_shown(const Widget *w)
{
	for (const Widget *p = w; p; p = p->parent_) {
		if (!p->shown())
			return false;
	}
	return true;
}

// Whether the widget is in the tree the window is showing right now.  Dawn
// keeps a page per mode alive across a switch to the browser or back, and
// the one that is not current is not hidden -- merely no longer the root of
// anything, which no flag on it says.  Popups root their own chains, so the
// stack is what answers for them.
static bool
in_exposed_tree(Window *window, const Widget *w)
{
	const Widget *root = w;
	while (root->parent_)
		root = root->parent_;
	if (root == window->active_page())
		return true;

	for (const Popup *p : window->kit().popups_) {
		if (p == root && p->shown() && !suppressed(p))
			return true;
	}
	return false;
}

// The same question Window::render() asks of the kit, but without waiting
// for a frame: under client-side decorations either surface may be holding
// the activation, and they are one window to everyone but Qt.
static bool
window_active(Window *window)
{
	const QWindow *shell = window->shell();
	return (shell && shell->isActive()) || window->isActive();
}

// Whether an accessible mutation on this widget may go through at all: it
// has to be really visible, in the tree this window is showing right now,
// and within whatever popup owns input.  Every action, every text edit and
// every selection asks this one question, and asks it again at the moment
// of the call rather than trusting what a client collected earlier.
static bool
operable(Window *window, const Widget *w)
{
	return window && w && effectively_shown(w) && in_exposed_tree(window, w) &&
		window->kit().in_input_scope(w);
}

// --- Semantic properties -----------------------------------------------------

static QAccessible::Role
role_of(const Widget *w)
{
	if (dynamic_cast<const MenuItem *>(w))
		return QAccessible::MenuItem;
	if (dynamic_cast<const ComboItem *>(w) || dynamic_cast<const FileRow *>(w))
		return QAccessible::ListItem;
	if (dynamic_cast<const Combo *>(w))
		return QAccessible::ComboBox;
	if (dynamic_cast<const Checkbox *>(w))
		return QAccessible::CheckBox;
	if (auto *button = dynamic_cast<const Button *>(w)) {
		// The app menu and the overflow "more" button unroll a list the
		// same way a combo does; a plain push-button does not.
		if (button->activate_on_press)
			return QAccessible::ButtonMenu;
		return QAccessible::Button;
	}
	if (dynamic_cast<const Entry *>(w))
		return QAccessible::EditableText;
	if (dynamic_cast<const Label *>(w))
		return QAccessible::StaticText;
	if (dynamic_cast<const Browser *>(w) || dynamic_cast<const FileRows *>(w))
		return QAccessible::List;
	if (auto *viewer = dynamic_cast<const Viewer *>(w)) {
		if (viewer->current_ &&
			(viewer->current_->page_next ||
				!viewer->current_->page_previous.expired()))
			return QAccessible::Document;
		return QAccessible::Graphic;
	}
	if (dynamic_cast<const Dialog *>(w))
		return QAccessible::Dialog;
	if (dynamic_cast<const MenuPopup *>(w))
		return QAccessible::PopupMenu;
	if (dynamic_cast<const Popup *>(w))
		return QAccessible::Pane;
	if (dynamic_cast<const Toolbar *>(w))
		return QAccessible::ToolBar;
	// Not TitleBar, which the bridge maps to ATSPI_ROLE_TEXT: this one is a
	// container of a label and three window buttons, and a client asked to
	// read a text node would find no text and walk into its children anyway.
	if (dynamic_cast<const Titlebar *>(w))
		return QAccessible::Grouping;

	// TODO: The cropper describes its own content.
	return QAccessible::Grouping;
}

// The Label that names w through Label::buddy, found on a shared ancestor
// rather than stored in reverse: settings rows and the location field put
// the two next to each other, and nothing else needs a second pointer.
static Label *
buddy_label(const Widget *w)
{
	for (Widget *p = w->parent_; p; p = p->parent_) {
		for (const auto &child : p->children()) {
			auto *label = dynamic_cast<Label *>(child.get());
			if (label && label->buddy == w)
				return label;
		}
	}
	return nullptr;
}

// The first label names the dialog: a heading, or the plain-text question.
static QString
heading_of(const Widget *w)
{
	vector<Widget *> kids;
	semantic_children(w, kids);
	for (Widget *k : kids) {
		if (auto *label = dynamic_cast<const Label *>(k))
			return label->text;

		const QString inner = heading_of(k);
		if (!inner.isEmpty())
			return inner;
	}
	return {};
}

// Display strings as they already are: menu_label() took the mnemonic
// markers out when these were built, and taking them out a second time would
// eat a literal underscore in a filename.
static QString
name_of(const Widget *w)
{
	if (auto *combo = dynamic_cast<const Combo *>(w))
		return combo->current_text();
	if (auto *button = dynamic_cast<const Button *>(w)) {
		// An icon-only control has no text of its own, and the action table
		// has already spelled out what it does.
		if (!button->text.isEmpty())
			return button->text;
		return button->tip_text;
	}
	if (auto *label = dynamic_cast<const Label *>(w))
		return label->text;
	if (auto *entry = dynamic_cast<const Entry *>(w)) {
		// A Label that names the field stays; the placeholder is only a
		// fallback, and must not vanish once typing begins.
		if (const Label *label = buddy_label(entry);
			label && !label->text.isEmpty())
			return label->text;
		return entry->placeholder;
	}
	if (dynamic_cast<const Dialog *>(w))
		return heading_of(w);
	// A popup is named after what opened it -- the list "More" unrolled is
	// called "More" -- which does mean two nodes share a name for as long
	// as it is up.  That is deliberate, and the pair is told apart by role,
	// never by name: a client looking a control up by name while a menu is
	// open finds the button and the menu, and both answers are right.
	if (auto *popup = dynamic_cast<const Popup *>(w); popup && popup->opener)
		return name_of(popup->opener);
	if (auto *browser = dynamic_cast<const Browser *>(w)) {
		// The directory, never the cursor: a list that renamed itself
		// with every selection would be a different object each time.
		const QString leaf = url_basename(browser->dir_url_);
		return leaf.isEmpty() ? url_parse_name(browser->dir_url_) : leaf;
	}
	if (auto *viewer = dynamic_cast<const Viewer *>(w))
		return QString::fromStdString(viewer->basename_);
	// The titlebar stays unnamed, even now that it is an ordinary group: the
	// window carries the title, and the Label inside the bar carries it
	// again, so a third copy on the container between them would have a
	// screen reader say it twice on the way in.
	return {};
}

static QString
description_of(const Widget *w)
{
	auto *viewer = dynamic_cast<const Viewer *>(w);
	if (!viewer || (viewer->image_width_ == 0 && viewer->image_height_ == 0))
		return {};

	QString text = QStringLiteral("%1 \u00d7 %2, %3%")
					   .arg(viewer->image_width_)
					   .arg(viewer->image_height_)
					   .arg(int(lround(double(viewer->scale_) * 100.0)));
	if (!viewer->current_ ||
		(!viewer->current_->page_next &&
			viewer->current_->page_previous.expired()))
		return text;

	int page = 1;
	for (dawn::ImagePtr prev = viewer->current_->page_previous.lock(); prev;
		prev = prev->page_previous.lock())
		page++;

	int pages = page;
	for (const dawn::Image *p = viewer->current_.get(); p->page_next;
		p = p->page_next.get())
		pages++;
	return QStringLiteral("Page %1 of %2, ").arg(page).arg(pages) + text;
}

static QString
value_of(const Widget *w)
{
	if (auto *combo = dynamic_cast<const Combo *>(w))
		return combo->current_text();
	if (auto *entry = dynamic_cast<const Entry *>(w))
		return entry->text;
	return {};
}

// The combo list a choice belongs to, and which choice it is.  The column is
// filled in Combo::items order and rebuilt on every open, so a row's position
// in it is its index -- there is no other handle on which choice this is.
static ComboPopup *
combo_list_of(const Widget *w, int *index)
{
	if (!dynamic_cast<const ComboItem *>(w) || !w->parent_)
		return nullptr;

	Widget *column = w->parent_;
	auto *list = dynamic_cast<ComboPopup *>(column->parent_);
	if (!list || list->col != column)
		return nullptr;

	const auto kids = column->children();
	for (size_t i = 0; i < kids.size(); i++) {
		if (kids[i].get() == w) {
			*index = int(i);
			return list;
		}
	}
	return nullptr;
}

static bool
opens_shown_popup(const Kit &kit, const Widget *w)
{
	for (const Popup *p : kit.popups_) {
		if (p && p->shown() && p->opener == w)
			return true;
	}
	return false;
}

static bool
has_popup(const Widget *w)
{
	if (dynamic_cast<const Combo *>(w))
		return true;
	if (auto *item = dynamic_cast<const MenuItem *>(w); item && item->sub)
		return true;
	if (auto *button = dynamic_cast<const Button *>(w))
		return button->activate_on_press;
	return false;
}

static QString
accelerator_of(const Widget *w)
{
	if (auto *item = dynamic_cast<const MenuItem *>(w);
		item && !item->accel.isEmpty())
		return item->accel;
	if (auto *button = dynamic_cast<const Button *>(w))
		return button->tip_accel;
	return {};
}

// The row a browser's cursor is on, or -1 for no row and for anything that
// is not a browser.  One fact answers three questions about a listing: what
// is selected, what its focused descendant is, and whether the list itself
// holds the focus or only passes it on.
static int
cursor_row(const Widget *w)
{
	auto *browser = dynamic_cast<const Browser *>(w);
	if (!browser || browser->cursor_ < 0 ||
		browser->cursor_ >= int(browser->files_.size()))
		return -1;
	return browser->cursor_;
}

static QAccessible::State
state_of(Window *window, const Widget *w)
{
	const Kit &kit = window->kit();
	QAccessible::State state;
	if (!effectively_shown(w) || !in_exposed_tree(window, w))
		state.invisible = 1;
	else if (visible_rect(w, host_rect(kit)).empty())
		state.offscreen = 1;

	if (w->focusable())
		state.focusable = 1;

	// Focus is only focus while the window has it.  A background window
	// keeps its own idea of where the keyboard would go, which is not the
	// same as the keyboard being there.
	// A list with the cursor on a row is focused through that row, which is
	// what focusChild() gives out; both saying so would leave two objects
	// claiming the keyboard at once.
	if (kit.focus_ == w && window_active(window) && cursor_row(w) < 0)
		state.focused = 1;

	// A menu traps focus as much as a dialog does, but only the one that
	// stays put until it is answered is modal in the sense a client means.
	if (auto *popup = dynamic_cast<const Popup *>(w);
		popup && !popup->transient())
		state.modal = 1;

	if (auto *button = dynamic_cast<const Button *>(w)) {
		if (!button->enabled_)
			state.disabled = 1;
		if (kit.pressed_ == w)
			state.pressed = 1;
		// A toolbar toggle is checkable; the "more" button, and any plain
		// button the kit happens to draw active, is not.
		if (button->action != Action::None &&
			(action_def(button->action).flags & ActionToggle)) {
			state.checkable = 1;
			if (button->actor && button->actor->checked)
				state.checked = button->actor->checked(button->action);
		}
	}

	if (auto *box = dynamic_cast<const Checkbox *>(w)) {
		state.checkable = 1;
		state.checked = box->checked;
	} else if (auto *item = dynamic_cast<const MenuItem *>(w);
		item && item->checkable) {
		state.checkable = 1;
		state.checked = item->checked;
	}

	// The list draws which choice is current by where it places itself, with
	// no check column to read it off: a client has to be told outright, and
	// the focus is not that -- it moves off the choice as soon as the arrow
	// keys walk past it.
	if (int choice = -1; const ComboPopup *list = combo_list_of(w, &choice)) {
		state.selectable = 1;
		if (list->combo && list->combo->current == choice)
			state.selected = 1;
	}

	if (auto *row = dynamic_cast<const FileRow *>(w)) {
		state.selectable = 1;
		state.selected = ((const FileRows *) row->parent_)->selected == row;
	}

	// has_popup() already answers for a combo, which is the only reason
	// there is no separate branch for one here.
	if (has_popup(w)) {
		state.hasPopup = 1;
		state.expandable = 1;
		if (opens_shown_popup(kit, w))
			state.expanded = 1;
		else
			state.collapsed = 1;
	}
	if (dynamic_cast<const Entry *>(w))
		state.editable = 1;
	return state;
}

// --- Geometry ----------------------------------------------------------------

static qreal
content_dpr(const QWindow &content)
{
	const qreal dpr = content.devicePixelRatio();
	return dpr > 0 ? dpr : qreal(1);
}

// Kit rectangles are device pixels within the content window; accessibility
// wants logical screen coordinates.  Divide once, keep the fractional edges
// until the final outward rounding, and let the window map itself: under
// client-side decorations the content hangs outside its shell by the glow,
// so the offset between the two is neither zero nor anything to assume.
static QRect
global_rect(const QWindow &content, Rect r)
{
	if (r.empty())
		return {};

	const qreal dpr = content_dpr(content);
	const int left = int(floor(qreal(r.x) / dpr));
	const int top = int(floor(qreal(r.y) / dpr));
	const int right = int(ceil(qreal(r.right()) / dpr));
	const int bottom = int(ceil(qreal(r.bottom()) / dpr));
	const QPoint at = content.mapToGlobal(QPoint(left, top));
	return {at.x(), at.y(), right - left, bottom - top};
}

// The way back, for hit testing.
static QPoint
kit_point(const QWindow &content, int x, int y)
{
	const qreal dpr = content_dpr(content);
	const QPoint local = content.mapFromGlobal(QPoint(x, y));
	return {int(qreal(local.x()) * dpr), int(qreal(local.y()) * dpr)};
}

// --- Registry ----------------------------------------------------------------

namespace
{
// What one Dawn window has registered with Qt.  Identity is the live widget
// pointer: every path that destroys a widget goes through Kit::forget_tree(),
// and a whole page through Window::drop_frames(), so nothing here outlives
// what it describes.  Ids are not promised across a window, or a launch.
struct Registry {
	// The client area, where the shell is the only QObject there is.
	QAccessible::Id client = 0;
	// Whoever we last told the platform has the focus, so that it can be
	// told that it no longer does.  An id rather than a pointer, because
	// retirement invalidates one for us.
	QAccessible::Id focus = 0;
	unordered_map<Widget *, QAccessible::Id> widgets;
	// File identity is (browser lifetime, full path), never a row index.
	// Wrappers are created on child(i), so this only holds what a client
	// has already asked for.
	unordered_map<Widget *, unordered_map<string, QAccessible::Id>> files;
	// Popups last announced as shown, as ids so a retired widget cannot
	// be looked up again and re-registered from a dangling pointer.
	vector<QAccessible::Id> popups;
	bool popups_known = false;
	// The client area's children, which are the active page's.  Kept here
	// rather than on the adapter, because on Wayland that one belongs to
	// Qt's object cache and comes and goes with the content window.
	vector<Widget *> client_children;
	bool client_known = false;
	// Whether the window was last announced as holding the keyboard.
	bool active = false;
	bool active_known = false;
};
}  // namespace

static unordered_map<Window *, Registry> g_registries;

static Registry *
find_registry(Window *window)
{
	auto it = g_registries.find(window);
	return it == g_registries.end() ? nullptr : &it->second;
}

static QAccessibleInterface *interface_for(Window *window, Widget *w);
static QAccessibleInterface *interface_for_file(
	Window *window, Browser *browser, const string &path);
static QAccessibleInterface *client_interface(Window *window);
static QAccessibleInterface *shell_interface(Window *window);

// Whatever a mutation here changed has to reach the screen the same way it
// would have after a keystroke; the frame settles layout, focus and the
// input method.  Every action and every edit ends with this, and with only
// this -- there is no second spelling of it in this file.
static void
schedule_render(Kit &kit)
{
	if (kit.request_render)
		kit.request_render();
}

static QAccessibleInterface *
announced_focus(Window *window, Widget *w)
{
	if (!w)
		return nullptr;

	QAccessibleInterface *iface = interface_for(window, w);
	if (iface) {
		if (QAccessibleInterface *inner = iface->focusChild())
			return inner;
	}
	return iface;
}

static void notify_focus(Window *window, Widget *w);

// Nothing is listening until a client shows up, and composing notifications
// for nobody would build the whole tree just to describe it.  Queries stay
// unconditional on purpose: a client that connects late has to see the tree
// as it is now, not as it was when the process started.
static void
notify(QAccessibleEvent *event)
{
	if (QAccessible::isActive())
		QAccessible::updateAccessibility(event);
}

// --- File adapter ------------------------------------------------------------

static Rect
file_visible_rect(
	Window *window, const Browser *browser, const Browser::File &file)
{
	if (!window || !browser || !in_exposed_tree(window, browser))
		return {};
	return file.tile.intersect(visible_rect(browser, host_rect(window->kit())));
}

static const Browser::File *
file_of(const Browser *browser, const string &path)
{
	if (!browser)
		return nullptr;

	const int i = browser->file_index(path);
	if (i < 0 || i >= int(browser->files_.size()))
		return nullptr;

	return &browser->files_[size_t(i)];
}

// The adapters themselves.  Qt owns every registered interface, and this
// file is the only place that names any of these types, which is what the
// unnamed namespace is here for; everything else in this file is static.
namespace
{

// One listing entry.  Objectless, and keyed by path so a rescan that keeps
// the file keeps the id; the row is resolved against files_ each query.
struct FileAdapter final : public QAccessibleInterface,
						   public QAccessibleActionInterface {
	Window *window_ = nullptr;
	Browser *browser_ = nullptr;
	string path_;

	FileAdapter(Window *window, Browser *browser, string path);
	~FileAdapter() override = default;

	void detach();
	[[nodiscard]] const Browser::File *resolve() const;

	// Valid until detach(), not until the path leaves files_: ObjectDestroyed
	// is emitted after scan_dir has already dropped the row, and Qt drops
	// that event if isValid() is already false.
	bool isValid() const override;
	QObject *object() const override { return nullptr; }
	QWindow *window() const override;

	QAccessibleInterface *parent() const override;
	QAccessibleInterface *child(int) const override { return nullptr; }
	QAccessibleInterface *childAt(int, int) const override { return nullptr; }
	QAccessibleInterface *focusChild() const override { return nullptr; }
	int childCount() const override { return 0; }
	int indexOfChild(const QAccessibleInterface *) const override { return -1; }

	QString text(QAccessible::Text t) const override;
	void setText(QAccessible::Text, const QString &) override {}
	QRect rect() const override;
	QAccessible::Role role() const override { return QAccessible::ListItem; }
	QAccessible::State state() const override;

	void *interface_cast(QAccessible::InterfaceType type) override;

	QStringList actionNames() const override;
	void doAction(const QString &name) override;
	QStringList keyBindingsForAction(const QString &) const override;
};

// --- Widget adapter ----------------------------------------------------------

// One exposed widget.  Objectless, because Dawn's widgets are not QObjects,
// and giving each one a shadow QObject would double the tree to say nothing.
//
// What every widget can do lives here.  An Entry's text and a Browser's
// listing are each one widget class's business, and are two subclasses
// below: on the one hand their optional interfaces then carry no methods
// that answer "not me" for everything else, and on the other the bookkeeping
// each needs is a field on the few adapters that have it rather than on all
// of them.  interface_for() picks the class; nothing else needs to know.
struct WidgetAdapter : public QAccessibleInterface,
					   public QAccessibleActionInterface {
	Window *window_ = nullptr;
	Widget *widget_ = nullptr;
	// Last committed name/value/state we told the platform about, so a
	// layout sweep can emit only what actually changed.
	QAccessible::State last_state_{};
	QString last_name_;
	QString last_value_;
	vector<Widget *> last_children_;
	// Qt's AT-SPI ObjectDestroyed asks parent() after overflow has already
	// reparented the widget.  Held only while that notification runs, so the
	// children-changed:remove names the container that lost it.
	//
	// This rests on notifyAboutDestruction() doing nothing with the node but
	// ask it for its parent and its path.  That function carries an upstream
	// FIXME about the child index it cannot work out, so if it ever learns
	// to, this hook is where overflow reparenting would quietly start
	// naming the wrong container.  The overflow case covers it.
	mutable QAccessibleInterface *forced_parent_ = nullptr;

	WidgetAdapter(Window *window, Widget *w);
	// Qt owns every registered interface, and is what deletes them.
	~WidgetAdapter() override = default;

	// Detached ahead of deletion, so that anything still holding this
	// answers "gone" rather than walking a freed widget.
	void detach() { this->widget_ = nullptr; }

	bool isValid() const override { return this->widget_ != nullptr; }
	QObject *object() const override { return nullptr; }
	QWindow *window() const override;

	QAccessibleInterface *parent() const override;
	QAccessibleInterface *child(int index) const override;
	QAccessibleInterface *childAt(int x, int y) const override;
	QAccessibleInterface *focusChild() const override;
	int childCount() const override;
	int indexOfChild(const QAccessibleInterface *other) const override;

	QString text(QAccessible::Text t) const override;
	void setText(QAccessible::Text t, const QString &value) override;
	QRect rect() const override;
	QAccessible::Role role() const override;
	QAccessible::State state() const override;
	QList<pair<QAccessibleInterface *, QAccessible::Relation>> relations(
		QAccessible::Relation match) const override;

	void *interface_cast(QAccessible::InterfaceType type) override;

	QStringList actionNames() const override;
	void doAction(const QString &name) override;
	QStringList keyBindingsForAction(const QString &name) const override;
};

// A text field.  Entry has no selection, so the Text interface reports none
// and the setters below stay honest about that; editing goes through the
// one Entry mutation helper, the same one a keystroke uses.
struct EntryAdapter final : public WidgetAdapter,
							public QAccessibleTextInterface,
							public QAccessibleEditableTextInterface {
	int last_caret_ = 0;

	EntryAdapter(Window *window, Entry *entry);
	~EntryAdapter() override = default;

	[[nodiscard]] Entry *entry() const;
	// Whether an edit may go through right now, which is operable() plus
	// the field still being there.
	[[nodiscard]] bool writable() const;

	void setText(QAccessible::Text t, const QString &value) override;
	void *interface_cast(QAccessible::InterfaceType type) override;

	void selection(
		int selectionIndex, int *startOffset, int *endOffset) const override;
	int selectionCount() const override;
	void addSelection(int startOffset, int endOffset) override;
	void removeSelection(int selectionIndex) override;
	void setSelection(
		int selectionIndex, int startOffset, int endOffset) override;
	int cursorPosition() const override;
	void setCursorPosition(int position) override;
	QString text(int startOffset, int endOffset) const override;
	QString textBeforeOffset(int offset,
		QAccessible::TextBoundaryType boundaryType, int *startOffset,
		int *endOffset) const override;
	QString textAfterOffset(int offset,
		QAccessible::TextBoundaryType boundaryType, int *startOffset,
		int *endOffset) const override;
	QString textAtOffset(int offset, QAccessible::TextBoundaryType boundaryType,
		int *startOffset, int *endOffset) const override;
	int characterCount() const override;
	QRect characterRect(int offset) const override;
	int offsetAtPoint(const QPoint &point) const override;
	void scrollToSubstring(int startIndex, int endIndex) override;
	QString attributes(
		int offset, int *startOffset, int *endOffset) const override;

	void deleteText(int startOffset, int endOffset) override;
	void insertText(int offset, const QString &text) override;
	void replaceText(
		int startOffset, int endOffset, const QString &text) override;

	// The base class resolves text() through QAccessible::Text; both names
	// are Qt's, and hiding one behind the other is not the intent.
	using WidgetAdapter::text;
};

// The file listing.  Children are the entries of files_ after filter and
// sort, made on child(i) and never all at once; the bookkeeping here is
// what a frame sweep diffs the listing and the cursor against.
struct BrowserAdapter final : public WidgetAdapter,
							  public QAccessibleSelectionInterface {
	vector<string> last_file_paths_;
	uint64_t last_file_rev_ = 0;
	string last_cursor_path_;

	BrowserAdapter(Window *window, Browser *browser);
	~BrowserAdapter() override = default;

	[[nodiscard]] Browser *browser() const;
	// Whether selection or activation may go through right now.
	[[nodiscard]] bool actionable() const;
	[[nodiscard]] int row_of(QAccessibleInterface *childItem) const;

	QAccessibleInterface *child(int index) const override;
	QAccessibleInterface *childAt(int x, int y) const override;
	QAccessibleInterface *focusChild() const override;
	int childCount() const override;
	int indexOfChild(const QAccessibleInterface *other) const override;

	void *interface_cast(QAccessible::InterfaceType type) override;

	int selectedItemCount() const override;
	QList<QAccessibleInterface *> selectedItems() const override;
	bool select(QAccessibleInterface *childItem) override;
	bool unselect(QAccessibleInterface *childItem) override;
	bool selectAll() override;
	bool clear() override;
};

// The list a combo drops.  Its children are the choices, and exactly one of
// them is current; selecting one sets the value without pressing it, which
// is what tells "look at this" apart from "take this".
struct ComboListAdapter final : public WidgetAdapter,
								public QAccessibleSelectionInterface {
	ComboListAdapter(Window *window, ComboPopup *list);
	~ComboListAdapter() override = default;

	[[nodiscard]] Combo *combo() const;
	// The choice this child stands for, or -1 for anything that is not a
	// choice of this very list.
	[[nodiscard]] int choice_of(QAccessibleInterface *childItem) const;

	void *interface_cast(QAccessible::InterfaceType type) override;

	int selectedItemCount() const override;
	QList<QAccessibleInterface *> selectedItems() const override;
	bool select(QAccessibleInterface *childItem) override;
	bool unselect(QAccessibleInterface *childItem) override;
	bool selectAll() override;
	bool clear() override;
};

struct FileRowsAdapter final : WidgetAdapter, QAccessibleSelectionInterface {
	QAccessible::Id last_selected = 0;

	FileRowsAdapter(Window *window, FileRows *rows);
	void *interface_cast(QAccessible::InterfaceType type) override;
	int selectedItemCount() const override;
	QList<QAccessibleInterface *> selectedItems() const override;
	bool select(QAccessibleInterface *item) override;
	bool unselect(QAccessibleInterface *item) override;
	bool selectAll() override { return false; }
	bool clear() override;
};

// The client area: everything the active page holds.  Object-backed where
// the content is a QWindow of its own -- which is what Qt hands back when it
// asks a Wayland shell's child who has the focus -- and objectless where the
// shell and the content are one QObject, so that either way there is one
// window, one client, and one route to the same controls.
struct ClientAdapter final : public QAccessibleInterface {
	Window *window_ = nullptr;

	explicit ClientAdapter(Window *window) : window_(window) {}
	~ClientAdapter() override = default;

	bool isValid() const override { return this->window_ != nullptr; }
	QObject *object() const override;
	QWindow *window() const override { return this->window_->shell(); }

	QAccessibleInterface *parent() const override;
	QAccessibleInterface *child(int index) const override;
	QAccessibleInterface *childAt(int x, int y) const override;
	QAccessibleInterface *focusChild() const override;
	int childCount() const override;
	int indexOfChild(const QAccessibleInterface *other) const override;

	QString text(QAccessible::Text) const override { return {}; }
	void setText(QAccessible::Text, const QString &) override {}
	QRect rect() const override;
	QAccessible::Role role() const override { return QAccessible::Client; }
	QAccessible::State state() const override;
};

// The semantic window: one per Dawn window, whatever the platform made of it.
struct ShellAdapter final : public QAccessibleObject {
	explicit ShellAdapter(QWindow *shell) : QAccessibleObject(shell) {}
	~ShellAdapter() override = default;

	[[nodiscard]] Window *content() const;

	QWindow *window() const override;

	QAccessibleInterface *parent() const override;
	QAccessibleInterface *child(int index) const override;
	QAccessibleInterface *childAt(int x, int y) const override;
	QAccessibleInterface *focusChild() const override;
	int childCount() const override;
	int indexOfChild(const QAccessibleInterface *other) const override;

	QString text(QAccessible::Text t) const override;
	QRect rect() const override;
	QAccessible::Role role() const override { return QAccessible::Window; }
	QAccessible::State state() const override;
};
}  // namespace

// The semantic child of scope under a screen point, or null.  Never scope
// itself: a client descends with this, and would not know when to stop.
static QAccessibleInterface *
child_at_point(Window *window, const Widget *scope, int x, int y)
{
	const Rect host = host_rect(window->kit());
	const QPoint at = kit_point(*window, x, y);
	vector<Widget *> kids;
	semantic_children(scope, kids);
	// Later children paint over earlier ones, so they are hit first.
	for (auto it = kids.rbegin(); it != kids.rend(); it++) {
		if (visible_rect(*it, host).contains(float(at.x()), float(at.y())))
			return interface_for(window, *it);
	}
	return nullptr;
}

FileAdapter::FileAdapter(Window *window, Browser *browser, string path)
	: window_(window), browser_(browser), path_(std::move(path))
{
}

void
FileAdapter::detach()
{
	this->browser_ = nullptr;
	this->path_.clear();
}

const Browser::File *
FileAdapter::resolve() const
{
	return file_of(this->browser_, this->path_);
}

bool
FileAdapter::isValid() const
{
	return this->browser_ != nullptr && !this->path_.empty();
}

QStringList
FileAdapter::keyBindingsForAction(const QString &) const
{
	return {};
}

QWindow *
FileAdapter::window() const
{
	return this->window_->shell();
}

QAccessibleInterface *
FileAdapter::parent() const
{
	return this->browser_ ? interface_for(this->window_, this->browser_)
						  : nullptr;
}

QString
FileAdapter::text(QAccessible::Text t) const
{
	const Browser::File *file = this->resolve();
	if (!file)
		return {};
	if (t == QAccessible::Name)
		return QString::fromStdString(file->name);
	if (t == QAccessible::Description)
		return QString::fromStdString(file->path);
	return {};
}

QRect
FileAdapter::rect() const
{
	const Browser::File *file = this->resolve();
	if (!file)
		return {};
	return global_rect(*this->window_,
		file_visible_rect(this->window_, this->browser_, *file));
}

QAccessible::State
FileAdapter::state() const
{
	QAccessible::State state;
	const Browser::File *file = this->resolve();
	if (!file) {
		state.invalid = 1;
		return state;
	}

	state.selectable = 1;
	state.focusable = 1;
	if (file_visible_rect(this->window_, this->browser_, *file).empty())
		state.offscreen = 1;
	if (!effectively_shown(this->browser_) ||
		!in_exposed_tree(this->window_, this->browser_))
		state.invisible = 1;

	const int row = this->browser_->file_index(this->path_);
	if (row >= 0 && row == this->browser_->cursor_) {
		state.selected = 1;
		if (this->window_->kit().focus_ == this->browser_ &&
			window_active(this->window_))
			state.focused = 1;
	}
	return state;
}

void *
FileAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::ActionInterface)
		return (QAccessibleActionInterface *) this;
	return nullptr;
}

// A row is operable when its listing is, and when it is still in it: what
// the listing can do, every row of it can do.
static bool
file_actionable(const FileAdapter *adapter)
{
	return adapter && adapter->resolve() &&
		operable(adapter->window_, adapter->browser_);
}

QStringList
FileAdapter::actionNames() const
{
	if (!file_actionable(this))
		return {};
	return {QAccessibleActionInterface::pressAction(),
		QAccessibleActionInterface::setFocusAction()};
}

void
FileAdapter::doAction(const QString &name)
{
	if (!this->actionNames().contains(name))
		return;

	const int row = this->browser_->file_index(this->path_);
	if (row < 0)
		return;

	Kit &kit = this->window_->kit();
	if (name == QAccessibleActionInterface::setFocusAction()) {
		this->browser_->select_index(row, true);
		kit.set_focus(this->browser_, true);
	} else {
		this->browser_->activate_file(this->browser_->file_url(row));
	}
	schedule_render(kit);
}

WidgetAdapter::WidgetAdapter(Window *window, Widget *w)
	: window_(window), widget_(w)
{
	this->last_state_ = state_of(window, w);
	this->last_name_ = name_of(w);
	this->last_value_ = value_of(w);
	semantic_children(w, this->last_children_);
}

EntryAdapter::EntryAdapter(Window *window, Entry *entry)
	: WidgetAdapter(window, entry), last_caret_(entry->caret)
{
}

Entry *
EntryAdapter::entry() const
{
	// Always an Entry while the widget is there: interface_for() chose this
	// class by asking, and a widget never changes type under its adapter.
	return (Entry *) this->widget_;
}

bool
EntryAdapter::writable() const
{
	return operable(this->window_, this->widget_);
}

BrowserAdapter::BrowserAdapter(Window *window, Browser *browser)
	: WidgetAdapter(window, browser), last_file_rev_(browser->file_rev_)
{
	this->last_file_paths_.reserve(browser->files_.size());
	for (const Browser::File &file : browser->files_)
		this->last_file_paths_.push_back(file.path);
	if (const int row = cursor_row(browser); row >= 0)
		this->last_cursor_path_ = browser->files_[size_t(row)].path;
}

Browser *
BrowserAdapter::browser() const
{
	return (Browser *) this->widget_;
}

bool
BrowserAdapter::actionable() const
{
	return operable(this->window_, this->widget_);
}

QWindow *
WidgetAdapter::window() const
{
	return this->window_->shell();
}

QAccessibleInterface *
WidgetAdapter::parent() const
{
	if (this->forced_parent_)
		return this->forced_parent_;
	if (!this->widget_)
		return nullptr;
	if (Widget *p = semantic_parent(this->widget_))
		return interface_for(this->window_, p);
	// A popup hangs off the kit's stack rather than off the page, and so is
	// a child of the window itself, beside the client area.
	if (dynamic_cast<const Popup *>(this->widget_))
		return shell_interface(this->window_);
	return client_interface(this->window_);
}

int
WidgetAdapter::childCount() const
{
	if (!this->widget_)
		return 0;

	vector<Widget *> kids;
	semantic_children(this->widget_, kids);
	return int(kids.size());
}

QAccessibleInterface *
WidgetAdapter::child(int index) const
{
	if (!this->widget_ || index < 0)
		return nullptr;

	vector<Widget *> kids;
	semantic_children(this->widget_, kids);
	if (size_t(index) >= kids.size())
		return nullptr;
	return interface_for(this->window_, kids[size_t(index)]);
}

int
WidgetAdapter::indexOfChild(const QAccessibleInterface *other) const
{
	auto *adapter = dynamic_cast<const WidgetAdapter *>(other);
	if (!this->widget_ || !adapter || !adapter->widget_)
		return -1;

	vector<Widget *> kids;
	semantic_children(this->widget_, kids);
	for (size_t i = 0; i < kids.size(); i++) {
		if (kids[i] == adapter->widget_)
			return int(i);
	}
	return -1;
}

QAccessibleInterface *
WidgetAdapter::childAt(int x, int y) const
{
	if (!this->widget_)
		return nullptr;
	return child_at_point(this->window_, this->widget_, x, y);
}

QAccessibleInterface *
WidgetAdapter::focusChild() const
{
	if (!this->widget_)
		return nullptr;

	Widget *focus = this->window_->kit().focus_;
	for (Widget *p = focus; p; p = p->parent_) {
		if (p == this->widget_)
			return interface_for(this->window_, focus);
	}
	return nullptr;
}

// The listing is the browser's children, and there is no Widget behind any
// of them: the rows are entries of files_, resolved by path.
int
BrowserAdapter::childCount() const
{
	return this->widget_ ? int(this->browser()->files_.size()) : 0;
}

QAccessibleInterface *
BrowserAdapter::child(int index) const
{
	if (!this->widget_ || index < 0)
		return nullptr;
	Browser *browser = this->browser();
	if (index >= int(browser->files_.size()))
		return nullptr;
	return interface_for_file(
		this->window_, browser, browser->files_[size_t(index)].path);
}

int
BrowserAdapter::indexOfChild(const QAccessibleInterface *other) const
{
	auto *file = dynamic_cast<const FileAdapter *>(other);
	if (!this->widget_ || !file || file->browser_ != this->browser())
		return -1;
	return this->browser()->file_index(file->path_);
}

QAccessibleInterface *
BrowserAdapter::childAt(int x, int y) const
{
	if (!this->widget_)
		return nullptr;

	Browser *browser = this->browser();
	const QPoint at = kit_point(*this->window_, x, y);
	for (const Browser::File &file : browser->files_) {
		// The clipped rectangle, which is the one rect() reports: a row
		// half scrolled out of the well must not answer for the half of
		// its tile that is not there, and one entirely out of it is no
		// clickable ghost at all.
		if (!file_visible_rect(this->window_, browser, file)
				.contains(float(at.x()), float(at.y())))
			continue;
		return interface_for_file(this->window_, browser, file.path);
	}
	return nullptr;
}

QAccessibleInterface *
BrowserAdapter::focusChild() const
{
	Browser *browser = this->widget_ ? this->browser() : nullptr;
	if (!browser || this->window_->kit().focus_ != browser)
		return nullptr;

	const int row = cursor_row(browser);
	if (row < 0)
		return nullptr;

	return interface_for_file(
		this->window_, browser, browser->files_[size_t(row)].path);
}

QRect
WidgetAdapter::rect() const
{
	// A page the window has switched away from keeps its last layout, and
	// would otherwise report rectangles nothing can be clicked in.
	if (!this->widget_ || !in_exposed_tree(this->window_, this->widget_))
		return {};
	return global_rect(*this->window_,
		visible_rect(this->widget_, host_rect(this->window_->kit())));
}

QAccessible::Role
WidgetAdapter::role() const
{
	return this->widget_ ? role_of(this->widget_) : QAccessible::NoRole;
}

QAccessible::State
WidgetAdapter::state() const
{
	QAccessible::State state;
	if (!this->widget_) {
		state.invalid = 1;
		return state;
	}
	return state_of(this->window_, this->widget_);
}

QString
WidgetAdapter::text(QAccessible::Text t) const
{
	if (!this->widget_)
		return {};

	switch (t) {
	case QAccessible::Name:
		return name_of(this->widget_);
	case QAccessible::Value:
		return value_of(this->widget_);
	case QAccessible::Accelerator:
		return accelerator_of(this->widget_);
	case QAccessible::Description: {
		if (const QString described = description_of(this->widget_);
			!described.isEmpty())
			return described;

		// The tooltip of a labelled control just repeats its label; only an
		// icon-only one has anything left to add, and that became its name.
		const QString tip = this->widget_->tip();
		return tip == name_of(this->widget_) ? QString() : tip;
	}
	default:
		return {};
	}
}

// Only a combo's value is settable here.  An Entry's goes through
// EntryAdapter, which is also where AT-SPI actually routes SetTextContents.
void
WidgetAdapter::setText(QAccessible::Text t, const QString &value)
{
	auto *combo = dynamic_cast<Combo *>(this->widget_);
	if (!combo || t != QAccessible::Value ||
		!operable(this->window_, this->widget_))
		return;

	Kit &kit = this->window_->kit();
	for (int i = 0; i < int(combo->items.size()); i++) {
		if (combo->items[size_t(i)] != value)
			continue;

		combo->select(kit, i);
		schedule_render(kit);
		return;
	}
}

QList<pair<QAccessibleInterface *, QAccessible::Relation>>
WidgetAdapter::relations(QAccessible::Relation match) const
{
	QList<pair<QAccessibleInterface *, QAccessible::Relation>> out;
	if (!this->widget_)
		return out;

	// Qt: Label means the returned object is the label for the origin;
	// Labelled means the returned object is labelled by the origin.
	// AT-SPI then inverts those two, so a control that reports Label
	// is what a client sees as LABELLED_BY.
	if (match & QAccessible::Label) {
		if (Label *label = buddy_label(this->widget_)) {
			if (QAccessibleInterface *iface =
					interface_for(this->window_, label))
				out.append({iface, QAccessible::Label});
		}
	}
	if (match & QAccessible::Labelled) {
		if (auto *label = dynamic_cast<Label *>(this->widget_);
			label && label->buddy) {
			if (QAccessibleInterface *iface =
					interface_for(this->window_, label->buddy))
				out.append({iface, QAccessible::Labelled});
		}
	}
	return out;
}

void *
WidgetAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::ActionInterface)
		return (QAccessibleActionInterface *) this;
	return nullptr;
}

void *
EntryAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::TextInterface)
		return (QAccessibleTextInterface *) this;
	if (type == QAccessible::EditableTextInterface)
		return (QAccessibleEditableTextInterface *) this;
	return WidgetAdapter::interface_cast(type);
}

void *
BrowserAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::SelectionInterface)
		return (QAccessibleSelectionInterface *) this;
	return WidgetAdapter::interface_cast(type);
}

// Single selection: the cursor row, or nothing.  Selecting must never open
// the file, which is what the separate press action is for.
int
BrowserAdapter::selectedItemCount() const
{
	return this->widget_ && cursor_row(this->browser()) >= 0 ? 1 : 0;
}

QList<QAccessibleInterface *>
BrowserAdapter::selectedItems() const
{
	QList<QAccessibleInterface *> items;
	Browser *browser = this->widget_ ? this->browser() : nullptr;
	const int row = browser ? cursor_row(browser) : -1;
	if (row < 0)
		return items;
	if (QAccessibleInterface *item = interface_for_file(
			this->window_, browser, browser->files_[size_t(row)].path))
		items.append(item);
	return items;
}

// The row this child stands for, or -1 for anything that is not a row of
// this very listing.
int
BrowserAdapter::row_of(QAccessibleInterface *childItem) const
{
	auto *file = dynamic_cast<FileAdapter *>(childItem);
	if (!this->actionable() || !file || file->browser_ != this->browser())
		return -1;
	return this->browser()->file_index(file->path_);
}

bool
BrowserAdapter::select(QAccessibleInterface *childItem)
{
	const int row = this->row_of(childItem);
	if (row < 0)
		return false;
	this->browser()->select_index(row, false);
	return true;
}

bool
BrowserAdapter::unselect(QAccessibleInterface *childItem)
{
	const int row = this->row_of(childItem);
	if (row < 0 || row != this->browser()->cursor_)
		return false;
	this->browser()->select_index(-1, false);
	return true;
}

// The model has one cursor, so there is nothing "all" could mean.
bool
BrowserAdapter::selectAll()
{
	return false;
}

bool
BrowserAdapter::clear()
{
	if (!this->actionable())
		return false;
	this->browser()->select_index(-1, false);
	return true;
}

ComboListAdapter::ComboListAdapter(Window *window, ComboPopup *list)
	: WidgetAdapter(window, list)
{
}

Combo *
ComboListAdapter::combo() const
{
	auto *list = (ComboPopup *) this->widget_;
	return list ? list->combo : nullptr;
}

void *
ComboListAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::SelectionInterface)
		return (QAccessibleSelectionInterface *) this;
	return WidgetAdapter::interface_cast(type);
}

int
ComboListAdapter::choice_of(QAccessibleInterface *childItem) const
{
	auto *adapter = dynamic_cast<WidgetAdapter *>(childItem);
	if (!adapter || !adapter->widget_)
		return -1;

	int choice = -1;
	const ComboPopup *list = combo_list_of(adapter->widget_, &choice);
	return list == this->widget_ ? choice : -1;
}

// A combo always stands on one of its choices: there is no empty state to
// report, and none to put it back into either.
int
ComboListAdapter::selectedItemCount() const
{
	Combo *combo = this->combo();
	return combo && combo->current >= 0 &&
			combo->current < int(combo->items.size())
		? 1
		: 0;
}

QList<QAccessibleInterface *>
ComboListAdapter::selectedItems() const
{
	QList<QAccessibleInterface *> items;
	if (!this->selectedItemCount())
		return items;

	const int n = this->childCount();
	for (int i = 0; i < n; i++) {
		QAccessibleInterface *item = this->child(i);
		if (this->choice_of(item) != this->combo()->current)
			continue;

		items.append(item);
		break;
	}
	return items;
}

bool
ComboListAdapter::select(QAccessibleInterface *childItem)
{
	const int choice = this->choice_of(childItem);
	if (choice < 0 || !operable(this->window_, this->widget_))
		return false;

	Kit &kit = this->window_->kit();
	// Not press(): that would take the choice and shut the list.  This is
	// the same value change, with the list left standing.
	this->combo()->select(kit, choice);
	schedule_render(kit);
	return true;
}

bool
ComboListAdapter::unselect(QAccessibleInterface *)
{
	return false;
}

bool
ComboListAdapter::selectAll()
{
	return false;
}

bool
ComboListAdapter::clear()
{
	return false;
}

FileRowsAdapter::FileRowsAdapter(Window *window, FileRows *rows)
	: WidgetAdapter(window, rows)
{
}

void *
FileRowsAdapter::interface_cast(QAccessible::InterfaceType type)
{
	if (type == QAccessible::SelectionInterface)
		return (QAccessibleSelectionInterface *) this;
	return WidgetAdapter::interface_cast(type);
}

int
FileRowsAdapter::selectedItemCount() const
{
	return this->widget_ && ((FileRows *) this->widget_)->selected ? 1 : 0;
}

QList<QAccessibleInterface *>
FileRowsAdapter::selectedItems() const
{
	if (!selectedItemCount())
		return {};
	return {
		interface_for(this->window_, ((FileRows *) this->widget_)->selected)};
}

bool
FileRowsAdapter::select(QAccessibleInterface *item)
{
	auto *adapter = dynamic_cast<WidgetAdapter *>(item);
	auto *row = adapter ? dynamic_cast<FileRow *>(adapter->widget_) : nullptr;
	if (!row || row->parent_ != this->widget_ ||
		!operable(this->window_, this->widget_))
		return false;

	Kit &kit = this->window_->kit();
	Kit::Input input(kit);
	((FileRows *) this->widget_)->select(kit, row);
	schedule_render(kit);
	return true;
}

bool
FileRowsAdapter::unselect(QAccessibleInterface *item)
{
	auto *adapter = dynamic_cast<WidgetAdapter *>(item);
	if (!this->widget_ || !adapter || !adapter->widget_ ||
		adapter->widget_ != ((FileRows *) this->widget_)->selected)
		return false;
	return clear();
}

bool
FileRowsAdapter::clear()
{
	if (!operable(this->window_, this->widget_))
		return false;
	Kit &kit = this->window_->kit();
	Kit::Input input(kit);
	((FileRows *) this->widget_)->select(kit, nullptr);
	schedule_render(kit);
	return true;
}

// What a widget's default action is called, or nothing for one that has
// none.  Advertising more than is there is worse than advertising less:
// focusing an entry is not pressing a button, and a label that merely names
// a control is not a second way to operate it.
static QString
default_action_of(const Widget *w)
{
	if (auto *item = dynamic_cast<const MenuItem *>(w))
		return item->sub ? QAccessibleActionInterface::showMenuAction()
						 : QAccessibleActionInterface::pressAction();
	if (dynamic_cast<const Combo *>(w))
		return QAccessibleActionInterface::showMenuAction();
	if (dynamic_cast<const Checkbox *>(w))
		return QAccessibleActionInterface::toggleAction();
	if (auto *button = dynamic_cast<const Button *>(w)) {
		if (button->activate_on_press)
			return QAccessibleActionInterface::showMenuAction();
		return QAccessibleActionInterface::pressAction();
	}
	return {};
}

QStringList
WidgetAdapter::actionNames() const
{
	QStringList names;
	if (!operable(this->window_, this->widget_))
		return names;

	auto *button = dynamic_cast<const Button *>(this->widget_);
	if (const QString action = default_action_of(this->widget_);
		!action.isEmpty() && (!button || button->enabled_))
		names += action;
	if (this->widget_->focusable())
		names += QAccessibleActionInterface::setFocusAction();
	return names;
}

void
WidgetAdapter::doAction(const QString &name)
{
	// Re-checked here rather than trusted: what a client collected a moment
	// ago may have been disabled, hidden, or covered by a dialog since.
	if (!actionNames().contains(name))
		return;

	Kit &kit = this->window_->kit();
	Kit::Input input(kit);
	if (name == QAccessibleActionInterface::setFocusAction())
		kit.set_focus(this->widget_, true);
	else
		kit.activate(this->widget_);
	// Whatever that changed has to reach the screen the same way it would
	// have after a keystroke; the frame settles focus and the input method.
	schedule_render(kit);
}

QStringList
WidgetAdapter::keyBindingsForAction(const QString &name) const
{
	QStringList keys;
	if (!this->widget_ || name == QAccessibleActionInterface::setFocusAction())
		return keys;

	if (const QString accel = accelerator_of(this->widget_); !accel.isEmpty())
		keys += accel;
	return keys;
}

// --- Entry text --------------------------------------------------------------

static void
clamp_text_range(int n, int *start, int *end)
{
	int s = *start;
	int e = *end;
	if (s < 0)
		s = 0;
	if (e < 0)
		e = n;
	if (s > n)
		s = n;
	if (e > n)
		e = n;
	if (e < s) {
		const int tmp = s;
		s = e;
		e = tmp;
	}
	*start = s;
	*end = e;
}

// Entry has no selection at all: the caret is the whole of its state.
// Reporting none, and refusing to pretend the setters work, is what keeps a
// client from believing it has selected something it has not.
void
EntryAdapter::selection(int, int *startOffset, int *endOffset) const
{
	if (startOffset)
		*startOffset = 0;
	if (endOffset)
		*endOffset = 0;
}

int
EntryAdapter::selectionCount() const
{
	return 0;
}

void
EntryAdapter::addSelection(int, int)
{
}

void
EntryAdapter::removeSelection(int)
{
}

void
EntryAdapter::setSelection(int, int, int)
{
}

int
EntryAdapter::cursorPosition() const
{
	return this->widget_ ? this->entry()->caret : 0;
}

void
EntryAdapter::setCursorPosition(int position)
{
	if (!this->writable())
		return;
	Kit &kit = this->window_->kit();
	this->entry()->move_caret(kit, position);
	schedule_render(kit);
}

QString
EntryAdapter::text(int startOffset, int endOffset) const
{
	if (!this->widget_)
		return {};
	const QString &text = this->entry()->text;
	clamp_text_range(int(text.size()), &startOffset, &endOffset);
	return text.mid(startOffset, endOffset - startOffset);
}

// Qt's boundary walkers take offsets, and -2 is AT-SPI's way of saying "at
// the caret"; the base class does the rest from text() and characterCount().
QString
EntryAdapter::textBeforeOffset(int offset,
	QAccessible::TextBoundaryType boundaryType, int *startOffset,
	int *endOffset) const
{
	if (offset == -2)
		offset = cursorPosition();
	return QAccessibleTextInterface::textBeforeOffset(
		offset, boundaryType, startOffset, endOffset);
}

QString
EntryAdapter::textAfterOffset(int offset,
	QAccessible::TextBoundaryType boundaryType, int *startOffset,
	int *endOffset) const
{
	if (offset == -2)
		offset = cursorPosition();
	return QAccessibleTextInterface::textAfterOffset(
		offset, boundaryType, startOffset, endOffset);
}

QString
EntryAdapter::textAtOffset(int offset,
	QAccessible::TextBoundaryType boundaryType, int *startOffset,
	int *endOffset) const
{
	if (offset == -2)
		offset = cursorPosition();
	return QAccessibleTextInterface::textAtOffset(
		offset, boundaryType, startOffset, endOffset);
}

int
EntryAdapter::characterCount() const
{
	return this->widget_ ? int(this->entry()->text.size()) : 0;
}

QRect
EntryAdapter::characterRect(int offset) const
{
	if (!this->widget_)
		return {};
	Entry *entry = this->entry();
	if (entry->r.empty())
		return {};

	Kit &kit = this->window_->kit();
	const int n = int(entry->text.size());
	offset = clamp(offset, 0, n);
	const int from =
		entry->text_cache_.caret_x(kit, entry->text, offset, false);
	const int to = offset < n
		? entry->text_cache_.caret_x(kit, entry->text, offset + 1, false)
		: from + kit.hairline();
	const int x = entry->r.x + kit.px(entry->pad_x) +
		int(lround(-entry->scroll_ + float(min(from, to))));
	const int w = max(1, to > from ? to - from : from - to);
	return global_rect(*this->window_, {x, entry->r.y, w, entry->r.h});
}

int
EntryAdapter::offsetAtPoint(const QPoint &point) const
{
	if (!this->widget_)
		return 0;

	Entry *entry = this->entry();
	Kit &kit = this->window_->kit();
	const QPoint at = kit_point(*this->window_, point.x(), point.y());
	const float x = float(at.x()) - float(entry->r.x + kit.px(entry->pad_x)) +
		entry->scroll_;
	return entry->text_cache_.index_at(kit, entry->text, x, false);
}

void
EntryAdapter::scrollToSubstring(int startIndex, int endIndex)
{
	// Revealing is not editing: a field under a dialog may still be asked
	// to show a range, as long as it is really on screen.
	if (!this->widget_ || !effectively_shown(this->widget_) ||
		!in_exposed_tree(this->window_, this->widget_))
		return;

	Kit &kit = this->window_->kit();
	this->entry()->reveal(kit, startIndex, endIndex);
	schedule_render(kit);
}

// No run of the text is special: one attribute run spans the whole field.
QString
EntryAdapter::attributes(int offset, int *startOffset, int *endOffset) const
{
	const int n = characterCount();
	if (offset < 0 || offset > n) {
		if (startOffset)
			*startOffset = -1;
		if (endOffset)
			*endOffset = -1;
		return {};
	}
	if (startOffset)
		*startOffset = 0;
	if (endOffset)
		*endOffset = n;
	return {};
}

void
EntryAdapter::deleteText(int startOffset, int endOffset)
{
	replaceText(startOffset, endOffset, {});
}

void
EntryAdapter::insertText(int offset, const QString &value)
{
	replaceText(offset, offset, value);
}

// Everything that edits ends up here, and here ends up in Entry::replace(),
// which is what a keystroke goes through as well: clamping, cluster
// boundaries, the caret, and telling the host are its business, not ours.
void
EntryAdapter::replaceText(int startOffset, int endOffset, const QString &value)
{
	if (!this->writable())
		return;
	Kit &kit = this->window_->kit();
	this->entry()->replace(kit, startOffset, endOffset, value);
	schedule_render(kit);
}

// Whole-value writes: AT-SPI routes SetTextContents to replaceText, so this
// is the road less travelled, and takes the same one anyway.
void
EntryAdapter::setText(QAccessible::Text t, const QString &value)
{
	if (t != QAccessible::Value)
		return;
	replaceText(0, characterCount(), value);
}

static void
notify_entry_text(EntryAdapter *adapter)
{
	if (!adapter->isValid())
		return;

	// Wow.
	const Entry *entry = adapter->entry();
	const QString now = entry->text;
	const int caret = entry->caret;
	const QString was = adapter->last_value_;
	const int was_caret = adapter->last_caret_;
	if (now != was) {
		const int n_was = int(was.size());
		const int n_now = int(now.size());
		int prefix = 0;
		while (prefix < n_was && prefix < n_now && was[prefix] == now[prefix])
			prefix++;
		while (prefix > 0 &&
			((prefix < n_was && was[prefix].isLowSurrogate()) ||
				(prefix < n_now && now[prefix].isLowSurrogate())))
			prefix--;

		int suffix = 0;
		while (prefix + suffix < n_was && prefix + suffix < n_now &&
			was[n_was - 1 - suffix] == now[n_now - 1 - suffix])
			suffix++;
		while (suffix > 0 &&
			((n_was - suffix > 0 && was[n_was - suffix].isLowSurrogate()) ||
				(n_now - suffix > 0 && now[n_now - suffix].isLowSurrogate())))
			suffix--;

		const int rem = n_was - prefix - suffix;
		const int ins = n_now - prefix - suffix;
		if (rem > 0 && ins == 0) {
			QAccessibleTextRemoveEvent event(
				adapter, prefix, was.mid(prefix, rem));
			notify(&event);
		} else if (rem == 0 && ins > 0) {
			QAccessibleTextInsertEvent event(
				adapter, prefix, now.mid(prefix, ins));
			notify(&event);
		} else if (rem > 0 && ins > 0) {
			QAccessibleTextUpdateEvent event(
				adapter, prefix, was.mid(prefix, rem), now.mid(prefix, ins));
			notify(&event);
		}
	}
	if (caret != was_caret || now != was) {
		QAccessibleTextCursorEvent event(adapter, caret);
		notify(&event);
	}
	adapter->last_value_ = now;
	adapter->last_caret_ = caret;
}

// --- Client and shell --------------------------------------------------------

QObject *
ClientAdapter::object() const
{
	// Never the shell's own QObject: Qt caches one interface per object, and
	// two of them claiming the same one would make the window its own child.
	QWindow *shell = this->window_->shell();
	return shell == this->window_ ? nullptr : this->window_;
}

QAccessibleInterface *
ClientAdapter::parent() const
{
	return shell_interface(this->window_);
}

int
ClientAdapter::childCount() const
{
	Page *page = this->window_->active_page();
	if (!page)
		return 0;

	vector<Widget *> kids;
	semantic_children(page, kids);
	return int(kids.size());
}

QAccessibleInterface *
ClientAdapter::child(int index) const
{
	Page *page = this->window_->active_page();
	if (!page || index < 0)
		return nullptr;

	vector<Widget *> kids;
	semantic_children(page, kids);
	if (size_t(index) >= kids.size())
		return nullptr;
	return interface_for(this->window_, kids[size_t(index)]);
}

int
ClientAdapter::indexOfChild(const QAccessibleInterface *other) const
{
	Page *page = this->window_->active_page();
	auto *adapter = dynamic_cast<const WidgetAdapter *>(other);
	if (!page || !adapter || !adapter->widget_)
		return -1;

	vector<Widget *> kids;
	semantic_children(page, kids);
	for (size_t i = 0; i < kids.size(); i++) {
		if (kids[i] == adapter->widget_)
			return int(i);
	}
	return -1;
}

QAccessibleInterface *
ClientAdapter::childAt(int x, int y) const
{
	Page *page = this->window_->active_page();
	if (!page)
		return nullptr;
	return child_at_point(this->window_, page, x, y);
}

QAccessibleInterface *
ClientAdapter::focusChild() const
{
	return announced_focus(this->window_, this->window_->kit().focus_);
}

QRect
ClientAdapter::rect() const
{
	return global_rect(*this->window_, host_rect(this->window_->kit()));
}

QAccessible::State
ClientAdapter::state() const
{
	QAccessible::State state;
	if (!this->window_->active_page())
		state.invisible = 1;
	return state;
}

Window *
ShellAdapter::content() const
{
	// Null once the content window has gone but its shell has not; every
	// caller here has to survive that, briefly, during teardown.
	return content_window(QAccessibleObject::object());
}

QWindow *
ShellAdapter::window() const
{
	return qobject_cast<QWindow *>(QAccessibleObject::object());
}

QAccessibleInterface *
ShellAdapter::parent() const
{
	return QAccessible::queryAccessibleInterface(qApp);
}

int
ShellAdapter::childCount() const
{
	Window *content = this->content();
	if (!content)
		return 0;

	vector<Widget *> popups;
	open_popups(content->kit(), popups);
	return int(popups.size()) + 1;
}

QAccessibleInterface *
ShellAdapter::child(int index) const
{
	Window *content = this->content();
	if (!content || index < 0)
		return nullptr;
	if (index == 0)
		return client_interface(content);

	vector<Widget *> popups;
	open_popups(content->kit(), popups);
	if (size_t(index - 1) >= popups.size())
		return nullptr;
	return interface_for(content, popups[size_t(index - 1)]);
}

int
ShellAdapter::indexOfChild(const QAccessibleInterface *other) const
{
	Window *content = this->content();
	if (!content || !other)
		return -1;
	if (dynamic_cast<const ClientAdapter *>(other))
		return 0;

	auto *adapter = dynamic_cast<const WidgetAdapter *>(other);
	if (!adapter || !adapter->widget_)
		return -1;

	vector<Widget *> popups;
	open_popups(content->kit(), popups);
	for (size_t i = 0; i < popups.size(); i++) {
		if (popups[i] == adapter->widget_)
			return int(i) + 1;
	}
	return -1;
}

QAccessibleInterface *
ShellAdapter::childAt(int x, int y) const
{
	Window *content = this->content();
	if (!content)
		return nullptr;

	// Popup stacking, topmost first, and only then what the page has there.
	vector<Widget *> popups;
	open_popups(content->kit(), popups);
	const Rect host = host_rect(content->kit());
	const QPoint at = kit_point(*content, x, y);
	for (auto it = popups.rbegin(); it != popups.rend(); it++) {
		if (visible_rect(*it, host).contains(float(at.x()), float(at.y())))
			return interface_for(content, *it);
	}

	QAccessibleInterface *client = client_interface(content);
	if (client && client->rect().contains(QPoint(x, y)))
		return client;
	return nullptr;
}

QAccessibleInterface *
ShellAdapter::focusChild() const
{
	Window *content = this->content();
	if (!content)
		return nullptr;
	if (Widget *focus = content->kit().focus_) {
		if (QAccessibleInterface *iface = announced_focus(content, focus))
			return iface;
	}
	return client_interface(content);
}

QRect
ShellAdapter::rect() const
{
	QWindow *shell = this->window();
	if (!shell || !shell->isVisible())
		return {};
	return {shell->mapToGlobal(QPoint()), shell->size()};
}

QString
ShellAdapter::text(QAccessible::Text t) const
{
	QWindow *shell = this->window();
	if (!shell || t != QAccessible::Name)
		return {};
	return shell->title();
}

QAccessible::State
ShellAdapter::state() const
{
	QAccessible::State state;
	QWindow *shell = this->window();
	Window *content = this->content();
	if (!shell || !content) {
		state.invalid = 1;
		return state;
	}
	if (!shell->isVisible())
		state.invisible = 1;
	// The same question notify_active() answers, and for the same reason:
	// under client-side decorations either surface may be holding the
	// activation, and the event and the state must not contradict.
	if (window_active(content))
		state.active = 1;
	state.sizeable = 1;
	state.movable = 1;
	return state;
}

// --- Registration ------------------------------------------------------------

// The one place that decides which adapter a widget gets.  A widget never
// changes class under it, so every later cast back is a static one.
static WidgetAdapter *
new_adapter(Window *window, Widget *w)
{
	if (auto *entry = dynamic_cast<Entry *>(w))
		return new EntryAdapter(window, entry);
	if (auto *browser = dynamic_cast<Browser *>(w))
		return new BrowserAdapter(window, browser);
	if (auto *list = dynamic_cast<ComboPopup *>(w))
		return new ComboListAdapter(window, list);
	if (auto *rows = dynamic_cast<FileRows *>(w))
		return new FileRowsAdapter(window, rows);
	return new WidgetAdapter(window, w);
}

static QAccessibleInterface *
interface_for(Window *window, Widget *w)
{
	if (!window || !w)
		return nullptr;

	Registry &registry = g_registries[window];
	auto it = registry.widgets.find(w);
	if (it != registry.widgets.end())
		return QAccessible::accessibleInterface(it->second);

	WidgetAdapter *adapter = new_adapter(window, w);
	registry.widgets[w] = QAccessible::registerAccessibleInterface(adapter);
	return adapter;
}

static QAccessibleInterface *
interface_for_file(Window *window, Browser *browser, const string &path)
{
	if (!window || !browser || path.empty() || browser->file_index(path) < 0)
		return nullptr;

	Registry &registry = g_registries[window];
	auto &ids = registry.files[browser];
	auto it = ids.find(path);
	if (it != ids.end())
		return QAccessible::accessibleInterface(it->second);

	auto *adapter = new FileAdapter(window, browser, path);
	ids[path] = QAccessible::registerAccessibleInterface(adapter);
	return adapter;
}

static QAccessibleInterface *
existing_file(Window *window, Browser *browser, const string &path)
{
	Registry *registry = find_registry(window);
	if (!registry || !browser || path.empty())
		return nullptr;
	auto bit = registry->files.find(browser);
	if (bit == registry->files.end())
		return nullptr;
	auto it = bit->second.find(path);
	if (it == bit->second.end())
		return nullptr;
	return QAccessible::accessibleInterface(it->second);
}

static QAccessibleInterface *
client_interface(Window *window)
{
	if (!window)
		return nullptr;

	// Where the content is a QWindow of its own, it is the client, and Qt
	// owns that interface through the factory like any other object's.
	if (window->shell() != window)
		return QAccessible::queryAccessibleInterface(window);

	Registry &registry = g_registries[window];
	if (!registry.client) {
		auto *adapter = new ClientAdapter(window);
		registry.client = QAccessible::registerAccessibleInterface(adapter);
	}
	return QAccessible::accessibleInterface(registry.client);
}

static QAccessibleInterface *
shell_interface(Window *window)
{
	return window ? QAccessible::queryAccessibleInterface(window->shell())
				  : nullptr;
}

// Dawn's classes have no Q_OBJECT, so each of them answers to QWindow and
// the class-name key says nothing useful; match the object itself instead.
// Returning null declines, and leaves other factories free to answer.
static QAccessibleInterface *
accessible_factory(const QString &, QObject *object)
{
	auto *window = qobject_cast<QWindow *>(object);
	if (!window)
		return nullptr;

	Window *content = content_window(window);
	if (!content)
		return nullptr;
	if (content->shell() == window)
		return new ShellAdapter(window);
	return new ClientAdapter(content);
}

// --- Notifications -----------------------------------------------------------

static void
retire_file(Registry &registry, Widget *browser, const string &path)
{
	auto bit = registry.files.find(browser);
	if (bit == registry.files.end())
		return;
	auto it = bit->second.find(path);
	if (it == bit->second.end())
		return;

	const QAccessible::Id id = it->second;
	bit->second.erase(it);
	if (bit->second.empty())
		registry.files.erase(bit);
	if (registry.focus == id)
		registry.focus = 0;

	auto *adapter =
		dynamic_cast<FileAdapter *>(QAccessible::accessibleInterface(id));
	if (!adapter)
		return;

	QAccessibleEvent event(adapter, QAccessible::ObjectDestroyed);
	notify(&event);
	adapter->detach();
	QAccessible::deleteAccessibleInterface(id);
}

static void
retire_browser_files(Registry &registry, Widget *browser)
{
	auto it = registry.files.find(browser);
	if (it == registry.files.end())
		return;

	vector<string> paths;
	paths.reserve(it->second.size());
	for (const auto &entry : it->second)
		paths.push_back(entry.first);
	for (const string &path : paths)
		retire_file(registry, browser, path);
}

static void
retire_widget(Registry &registry, Widget *w)
{
	if (dynamic_cast<Browser *>(w))
		retire_browser_files(registry, w);

	auto it = registry.widgets.find(w);
	if (it == registry.widgets.end())
		return;

	const QAccessible::Id id = it->second;
	registry.widgets.erase(it);
	if (registry.focus == id)
		registry.focus = 0;
	registry.popups.erase(
		remove(registry.popups.begin(), registry.popups.end(), id),
		registry.popups.end());

	auto *adapter =
		dynamic_cast<WidgetAdapter *>(QAccessible::accessibleInterface(id));
	if (!adapter)
		return;

	// While the node and its parent are both still whole: the bridge reports
	// the removal on the parent, and has nowhere to report it otherwise.
	QAccessibleEvent event(adapter, QAccessible::ObjectDestroyed);
	notify(&event);
	adapter->detach();
	QAccessible::deleteAccessibleInterface(id);
}

// A popup is never anybody's child -- it floats on the kit's stack, and its
// owner holds it in a field of its own.  Following child() alone therefore
// walks straight past a combo's list, a toolbar's overflow and a menu's
// submenus, and would leave their nodes registered over freed memory once
// the owner goes.
static void
owned_popups(const Widget *w, vector<Widget *> &out)
{
	if (auto *combo = dynamic_cast<const Combo *>(w); combo && combo->popup_)
		out.push_back(combo->popup_.get());
	if (auto *toolbar = dynamic_cast<const Toolbar *>(w);
		toolbar && toolbar->overflow)
		out.push_back(toolbar->overflow);
	if (auto *menu = dynamic_cast<const Menu *>(w)) {
		for (const auto &sub : menu->subs_) {
			if (sub)
				out.push_back(sub.get());
		}
	}
}

static void
retire_subtree(Registry &registry, Widget *w)
{
	for (const auto &k : w->children())
		retire_subtree(registry, k.get());

	vector<Widget *> owned;
	owned_popups(w, owned);
	for (Widget *p : owned)
		retire_subtree(registry, p);

	retire_widget(registry, w);
}

static void
reconcile_widget(WidgetAdapter *adapter)
{
	if (!adapter->widget_)
		return;

	const QAccessible::State now = state_of(adapter->window_, adapter->widget_);
	const QString name = name_of(adapter->widget_);
	const QString value = value_of(adapter->widget_);

	// Qt's AT-SPI bridge handles one StateChanged bit per event, so checked
	// and disabled each get a message of their own.  Expanded is not one of
	// the bits it translates; ObjectShow/Hide on the popup covers that.
	if (now.checked != adapter->last_state_.checked) {
		QAccessible::State bit;
		bit.checked = 1;
		QAccessibleStateChangeEvent event(adapter, bit);
		notify(&event);
	}
	if (now.disabled != adapter->last_state_.disabled) {
		QAccessible::State bit;
		bit.disabled = 1;
		QAccessibleStateChangeEvent event(adapter, bit);
		notify(&event);
	}
	// A combo is named by its current choice, and the bridge special-cases
	// ValueChanged on one into exactly the accessible-name change this
	// would send -- plus the selection-changed that goes with it.  Saying
	// it here as well is the same news twice.
	const bool combo = dynamic_cast<const Combo *>(adapter->widget_);
	if (name != adapter->last_name_ && !combo) {
		QAccessibleEvent event(adapter, QAccessible::NameChanged);
		notify(&event);
	}
	// Entry value is the committed string, announced as text/caret events
	// rather than AT-SPI ValueChanged: that signal wants a ValueInterface.
	if (value != adapter->last_value_ &&
		!dynamic_cast<const Entry *>(adapter->widget_)) {
		QAccessibleValueChangeEvent event(adapter, QVariant(value));
		notify(&event);
	}
	adapter->last_state_ = now;
	adapter->last_name_ = name;
	adapter->last_value_ = value;
}

static QAccessibleInterface *
existing_interface(Window *window, Widget *w)
{
	Registry *registry = find_registry(window);
	if (!registry)
		return nullptr;

	auto it = registry->widgets.find(w);
	if (it == registry->widgets.end())
		return nullptr;

	return QAccessible::accessibleInterface(it->second);
}

static bool
listed(const vector<Widget *> &list, Widget *w)
{
	return find(list.begin(), list.end(), w) != list.end();
}

bool
listed_id(const vector<QAccessible::Id> &list, QAccessible::Id id)
{
	return find(list.begin(), list.end(), id) != list.end();
}

// A listing is replaced wholesale, and the bridge has no way to say that:
// its ObjectReorder packs the child path as a plain string, which libatspi
// throws out, and everything else it translates is per-child--twenty
// thousand signals for a directory no client asked to see the whole of, and
// an adapter allocated for every row to compose them.
//
// Destroy and create the list itself instead.  Both of those the bridge does
// emit correctly, the id stays what it was, so nothing anybody holds goes
// stale, and a client drops the children it had cached and asks again.
static void
notify_listing_replaced(QAccessibleInterface *list)
{
	if (!list)
		return;

	QAccessibleEvent gone(list, QAccessible::ObjectDestroyed);
	notify(&gone);
	QAccessibleEvent created(list, QAccessible::ObjectCreated);
	notify(&created);
}

static void
reconcile_file_membership(
	Window *window, BrowserAdapter *adapter, Browser *browser)
{
	adapter->last_file_rev_ = browser->file_rev_;

	vector<string> now;
	now.reserve(browser->files_.size());
	for (const Browser::File &file : browser->files_)
		now.push_back(file.path);
	// A rescan that found the directory exactly as it was is not a change.
	if (adapter->last_file_paths_ == now)
		return;

	// Only what a client has already asked for gets retired: an id that was
	// never handed out has nothing to invalidate, and a path that survives
	// the rescan keeps the one it had.
	unordered_set<string> next(now.begin(), now.end());
	Registry *registry = find_registry(window);
	for (const string &path : adapter->last_file_paths_) {
		if (registry && !next.count(path))
			retire_file(*registry, browser, path);
	}

	// Arrivals are not announced one by one.  Wrappers are made on child(i),
	// and making one for every new path just to say it exists is the event
	// flood and the eager materialisation the plan rules out together.
	notify_listing_replaced(adapter);
	adapter->last_file_paths_ = std::move(now);
}

static void
reconcile_file_selection(Window *window, BrowserAdapter *list, Browser *browser,
	const string &was, const string &now)
{
	if (was == now)
		return;

	if (!was.empty()) {
		if (QAccessibleInterface *iface = existing_file(window, browser, was)) {
			QAccessibleEvent event(iface, QAccessible::SelectionRemove);
			notify(&event);
		}
	}
	if (!now.empty()) {
		if (QAccessibleInterface *iface =
				interface_for_file(window, browser, now)) {
			QAccessibleEvent event(iface, QAccessible::SelectionAdd);
			notify(&event);
		}
	}

	// The old file may already have been retired with the listing.  The
	// list still has a selection change to announce.
	QAccessibleEvent within(list, QAccessible::SelectionWithin);
	notify(&within);
}

// Scrolling changes extents and offscreen state, and neither is anything the
// bridge will carry: it translates StateChanged only for checked, active,
// disabled and focused, and drops LocationChanged on the floor.  Extents and
// states are queried live, so a client that wants them asks -- and walking
// every materialised row per frame to map rectangles nobody would receive is
// the whole of what that would buy.  Selection has its own events below.
static void
reconcile_files(Window *window, BrowserAdapter *adapter)
{
	if (!adapter->isValid())
		return;

	Browser *browser = adapter->browser();
	if (browser->file_rev_ != adapter->last_file_rev_)
		reconcile_file_membership(window, adapter, browser);

	string cursor_path;
	if (const int row = cursor_row(browser); row >= 0)
		cursor_path = browser->files_[size_t(row)].path;

	if (cursor_path != adapter->last_cursor_path_) {
		reconcile_file_selection(
			window, adapter, browser, adapter->last_cursor_path_, cursor_path);
		notify_focus(window, window->kit().focus_);
		adapter->last_cursor_path_ = std::move(cursor_path);
	}
}

// One container's children, before against after.  The parent is passed as an
// interface rather than found from a widget, because the client area has no
// widget behind it and is a container all the same.
static void
reconcile_child_list(Window *window, QAccessibleInterface *parent,
	vector<Widget *> &last, vector<Widget *> &now)
{
	if (last == now)
		return;

	for (Widget *w : last) {
		if (listed(now, w))
			continue;
		QAccessibleInterface *iface = existing_interface(window, w);
		if (!iface)
			continue;
		QAccessibleEvent hide(iface, QAccessible::ObjectHide);
		notify(&hide);
		// Qt translates this into children-changed:remove on parent(),
		// without deleting the interface, so a cached client drops the
		// child and a held id still names the same node.  Overflow has
		// already reparented the widget, and a page switched away from is
		// no longer anybody's; force the container that lost it, so the
		// event does not land on the new parent or on nothing at all.
		auto *child = dynamic_cast<WidgetAdapter *>(iface);
		if (child)
			child->forced_parent_ = parent;
		QAccessibleEvent gone(iface, QAccessible::ObjectDestroyed);
		notify(&gone);
		if (child)
			child->forced_parent_ = nullptr;
	}

	for (Widget *w : now) {
		if (listed(last, w))
			continue;

		if (QAccessibleInterface *iface = interface_for(window, w)) {
			QAccessibleEvent created(iface, QAccessible::ObjectCreated);
			notify(&created);
			QAccessibleEvent show(iface, QAccessible::ObjectShow);
			notify(&show);
		}
	}
	last = std::move(now);
}

static void
reconcile_file_rows(Window *window, FileRowsAdapter *adapter)
{
	auto *rows = (FileRows *) adapter->widget_;
	vector<Widget *> now;
	semantic_children(rows, now);
	if (now != adapter->last_children_) {
		notify_listing_replaced(adapter);
		adapter->last_children_ = std::move(now);
	}
	QAccessibleInterface *selected = interface_for(window, rows->selected);
	const QAccessible::Id id = selected ? QAccessible::uniqueId(selected) : 0;
	if (id == adapter->last_selected)
		return;
	if (auto *was = QAccessible::accessibleInterface(adapter->last_selected)) {
		QAccessibleEvent removed(was, QAccessible::SelectionRemove);
		notify(&removed);
	}
	if (selected) {
		QAccessibleEvent added(selected, QAccessible::SelectionAdd);
		notify(&added);
	}
	QAccessibleEvent within(adapter, QAccessible::SelectionWithin);
	notify(&within);
	adapter->last_selected = id;
}

static void
reconcile_children(Window *window, WidgetAdapter *adapter)
{
	if (!adapter->widget_)
		return;

	// A listing's children are rows rather than widgets, and are diffed by
	// path against the model instead of by pointer against the tree.
	if (auto *list = dynamic_cast<BrowserAdapter *>(adapter)) {
		reconcile_files(window, list);
		return;
	}

	if (auto *rows = dynamic_cast<FileRowsAdapter *>(adapter)) {
		reconcile_file_rows(window, rows);
		return;
	}

	vector<Widget *> now;
	semantic_children(adapter->widget_, now);
	reconcile_child_list(window, adapter, adapter->last_children_, now);
}

// The client area is a container like any other, but no Widget backs it and
// it is not in registry.widgets, so the sweep below would never reach it.
// Its children are the active page's, and a mode switch replaces all of them
// at once without destroying anything: the pages both stay alive, and only
// one of them is the root of the tree at a time.  Without this, opening a
// file drops the browser and puts up the viewer with nobody told.
static void
reconcile_client(Window *window, Registry &registry)
{
	auto *client = dynamic_cast<ClientAdapter *>(client_interface(window));
	if (!client)
		return;

	vector<Widget *> now;
	if (Page *page = window->active_page())
		semantic_children(page, now);
	if (!registry.client_known) {
		registry.client_children = std::move(now);
		registry.client_known = true;
		return;
	}
	reconcile_child_list(window, client, registry.client_children, now);
}

static void
reconcile_popups(Window *window, Registry &registry)
{
	vector<Widget *> now;
	open_popups(window->kit(), now);
	vector<QAccessible::Id> now_ids;
	now_ids.reserve(now.size());
	for (Widget *w : now) {
		if (QAccessibleInterface *iface = interface_for(window, w))
			now_ids.push_back(QAccessible::uniqueId(iface));
	}
	if (!registry.popups_known) {
		registry.popups = now_ids;
		registry.popups_known = true;
		return;
	}

	// A shut popup is no longer a child of the window, and there is no event
	// that says so wholesale: ObjectReorder on the shell packs the child
	// path as a plain string, which libatspi rejects outright.  The pair the
	// bridge does translate is the same one a widget leaving its container
	// gets -- ObjectDestroyed removes it from a cached client's children
	// without the interface, or its id, going anywhere.
	for (QAccessible::Id id : registry.popups) {
		if (listed_id(now_ids, id))
			continue;

		QAccessibleInterface *iface = QAccessible::accessibleInterface(id);
		if (!iface)
			continue;

		QAccessibleEvent hide(iface, QAccessible::ObjectHide);
		notify(&hide);
		QAccessibleEvent gone(iface, QAccessible::ObjectDestroyed);
		notify(&gone);
	}
	for (size_t i = 0; i < now.size(); i++) {
		if (listed_id(registry.popups, now_ids[i]))
			continue;
		if (QAccessibleInterface *iface = interface_for(window, now[i])) {
			QAccessibleEvent created(iface, QAccessible::ObjectCreated);
			notify(&created);
			QAccessibleEvent show(iface, QAccessible::ObjectShow);
			notify(&show);
		}
	}
	registry.popups = std::move(now_ids);
}

static void
reconcile_window(Window *window)
{
	Registry *registry = find_registry(window);
	if (!registry)
		return;

	reconcile_popups(window, *registry);
	reconcile_client(window, *registry);

	vector<Widget *> widgets;
	widgets.reserve(registry->widgets.size());
	for (const auto &entry : registry->widgets)
		widgets.push_back(entry.first);
	for (Widget *w : widgets) {
		auto it = registry->widgets.find(w);
		if (it == registry->widgets.end())
			continue;

		auto *adapter = dynamic_cast<WidgetAdapter *>(
			QAccessible::accessibleInterface(it->second));
		if (!adapter)
			continue;

		reconcile_children(window, adapter);
		reconcile_widget(adapter);
	}
}

// Losing the focus is not gaining it elsewhere: there is one event for
// taking the focus and none for dropping it, so the control that had it says
// that it no longer does, and only a real new focus announces itself.
static void
notify_focus(Window *window, Widget *w)
{
	if (!QAccessible::isActive())
		return;

	Registry &registry = g_registries[window];
	// A background window keeps its own idea of where the keyboard would go,
	// and may even move it; none of that is the keyboard being there.
	Widget *effective = window_active(window) ? w : nullptr;
	QAccessibleInterface *iface = announced_focus(window, effective);
	const QAccessible::Id id = iface ? QAccessible::uniqueId(iface) : 0;
	// Qt hands activation from the shell to the content surface on the first
	// click into it, which is no change at all to anybody outside.
	if (id == registry.focus)
		return;

	const QAccessible::Id was = registry.focus;
	registry.focus = id;
	if (iface) {
		// The bridge clears the previous focus as part of announcing this
		// one, so saying it here as well would be the same news twice.
		QAccessibleEvent event(iface, QAccessible::Focus);
		notify(&event);
		return;
	}
	// What it has no event for is focus going nowhere; without this the
	// control that had it keeps it for as long as nothing else takes it.
	if (QAccessibleInterface *before = QAccessible::accessibleInterface(was)) {
		QAccessible::State changed;
		changed.focused = 1;
		QAccessibleStateChangeEvent event(before, changed);
		notify(&event);
	}
}

// QtGui emits nothing of its own when a window is activated: QGuiApplication
// only ever sets the accessible root, and the window:activate the bridge
// sends comes out of a state change on a Window-role interface, which in Qt
// is QtWidgets' doing.  Orca picks the window it reads from that signal, so
// without it a focus event lands on no window in particular.
static void
notify_active(Window *window)
{
	if (!QAccessible::isActive())
		return;

	Registry &registry = g_registries[window];
	const bool now = window_active(window);
	// Qt hands activation from the shell to the content surface on the first
	// click into it, and both halves report it; that is one window to
	// everybody else, and must not be read out twice.
	if (registry.active_known && registry.active == now)
		return;

	registry.active = now;
	registry.active_known = true;
	if (QAccessibleInterface *shell = shell_interface(window)) {
		QAccessible::State changed;
		changed.active = 1;
		// Qt 6.11's interface constructor stores both the QObject and the
		// interface ID, which uniqueId() then mistakes for a child index.
		QAccessibleStateChangeEvent event(shell->object(), changed);
		notify(&event);
	}
}

void
accessible_changed(Window *window, Change what, Widget *w)
{
	switch (what) {
	case Change::Focus:
		notify_focus(window, w);
		break;
	case Change::Retired:
		if (Registry *registry = find_registry(window); registry && w)
			retire_subtree(*registry, w);
		break;
	case Change::State:
		if (window)
			reconcile_window(window);
		break;
	case Change::Text:
		if (window && w) {
			if (auto *adapter =
					dynamic_cast<EntryAdapter *>(existing_interface(window, w)))
				notify_entry_text(adapter);
		}
		break;
	}
}

void
accessible_retire_page(Window *window)
{
	Registry *registry = find_registry(window);
	if (!registry)
		return;

	registry->popups.clear();
	registry->popups_known = false;
	// The widgets these name are going with the page, and a later frame
	// must not diff a new tree against pointers into the old one.
	registry->client_children.clear();
	registry->client_known = false;

	// Reporting a removal has the bridge ask after the parent, which
	// registers it anew where it had not been asked for before.  Each pass
	// therefore leaves only ancestors of what it retired, which are strictly
	// shallower; the cap is there so that an unforeseen cycle cannot hang a
	// window that is already on its way out.
	for (int pass = 0; pass < 64 && !registry->widgets.empty(); pass++) {
		vector<pair<int, Widget *>> doomed;
		doomed.reserve(registry->widgets.size());
		for (const auto &entry : registry->widgets)
			doomed.emplace_back(widget_depth(entry.first), entry.first);

		// Deepest first, because a removal is reported on the parent, and
		// unlike walking the tree this still holds for a page half taken.
		sort(doomed.begin(), doomed.end(),
			[](const auto &a, const auto &b) { return a.first > b.first; });
		for (const auto &entry : doomed)
			retire_widget(*registry, entry.second);
	}
}

void
accessible_forget_window(Window *window)
{
	auto it = g_registries.find(window);
	if (it == g_registries.end())
		return;

	// Whatever drop_frames() left behind, dropped without asking the widgets
	// anything: the page trees have already gone, so a depth, a parent, or
	// an event that has the bridge walk back up would all read freed memory.
	// Nobody is told, because the window they would be told about is going
	// with them, and Qt drops the shell's own interface with its QObject.
	Registry &registry = it->second;
	for (const auto &entry : registry.widgets) {
		if (auto *adapter = dynamic_cast<WidgetAdapter *>(
				QAccessible::accessibleInterface(entry.second)))
			adapter->detach();
		QAccessible::deleteAccessibleInterface(entry.second);
	}
	for (const auto &browser : registry.files) {
		for (const auto &entry : browser.second) {
			if (auto *adapter = dynamic_cast<FileAdapter *>(
					QAccessible::accessibleInterface(entry.second)))
				adapter->detach();
			QAccessible::deleteAccessibleInterface(entry.second);
		}
	}
	// The shell's interface, and an object-backed client's, belong to Qt's
	// cache, which drops them when their QObject goes.
	if (registry.client)
		QAccessible::deleteAccessibleInterface(registry.client);
	g_registries.erase(it);
}

void
accessible_renamed(Window *window)
{
	if (!window || !QAccessible::isActive())
		return;

	if (QAccessibleInterface *iface = shell_interface(window)) {
		QAccessibleEvent event(iface->object(), QAccessible::NameChanged);
		notify(&event);
	}
}

void
accessible_activated(Window *window)
{
	if (!window)
		return;

	// The window first, because a focus event means the focus within the
	// active window, and whatever the kit thinks has the focus now either
	// really has it or really does not.  Saying either of these again when
	// nothing changed is what both of them settle for us.
	notify_active(window);
	notify_focus(window, window->kit().focus_);
}

// --- Activation --------------------------------------------------------------

void
accessible_init()
{
	QAccessible::installFactory(accessible_factory);
}

}  // namespace dn

#endif  // DN_WITH_ACCESSIBILITY
