//
// kit.hpp: Vulkan overlay kit and atlas engine
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "overlay.hpp"
#include "sheet.hpp"

#include "libdn/libdn.h"

#include <QFont>
#include <QImage>
#include <QRawFont>
#include <QString>
#include <Qt>

#include <chrono>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace dn
{

class Renderer;

// Widget geometry, in device pixels.  Integral by construction: layout
// arithmetic composes exactly, and there is nothing left to snap.  Floats
// belong to drawing, and to whatever is genuinely continuous -- a pointer
// position, a scroll offset mid-drag, the viewer's pan and zoom.
struct Rect {
	int x = 0;
	int y = 0;
	int w = 0;
	int h = 0;
	// Takes floats: the pointer is continuous, and comparing it against an
	// integral edge is exact.
	[[nodiscard]] bool contains(float px, float py) const
	{
		return px >= float(this->x) && py >= float(this->y) &&
			px < float(this->x + this->w) && py < float(this->y + this->h);
	}
	[[nodiscard]] int right() const { return this->x + this->w; }
	[[nodiscard]] int bottom() const { return this->y + this->h; }
	[[nodiscard]] Box box() const
	{
		return {this->x, this->y, this->right(), this->bottom()};
	}
	[[nodiscard]] bool empty() const { return this->w <= 0 || this->h <= 0; }
	[[nodiscard]] Rect inset(int px, int py) const;
};

enum class Align : uint8_t { Start, Center, End };
enum class Fill : uint8_t { None, Toolbar, Tooltip, Panel };
enum class Stroke : uint8_t { None, All, Bottom };

// Sentinel for "no constraint", in pixels: large, but far from overflowing
// any sum it takes part in.
constexpr int kUnlim = 1 << 24;

// Design sizes, in points: they must keep their physical size across displays,
// so they are converted to pixels on use, through Kit::px().
constexpr float kIconPts = 16.f;
constexpr float kFramePadX = 6.f;
constexpr float kFramePadY = 4.f;
constexpr float kTooltipPadX = 8.f;
constexpr float kGlowPts = 8.f;
constexpr float kResizeBorderPts = 8.f;
constexpr float kScrollBarW = 8.f;
constexpr float kScrollStep = 32.f;
constexpr float kScrollHideMs = 1000.f;

// Indexes into Kit::colours_, which Kit::bake_colours fills in for the
// current theme and display profile.
enum : uint8_t {
	ColourWell,
	ColourToolbarTop,
	ColourToolbarBottom,
	ColourHover,
	ColourPress,
	ColourDivider,
	ColourBusy,
	ColourInk,
	ColourFrame,
	ColourEntryTop,
	ColourEntryBottom,
	ColourPanel,
	ColourHint,
	ColourCount,
};

struct Kit;
struct Scroll;

// One keystroke, as the platform delivered it.
struct Key {
	int key = 0;
	unsigned mods = 0;
	// What Qt composed for this keystroke: layout- and dead-key-aware.
	// Empty for bare modifiers, and a control code for Return and friends.
	QString text;
};

// What an input method needs to know about the widget it is composing into.
// Everything the platform asks for is derived from these; a widget that
// fills one in is by that fact a text target.
struct TextTarget {
	// Committed text only, without any preedit: what the input method may
	// reconsider around the caret.
	QString text;
	// Caret offset into text, in UTF-16 units, as Qt counts them.
	int caret = 0;
	// Where to park the candidate window, in the same coordinates as
	// Widget::r -- getting this wrong strands the list in a screen corner.
	Rect caret_rect;
};

// --- Kit ---------------------------------------------------------------------

struct Widget {
	Rect r;
	bool visible = true;
	bool layout_visible = true;
	bool hittable = false;

	Widget *parent_ = nullptr;

	virtual ~Widget() = default;
	virtual void measure(Kit &kit, int max_w, int max_h) = 0;
	virtual void arrange(Kit &kit, Rect alloc) = 0;
	virtual void paint(Kit &kit) const;
	virtual Widget *hit_at(float x, float y);
	virtual bool grows() const { return false; }
	[[nodiscard]] bool shown() const
	{
		return this->visible && this->layout_visible;
	}
	virtual bool clips_children() const { return false; }
	virtual bool focusable() const { return false; }
	// This widget's default action, as a menu item or a hint would trigger
	// it.  Most widgets have none and say so; what to do instead is then the
	// caller's to decide, which is why this does not fall back to taking
	// focus on its own.
	virtual bool activate(Kit &kit) { return false; }
	virtual bool traps_focus() const { return false; }
	// A scrollbar is a hover effect rather than an event, so it must not
	// depend on who ends up consuming the motion.  Neither must the cursor.
	virtual Scroll *scrollbar() { return nullptr; }
	virtual Qt::CursorShape cursor() const { return Qt::ArrowCursor; }
	virtual QString tip() const { return {}; }
	virtual QString tip_key() const { return {}; }
	// Below this->r. Empty (w <= 0) means follow the pointer.
	[[nodiscard]] virtual Rect tip_anchor() const { return this->r; }
	virtual void prepare(Kit &kit);
	virtual bool press(Kit &kit, float x, float y, Qt::MouseButton button);
	virtual bool release(Kit &kit, float x, float y, Qt::MouseButton button);
	virtual bool motion(Kit &kit, float x, float y);
	virtual bool scroll(Kit &kit, float x, float y, int delta);
	virtual bool pan(Kit &kit, float x, float y, float dx, float dy);
	virtual bool gesture(
		Kit &kit, float x, float y, float scale_factor, float angle_delta);
	virtual bool key(Kit &kit, const Key &ev);
	// An input method updated its preedit, or committed to it.
	virtual bool input_method(
		Kit &kit, const QString &commit, const QString &preedit, int caret);
	// The other half of that channel: what the input method may ask back.
	// Returning false means this widget does not take text.
	virtual bool text_target(const Kit &kit, TextTarget &out) const;
	virtual bool double_click(
		Kit &kit, float x, float y, Qt::MouseButton button, unsigned mods);
	[[nodiscard]] virtual int wake_ms() const { return -1; }
	virtual std::size_t child_count() const { return 0; }
	virtual Widget *child(std::size_t) const { return nullptr; }
	void paint_children(Kit &kit) const;
};

struct Composite : Widget {
	std::vector<std::unique_ptr<Widget>> kids;

	Widget *add_child(
		std::unique_ptr<Widget> child, std::size_t at = std::size_t(-1));
	// The inverse: detaches one child and hands its ownership back.
	std::unique_ptr<Widget> take_child(std::size_t at);
	void erase_children(std::size_t from = 0);
	std::size_t child_count() const override { return this->kids.size(); }
	Widget *child(std::size_t i) const override
	{
		return i < this->kids.size() ? this->kids[i].get() : nullptr;
	}
};

struct Button : Widget {
	Action action = Action::None;
	const char *icon = nullptr;
	// An index into text, underlined when drawn; -1 for none.  Nothing
	// dispatches on it yet -- outside a menu it is a hint, not a key.
	int mnemonic = -1;
	QString tip_text;
	QString tip_accel;
	QString text;
	bool enabled_ = true;
	bool active = false;
	bool dim = false;
	bool flat = false;
	bool activate_on_press = false;
	float pad_x = 0;
	std::function<void(Kit &)> on_click;

	Button() { this->hittable = true; }
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
	bool focusable() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, const Key &ev) override;
	bool activate(Kit &kit) override;
};

