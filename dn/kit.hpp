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
enum class Fill : uint8_t { None, Solid, Gradient, Popup, Panel };
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

struct Kit;
struct Popup;

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
	std::function<void(float mouse_x)> on_drag;

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

	Colour well_{};
	Colour toolbar_top_{};
	Colour toolbar_bottom_{};
	Colour hover_{};
	Colour press_{};
	Colour divider_{};
	Colour busy_{};
	Colour ink_{};
	Colour frame_{};
	Colour panel_{};
	Colour hint_{};

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
