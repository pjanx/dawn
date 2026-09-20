//
// kit.hpp: Vulkan overlay kit and atlas engine
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "overlay.hpp"
#include "text.hpp"

#include "libdn/libdn.hpp"

#include <QFont>
#include <QImage>
#include <QString>
#include <Qt>

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace dn
{

class Renderer;
struct Page;

// Immutable colour data shared with jobs. Workers recreate their own lcms
// profile from these bytes; curves and primaries always describe those bytes.
struct ScreenColour {
	std::vector<uint8_t> icc;
	dawn::ProfileEncoding encoding;
};

struct ScreenState {
	std::shared_ptr<dawn::Cmm> cmm;
	std::shared_ptr<dawn::Profile> profile;
	std::shared_ptr<const ScreenColour> colour;
	bool fallback = true;
};

struct [[nodiscard]] Size {
	int w = 0;
	int h = 0;
};

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
	// The overlap, or an empty rectangle where there is none.  Never
	// negative: a miss is {x, y, 0, 0}, not a rectangle inside out.
	[[nodiscard]] Rect intersect(Rect other) const;
	bool operator==(const Rect &) const = default;
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
// One transparency checkerboard square, for the browser's own drawing and
// for the renderer's shader alike.
constexpr float kCheckPts = 20.f;
constexpr float kFramePadX = 6.f;
constexpr float kFramePadY = 4.f;
constexpr float kTooltipPadX = 8.f;
constexpr float kGlowPts = 8.f;
constexpr float kResizeBorderPts = 8.f;
// How far the pointer must travel before a press becomes a drag.
constexpr float kDragPts = 4.f;
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
struct Menu;
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

// Shaping belongs to the text's owner, independently of its allocation or
// glyph atlas. Keep the variants used by the current and preceding frames.
struct TextCache {
	struct Text {
		std::unique_ptr<TextLayout> layout;
		int width = 0;
		int height = 0;
		float x = 0;  // Remove native single-line alignment when placing text.
		uint64_t used = 0;
	};
	std::map<std::tuple<QString, int, int, bool, bool>, Text> texts;
	uint64_t frame = 0;
	uint64_t epoch = 0;

	Text &get(const Kit &kit, const QString &text, int wrap, int max_lines,
		bool bold, bool center);
	int text_width(const Kit &kit, const QString &text, bool bold);
	int text_height(const Kit &kit, const QString &text, int wrap, bool bold);
	TextRect caret_rect(const Kit &kit, const QString &text, int index,
		TextAffinity affinity, bool bold);
	TextHit hit_test(
		const Kit &kit, const QString &text, float x, float y, bool bold);
	std::vector<TextRect> range_rects(
		const Kit &kit, const QString &text, int start, int length, bool bold);
};

struct Widget {
	Page *page_ = nullptr;
	Rect r;
	bool visible = true;
	bool layout_visible = true;
	bool hittable = false;
	// The parent distributes spare space along its packing axis.
	bool grow = false;

	Widget *parent_ = nullptr;
	// A toolbar also measures items temporarily parented to its overflow.
	Widget *measure_owner_ = nullptr;
	struct Measurement {
		int max_w = 0, max_h = 0;
		Size size;
	};
	std::vector<Measurement> measurements_;
	uint64_t measure_epoch_ = 0;
	mutable TextCache text_cache_;
	bool arrange_dirty_ = true;
	uint64_t arrange_epoch_ = 0;
	Rect allocation_{};
	Rect arranged_{};

	// After changing public sizing fields, invalidate the widget. Construction
	// needs no invalidation; adding/removing children does it automatically.
	virtual void invalidate_measure();
	// For placement alone, such as scrolling or changing alignment.
	void invalidate_arrange();
	void arrange(Kit &kit, Rect alloc);
	void set_visible(bool value);
	Size measure(Kit &kit, int max_w, int max_h);

	virtual ~Widget() = default;
	// Content updates precede layout; placed() sees the resulting geometry.
	virtual void update(Kit &) {}
	virtual void placed(Kit &) {}
	virtual bool busy() const { return false; }
	virtual void screen_changed(const ScreenState &, bool, bool) {}
	virtual void rescale(Kit &) {}
	// At least as of now, we don't seem to need baseline measurements.
	// Returns the requested size without changing arranged geometry.
	virtual Size measure_content(Kit &kit, int max_w, int max_h) = 0;
	virtual void arrange_content(Kit &kit, Rect alloc);
	virtual void paint(Kit &kit) const;
	virtual Widget *hit_at(float x, float y);
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
	// The letter a mnemonic selects this widget by, or a null QChar for
	// none.  An accessor rather than a field: the text it indexes into goes
	// by a different name in every widget that draws one.
	[[nodiscard]] virtual QChar mnemonic_key() const { return {}; }
	virtual bool traps_focus() const { return false; }
	// The one focusable below this that stands for the whole of it in the
	// Tab order: a listing is one stop, and the arrows are what walk it.
	// Which one that is belongs to the widget -- a list answers with its
	// selection, so that tabbing away and back returns to the same row.
	// Null for everything that is no such group.
	virtual Widget *tab_stop() { return nullptr; }
	// A scrollbar is a hover effect rather than an event, so it must not
	// depend on who ends up consuming the motion.  Neither must the cursor.
	virtual Scroll *scrollbar() { return nullptr; }
	virtual Qt::CursorShape cursor() const { return Qt::ArrowCursor; }
	virtual QString tip() const { return {}; }
	virtual QString tip_key() const { return {}; }
	// Below this->r. Empty (w <= 0) means follow the pointer.
	[[nodiscard]] virtual Rect tip_anchor() const { return this->r; }
	virtual void prepare(Kit &kit);

	virtual bool press(Kit &, float x, float y, Qt::MouseButton)
	{
		return false;
	}
	virtual bool release(Kit &, float x, float y, Qt::MouseButton)
	{
		return false;
	}
	virtual bool double_click(
		Kit &, float x, float y, Qt::MouseButton, unsigned mods)
	{
		return false;
	}
	virtual bool motion(Kit &, float x, float y) { return false; }

	virtual bool scroll(Kit &, float x, float y, int delta) { return false; }
	virtual bool pan(Kit &, float x, float y, float dx, float dy)
	{
		return false;
	}
	virtual bool gesture(
		Kit &, float x, float y, float scale_factor, float angle_delta)
	{
		return false;
	}

	virtual bool key(Kit &, const Key &) { return false; }
	// An input method updated its preedit, or committed to it.
	virtual bool input_method(
		Kit &, const QString &commit, const QString &preedit, int caret)
	{
		return false;
	}
	// The other half of that channel: what the input method may ask back.
	// Returning false means this widget does not take text.
	virtual bool text_target(const Kit &, TextTarget &out) const
	{
		return false;
	}

	// Delivered after input dispatch, while the widget tree is idle.
	virtual void focus_lost(Kit &) {}
	[[nodiscard]] virtual int wake_ms() const { return -1; }

	virtual std::span<const std::unique_ptr<Widget>> children() const
	{
		return {};
	}
	Widget *child(std::size_t i) const;
	void paint_children(Kit &kit) const;
};

// The part of w that is really on screen: clipped by host and by every
// clipping ancestor, and empty when anything in the chain is hidden.  The
// hint overlay and the accessibility adapters both ask this, and the two
// must not disagree about what a user can see.
[[nodiscard]] Rect visible_rect(const Widget *w, Rect host);

struct Composite : Widget {
	std::vector<std::unique_ptr<Widget>> kids;

	Widget *add_child(std::unique_ptr<Widget> child, std::size_t at);
	// The inverse: detaches one child and hands its ownership back.
	std::unique_ptr<Widget> take_child(std::size_t at);
	// Forgets toolkit state pointing into the removed subtrees before
	// releasing their ownership.
	void erase_children(Kit &kit, std::size_t from);
	std::span<const std::unique_ptr<Widget>> children() const override
	{
		return this->kids;
	}
};

struct Button : Widget {
	Action action = Action::None;
	// The page owns the action context and outlives its bound controls.
	const Actor *actor = nullptr;
	const char *icon = nullptr;
	// An index into text, underlined when drawn; -1 for none.
	int mnemonic = -1;
	QString tip_text;
	QString tip_accel;
	QString text;
	bool enabled_ = true;
	bool active = false;
	bool bold = false;
	bool dim = false;
	bool flat = false;
	bool activate_on_press = false;
	bool focus_on_press = true;
	float pad_x = 0;
	std::function<void(Kit &)> on_click;

	Button() { this->hittable = true; }
	// Refresh enabled state and return the action's checked state.
	bool sync_action();
	void set_text(const QString &value);
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
	bool focusable() const override;
	[[nodiscard]] QChar mnemonic_key() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, const Key &ev) override;
	bool activate(Kit &kit) override;
};