struct Checkbox : Button {
	bool checked = false;

	void measure(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool activate(Kit &kit) override;
};

struct Label : Widget {
	QString text;
	int mnemonic = -1;
	float min_w = 0;
	float pad_x = 0;
	float pad_y = 0;
	bool bold = false;
	bool wrap = false;
	bool grow = false;
	bool dim = false;
	Align align = Align::Start;
	Align valign = Align::Center;
	QString tip_text;
	QString tip_accel;

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool grows() const override { return this->grow; }
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
};

// A single-line text field.  There is no selection: the caret is the whole
// of the state, and a click just places it.
struct Entry : Widget {
	QString text;
	QString placeholder;
	// Uncommitted input-method text, shown at the caret but not part of text.
	QString preedit;
	int caret = 0;
	int preedit_caret = 0;
	float min_w = 160.f;
	float pad_x = kFramePadX;
	bool flat = false;
	// A field takes what room it is given -- in the overflow popup it moved
	// into as much as in the bar, where it fills out the rest of the line it
	// wrapped onto.
	bool grow = true;
	std::function<void(Kit &)> on_change;
	std::function<void(Kit &)> on_submit;
	std::function<void(Kit &)> on_cancel;

	// Horizontal scroll, in points, kept so that the caret stays visible.
	float scroll_ = 0;
	std::chrono::steady_clock::time_point caret_at_{};
	// Both decided in prepare, which is the only place with a Kit to ask
	// about focus; paint and wake_ms are const and just read them.
	bool focused_ = false;
	bool caret_on_ = false;

