//
// wayland-window.cpp: Wayland-specific viewer window
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "wayland-window.hpp"

#include "app.hpp"
#include "wayland-color-bridge.hpp"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QExposeEvent>
#include <QKeyEvent>
#include <QPainter>
#include <QPlatformSurfaceEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QSurfaceFormat>
#include <QTimer>

#include <vulkan/vulkan.h>

#include <cstdio>

using namespace std;

namespace dn
{
WaylandWindow::WaylandWindow(App *app)
	: app_(app), backing_store_(this), content_(app, this),
	  color_bridge_(make_unique<WaylandColorBridge>())
{
	setSurfaceType(QSurface::RasterSurface);
	setTitle(QStringLiteral("dn"));
	QSurfaceFormat format = this->content_.format();
	format.setSwapInterval(0);
	this->content_.setFormat(format);
	resize(this->content_.size());
	this->content_.setGeometry(QRect(QPoint(), size()));
	this->content_.installEventFilter(this);
}

WaylandWindow::~WaylandWindow()
{
	hide();
	this->content_.removeEventFilter(this);
}

bool
WaylandWindow::initialize(const QString &path, BrowseSetup setup)
{
	create();
	this->content_.setGeometry(QRect(QPoint(), size()));
	if (!this->content_.initialize(path, setup))
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
	if (event->type() == QEvent::WindowStateChange)
		QCoreApplication::sendEvent(&this->content_, event);
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
	this->content_.setGeometry(QRect(QPoint(), event->size()));
	requestUpdate();
}

void
WaylandWindow::showEvent(QShowEvent *event)
{
	QWindow::showEvent(event);
	this->content_.show();
	requestUpdate();
}

}  // namespace dn