struct Checkbox : Button {
	bool checked = false;
	bool wrap = false;

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	bool activate(Kit &kit) override;
};

struct Label : Widget {
	QString text;
	int mnemonic = -1;
	// Mnemonic target. Should be a weak_ptr.
	Widget *buddy = nullptr;
	float min_w = 0;
	float pad_x = 0;
	float pad_y = 0;
	bool bold = false;
	bool wrap = false;
	bool dim = false;
	Align align = Align::Start;
	Align valign = Align::Center;
	QString tip_text;
	QString tip_accel;

	void set_text(const QString &value);
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	bool activate(Kit &kit) override;
	[[nodiscard]] QChar mnemonic_key() const override;
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
};

// UTF-16 offsets that sit on a grapheme cluster: a surrogate pair or a
// combining mark is never split.  before/after walk to the neighbouring
// cluster; at_or_* stay put when already on a boundary.
int grapheme_before(const QString &text, int at);
int grapheme_after(const QString &text, int at);
int grapheme_at_or_before(const QString &text, int at);
int grapheme_at_or_after(const QString &text, int at);

// A single-line text field.  There is no selection: the caret is the whole
// of the state, and a click just places it.  Everything it can be told to do
// is therefore a caret move or a splice at the caret, the right-click menu
// included -- which is why that menu has Paste on it, and nothing else.
struct Entry : Widget {
	QString text;
	QString placeholder;
	// Uncommitted input-method text, shown at the caret but not part of text.
	QString preedit;
	int caret = 0;
	TextAffinity caret_affinity = TextAffinity::Leading;
	int preedit_caret = 0;
	float min_w = 160.f;
	float pad_x = kFramePadX;
	bool flat = false;
	std::function<void(Kit &)> on_change;
	std::function<void(Kit &)> on_cancel;
	// Return commits immediately. Focus loss commits after input dispatch,
	// when the callback can safely replace the newly focused widget tree.
	std::function<void(Kit &)> on_commit;

