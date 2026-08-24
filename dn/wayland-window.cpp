//
// wayland-window.cpp: Wayland-specific viewer window
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "wayland-window.hpp"

#include "app.hpp"
#include "wayland-color-bridge.hpp"
#include "xdg-shell-client-protocol.h"

#include <QByteArray>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QExposeEvent>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>
#include <QtGui/qguiapplication_platform.h>

#include <vulkan/vulkan.h>

#include <cmath>
#include <cstdio>

using namespace std;

namespace dn
{
namespace
{

// QGuiApplication::platformNativeInterface() is public, its class is QPA.
// Mirror enough of QPlatformNativeInterface to reach its fourth virtual,
// nativeResourceForWindow(); Qt's xdg-shell exports the toplevel there.
struct NativeResources : QObject {
	virtual void *integration(const QByteArray &) = 0;
	virtual void *context(const QByteArray &, void *) = 0;
	virtual void *screen(const QByteArray &, void *) = 0;
	virtual void *window(const QByteArray &, QWindow *) = 0;
};

}  // namespace

void
wayland_show_window_menu(QWindow *shell, int x, int y)
{
	auto *iface = (NativeResources *) (void *)
		QGuiApplication::platformNativeInterface();
	auto *native =
		qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
	if (!iface || !native)
		return;
	auto *toplevel = (xdg_toplevel *) iface->window(
		QByteArrayLiteral("xdg_toplevel"), shell);
	if (!toplevel)
		return;
	xdg_toplevel_show_window_menu(
		toplevel, native->lastInputSeat(), native->lastInputSerial(), x, y);
}

WaylandWindow::WaylandWindow(App *app)
	: app_(app), backing_store_(this), content_(app, this),
	  color_bridge_(make_unique<WaylandColorBridge>())
{
	setSurfaceType(QSurface::RasterSurface);
	setTitle(QStringLiteral("dn"));
	if (app && app->needs_csd()) {
		setFlag(Qt::FramelessWindowHint);
		// Qt Wayland treats alphaBufferSize<=0 as opaque and stamps
		// wl_surface.set_opaque_region over the whole child, including
		// glow that hangs outside the shell. Keep the child non-opaque.
		QSurfaceFormat child = this->content_.format();
		child.setAlphaBufferSize(8);
		this->content_.setFormat(child);
	}
	QSurfaceFormat format = this->content_.format();
	format.setSwapInterval(0);
	this->content_.setFormat(format);
	resize(this->content_.size());
	place_content();
	this->content_.installEventFilter(this);
}

WaylandWindow::~WaylandWindow()
{
	hide();
	this->content_.removeEventFilter(this);
}

bool
WaylandWindow::initialize(
	const QString &path, BrowseSetup setup, bool browse)
{
	create();
	place_content();
	if (!this->content_.initialize(path, setup, browse))
		return false;
	this->initialized_ = true;
	attach_color_management(true);
	return true;
}

void
WaylandWindow::render_background()
{
	if (!isExposed() || size().isEmpty())
		return;
	if (this->backing_store_.size() != size())
		this->backing_store_.resize(size());

	const QRect rect(QPoint(), size());
	this->backing_store_.beginPaint(rect);
	{
		QPainter painter(this->backing_store_.paintDevice());
		painter.fillRect(rect, Qt::black);
	}
	this->backing_store_.endPaint();
	this->backing_store_.flush(rect);
}

void
WaylandWindow::begin_close()
{
	if (this->close_state_ != CloseState::Open)
		return;
	// Keep the top-level wl_surface alive until Qt receives text-input leave.
	this->close_state_ = CloseState::WaitingForLeave;
	hide();
	if (!isActive())
		finish_close();
}

void
WaylandWindow::finish_close()
{
	if (this->close_state_ != CloseState::WaitingForLeave)
		return;
	this->close_state_ = CloseState::ReadyToClose;
	QTimer::singleShot(0, this, [this] {
		if (this->close_state_ != CloseState::ReadyToClose)
			return;
		// Accepting the second close destroys both platform surfaces. The shell
		// is already hidden, so Qt will not emit lastWindowClosed for it.
		close();
		this->app_->close_later(this);
	});
}

bool
WaylandWindow::event(QEvent *event)
{
	if (event->type() == QEvent::FocusOut)
		finish_close();
	if (event->type() == QEvent::WindowStateChange) {
		place_content();
		QCoreApplication::sendEvent(&this->content_, event);
	}
	if (event->type() == QEvent::UpdateRequest) {
		render_background();
		return true;
	}
	if (event->type() == QEvent::DevicePixelRatioChange)
		requestUpdate();
	if (event->type() == QEvent::DragEnter ||
		event->type() == QEvent::DragMove || event->type() == QEvent::Drop)
		return QCoreApplication::sendEvent(&this->content_, event);
	return QWindow::event(event);
}

bool
WaylandWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != &this->content_)
		return QWindow::eventFilter(watched, event);
	if (event->type() == QEvent::FocusOut)
		finish_close();
	if (event->type() == QEvent::PlatformSurface) {
		auto *surface_event = static_cast<QPlatformSurfaceEvent *>(event);
		if (surface_event->surfaceEventType() ==
			QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
			this->color_bridge_->detach();
		} else if (this->initialized_) {
			QTimer::singleShot(
				0, this, [this] { attach_color_management(false); });
		}
	}
	return QWindow::eventFilter(watched, event);
}

void
WaylandWindow::attach_color_management(bool report_fallback)
{
	if (!this->content_.renderer_ready()) {
		this->color_bridge_->detach();
		return;
	}
	if (this->content_.color_space() == VK_COLOR_SPACE_PASS_THROUGH_EXT) {
		this->color_bridge_->attach(&this->content_);
	} else {
		this->color_bridge_->detach();
		if (report_fallback) {
			fprintf(stderr,
				"Wayland CM identity unavailable: Vulkan PASS_THROUGH color "
				"space not exposed; using compositor-managed sRGB\n");
		}
	}
}

void
WaylandWindow::closeEvent(QCloseEvent *event)
{
	if (this->close_state_ == CloseState::ReadyToClose) {
		event->accept();
		return;
	}
	event->ignore();
	begin_close();
}

void
WaylandWindow::exposeEvent(QExposeEvent *)
{
	if (isExposed())
		render_background();
}

void
WaylandWindow::keyPressEvent(QKeyEvent *event)
{
	QCoreApplication::sendEvent(&this->content_, event);
}

void
WaylandWindow::keyReleaseEvent(QKeyEvent *event)
{
	QCoreApplication::sendEvent(&this->content_, event);
}

void
WaylandWindow::resizeEvent(QResizeEvent *event)
{
	this->backing_store_.resize(event->size());
	place_content();
	requestUpdate();
}

QRect
WaylandWindow::content_geometry() const
{
	if ((flags() & Qt::FramelessWindowHint) &&
		!(windowState() & (Qt::WindowMaximized | Qt::WindowFullScreen))) {
		const int glow = int(lround(double(kGlowPts)));
		return {-glow, -glow, width() + 2 * glow, height() + 2 * glow};
	}
	return {QPoint(), size()};
}

void
WaylandWindow::place_content()
{
	const QRect g = content_geometry();
	if (this->content_.geometry() != g)
		this->content_.setGeometry(g);
}

void
WaylandWindow::showEvent(QShowEvent *event)
{
	QWindow::showEvent(event);
	this->content_.show();
	requestUpdate();
}

}  // namespace dn
