//
// window.hpp: colour-managed image viewer window (shell)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "display-profile.hpp"
#include "kit-browser.hpp"
#include "kit-chrome.hpp"
#include "kit-commander.hpp"
#include "kit-crop-jpeg.hpp"
#include "kit-viewer.hpp"
#include "kit.hpp"
#include "renderer.hpp"

#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWindow>

#include <libdn/libdn.hpp>

#include <memory>
#include <span>
#include <string>

class QAccessibleInterface;
class QCloseEvent;
class QExposeEvent;
class QInputMethodEvent;
class QInputMethodQueryEvent;
class QKeyEvent;
class QMouseEvent;
class QNativeGestureEvent;
class QObject;
class QResizeEvent;
class QScreen;
class QTouchEvent;
class QWheelEvent;

namespace dn
{

class App;

class Window final : public QWindow
{

	bool refresh_screen_profile(QScreen *target_screen);
	void apply_screen_profile(QScreen *target_screen, bool force_reload);
	void request_render();
	void arm_ui_wake();
	void render();
	void focus_gained();
	void focus_lost();
	void begin_close();
	void show_browser(bool select);
	void go_back();
	void go_forward();
	void toggle_fullscreen();
	void apply_dark(bool dark);
	void toggle_dark();
	void apply_window(Action a);
	void open_viewer(const QUrl &url);
	void sync_viewer_preloads();
	void cancel_viewer_loads();
	void launch_exiftool(const QUrl &url);
	void trash_url(const QUrl &url);
	void show_viewer_error(const QString &message);
	void show_help();
	int viewer_file_index(const QUrl &url) const;
	void open_sibling(int delta);
	Page *active_ui();
	const Page *active_ui() const;
	void drop_frames();
	void bind_host();
	void set_mode(Mode m);
	void sync_title();
	void sync_csd();
	bool handle_native_gesture(QNativeGestureEvent *event);
	bool handle_touch(QTouchEvent *event);
	bool handle_input_method(QInputMethodEvent *event);
	bool handle_input_method_query(QInputMethodQueryEvent *event);
	[[nodiscard]] QVariant input_method_value(
		const TextTarget &target, Qt::InputMethodQuery q) const;
	void sync_input_method();
	[[nodiscard]] Extent pixel_size() const;

	App *app_ = nullptr;
	Renderer renderer_;
	Kit kit_;
	HostActions host_;
	std::unique_ptr<Page> pages_[size_t(Mode::Count)];
	Cropper *cropper_ = nullptr;
	Commander *commander_ = nullptr;
	Browser *browser_ = nullptr;
	Viewer *viewer_ = nullptr;
	Mode mode_ = Mode::View;
	ScreenState screen_state_;
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	std::shared_ptr<dawn::Cmm> cmm_;
	std::shared_ptr<dawn::Profile> screen_profile_;
	bool renderer_ready_ = false;
	bool exposed_ = false;
	bool resize_pending_ = false;
	bool settings_apply_pending_ = false;
	bool font_change_pending_ = false;
	bool update_pending_ = false;
	bool screen_profile_fallback_ = true;
	// What the screen profile last saw of Windows Advanced Color, against
	// which activation tells a brightness change from a mode change.
	AdvancedColor advanced_color_;
	// macOS screen-parameter notifications, for as long as this lives.
	std::shared_ptr<void> screen_parameters_;
	bool awaiting_view_ = false;
	QTimer ui_wake_;
	QTimer present_retry_;
	bool pinch_active_ = false;
	float pinch_last_zoom_ = 0;
	float pinch_last_rot_ = 0;
	bool touch_pinch_ = false;
	// Keep the touch sequence accepted after a pinch, until all fingers lift.
	bool touch_multi_ = false;
	int touch_id0_ = -1;
	int touch_id1_ = -1;
	float touch_x0_ = 0;
	float touch_y0_ = 0;
	float touch_x1_ = 0;
	float touch_y1_ = 0;
	bool alt_armed_ = false;
	bool ime_sync_pending_ = false;
	bool csd_ = false;
	bool system_grab_ = false;
	bool fullscreen_from_maximized_ = false;
	Qt::CursorShape cursor_applied_ = Qt::ArrowCursor;

protected:
	bool event(QEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void exposeEvent(QExposeEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void wheelEvent(QWheelEvent *event) override;

public:
	explicit Window(App *app, QWindow *parent);
	~Window() override;

	bool initialize(const QUrl &url, BrowseSetup setup, Mode mode);
	void shutdown();
	void handle_screen_change(QScreen *target_screen);
	void refresh_headroom();
	void set_headroom(float headroom);
	void open_any(const QUrl &url);
	void reveal_file(const QUrl &url);
	Mode application() const { return application_mode(mode_); }
	[[nodiscard]] QUrl current_url() const;
	[[nodiscard]] HostActions &host() { return this->host_; }
	// What the accessibility adapters need of a window, and no more: where
	// the semantic root lives, and the widget tree hanging off it.
	[[nodiscard]] QWindow *shell();
	// Qt's application interface lists top-level windows through this, which
	// QWindow itself leaves null.  The factory decides shell vs client.
	QAccessibleInterface *accessibleRoot() const override;
	[[nodiscard]] Kit &kit() { return this->kit_; }
	[[nodiscard]] Page *active_page() { return active_ui(); }
	[[nodiscard]] const Actor *active_actor() const;
	[[nodiscard]] std::span<const MenuNode> active_menu() const;
	[[nodiscard]] bool renderer_ready() const { return this->renderer_ready_; }
	[[nodiscard]] VkColorSpaceKHR color_space() const
	{
		return this->renderer_.color_space();
	}
};

}  // namespace dn