	Entry() { this->hittable = true; }
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool focusable() const override;
	bool grows() const override { return this->grow; }
	Qt::CursorShape cursor() const override { return Qt::IBeamCursor; }
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, const Key &ev) override;
	bool input_method(Kit &kit, const QString &commit, const QString &pre,
		int pre_caret) override;
	bool text_target(const Kit &kit, TextTarget &out) const override;
	[[nodiscard]] int wake_ms() const override;

	void set_text(Kit &kit, const QString &next);
	void move_caret(Kit &kit, int to);
	// Resets the blink, and re-scrolls to keep the caret in view.
	void touch_caret(const Kit &kit);
	// Just the scroll: layout runs every frame, and must not touch the blink.
	void rescroll(const Kit &kit);
	[[nodiscard]] int inner_w(const Kit &kit) const;
	// The text as painted: the placeholder stands in when empty.
	[[nodiscard]] QString painted() const;
};

struct Sep : Widget {
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
};

struct Splitter : Widget {
	float min_w = 8.f;
	std::function<void(Kit &kit, float mouse_x)> on_drag;

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	Qt::CursorShape cursor() const override { return Qt::SplitHCursor; }
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

struct Container : Composite {
	float gap = 0;
	float pad_x = 0;
	float pad_y = 0;
	bool grow = false;
	bool grows() const override { return this->grow; }

	void measure_pack(Kit &kit, int max_w, int max_h, bool hz);
	void arrange_pack(Kit &kit, Rect alloc, bool hz, Align align);
};

inline bool
is_sep(const Widget *w)
{
	return dynamic_cast<const Sep *>(w);
}

inline bool
context_key(int key, unsigned mods)
{
	if (key == Qt::Key_Menu)
		return true;
	return key == Qt::Key_F10 && mods == unsigned(Qt::ShiftModifier);
}

struct Row : Container {
	Align align = Align::Start;

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
};

struct Column : Container {
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
};

// Packs sideways like a Row, but breaks onto a new line when the next child
// would not fit.  Children keep their natural widths: this is for a strip of
// toolbar items that ran out of bar, not for a menu.
struct Flow : Container {
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;

private:
	// One wrapped line.  Indices are into kids, and the run may contain
	// hidden children; count is the extent, not a population.
	struct Line {
		std::size_t first = 0;
		std::size_t count = 0;
		int y = 0;
		int h = 0;
	};

	// Measures the children and breaks them into lines for an inner width,
	// answering the width of the widest one produced.  Both passes need the
	// measuring and the widths; only arrange() needs the lines themselves.
	int wrap(Kit &kit, int inner_w, int *total_h, std::vector<Line> *lines);
};

struct Scroll {
	// Continuous: a drag moves it by arbitrary amounts, and quantising it
	// mid-drag would make the thumb stutter.  Rounded where it becomes
	// geometry, never before.
	float offset = 0;
	float content = 0;
	float view = 0;
	bool dragging = false;
	// Point constants resolved against the current scale, by set_metrics():
	// the bar is drawn and hit-tested far from any Kit.
	int bar_w = 8;
	int step = 32;