	// Horizontal scroll, in points, kept so that the caret stays visible.
	float scroll_ = 0;
	std::chrono::steady_clock::time_point caret_at_{};
	// Both decided in prepare, which is the only place with a Kit to ask
	// about focus; paint and wake_ms are const and just read them.
	bool focused_ = false;
	bool caret_on_ = false;
	// Built when first asked for, and owned here because a popup outlives
	// the click that opened it.
	std::unique_ptr<Menu> menu_;

	Entry() { this->hittable = this->grow = true; }
	~Entry() override;
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool focusable() const override;
	void focus_lost(Kit &kit) override;
	Qt::CursorShape cursor() const override { return Qt::IBeamCursor; }
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	// Opens the caret menu, at the pointer or at the caret.
	void context(Kit &kit, Rect anchor, bool kbd);
	bool key(Kit &kit, const Key &ev) override;
	bool input_method(Kit &kit, const QString &commit, const QString &pre,
		int pre_caret) override;
	bool text_target(const Kit &kit, TextTarget &out) const override;
	[[nodiscard]] int wake_ms() const override;

	// Every committed edit ends up in splice(): it clamps the span to whole
	// grapheme clusters, leaves the caret after what went in, tells the host
	// and then runs on_change.  replace() is that plus what a finished edit
	// owes the platform -- the preedit is over, and the input method has to
	// hear about it.  The input method itself uses splice() directly, since
	// it has a new preedit to set before saying anything.
	void splice(Kit &kit, int start, int end, const QString &with);
	void replace(Kit &kit, int start, int end, const QString &with);
	// Whole-value assignment, which leaves the caret at the end.
	void set_text(Kit &kit, const QString &next);
	void move_caret(Kit &kit, int to);
	void move_caret_to_hit(Kit &kit, TextHit hit);
	// Resets the blink, and re-scrolls to keep the caret in view.
	void touch_caret(const Kit &kit);
	// Just the scroll: arranging the field must not restart its blink.
	void rescroll(const Kit &kit);
	// Bring [start, end] into view without moving the caret.
	void reveal(const Kit &kit, int start, int end);
	[[nodiscard]] int inner_w(const Kit &kit) const;
	// The text as painted: the placeholder stands in when empty.
	[[nodiscard]] QString painted() const;
};

