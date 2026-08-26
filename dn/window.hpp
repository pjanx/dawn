//
// window.hpp: colour-managed image viewer window (shell)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "browser.hpp"
#include "chrome.hpp"
#include "kit.hpp"
#include "renderer.hpp"
#include "viewer.hpp"

#include <QString>
#include <QTimer>
#include <QUrl>
#include <QWindow>

#include <libdn.h>

#include <memory>
#include <string>

class QCloseEvent;
class QExposeEvent;
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
	enum class Mode : uint8_t { View, Browser };

	bool refresh_screen_profile(QScreen *target_screen);
	void request_render();
	void arm_ui_wake();
	void render();
	void handle_screen_change(QScreen *target_screen);
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
	[[nodiscard]] Extent pixel_size() const;
	[[nodiscard]] QWindow *shell();

	App *app_ = nullptr;
	Renderer renderer_;
	Kit kit_;
	HostActions host_;
	std::unique_ptr<Page> browser_ui_;
	std::unique_ptr<Page> viewer_ui_;
	Browser *browser_ = nullptr;
	Viewer *viewer_ = nullptr;
	Mode mode_ = Mode::View;
	VkSurfaceKHR surface_ = VK_NULL_HANDLE;
	std::shared_ptr<dn::Cmm> cmm_;
	std::shared_ptr<dn::Profile> screen_profile_;
	bool renderer_ready_ = false;
	bool exposed_ = false;
	bool resize_pending_ = false;
	bool update_pending_ = false;
	bool screen_profile_fallback_ = true;
	bool awaiting_view_ = false;
	QTimer ui_wake_;
	QTimer present_retry_;
	bool pinch_active_ = false;
	float pinch_last_zoom_ = 0;
	float pinch_last_rot_ = 0;
	bool touch_pinch_ = false;
	int touch_id0_ = -1;
	int touch_id1_ = -1;
	float touch_x0_ = 0;
	float touch_y0_ = 0;
	float touch_x1_ = 0;
	float touch_y1_ = 0;
	bool alt_armed_ = false;
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
	explicit Window(App *app, QWindow *parent = nullptr);
	~Window() override;

	bool initialize(
		const QUrl &url, BrowseSetup setup = {}, bool browse = false);
	void shutdown();
	void open_any(const QUrl &url, bool browse = false);
	[[nodiscard]] QUrl current_url() const;
	[[nodiscard]] HostActions &host() { return this->host_; }
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