	[[nodiscard]] float max_offset() const;
	void clamp();
	void set_metrics(const Kit &kit, float content_h, float view_h);
	void reveal();
	[[nodiscard]] bool visible() const;
	[[nodiscard]] int wake_ms() const;
	[[nodiscard]] Rect bar_rect(Rect viewport) const;
	[[nodiscard]] Rect thumb_rect(Rect viewport) const;
	bool wheel(int delta, float step_px);
	bool pan(float dy);
	bool page(int dir);
	bool press(float x, float y, Qt::MouseButton button, Rect viewport);
	bool motion(float y, Rect viewport);
	bool release(Qt::MouseButton button);
	void paint(Kit &kit, Rect viewport) const;

private:
	std::chrono::steady_clock::time_point shown_at_{};
	float grab_ = 0;
	void set_from_y(float y, Rect viewport);
};

struct ScrollColumn : Column {
	Scroll scroll_;
	bool follow_focus = false;
	Widget *followed_ = nullptr;

	ScrollColumn() { this->hittable = true; }
	Scroll *scrollbar() override { return &this->scroll_; }
	bool clips_children() const override { return true; }
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	Widget *hit_at(float x, float y) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool scroll(Kit &kit, float x, float y, int delta) override;
	bool pan(Kit &kit, float x, float y, float dx, float dy) override;
	bool key(Kit &kit, const Key &ev) override;
	[[nodiscard]] int wake_ms() const override;
};

// Decorated vertical stack. Subclasses with overlay/custom layout override
// arrange.
struct Panel : Composite {
	float pad_x = 0;
	float pad_y = 0;
	float min_w = 0;
	float min_h = 0;
	float max_h = 0;
	Fill fill = Fill::None;
	Stroke stroke = Stroke::None;
	bool busy = false;
	bool grow = false;
	bool clip = false;
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	bool grows() const override { return this->grow; }
	bool clips_children() const override { return this->clip; }
};

// A panel that floats above the widget tree, on Kit's popup stack.
// Menu behaviour is not here but in MenuPopup below.
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
	// The half of place() that is not about x: drops the popup below its
	// anchor, flips it above when it would not fit, and lays it out.
	void place_below(Kit &kit, int x);
	virtual void place_sub(Kit &kit);
	bool traps_focus() const override { return true; }
	virtual bool captures_keys() const { return false; }
	// Focus loss dismisses transient popups; a dialog waits for Escape
	// or its Close button.
	virtual bool transient() const { return true; }
	void paint(Kit &kit) const override;
	bool key(Kit &kit, const Key &ev) override;
};

// Dismissed by Escape or a footer button; the caller fills the body and may
// replace the default Close action.
struct Dialog : Popup {
	Panel *frame = nullptr;
	ScrollColumn *body = nullptr;
	Row *footer = nullptr;

	Dialog();
	void show(Kit &kit, std::unique_ptr<Widget> content, float min_w,
		std::unique_ptr<Widget> actions);
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void paint(Kit &kit) const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool transient() const override { return false; }
};

struct MenuItem;

// A popup navigated like a menu: hovering moves the focus, arrows walk the
// items, and a click anywhere else in the stack dismisses it.
struct MenuPopup : Popup {
	void focus_item(Kit &kit, Widget *w, bool kbd) const;
	void reveal(Kit &kit, Widget *w);
	bool motion(Kit &kit, float x, float y) override;
	bool key(Kit &kit, const Key &ev) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

struct ToolbarSlot;

// What a ToolbarSlot puts the items it could not fit into.  These are the
// toolbar items themselves, moved here for as long as the popup is up rather
// than stood in for, so they keep flowing sideways, and wrap.
struct Overflow : MenuPopup {
	Flow *col = nullptr;
	// Whose items col is currently holding.  One Overflow serves all three
	// slots, so this, not the slot asking, says who to hand them back to.
	ToolbarSlot *lender = nullptr;
	std::function<void()> refill;

	Overflow();
	~Overflow() override;
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	bool key(Kit &kit, const Key &ev) override;
	bool motion(Kit &kit, float x, float y) override;

private:
	// Moves the focus a line up or down, keeping to one track.
	void step_line(Kit &kit, int dir);