struct Sep : Widget {
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
};

struct Splitter : Widget {
	float min_w = 8.f;
	std::function<void(Kit &kit, float mouse_x)> on_drag;

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
	Qt::CursorShape cursor() const override { return Qt::SplitHCursor; }
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

struct Container : Composite {
	bool horizontal = false;
	Align align = Align::Start;
	float gap = 0;
	float pad_x = 0;
	float pad_y = 0;

	// Retain the packing result independently of the parent's position.
	std::vector<Size> sizes_;
	Size packed_;
	int packed_w_ = -1, packed_h_ = -1;
	uint64_t packed_epoch_ = 0;

	void invalidate_measure() override;
	bool packing_valid(const Kit &kit, int max_w, int max_h);
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
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
	Row() { this->horizontal = true; }
};

struct Column : Container {
};

// A row whose first cell is sized by the GutterColumn that owns it, so that
// its second cell starts where its peers' do.  A cell is one widget: nest a
// container when it needs to hold more.
struct GutterRow : Container {
	// Retained measurement output, in pixels, written by the owner.
	int gutter_ = 0;

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
};

// Gives every GutterRow beneath it one shared first column.  Anything else it
// holds, such as a heading or a separator, spans.
struct GutterColumn : Column {
	Size measure_content(Kit &kit, int max_w, int max_h) override;
};

// Packs sideways like a Row, but breaks onto a new line when the next child
// would not fit.  Children keep their natural widths: this is for a strip of
// toolbar items that ran out of bar, not for a menu.
struct Flow : Container {
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;

private:
	// Child allocations relative to the padded content origin.
	std::vector<Rect> cells_;
	Size wrap(Kit &kit, int inner_w);
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
	void arrange_content(Kit &kit, Rect alloc) override;
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

// Decorated single-child wrapper. Use a Column to stack children.
// Subclasses with custom layout override measure_content/arrange_content.
struct Panel : Composite {
	float pad_x = 0;
	float pad_y = 0;
	float min_w = 0;
	float min_h = 0;
	float max_h = 0;
	Fill fill = Fill::None;
	Stroke stroke = Stroke::None;
	bool busy = false;
	bool clip = false;
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	bool clips_children() const override { return this->clip; }
};

// A panel that floats above the widget tree, on Kit's popup stack.
// Menu behaviour is not here but in MenuPopup below.
struct Popup : Panel {
	Button *opener = nullptr;
	Popup *parent_popup = nullptr;
	Rect at{};

