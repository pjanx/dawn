//
// wayland-window.hpp: Wayland-specific viewer window
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "window.hpp"

#include <QBackingStore>
#include <QWindow>

#include <memory>

class QCloseEvent;
class QExposeEvent;
class QKeyEvent;
class QResizeEvent;
class QShowEvent;
namespace dn
{

class App;
class WaylandColorBridge;

class WaylandWindow final : public QWindow
{
	enum class CloseState { Open, WaitingForLeave, ReadyToClose };

	void render_background();
	void begin_close();
	void finish_close();
	void attach_color_management(bool report_fallback);
	void place_content();
	[[nodiscard]] QRect content_geometry() const;

	App *app_ = nullptr;
	QBackingStore backing_store_;
	dn::Window content_;
	std::unique_ptr<WaylandColorBridge> color_bridge_;
	CloseState close_state_ = CloseState::Open;
	bool initialized_ = false;

protected:
	bool event(QEvent *event) override;
	bool eventFilter(QObject *watched, QEvent *event) override;
	void closeEvent(QCloseEvent *event) override;
	void exposeEvent(QExposeEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void keyReleaseEvent(QKeyEvent *event) override;
	void resizeEvent(QResizeEvent *event) override;
	void showEvent(QShowEvent *event) override;

public:
	explicit WaylandWindow(App *app);
	~WaylandWindow() override;

	bool initialize(const QString &path, BrowseSetup setup = {});
};

// What Qt's own CSD does on titlebar right-click, in shell-local coordinates.
void wayland_show_window_menu(QWindow *shell, int x, int y);

}  // namespace dn