	// Which column Up/Down aim for, so that a run of them keeps to one
	// track across lines of differing item counts.  Negative means unset.
	float want_x_ = -1;
};

struct Menu : MenuPopup {
	Column *col = nullptr;
	Actor actor;
	std::vector<std::unique_ptr<Menu>> subs_;

	Menu();
	void build(Kit &kit, std::span<const MenuNode> nodes, const Actor &actor);
	void sync();
	MenuItem *add_item(const QString &text);
	MenuItem *add_item_with_mnemonic(const QString &text);
	void add_sep();
	void clear(Kit &kit);
	void measure(Kit &kit, int max_w, int max_h) override;
	bool key(Kit &kit, const Key &ev) override;
};

struct MenuItem : Button {
	QString accel;
	Menu *sub = nullptr;
	bool checked = false;
	bool checkable = false;
	int label_col = 0;
	int accel_col = 0;

	void measure(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool activate(Kit &kit) override;
	int label_width(const Kit &kit) const;
	int accel_width(const Kit &kit) const;
};

struct Combo;

// One row of a Combo's list: a menu item without the menu's furniture.
// There is no lead column, because there is nothing to check off there --
// which item is current is said by where the list was placed, and by the
// focus, exactly as the item under the pointer is said in a menu.
struct ComboItem : Button {
	void measure(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
};

// The list a Combo drops.  Menu navigation applies to it unchanged; all
// that differs is where it lands, and that is the whole point of it.
struct ComboPopup : MenuPopup {
	Column *col = nullptr;
	Combo *combo = nullptr;

	ComboPopup();
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void place_sub(Kit &kit) override;
};

// A closed choice: a bordered label with a chevron.  Nothing here is
// editable, nor looks it, which is the one thing every Win32 combo box
// gets wrong for this purpose.
struct Combo : Button {
	std::vector<QString> items;
	int current = 0;
	// The index is what changed; the caller usually wants it rather than
	// having to read it back off the widget.
	std::function<void(Kit &, int)> on_select;

	std::unique_ptr<ComboPopup> popup_;

	Combo();
	void measure(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool activate(Kit &kit) override;
	bool key(Kit &kit, const Key &ev) override;
	// Clamps, and notifies only on an actual change.
	void select(Kit &kit, int index);
	[[nodiscard]] QString current_text() const;
};

// One end of a toolbar. What does not fit goes behind the "more" button.
struct ToolbarSlot : Row {
	Button *more = nullptr;
	std::size_t split_ = 0;

	// Every item this slot has, in bar order, never null and never reordered:
	// raw, because while the overflow is up the tail of them is owned by its
	// Flow rather than by this slot's kids.  The split is measured against
	// all of them wherever they live, which is what keeps it steady.
	std::vector<Widget *> items_;

	ToolbarSlot();
	Widget *add_item(
		std::unique_ptr<Widget> item, std::size_t at = std::size_t(-1));
	// Moves everything past the split into the popup, or brings it back.
	void lend_to(Overflow &overflow);
	void reclaim();
	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;

private:
	// On a toolbar item layout_visible means "I am in somebody's kids right
	// now": it is what keeps an item that overflowed while the popup is shut
	// -- parented here, but no child of anyone -- out of the focus order.
	void sync_layout_visible();

	// Where the lent items came from, so that they go back in bar order.
	// Empty (first >= last) when the popup holds none of ours.
	std::size_t lent_first_ = 0;
	std::size_t lent_last_ = 0;
	Overflow *borrower_ = nullptr;

	using Composite::add_child;
	using Composite::erase_children;
	using Composite::take_child;
};

struct Toolbar : Panel {
	Actor actor;
	ToolbarSlot *left = nullptr;
	ToolbarSlot *mid = nullptr;
	ToolbarSlot *right = nullptr;
	Overflow *overflow = nullptr;

	Toolbar(std::unique_ptr<ToolbarSlot> left_row,
		std::unique_ptr<ToolbarSlot> mid_row,
		std::unique_ptr<ToolbarSlot> right_row);
	void sync_buttons();

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;

private:
	std::unique_ptr<Overflow> overflow_owned_;
	void place_slots(Kit &kit);
	ToolbarSlot *slot_for_more(const Button *more) const;
};

// Client-side decorations: shown only while Kit::csd_ is on.
struct Titlebar : Panel {
	Label *title = nullptr;
	Button *minimize = nullptr;
	Button *maximize = nullptr;
	Button *close = nullptr;
	Actor actor;
	QString text;
	float drag_x_ = 0.f;
	float drag_y_ = 0.f;
	bool drag_armed_ = false;