	Popup();
	void open(Kit &kit, Button *anchor);
	void open_at(Kit &kit, Rect anchor);
	void close(Kit &kit);
	// Content cleanup after removal from the stack, before settling focus.
	// This hook must not open or close popups.
	virtual void after_close(Kit &kit) {}
	virtual bool restores_focus() const { return false; }
	virtual void place(Kit &kit);
	// The half of place() that is not about x: drops the popup below its
	// anchor, flips it above when it would not fit, and lays it out.
	void place_below(Kit &kit, int x, Size size);
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
	// Drawn bold, and what Return means where nothing else wanted it.
	Button *default_button = nullptr;
	// How wide it may grow; the file chooser wants more than the others.
	float max_w = 560.f;
	// Accelerators of its own, wherever the focus is within it: under a
	// modal the window's are all off, and a chooser still wants Alt+Up.
	std::function<bool(Kit &kit, const Key &ev)> on_key;
	// Closed, and waiting for Kit to drop it at the frame boundary: the
	// footer button that did it is still running inside this very tree.
	bool retired_ = false;

	Dialog();
	void show(Kit &kit, std::unique_ptr<Widget> content, float min_w,
		std::unique_ptr<Button> default_button, std::unique_ptr<Button> cancel);
	void after_close(Kit &kit) override;
	bool key(Kit &kit, const Key &ev) override;
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

	Overflow();
	~Overflow() override;
	void open_slot(Kit &kit, ToolbarSlot &slot);
	void after_close(Kit &kit) override;
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
	std::vector<std::unique_ptr<Menu>> subs_;

	Menu();
	void build(Kit &kit, std::span<const MenuNode> nodes, const Actor &actor);
	void sync();
	MenuItem *add_item(const QString &text);
	MenuItem *add_item_with_mnemonic(const char *label);
	void add_sep();
	void clear(Kit &kit);
	void place(Kit &kit) override;
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	bool key(Kit &kit, const Key &ev) override;
};

struct MenuItem : Button {
	QString accel;
	Menu *sub = nullptr;
	bool checked = false;
	bool checkable = false;
	int label_col = 0;
	int accel_col = 0;

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
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
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
};

// The list a Combo drops.  Menu navigation applies to it unchanged; all
// that differs is where it lands, and that is the whole point of it.
struct ComboPopup : MenuPopup {
	Column *col = nullptr;
	Combo *combo = nullptr;

	ComboPopup();
	bool restores_focus() const override { return true; }
	void place(Kit &kit) override;
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
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void paint(Kit &kit) const override;
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
	Widget *add_item(std::unique_ptr<Widget> item, std::size_t at);
	// Moves everything past the split into the popup, or brings it back.
	void lend_to(Overflow &overflow);
	void reclaim();
	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;

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
	ToolbarSlot *left = nullptr;
	ToolbarSlot *mid = nullptr;
	ToolbarSlot *right = nullptr;
	Overflow *overflow = nullptr;

	Toolbar(std::unique_ptr<ToolbarSlot> left_row,
		std::unique_ptr<ToolbarSlot> mid_row,
		std::unique_ptr<ToolbarSlot> right_row);
	void sync_buttons();

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;

private:
	std::unique_ptr<Overflow> overflow_owned_;
	void place_slots(Kit &kit);
};

// Client-side decorations: shown only while Kit::csd_ is on.
struct Titlebar : Panel {
	Label *title = nullptr;
	Button *minimize = nullptr;
	Button *maximize = nullptr;
	Button *close = nullptr;
	QString text;
	float drag_x_ = 0.f;
	float drag_y_ = 0.f;
	bool drag_armed_ = false;

	Titlebar();
	void sync(Kit &kit);

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
};

// Why the host is being told about a widget.  These are the distinctions an
// accessibility adapter has to make, and no more: the kit does not know what
// the platform calls any of them, nor that anyone is listening at all.
enum class Change : uint8_t {
	// The keyboard focus moved to the widget, or away from it when null.
	Focus,
	// The widget and everything below it is about to stop existing.
	Retired,
	// After layout: enabled, checked, expanded, names, and which popups
	// are on the stack.  Null widget, because this is a sweep of what
	// the last frame committed rather than one mutation.
	State,
	// Committed text and caret of the widget, already applied.  Preedit is
	// not this: it is not the Value a client reads back.
	Text,
};

struct Kit {
	// Nested dispatch shares one boundary for focus-loss events.
	// XXX: This invention feels extremely wrong.
	struct Input {
		Kit &kit;
		explicit Input(Kit &kit);
		~Input();
	};

