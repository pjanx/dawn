//
// kit.hpp: Vulkan overlay kit and atlas engine
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "action.hpp"
#include "overlay.hpp"
#include "sheet.hpp"

#include <libdn.h>

#include <QFont>
#include <QImage>
#include <QRawFont>
#include <QString>
#include <Qt>

#include <chrono>
#include <cstddef>
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

struct Rect {
	float x = 0;
	float y = 0;
	float w = 0;
	float h = 0;
	[[nodiscard]] bool contains(float px, float py) const
	{
		return px >= this->x && py >= this->y && px < this->x + this->w &&
			py < this->y + this->h;
	}
	[[nodiscard]] Rect inset(float px, float py) const;
};

enum class Align : uint8_t { Start, Center, End };
enum class Fill : uint8_t { None, Toolbar, Tooltip, Panel };
enum class Stroke : uint8_t { None, All, Bottom };

constexpr float kUnlim = 1.0e8f;
constexpr float kIconPx = 16.0f;
constexpr float kFramePadX = 6.0f;
constexpr float kFramePadY = 6.0f;
constexpr float kTooltipPadX = 8.0f;
constexpr float kGlowPts = 8.0f;
constexpr float kResizeBorderPts = 8.0f;
constexpr float kScrollBarW = 8.0f;
constexpr float kScrollStep = 32.0f;
constexpr float kScrollHideMs = 1000.0f;

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
	ColourPanel,
	ColourHint,
	ColourCount,
};

struct Kit;
struct Scroll;

// --- Kit ---------------------------------------------------------------------

struct Widget {
	Rect r;
	bool visible = true;
	bool layout_visible = true;
	bool hittable = false;

	Widget *parent_ = nullptr;

	virtual ~Widget() = default;
	virtual void measure(Kit &kit, float max_w, float max_h) = 0;
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
	virtual bool traps_focus() const { return false; }
	virtual float min_width() const { return 0.0f; }
	// A scrollbar is a hover effect rather than an event, so it must not
	// depend on who ends up consuming the motion.
	virtual Scroll *scrollbar() { return nullptr; }
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
	virtual bool key(Kit &kit, int key, unsigned mods);
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
	QString tip_text;
	QString tip_accel;
	QString text;
	bool enabled_ = true;
	bool active = false;
	bool dim = false;
	bool activate_on_press = false;
	float pad_x = 0;
	std::function<void(Kit &)> on_click;

	Button() { this->hittable = true; }
	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
	bool focusable() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	virtual bool activate(Kit &kit);
};

struct Label : Widget {
	QString text;
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

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	bool grows() const override { return this->grow; }
	QString tip() const override { return this->tip_text; }
	QString tip_key() const override { return this->tip_accel; }
};

struct Sep : Widget {
	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
};

struct Splitter : Widget {
	float min_w = 8.0f;
	std::function<void(Kit &kit, float mouse_x)> on_drag;

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	float min_width() const override { return this->min_w; }
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

	void measure_pack(Kit &kit, float max_w, float max_h, bool hz);
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

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
};

struct Column : Container {
	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
};

struct Scroll {
	float offset = 0;
	float content = 0;
	float view = 0;
	bool dragging = false;

	[[nodiscard]] float max_offset() const;
	void clamp();
	void set_metrics(float content_h, float view_h);
	void reveal();
	[[nodiscard]] bool visible() const;
	[[nodiscard]] int wake_ms() const;
	[[nodiscard]] Rect bar_rect(Rect viewport) const;
	[[nodiscard]] Rect thumb_rect(Rect viewport) const;
	bool wheel(int delta, float step);
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
	bool key(Kit &kit, int key, unsigned mods) override;
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
	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	bool grows() const override { return this->grow; }
	bool clips_children() const override { return this->clip; }
	float min_width() const override { return this->min_w; }
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
	void place_sub(Kit &kit);
	bool traps_focus() const override { return true; }
	virtual bool captures_keys() const { return false; }
	void paint(Kit &kit) const override;
	bool key(Kit &kit, int key, unsigned mods) override;
};

// Dismissed by Escape or its Close button only; the caller fills the body
// with whatever the case needs, see dialog_about() and dialog_shortcuts().
struct Dialog : Popup {
	Panel *frame = nullptr;
	ScrollColumn *body = nullptr;

	Dialog();
	void show(Kit &kit, std::unique_ptr<Widget> content, float min_w);
	void close(Kit &kit) override;
	void place(Kit &kit) override;
	void paint(Kit &kit) const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
};

struct MenuItem;