	Titlebar();
	void sync(Kit &kit);

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void prepare(Kit &kit) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
};

struct Kit {
	using Packed = Sheet::Packed;
	struct Glyph {
		Packed rect;
		int bearing_x = 0;
		int bearing_y = 0;
	};

	float dpr_ = 1.f;
	bool inited_ = false;
	Sheet atlas_;
	uint32_t atlas_epoch_ = 0;
	Packed white_{};
	Packed glow_{};
	QFont font_;
	QFont font_bold_;
	QFont font_px_;
	QFont font_bold_px_;
	QRawFont raw_;
	QRawFont raw_bold_;
	std::unordered_map<std::string, Packed> icons_;
	std::unordered_map<uint64_t, Glyph> glyphs_;
	std::vector<QRawFont> fonts_;
	OverlayList list_;

	Colour colours_[ColourCount]{};

	Widget *root_ = nullptr;
	Widget *focus_ = nullptr;
	Widget *default_focus_ = nullptr;
	bool focus_visible_ = false;
	Widget *hot_ = nullptr;
	float mouse_x_ = -1.f;
	float mouse_y_ = -1.f;
	bool left_down_ = false;
	Widget *pressed_ = nullptr;
	unsigned mods_ = 0;
	std::vector<Popup *> popups_;
	std::unique_ptr<Widget> scrim_;
	int host_w_ = 0;
	int host_h_ = 0;
	Renderer *renderer_ = nullptr;
	std::function<void(std::function<void()>)> post;
	std::function<void()> request_render;
	// The focused Entry changed, or moved its caret: the platform has to
	// re-query the input method state.
	std::function<void()> input_method_changed;
	bool fullscreen_ = false;
	bool maximized_ = false;
	bool active_ = true;
	bool csd_ = false;
	bool csd_shadow_ = false;
	bool dark_ = false;
	Qt::CursorShape cursor_ = Qt::ArrowCursor;
	std::function<void()> start_move;
	std::function<void(Qt::Edges)> start_resize;
	std::function<void(float, float)> start_menu;
	std::chrono::steady_clock::time_point hover_at_{};
	std::chrono::steady_clock::time_point popup_at_{};
	float hover_x_ = 0;
	float hover_y_ = 0;
	QString tooltip_text_;
	QString tooltip_accel_;
	bool tooltip_visible_ = false;
	const Widget *tooltip_anchor_ = nullptr;  // set for keyboard-focus tips

	Kit() = default;
	~Kit() { destroy(); }

	Kit(const Kit &) = delete;
	Kit &operator=(const Kit &) = delete;

	void init(float dpr);
	void destroy();
	void forget_tree(Widget *tree);
	void sync_focus();
	void open_popup(Popup *p);
	void close_popups();
	void close_transient_popups();
	void close_above(const Popup *p);
	void relayout_popups();
	void prepare_popups();
	[[nodiscard]] bool popup_open() const;
	[[nodiscard]] Popup *top_popup() const;
	Widget *hit(float x, float y);
	bool track_popups(float x, float y);

	// Client-side decorations. The host window is host_w_ by host_h_;
	// under csd_shadow_ the window itself only fills the frame within it.
	[[nodiscard]] Rect frame() const;
	[[nodiscard]] Qt::Edges resize_edges(float x, float y) const;
	bool start_resize_at(float x, float y);
	void sync_cursor();