	int input_depth_ = 0;
	std::vector<Widget *> lost_focus_;
	using Packed = Sheet::Packed;
	struct Glyph {
		Packed rect;
		int bearing_x = 0;
		int bearing_y = 0;
	};

	float dpr_ = 1.f;
	int dpi_ = 96;  ///< Physical pixels per inch, for physical units
	bool inited_ = false;
	Sheet atlas_;
	uint32_t atlas_epoch_ = 0;
	uint64_t font_epoch_ = 0;
	uint64_t text_frame_ = 0;
	mutable TextCache text_cache_;
	mutable TextBackend text_backend_;
	Packed white_{};
	Packed glow_{};
	std::map<std::pair<std::string, int>, Packed> icons_;
	std::unordered_map<uint64_t, Glyph> glyphs_;
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
	// Touch synthesizes mouse events; gestures that only make sense for a
	// real pointer ask this before arming.
	bool touch_press_ = false;
	// Press position until scrolling starts, then the last pan position.
	float touch_x_ = 0;
	float touch_y_ = 0;
	// Suppress click activation once a pan has been consumed.
	bool touch_panned_ = false;
	// Initial hit, even if no widget accepted the press.
	Widget *touch_target_ = nullptr;
	Widget *pressed_ = nullptr;
	unsigned mods_ = 0;
	// Non-null, unique, open popups: dialogs followed by transient popups.
	// Closing an entry closes its entire tail, innermost first.
	std::vector<Popup *> popups_;
	// Dialogs are opened, not owned by whoever opens them: one stacks over
	// another, and the one underneath has to outlive the click that did it.
	std::vector<std::unique_ptr<Dialog>> dialogs_;
	std::unique_ptr<Widget> scrim_;
	int host_w_ = 0;
	int host_h_ = 0;
	Renderer *renderer_ = nullptr;
	std::function<void(std::function<void()>)> post;
	std::function<void()> request_render;
	// The focused Entry changed, or moved its caret: the platform has to
	// re-query the input method state.
	std::function<void()> input_method_changed;
	// Semantic changes, for whoever exposes this tree to the outside.  A
	// separate channel from the one above on purpose: the input method has
	// its own needs, and must not have them overwritten by a second listener.
	std::function<void(Change, Widget *)> notify;
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
	// Hand a drag off to the platform: the window owns QDrag and its nested
	// event loop, and takes the mime data with it.
	std::function<void(QMimeData *, const QImage &)> start_drag;
	std::chrono::steady_clock::time_point hover_at_{};
	std::chrono::steady_clock::time_point popup_at_{};
	float hover_x_ = 0;
	float hover_y_ = 0;
	std::unique_ptr<Panel> tooltip_panel_;
	QString tooltip_text_;
	QString tooltip_accel_;
	bool tooltip_visible_ = false;
	const Widget *tooltip_anchor_ = nullptr;  // set for keyboard-focus tips

	ScreenState screen_state_;

	Kit() = default;
	~Kit() { destroy(); }

	Kit(const Kit &) = delete;
	Kit &operator=(const Kit &) = delete;