// A popup navigated like a menu: hovering moves the focus, arrows walk the
// items, and a click anywhere else in the stack dismisses it.
struct MenuPopup : Popup {
	void focus_item(Kit &kit, Widget *w, bool kbd) const;
	void reveal(Kit &kit, Widget *w);
	bool motion(Kit &kit, float x, float y) override;
	bool key(Kit &kit, int key, unsigned mods) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
};

// What a ToolbarSlot puts the items it could not fit into.
struct Overflow : MenuPopup {
	Column *col = nullptr;
	std::vector<Widget *> sources;
	std::function<void()> fill_items;

	Overflow();
	void place(Kit &kit) override;
};

struct Menu : MenuPopup {
	Column *col = nullptr;
	Actor actor;
	std::vector<std::unique_ptr<Menu>> subs_;

	Menu();
	void build(std::span<const MenuNode> nodes, const Actor &actor);
	void sync();
	MenuItem &add_item(const QString &text);
	MenuItem &add_item_with_mnemonic(const QString &text);
	void add_sep();
	void clear();
	void measure(Kit &kit, float max_w, float max_h) override;
	bool key(Kit &kit, int key, unsigned mods) override;
};

struct MenuItem : Button {
	QString accel;
	// FIXME: This is an index into a label, it should at least be a unichar.
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

// One end of a toolbar. What does not fit goes behind the "more" button.
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
	ToolbarSlot *left = nullptr;
	ToolbarSlot *mid = nullptr;
	ToolbarSlot *right = nullptr;
	Overflow *overflow = nullptr;

	Toolbar(std::unique_ptr<ToolbarSlot> left_row,
		std::unique_ptr<ToolbarSlot> mid_row,
		std::unique_ptr<ToolbarSlot> right_row);
	void sync_buttons();

	void measure(Kit &kit, float max_w, float max_h) override;
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
	float drag_x_ = 0.0f;
	float drag_y_ = 0.0f;
	bool drag_armed_ = false;

	Titlebar();
	void sync(Kit &kit);

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
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
		float bearing_x = 0;
		float bearing_y = 0;
	};

	float dpr_ = 1.0f;
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
	float mouse_x_ = -1.0f;
	float mouse_y_ = -1.0f;
	bool left_down_ = false;
	Widget *pressed_ = nullptr;
	unsigned mods_ = 0;
	std::vector<Popup *> popups_;
	std::unique_ptr<Widget> scrim_;
	float host_w_ = 0;
	float host_h_ = 0;
	Renderer *renderer_ = nullptr;
	std::function<void(std::function<void()>)> post;
	std::function<void()> request_render;
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

	Widget *focus_scope() const;
	void cycle_focus(int dir);
	bool cycle_focus(Widget *scope, int dir, bool wrap = true);
	void focus_first(Widget *scope);
	bool key(int key, unsigned mods);
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
	void bake_colours(Cmm *cmm, Profile *target);
	void pack_icon(const char *name, int px);
	void draw_icon(
		float x, float y, float size, const char *name, Colour colour);
	Packed pack_bitmap(const QImage &image);
	void cache_text(const QString &text, bool bold);
	void emit_text(
		float x, float y, const QString &text, Colour colour, bool bold);
	void draw_glow(float ix, float iy, float iw, float ih, Colour col);
	// An inactive window halves whatever alpha its ink already had.
	[[nodiscard]] float ink_alpha() const
	{
		return this->active_ ? 1.0f : 0.5f;
	}
	void focus_ring(Rect w);   // 1px inset ring
	void draw_shadow(Rect w);  // popup/tooltip drop shadow
	void tooltip(const Widget *hot);
	[[nodiscard]] int wake_ms() const;
	void paint();
	[[nodiscard]] float text_width(const QString &text, bool bold) const;
	[[nodiscard]] float text_ascent(bool bold) const;
	[[nodiscard]] QString elide_lines(
		const QString &text, float wrap_pts, int max_lines, bool bold) const;
	[[nodiscard]] float text_height(
		const QString &text, float wrap_pts, bool bold) const;
	[[nodiscard]] float snap(float v) const;
	[[nodiscard]] float snap_size(float v) const;
	[[nodiscard]] Rect snap_rect(Rect r) const;
	// Raw atlas bytes: 16-bit RGBA UNORM, 8 bytes/pixel, row-major.
	[[nodiscard]] bool font_pixels(
		unsigned char **out_pixels, int *width, int *height) const;
	[[nodiscard]] bool take_atlas_dirty();
};

}  // namespace dn