	// Moving focus says in the same breath whether to draw it: a ring means
	// the keyboard put focus here.  Anything that changes who has focus goes
	// through here, so the two can never drift into a ring that outlives its
	// focus, or focus with no ring.  Re-seating the same focus across a tree
	// rebuild is not a focus change, and leaves the ring as it found it.
	void set_focus(Widget *w, bool ring);
	Widget *focus_scope() const;
	void cycle_focus(int dir);
	bool cycle_focus(Widget *scope, int dir, bool wrap = true);
	void focus_first(Widget *scope);
	bool key(const Key &ev);
	bool input_method(const QString &commit, const QString &preedit, int caret);
	[[nodiscard]] bool text_target(TextTarget &out) const;
	bool mouse_press(
		float x, float y, Qt::MouseButton button, unsigned mods = 0);
	bool mouse_release(float x, float y, Qt::MouseButton button);
	bool mouse_motion(float x, float y);
	bool mouse_scroll(float x, float y, int delta);
	bool pan(float x, float y, float dx, float dy);
	bool gesture(float x, float y, float scale_factor, float angle_delta);
	bool mouse_double_click(
		float x, float y, Qt::MouseButton button, unsigned mods);
	bool set_dpr(float dpr);
	void bake_colours(dawn::Cmm *cmm, dawn::Profile *target);
	void pack_icon(const char *name, int px);
	void draw_icon(int x, int y, int size, const char *name, Colour colour);
	Packed pack_bitmap(const QImage &image);
	void cache_text(const QString &text, bool bold);
	void emit_text(
		float x, float y, const QString &text, Colour colour, bool bold);
	void draw_glow(Rect w, Colour col);
	// An inactive window halves whatever alpha its ink already had.
	[[nodiscard]] float ink_alpha() const
	{
		return this->active_ ? 1.f : 0.5f;
	}
	void focus_ring(Rect w);   // 1pt inset ring
	void draw_shadow(Rect w);  // popup/tooltip drop shadow
	// Rect-shaped wrappers over the corner-based draw list.
	void draw_fill(Rect w, Colour col);
	void draw_border(Rect w, Colour col, int thickness);
	void clip_to(Rect w);
	void clip_pop();
	void tooltip(const Widget *hot);
	[[nodiscard]] int wake_ms() const;
	// One frame of the widget tree: lay it out, settle what the layout may
	// have moved -- popups, focus, the hover under the pointer -- and paint.
	// Between arranging and settling, the caller does whatever depends on the
	// fresh geometry but has to precede prepare().
	void frame_ui(Widget &ui, const std::function<void()> &placed);
	void paint();
	// Text metrics, all in device pixels: the fonts are already rasterised at
	// that size, so this is what Qt measures, without a round trip through
	// points.  Extents round up, so a glyph is never clipped by a pixel.
	[[nodiscard]] int text_width(const QString &text, bool bold) const;
	// Caret geometry, both in pixels, and both counting in UTF-16 units.
	[[nodiscard]] int caret_x(const QString &text, int index, bool bold) const;
	[[nodiscard]] int index_at(const QString &text, float x, bool bold) const;
	[[nodiscard]] QString elide_lines(
		const QString &text, int wrap_px, int max_lines, bool bold) const;
	[[nodiscard]] int text_height(
		const QString &text, int wrap_px, bool bold) const;

	// Points to device pixels.  Converted on use rather than cached: a sum of
	// point terms rounds once here, where baked-up constants would each round
	// separately and accumulate the error.
	[[nodiscard]] int px(float pts) const
	{
		return int(lround(double(pts) * double(this->dpr_)));
	}

	// The inverse, for the widget fields that are declared in points: a
	// width measured off the text has to go back through this before it
	// can be handed to one, or it gets scaled a second time.
	[[nodiscard]] float pts(int px) const
	{
		return float(double(px) / double(this->dpr_));
	}

	// An appropriately thick rule, border or caret, in device pixels.
	[[nodiscard]] int hairline() const { return std::max(px(1.f), 1); }

	// One icon square, in device pixels: the size pack_icon() rasterises at,
	// and the size the quad that samples it is drawn at.
	[[nodiscard]] int icon_px() const { return std::max(px(kIconPts), 16); }

	// Raw atlas bytes: 16-bit RGBA UNORM, 8 bytes/pixel, row-major.
	[[nodiscard]] bool font_pixels(
		unsigned char **out_pixels, int *width, int *height) const;
	[[nodiscard]] bool take_atlas_dirty();
};

}  // namespace dn