	void init(float dpr);
	void destroy();
	void forget_tree(Widget *tree);
	void sync_focus();
	// A fresh dialog, owned here until it closes.  Filled in by whichever
	// dialog_*() builds it, and reaped once it is done.
	Dialog &new_dialog();
	// Whether anything the user has to answer is up.
	[[nodiscard]] bool modal() const;
	void open_popup(Popup &p, Popup *owner, Button *opener, Rect anchor);
	void close_popup(Popup *p, bool keyboard);
	void close_popups();
	void close_transient_popups();
	void close_above(const Popup *p);
	void relayout_popups();
	void prepare_popups();
	[[nodiscard]] bool popup_open() const;
	[[nodiscard]] Popup *top_popup() const;
	// Popups accepting input: the transient tail, or the topmost dialog.
	[[nodiscard]] std::span<Popup *const> input_popups() const;
	[[nodiscard]] bool in_input_scope(const Widget *w) const;
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
	// Focus carried over to the successor of a widget that has just been
	// rebuilt away.  The ring is whatever it already was, because nothing
	// about how the user got here has changed -- but the object that has
	// the focus has, and that much is still worth saying out loud.
	void reseat_focus(Widget *w);
	// What a mnemonic, a hint chip and a buddy label all mean by "use this":
	// the widget's default action if it has one, and the keyboard otherwise.
	bool activate(Widget *w);
	// The letter's candidates within one scope.  One is used outright;
	// several only cycle the focus between them, so a letter two controls
	// share can still reach both.
	bool activate_mnemonic(Widget *scope, int key);
	Widget *focus_scope() const;
	void cycle_focus(int dir);
	bool cycle_focus_in(Widget *scope, int dir, bool wrap);
	void focus_first(Widget *scope);
	bool key(const Key &ev);
	bool input_method(const QString &commit, const QString &preedit, int caret);
	[[nodiscard]] bool text_target(TextTarget &out) const;
	bool mouse_press(float x, float y, Qt::MouseButton button, unsigned mods);
	bool mouse_release(float x, float y, Qt::MouseButton button);
	// End the widget interaction without a click when the release is lost.
	void cancel_press();
	bool mouse_motion(float x, float y);
	// Scroll from the initial touch target when widget motion is unhandled.
	bool touch_pan(float x, float y);
	bool mouse_scroll(float x, float y, int delta);
	bool pan(float x, float y, float dx, float dy);
	// Bubble pan from a fixed target; coordinates and deltas are device pixels.
	bool pan_at(Widget *from, float x, float y, float dx, float dy);
	bool gesture(float x, float y, float scale_factor, float angle_delta);
	bool mouse_double_click(
		float x, float y, Qt::MouseButton button, unsigned mods);
	bool set_dpr(float dpr);
	bool set_host(float width_pts, float height_pts, float dpr);
	bool reset_fonts();
	[[nodiscard]] bool text_settings_changed() const;
	void bake_colours(const ScreenState &state);
	void draw_icon(int x, int y, int size, const char *name, Colour colour);
	// Coverage copies scalar alpha; other bitmaps are sRGB premultiplied.
	Packed pack_bitmap(const QImage &image, bool coverage);
	void emit_layout(float x, float y, const TextCache::Text &cached,
		Colour colour, int mnemonic);
	void emit_text(
		float x, float y, const QString &text, Colour colour, bool bold);
	void draw_glow(Rect w, Colour col);
	// An inactive window halves whatever alpha its ink already had.
	[[nodiscard]] float ink_alpha() const { return this->active_ ? 1.f : 0.5f; }
	void focus_ring(Rect w);   // 1pt inset ring
	void draw_shadow(Rect w);  // popup/tooltip drop shadow
	// Rect-shaped wrappers over the corner-based draw list.
	void draw_fill(Rect w, Colour col);
	void draw_border(Rect w, Colour col, int thickness);
	void clip_to(Rect w);
	void clip_pop();
	void tooltip(const Widget *hot);
	void hide_tooltip();
	[[nodiscard]] int wake_ms() const;
	// One frame of the widget tree: lay it out, settle what the layout may
	// have moved -- popups, focus, the hover under the pointer -- and paint.
	// Content update() runs before layout and placed() after it.
	void frame_ui(Page &ui);
	void paint();
	// Native layout metrics in device pixels. Logical extents round outward
	// when handed to widget layout; glyph bearings remain independent.
	[[nodiscard]] int text_width(const QString &text, bool bold) const;
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

	// One icon square, in device pixels: the size draw_icon() rasterises at,
	// and the size the quad that samples it is drawn at.
	[[nodiscard]] int icon_px() const { return std::max(px(kIconPts), 16); }
};

}  // namespace dn
