//
// window.cpp: colour-managed image viewer window (shell)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "window.hpp"

#include "action.hpp"
#include "app-menu-macos.hpp"
#include "app.hpp"
#include "dawn-config.h"
#include "display-profile.hpp"
#include "url.hpp"
#include "window-appearance-macos.hpp"

#if DN_WITH_WAYLAND
#include "wayland-window.hpp"
#endif

#include <QByteArray>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QDesktopServices>
#include <QDir>
#include <QDropEvent>
#include <QEvent>
#include <QEventPoint>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QInputDevice>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPlatformSurfaceEvent>
#include <QPointer>
#include <QProcess>
#include <QRegion>
#include <QScreen>
#include <QStyleHints>
#include <QTemporaryFile>
#include <QTimer>
#include <QTouchEvent>
#include <QUrl>
#include <QVulkanInstance>
#include <QWheelEvent>
#include <QWindowStateChangeEvent>
#include <QtLogging>

#include <algorithm>
#include <cmath>
#include <functional>
#include <numbers>

using namespace std;

namespace dn
{
namespace
{

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 800;

int
sooner(int a, int b)
{
	if (a < 0)
		return b;
	return b < 0 ? a : min(a, b);
}

float
host_dpr(const QWindow &w)
{
	const qreal r = w.devicePixelRatio();
	return float(r > 0 ? r : 1.0);
}

int
wheel_axis(const QPoint &ang, const QPoint &pix, bool horizontal)
{
	const int a = horizontal ? ang.x() : ang.y();
	if (a)
		return a;
	return horizontal ? pix.x() : pix.y();
}

QUrl
first_dropped_file(const QMimeData *mime)
{
	if (!mime)
		return {};
	for (const QUrl &url : mime->urls()) {
		const QFileInfo info(url_to_path(url));
		if (info.exists() && info.isFile())
			return QUrl::fromLocalFile(info.absoluteFilePath());
	}
	return {};
}

QString
help_document_path()
{
	const QString app_dir = QCoreApplication::applicationDirPath();
#if defined Q_OS_WIN
	return QDir::cleanPath(QDir(app_dir).absoluteFilePath(
		QStringLiteral("share/doc/dawn/dn.html")));
#elif defined Q_OS_MACOS
	return QDir::cleanPath(QDir(app_dir).absoluteFilePath(
		QStringLiteral("../Resources/share/doc/dawn/dn.html")));
#else
	return QDir::cleanPath(QDir(app_dir).absoluteFilePath(
		QStringLiteral("../share/doc/dawn/dn.html")));
#endif
}

}  // namespace

Window::Window(App *app, QWindow *parent) : QWindow(parent), app_(app)
{
	setSurfaceType(QSurface::VulkanSurface);
	setVulkanInstance(&this->app_->vulkan_instance);
	setTitle(QStringLiteral(DAWN_NAME));
	resize(kWindowWidth, kWindowHeight);
	connect(this, &QWindow::screenChanged, this,
		[this](QScreen *new_screen) { handle_screen_change(new_screen); });
	this->ui_wake_.setSingleShot(true);
	connect(
		&this->ui_wake_, &QTimer::timeout, this, [this] { request_render(); });
	this->present_retry_.setSingleShot(true);
	connect(&this->present_retry_, &QTimer::timeout, this,
		[this] { request_render(); });
	if (QGuiApplication *app = qGuiApp)
		app->installEventFilter(this);
	auto post = [this](function<void()> fn) {
		QMetaObject::invokeMethod(
			this, [fn = std::move(fn)]() { fn(); }, Qt::QueuedConnection);
	};
	this->kit_.post = std::move(post);
	this->kit_.request_render = [this] { request_render(); };
	this->kit_.start_move = [this] {
		this->system_grab_ = shell()->startSystemMove();
		request_render();
	};
	this->kit_.start_resize = [this](Qt::Edges edges) {
		this->system_grab_ = shell()->startSystemResize(edges);
		request_render();
	};
#if DN_WITH_WAYLAND
	this->kit_.start_menu = [this](float x, float y) {
		// Wants shell-local coordinates; we may hang off it by the glow.
		const QPoint p = position() + QPoint(int(x), int(y));
		wayland_show_window_menu(shell(), p.x(), p.y());
	};
#endif
	this->csd_ = this->app_ && this->app_->needs_csd;
	if (qGuiApp) {
		auto hints = qGuiApp->styleHints();
		this->kit_.dark_ = hints->colorScheme() == Qt::ColorScheme::Dark;
	}
	bind_host();
}

Window::~Window()
{
	if (this->app_)
		this->app_->display_profiles.unlisten(this);
	if (QGuiApplication *app = qGuiApp)
		app->removeEventFilter(this);
	shutdown();
}

Extent
Window::pixel_size() const
{
	const float ratio = host_dpr(*this);
	return {uint32_t(max(0L, lround(double(width()) * double(ratio)))),
		uint32_t(max(0L, lround(double(height()) * double(ratio))))};
}

bool
Window::initialize(const QUrl &url, BrowseSetup setup, bool browse)
{
	QVulkanInstance *const instance = &this->app_->vulkan_instance;
	create();
#ifdef Q_OS_MACOS
	sync_macos_window_appearance(this, this->kit_.dark_);
#endif
	this->surface_ = QVulkanInstance::surfaceForWindow(this);
	if (!this->surface_) {
		qWarning("Qt failed to create a Vulkan window surface");
		return false;
	}
	if (!this->app_->gpu.device()) {
		if (!this->app_->gpu.init(instance->vkInstance(), this->surface_,
				[this, instance](VkPhysicalDevice physical, uint32_t family) {
					return instance->supportsPresent(physical, family, this);
				}))
			return false;
	} else if (!this->app_->gpu.supports_present(this->surface_)) {
		qWarning("chosen GPU cannot present to this window surface");
		return false;
	}
	if (!this->app_->thumbnailer.init(this->app_->gpu))
		return false;
	// A parent currently identifies the Vulkan subsurface owned by
	// WaylandWindow. Prefer MAILBOX there because Mesa's legacy Wayland FIFO
	// path can wait inside vkQueuePresentKHR while the workspace is hidden.
	// TODO: Pass an explicit presentation policy from WaylandWindow instead of
	// using parenthood as this platform/role proxy.
	this->renderer_.set_prefer_premultiplied(this->csd_);
	if (!this->renderer_.init(
			this->app_->gpu, this->surface_, pixel_size(),
			parent() ? VK_PRESENT_MODE_MAILBOX_KHR : VK_PRESENT_MODE_FIFO_KHR,
			[this, instance] { instance->presentAboutToBeQueued(this); },
			[this, instance] { instance->presentQueued(this); }))
		return false;
	this->renderer_ready_ = true;

	this->cmm_ = dn::Cmm::get_default();
	refresh_screen_profile(screen());
	this->app_->display_profiles.listen(
		this, [this] { handle_screen_change(screen()); });
	this->kit_.init(host_dpr(*this));
	this->kit_.renderer_ = &this->renderer_;
	this->browser_ui_ = make_browser_page(
		this->kit_, this->host_, this->app_->thumbnailer, &this->browser_);
	this->viewer_ui_ =
		make_viewer_page(this->kit_, this->host_, &this->viewer_);
	if (this->viewer_)
		this->viewer_->set_screen_profile(
			this->cmm_, this->screen_profile_, this->screen_profile_fallback_);
	if (this->browser_) {
		this->browser_->set_screen_profile(this->cmm_, this->screen_profile_);
		this->browser_->setup_ = setup;
	}

	open_any(url.isEmpty() ? path_to_url(QDir::currentPath()) : url, browse);
	return true;
}

void
Window::drop_frames()
{
	this->kit_.popups_.clear();
	this->kit_.root_ = nullptr;
	this->kit_.pressed_ = nullptr;
	this->kit_.hot_ = nullptr;
	this->kit_.focus_ = nullptr;
	this->kit_.default_focus_ = nullptr;
	this->kit_.focus_visible_ = false;
	this->alt_armed_ = false;
	this->browser_ = nullptr;
	this->viewer_ = nullptr;
	this->browser_ui_.reset();
	this->viewer_ui_.reset();
}

void
Window::bind_host()
{
	this->host_.apply = [this](Action a) {
		switch (a) {
		case Action::CloseWindow:
			begin_close();
			break;
		case Action::Minimize:
			shell()->showMinimized();
			break;
		case Action::Maximize:
			if (shell()->windowState() & Qt::WindowMaximized)
				shell()->showNormal();
			else
				shell()->showMaximized();
			break;
		case Action::Quit:
			if (this->app_)
				this->app_->quit();
			break;
		case Action::NewWindow:
			if (this->host_.new_window)
				this->host_.new_window({});
			break;
		case Action::Fullscreen:
			toggle_fullscreen();
			break;
		case Action::DarkMode:
			toggle_dark();
			break;
		case Action::Browse:
			show_browser(true);
			break;
		case Action::Back:
			go_back();
			break;
		case Action::Forward:
			go_forward();
			break;
		case Action::PrevFile:
			open_sibling(-1);
			break;
		case Action::NextFile:
			open_sibling(1);
			break;
		case Action::Hint:
			if (Page *ui = active_ui(); ui && ui->hint)
				ui->hint->open(this->kit_);
			break;
		case Action::Help:
			show_help();
			break;
		case Action::About:
		case Action::Shortcuts: {
			Page *ui = active_ui();
			if (!ui || !ui->dialog)
				break;
			if (a == Action::About)
				dialog_about(this->kit_, *ui->dialog);
			else
				dialog_shortcuts(
					this->kit_, *ui->dialog, ui->menu_tree, ui->keys);
			request_render();
			break;
		}
		default:
			break;
		}
	};
	this->host_.enabled = [this](Action a) {
		if (a == Action::Back) {
			if (this->mode_ == Mode::Browser && this->browser_)
				return this->browser_->hist_can_back();
			return this->browser_ && !this->browser_->dir_url_.isEmpty();
		}
		if (a == Action::Forward) {
			if (this->mode_ == Mode::Browser && this->browser_)
				return this->browser_->hist_can_forward() ||
					(this->viewer_ && this->viewer_->has_view());
			return this->browser_ && this->browser_->hist_can_forward();
		}
		return true;
	};
	this->host_.activate = [this](QUrl url) {
		if (!this->viewer_)
			return;
		if (this->browser_)
			this->browser_->hist_clear_forward();
		open_viewer(url);
		this->awaiting_view_ = true;
		request_render();
	};
	this->host_.new_window = [this](QUrl url) {
		if (url.isEmpty())
			url = current_url();
		BrowseSetup setup;
		if (this->browser_)
			setup = this->browser_->browse_setup();
		this->app_->open(url, {}, setup);
	};
	this->host_.launch_exiftool = [this](QUrl url) { launch_exiftool(url); };
	this->host_.trash = [this](QUrl url) { trash_url(url); };
}

void
Window::trash_url(const QUrl &url)
{
	const QString path = url_to_path(url);
	if (path.isEmpty() || !QFileInfo(path).isFile())
		return;
	const bool viewing = this->mode_ == Mode::View && this->viewer_ &&
		this->viewer_->url_ == url;
	if (!move_to_trash(path))
		return;
	if (this->browser_)
		this->browser_->file_gone(url);
	if (viewing) {
		if (this->browser_ && this->browser_->cursor_ >= 0 &&
			this->browser_->cursor_ < int(this->browser_->files_.size())) {
			open_viewer(this->browser_->file_url(this->browser_->cursor_));
			set_mode(Mode::View);
		} else {
			show_browser(false);
		}
	}
	request_render();
}

void
Window::show_viewer_error(const QString &message)
{
	if (!this->viewer_)
		return;
	this->viewer_->message_ = message.toStdString();
	this->viewer_->message_dismissed_ = false;
	request_render();
}

void
Window::show_help()
{
	const QString path = help_document_path();
	if (!QFile::exists(path)) {
		const QString message =
			QStringLiteral("Help document not found: ") + path;
		qWarning("%s", qUtf8Printable(message));
		show_viewer_error(message);
		return;
	}
	if (!QDesktopServices::openUrl(QUrl::fromLocalFile(path))) {
		const QString message =
			QStringLiteral("Could not open the help document: ") + path;
		qWarning("%s", qUtf8Printable(message));
		show_viewer_error(message);
	}
}

void
Window::launch_exiftool(const QUrl &url)
{
	const QString path = url_to_path(url);
	if (path.isEmpty())
		return;

	QPointer<Window> origin(this);
	auto show_error = [origin](const QString &message) {
		if (origin)
			origin->show_viewer_error(message);
	};
	// The report and process outlive the window that launched them. The guarded
	// pointer is used only to surface errors if that window still exists.
	auto *report = new QTemporaryFile(
		QDir::tempPath() + QStringLiteral("/dn-exiftool-XXXXXX.txt"), qGuiApp);
	if (!report->open()) {
		show_error(
			QStringLiteral("Could not create a temporary ExifTool report: ") +
			report->errorString());
		report->deleteLater();
		return;
	}
	const QString report_path = report->fileName();
	report->close();

	auto *process = new QProcess(qGuiApp);
	process->setProcessChannelMode(QProcess::MergedChannels);
	process->setStandardOutputFile(report_path, QIODeviceBase::Truncate);
	QStringList arguments;
#ifdef Q_OS_WIN
	const QString app_dir = QCoreApplication::applicationDirPath();
	process->setProgram(app_dir + QStringLiteral("/wperl.exe"));
	arguments.append(app_dir + QStringLiteral("/exiftool"));
#else
	process->setProgram(QStringLiteral("exiftool"));
#endif
	arguments.append(
		{QStringLiteral("-groupNames"), QStringLiteral("-duplicates"),
			QStringLiteral("-extractEmbedded"), QStringLiteral("--binary"),
			QStringLiteral("-quiet"), QStringLiteral("--"), path});
	process->setArguments(arguments);

	auto finished = make_shared<bool>(false);
	connect(process, &QProcess::errorOccurred, process,
		[process, report, finished, show_error](QProcess::ProcessError error) {
			if (error != QProcess::FailedToStart || *finished)
				return;
			*finished = true;
			show_error(QStringLiteral("Could not launch ExifTool: ") +
				process->errorString());
			report->deleteLater();
			process->deleteLater();
		});
	connect(process, &QProcess::finished, process,
		[process, report, report_path, finished, show_error](
			int exit_code, QProcess::ExitStatus exit_status) {
			if (*finished)
				return;
			*finished = true;
			QFile output(report_path);
			if (output.open(QIODeviceBase::Append | QIODeviceBase::Text)) {
				if (exit_status == QProcess::CrashExit) {
					output.write("\nExifTool terminated abnormally.\n");
				} else if (exit_code != 0) {
					output.write(
						QStringLiteral("\nExifTool exited with status %1.\n")
							.arg(exit_code)
							.toUtf8());
				} else if (output.size() == 0) {
					output.write("ExifTool produced no output.\n");
				}
				output.close();
			}
			if (!QDesktopServices::openUrl(QUrl::fromLocalFile(report_path)))
				show_error(QStringLiteral("Could not open the ExifTool report "
										  "through a .txt association."));
			process->deleteLater();
		});
	process->start();
}

void
Window::set_mode(Mode m)
{
	if (this->mode_ == m)
		return;
	this->mode_ = m;
	sync_macos_app_menu(this->app_);
	sync_title();
}

void
Window::sync_title()
{
	QUrl url;
	if (this->mode_ == Mode::View && this->viewer_ &&
		!this->viewer_->url_.isEmpty())
		url = this->viewer_->url_;
	else if (this->browser_ && !this->browser_->dir_url_.isEmpty())
		url = this->browser_->dir_url_;
	const QString app = QStringLiteral(DAWN_NAME);
	const QString title = url.isEmpty()
		? app
		: url_parse_name(url) + QStringLiteral(" \u2014 ") + app;
	QWindow *w = shell();
	if (w->title() != title)
		w->setTitle(title);
	if (this != w && this->title() != title)
		setTitle(title);
	auto set_bar = [&](Page *ui) {
		if (ui && ui->titlebar)
			ui->titlebar->text = title;
	};
	set_bar(this->browser_ui_.get());
	set_bar(this->viewer_ui_.get());
}

Page *
Window::active_ui()
{
	if (this->mode_ == Mode::Browser)
		return this->browser_ui_.get();
	return this->viewer_ui_.get();
}

const Page *
Window::active_ui() const
{
	if (this->mode_ == Mode::Browser)
		return this->browser_ui_.get();
	return this->viewer_ui_.get();
}

const Actor *
Window::active_actor() const
{
	if (const Page *p = active_ui())
		return &p->actor;
	return nullptr;
}

span<const MenuNode>
Window::active_menu() const
{
	if (const Page *p = active_ui())
		return p->menu_tree;
	return {};
}

void
Window::shutdown()
{
	this->ui_wake_.stop();
	this->renderer_ready_ = false;
	drop_frames();
	this->kit_.destroy();
	this->renderer_.destroy();
	this->surface_ = VK_NULL_HANDLE;
}

bool
Window::refresh_screen_profile(QScreen *target_screen)
{
	if (!this->cmm_)
		this->cmm_ = dn::Cmm::get_default();
	DisplayProfile discovered =
		this->app_->display_profiles.load(target_screen);
	shared_ptr<dn::Profile> next;
	string label = "sRGB (fallback)";
	string source = "srgb";
	if (!discovered.icc.empty()) {
		next = this->cmm_->get_profile(discovered.icc);
		if (next) {
			label =
				discovered.label.empty() ? discovered.source : discovered.label;
			source = discovered.source;
		}
	}
	if (!next)
		next = this->cmm_->get_profile_sRGB();
	this->screen_profile_fallback_ = source == "srgb";

	const bool changed =
		!profiles_equal(this->screen_profile_.get(), next.get());
	this->screen_profile_ = std::move(next);
	if (changed)
		qInfo("screen profile: %s", label.c_str());
	return changed;
}

QWindow *
Window::shell()
{
	if (QWindow *parent_window = parent())
		return parent_window;
	return this;
}

void
Window::sync_csd()
{
	this->kit_.sync_cursor();
	if (this->kit_.cursor_ != this->cursor_applied_) {
		this->cursor_applied_ = this->kit_.cursor_;
		setCursor(this->cursor_applied_);
	}
	const bool shadow = this->kit_.csd_shadow_;
	QRegion mask;
	if (shadow) {
		// Input still has to reach the resize band, which lies in the shadow.
		const int band = int(lround(double(kResizeBorderPts)));
		const Rect f = this->kit_.frame();
		const QRect frame(int(lround(double(f.x))), int(lround(double(f.y))),
			int(lround(double(f.w))), int(lround(double(f.h))));
		mask = QRegion(frame.adjusted(-band, -band, band, band));
	}
	setMask(mask);
	if (QWindow *sh = shell(); sh != this)
		sh->setMask(QRegion());
	const float dpr = host_dpr(*this);
	const uint32_t inset =
		shadow ? uint32_t(max(0L, lround(double(kGlowPts) * double(dpr)))) : 0;
	this->renderer_.set_dest_inset(inset);
}

void
Window::toggle_fullscreen()
{
	// windowState() collapses the mask, hiding Maximized behind FullScreen,
	// and showFullScreen() overwrites it anyway. Remember it ourselves.
	QWindow *target = shell();
	if (target->windowStates() & Qt::WindowFullScreen) {
		if (this->fullscreen_from_maximized_)
			target->showMaximized();
		else
			target->showNormal();
		this->fullscreen_from_maximized_ = false;
	} else {
		this->fullscreen_from_maximized_ =
			bool(target->windowStates() & Qt::WindowMaximized);
		target->showFullScreen();
	}
}

void
Window::apply_dark(bool dark)
{
	this->kit_.dark_ = dark;
	this->kit_.bake_colours(this->cmm_.get(), this->screen_profile_.get());
	if (this->kit_.renderer_) {
		const Colour well = this->kit_.colours_[ColourWell];
		const Colour tile = this->kit_.colours_[ColourToolbarBottom];
		this->kit_.renderer_->set_well_colour(well.r, well.g, well.b);
		this->kit_.renderer_->set_checker_colour(tile.r, tile.g, tile.b);
	}
#ifdef Q_OS_MACOS
	sync_macos_window_appearance(this, dark);
#endif
	request_render();
}

void
Window::toggle_dark()
{
	apply_dark(!this->kit_.dark_);
}

void
Window::sync_viewer_preloads()
{
	if (!this->viewer_ || !this->browser_)
		return;
	const int i = viewer_file_index(this->viewer_->url_);
	const int n = int(this->browser_->files_.size());
	if (i < 0 || n < 2) {
		this->viewer_->set_preload_urls({}, {});
		return;
	}
	this->viewer_->set_preload_urls(this->browser_->file_url((i + n - 1) % n),
		this->browser_->file_url((i + 1) % n));
}

int
Window::viewer_file_index(const QUrl &url) const
{
	if (!this->browser_ || url.isEmpty())
		return -1;
	for (int n = 0; n < int(this->browser_->files_.size()); ++n) {
		if (this->browser_->file_url(n) == url)
			return n;
	}
	return -1;
}

void
Window::open_viewer(const QUrl &url)
{
	if (!this->viewer_)
		return;
	this->viewer_->open(url);
	sync_viewer_preloads();
}

void
Window::cancel_viewer_loads()
{
	this->awaiting_view_ = false;
	if (this->viewer_)
		this->viewer_->cancel_loads();
}

void
Window::open_sibling(int delta)
{
	if (!this->viewer_ || !this->browser_ || this->browser_->files_.empty() ||
		delta == 0)
		return;
	const int i = viewer_file_index(this->viewer_->url_);
	if (i < 0)
		return;
	const int n = int(this->browser_->files_.size());
	int j = (i + delta) % n;
	if (j < 0)
		j += n;
	if (j == i)
		return;
	open_viewer(this->browser_->file_url(j));
	set_mode(Mode::View);
	request_render();
}

void
Window::request_render()
{
	if (!this->renderer_ready_ || this->update_pending_)
		return;
	this->update_pending_ = true;
	// This window presents FIFO itself. QWindow::requestUpdate asks the
	// platform frame clock for a slot we do not drive (wl_surface.frame,
	// Cocoa's display link), so the paint is withheld or invented. One
	// posted UpdateRequest is the schedule; present is the throttle.
	QCoreApplication::postEvent(this, new QEvent(QEvent::UpdateRequest));
}

void
Window::render()
{
	if (!this->renderer_ready_)
		return;
	const QWindow *surface = parent() ? parent() : this;
	if (!surface->isExposed())
		return;
	if (this->resize_pending_) {
		this->renderer_.resize(pixel_size());
		this->resize_pending_ = false;
	}
	const float w = float(width());
	const float h = float(height());
	const float dpr = host_dpr(*this);
	const bool fullscreen = bool(shell()->windowState() & Qt::WindowFullScreen);
	if (this->mode_ == Mode::Browser && this->browser_)
		this->browser_->set_host(w, h, dpr);
	else if (this->viewer_)
		this->viewer_->set_host(w, h, dpr);
	// Nothing to do for a resize: relayout_popups() re-places every popup
	// and drops the ones whose opener stopped being shown.
	Page *ui = active_ui();
	if (!ui)
		return;
	this->kit_.fullscreen_ = fullscreen;
	this->kit_.maximized_ = bool(shell()->windowState() & Qt::WindowMaximized);
	this->kit_.csd_ = this->csd_ && !fullscreen;
	this->kit_.csd_shadow_ = this->kit_.csd_ && !this->kit_.maximized_;
	this->kit_.active_ =
		this->system_grab_ || shell()->isActive() || isActive();
	sync_title();
	if (ui->titlebar)
		ui->titlebar->sync(this->kit_);
	if (ui->toolbar) {
		if (this->mode_ == Mode::Browser)
			ui->toolbar->busy = this->awaiting_view_ ||
				(this->browser_ && this->browser_->thumbs_busy());
		else
			ui->toolbar->busy = this->viewer_ && this->viewer_->opening_;
	}
	if (this->mode_ == Mode::Browser && this->browser_)
		this->browser_->present(*ui);
	else if (this->viewer_)
		this->viewer_->present(*ui);
	sync_csd();
	if (this->viewer_ && this->viewer_->consume_open_done() &&
		this->awaiting_view_) {
		this->awaiting_view_ = false;
		set_mode(Mode::View);
		request_render();
	}
	const bool deferred = this->present_retry_.isActive();
	const bool presented =
		!deferred && this->renderer_.draw_frame(this->kit_.list_.mesh());
	if (presented) {
		if (QWindow *shell = parent()) {
			QEvent commit(QEvent::UpdateRequest);
			QCoreApplication::sendEvent(shell, &commit);
		}
	} else if (!deferred) {
		// Do not block the GUI thread when a hidden surface has no available
		// swapchain image. Visible surfaces get another bounded attempt.
		this->present_retry_.start(16);
	}
	if (this->renderer_.needs_resize()) {
		this->resize_pending_ = true;
		request_render();
	}
	arm_ui_wake();
}

void
Window::arm_ui_wake()
{
	int ms = this->kit_.wake_ms();
	if (this->mode_ == Mode::View && this->viewer_)
		ms = sooner(ms, this->viewer_->wake_ms());
	if (ms >= 0)
		this->ui_wake_.start(ms);
	else
		this->ui_wake_.stop();
}

void
Window::handle_screen_change(QScreen *target_screen)
{
	this->resize_pending_ = true;
	if (this->renderer_ready_ && refresh_screen_profile(target_screen)) {
		if (this->viewer_)
			this->viewer_->set_screen_profile(this->cmm_, this->screen_profile_,
				this->screen_profile_fallback_);
		if (this->browser_)
			this->browser_->set_screen_profile(
				this->cmm_, this->screen_profile_);
	}
	request_render();
}

void
Window::begin_close()
{
	if (QWindow *parent_window = parent()) {
		parent_window->close();
		return;
	}
	this->app_->close_later(this);
}

void
Window::closeEvent(QCloseEvent *event)
{
	if (parent()) {
		QWindow::closeEvent(event);
		return;
	}
	event->ignore();
	begin_close();
}

void
Window::show_browser(bool select)
{
	if (!this->browser_ || this->browser_->dir_url_.isEmpty())
		return;
	if (this->viewer_ui_)
		this->kit_.close_popups();
	if (select && this->viewer_)
		this->browser_->select_file(this->viewer_->url_);
	cancel_viewer_loads();
	set_mode(Mode::Browser);
	request_render();
}

void
Window::go_back()
{
	if (this->mode_ == Mode::View) {
		show_browser(false);
		return;
	}
	if (this->browser_)
		this->browser_->hist_back();
}

void
Window::go_forward()
{
	if (this->browser_ && this->browser_->hist_forward()) {
		this->kit_.close_popups();
		cancel_viewer_loads();
		set_mode(Mode::Browser);
		request_render();
		return;
	}
	if (this->viewer_ && this->viewer_->has_view()) {
		this->kit_.close_popups();
		sync_viewer_preloads();
		set_mode(Mode::View);
		request_render();
	}
}

bool
Window::event(QEvent *event)
{
	// TODO(p): switch statement.
	if (event->type() == QEvent::NativeGesture)
		return handle_native_gesture((QNativeGestureEvent *) event);
	if (event->type() == QEvent::TouchBegin ||
		event->type() == QEvent::TouchUpdate ||
		event->type() == QEvent::TouchEnd ||
		event->type() == QEvent::TouchCancel)
		return handle_touch((QTouchEvent *) event);
	if (event->type() == QEvent::UpdateRequest) {
		this->update_pending_ = false;
		render();
		return true;
	}
	if (event->type() == QEvent::WindowStateChange) {
		auto *change = (QWindowStateChangeEvent *) event;
		if ((change->oldState() ^ shell()->windowState()) &
			(Qt::WindowFullScreen | Qt::WindowMaximized)) {
			// Double click to fullscreen may make us not receive a MouseUp.
			this->kit_.left_down_ = false;
			request_render();
		}
	}
	if (event->type() == QEvent::DevicePixelRatioChange) {
		this->resize_pending_ = true;
		request_render();
	}
	if (event->type() == QEvent::PlatformSurface) {
		auto *surface_event = (QPlatformSurfaceEvent *) event;
		if (surface_event->surfaceEventType() ==
			QPlatformSurfaceEvent::SurfaceAboutToBeDestroyed) {
			this->renderer_ready_ = false;
			this->exposed_ = false;
			drop_frames();
			this->renderer_.destroy();
			this->surface_ = VK_NULL_HANDLE;
		}
	}
	if (event->type() == QEvent::FocusIn ||
		event->type() == QEvent::WindowActivate) {
		sync_macos_app_menu(this->app_);
		request_render();
	}
	if (event->type() == QEvent::FocusOut ||
		event->type() == QEvent::WindowDeactivate) {
		this->alt_armed_ = false;
		if (this->kit_.popup_open())
			this->kit_.close_popups();
		request_render();
	}
	if (event->type() == QEvent::DragEnter ||
		event->type() == QEvent::DragMove) {
		auto *drag = (QDropEvent *) event;
		if (this->mode_ != Mode::View ||
			first_dropped_file(drag->mimeData()).isEmpty()) {
			event->ignore();
			return true;
		}
		drag->acceptProposedAction();
		return true;
	}
	if (event->type() == QEvent::Drop) {
		auto *drop = (QDropEvent *) event;
		const QUrl url = first_dropped_file(drop->mimeData());
		if (this->mode_ != Mode::View || url.isEmpty()) {
			event->ignore();
			return true;
		}
		open_any(url);
		drop->acceptProposedAction();
		return true;
	}
	return QWindow::event(event);
}

bool
Window::eventFilter(QObject *watched, QEvent *event)
{
	if (watched != this && watched != shell())
		return false;
	// The compositor holds the pointer for as long as it moves or resizes us,
	// and drops our keyboard focus with it. Getting the pointer back is the
	// only word we get that the grab is over.
	if (this->system_grab_ &&
		(event->type() == QEvent::Enter || event->type() == QEvent::MouseMove ||
			event->type() == QEvent::MouseButtonPress)) {
		this->system_grab_ = false;
		request_render();
	}
	if (watched != this &&
		(event->type() == QEvent::FocusIn ||
			event->type() == QEvent::WindowActivate)) {
		sync_macos_app_menu(this->app_);
		request_render();
	}
	if (watched != this &&
		(event->type() == QEvent::FocusOut ||
			event->type() == QEvent::WindowDeactivate))
		request_render();
	if (event->type() != QEvent::MouseMove)
		return false;
	auto *mouse = (QMouseEvent *) event;
	const QPointF local = mapFromGlobal(mouse->globalPosition());
	if (local.x() < 0 || local.y() < 0 || local.x() >= width() ||
		local.y() >= height())
		return false;
	this->kit_.mouse_motion(float(local.x()), float(local.y()));
	request_render();
	return false;
}

void
Window::exposeEvent(QExposeEvent *)
{
	// Only the transition is news: we draw the whole surface from our own
	// state, so an expose while already exposed asks for nothing new.
	// MoltenVK marks the layer as needing display on every present, so Cocoa
	// exposes us once per presented frame; repainting on that never settles.
	// X11/i3 keeps us mapped across workspaces and still sends Expose when
	// we are shown again, including without keyboard focus.
	const bool was_exposed = this->exposed_;
	this->exposed_ = isExposed();
	if (this->exposed_ &&
		(!was_exposed ||
			QGuiApplication::platformName() == QLatin1String("xcb")))
		request_render();
}

void
Window::resizeEvent(QResizeEvent *)
{
	this->resize_pending_ = true;
	request_render();
}

void
Window::apply_window(Action a)
{
	if (a == Action::Menu) {
		if (Page *ui = active_ui())
			ui->open_app_menu(this->kit_, true);
		return;
	}
	if (Page *ui = active_ui(); ui && ui->actor.apply)
		ui->actor.apply(a);
	else if (this->host_.apply)
		this->host_.apply(a);
}

QUrl
Window::current_url() const
{
	if (this->mode_ == Mode::View && this->viewer_ &&
		!this->viewer_->url_.isEmpty())
		return this->viewer_->url_;
	if (this->browser_ && !this->browser_->dir_url_.isEmpty())
		return this->browser_->dir_url_;
	return path_to_url(QDir::currentPath());
}

void
Window::open_any(const QUrl &url, bool browse)
{
	// The browser always wants a directory: a file opens the one holding it.
	const QFileInfo info(url_to_path(url));
	if (info.isDir()) {
		if (this->browser_)
			this->browser_->open_dir(url);
		cancel_viewer_loads();
		set_mode(Mode::Browser);
	} else if (browse) {
		// A file with --browse is a request to point at it, not to view it:
		// browse the parent and put the cursor on the file.
		if (this->browser_) {
			this->browser_->open_dir(path_to_url(info.absolutePath()));
			this->browser_->select_file(url);
		}
		cancel_viewer_loads();
		set_mode(Mode::Browser);
	} else {
		if (this->viewer_)
			this->viewer_->open(url);
		set_mode(Mode::View);
		if (this->browser_)
			this->browser_->open_dir(path_to_url(info.absolutePath()));
		sync_viewer_preloads();
	}
	request_render();
	sync_title();
}

void
Window::keyPressEvent(QKeyEvent *event)
{
	const unsigned mods = unsigned(event->modifiers() &
		(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier |
			Qt::MetaModifier));
	const int key = event->key();
	if (key == Qt::Key_Alt && !event->isAutoRepeat() &&
		(mods == 0 || mods == unsigned(Qt::AltModifier))) {
		if (Popup *p = this->kit_.top_popup();
			p && p->shown() && dynamic_cast<Menu *>(p)) {
			this->kit_.close_popups();
			this->alt_armed_ = false;
			request_render();
			return;
		}
		if (this->kit_.popup_open())
			return;
		this->alt_armed_ = true;
		request_render();
		return;
	}
	if (this->alt_armed_ && key != Qt::Key_Alt)
		this->alt_armed_ = false;
	if (event->isAutoRepeat()) {
		if (Popup *p = this->kit_.top_popup();
			p && p->visible && p->captures_keys() && key != Qt::Key_Backspace)
			return;
	}
	if (this->kit_.key(key, mods)) {
		request_render();
		return;
	}
	const Action a = match_key(window_keys(), key, mods);
	if (a != Action::None) {
		apply_window(a);
		request_render();
		return;
	}
	if (key == Qt::Key_Escape && mods == 0) {
		if (this->mode_ == Mode::View && this->browser_ &&
			!this->browser_->dir_url_.isEmpty()) {
			show_browser(true);
			return;
		}
		if (this->mode_ == Mode::Browser && this->awaiting_view_) {
			cancel_viewer_loads();
			request_render();
			return;
		}
		if (this->mode_ == Mode::Browser)
			return;
		begin_close();
	}
}

void
Window::keyReleaseEvent(QKeyEvent *event)
{
	if (event->key() != Qt::Key_Alt) {
		QWindow::keyReleaseEvent(event);
		return;
	}
	if (!event->isAutoRepeat() && this->alt_armed_) {
		this->alt_armed_ = false;
		apply_window(Action::Menu);
		request_render();
	}
	event->accept();
}

void
Window::mousePressEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	const float x = float(pos.x());
	const float y = float(pos.y());
	this->alt_armed_ = false;
	if (event->button() == Qt::LeftButton && this->kit_.start_resize_at(x, y)) {
		request_render();
		event->accept();
		return;
	}
	if (event->button() == Qt::BackButton) {
		apply_window(Action::Back);
		request_render();
		event->accept();
		return;
	}
	if (event->button() == Qt::ForwardButton) {
		apply_window(Action::Forward);
		request_render();
		event->accept();
		return;
	}
	this->kit_.mouse_press(x, y, event->button(),
		unsigned(event->modifiers() &
			(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier |
				Qt::MetaModifier)));
	request_render();
	event->accept();
}

void
Window::mouseDoubleClickEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	const unsigned mods = unsigned(event->modifiers() &
		(Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier |
			Qt::MetaModifier));
	const float x = float(pos.x());
	const float y = float(pos.y());
	this->kit_.mouse_double_click(x, y, event->button(), mods);
	request_render();
	event->accept();
}

void
Window::mouseReleaseEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	const float x = float(pos.x());
	const float y = float(pos.y());
	this->kit_.mouse_release(x, y, event->button());
	request_render();
	event->accept();
}

void
Window::mouseMoveEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	const float x = float(pos.x());
	const float y = float(pos.y());
	this->kit_.mouse_motion(x, y);
	request_render();
	event->accept();
}

void
Window::wheelEvent(QWheelEvent *event)
{
	const QPointF pos = event->position();
	const float x = float(pos.x());
	const float y = float(pos.y());
	const QPoint ang = event->angleDelta();
	const QPoint pix = event->pixelDelta();
	const bool alt = event->modifiers() & Qt::AltModifier;
	// Qt Wayland (and Windows) transpose Alt+wheel onto the X axis.
	// Sway still sends a vertical wl_pointer.axis; Qt swaps it before
	// QWheelEvent. Read Y, then X.
	if (alt) {
		this->alt_armed_ = false;
		int rot = wheel_axis(ang, pix, false);
		if (!rot)
			rot = wheel_axis(ang, pix, true);
		if (rot) {
			this->kit_.gesture(x, y, 1.0f, rot > 0 ? 0.05f : -0.05f);
			request_render();
		}
		event->accept();
		return;
	}
#if defined Q_OS_MACOS
	// Cocoa provides pixelDelta even for a detented mouse wheel. Qt classifies
	// precise scrolling devices (trackpads and Magic Mouse) as TouchPad; only
	// those should pan, while a real wheel falls through to discrete zooming.
	if (event->device()->type() == QInputDevice::DeviceType::TouchPad &&
		(pix.x() || pix.y())) {
		const float dpr = kit_.dpr_ > 0.0f ? kit_.dpr_ : 1.0f;
		if (kit_.pan(x, y, float(pix.x()) / dpr, float(pix.y()) / dpr)) {
			request_render();
			event->accept();
			return;
		}
	}
#endif
	int delta = event->angleDelta().y();
	if (!delta)
		delta = event->pixelDelta().y();
	this->kit_.mouse_scroll(x, y, delta);
	request_render();
	event->accept();
}

bool
Window::handle_native_gesture(QNativeGestureEvent *event)
{
	const QPointF pos = event->position();
	const float x = float(pos.x());
	const float y = float(pos.y());
	switch (event->gestureType()) {
	case Qt::BeginNativeGesture:
		this->pinch_active_ = true;
		this->pinch_last_zoom_ = 0;
		this->pinch_last_rot_ = 0;
		event->accept();
		return true;
	case Qt::EndNativeGesture:
		this->pinch_active_ = false;
		this->pinch_last_zoom_ = 0;
		this->pinch_last_rot_ = 0;
		event->accept();
		return true;
	case Qt::ZoomNativeGesture: {
		const float v = float(event->value());
		float factor = 1.0f;
		if (v > 0.4f && v < 8.0f) {
			const float last =
				this->pinch_last_zoom_ > 0.4f ? this->pinch_last_zoom_ : 1.0f;
			factor = v / last;
			this->pinch_last_zoom_ = v;
		} else
			factor = 1.0f + v;
		if (factor > 0.0f && factor != 1.0f) {
			this->kit_.gesture(x, y, factor, 0.0f);
			request_render();
		}
		event->accept();
		return true;
	}
	case Qt::RotateNativeGesture: {
		const float v = float(event->value());
		float ddeg = v;
		if (this->pinch_active_ && this->pinch_last_rot_ != 0.0f &&
			fabs(v) >= 5.0f && fabs(v - this->pinch_last_rot_) < 40.0f)
			ddeg = v - this->pinch_last_rot_;
		if (fabs(v) >= 5.0f)
			this->pinch_last_rot_ = v;
		const float dangle = ddeg * (numbers::pi_v<float> / 180.0f);
		if (dangle != 0.0f) {
			this->kit_.gesture(x, y, 1.0f, dangle);
			request_render();
		}
		event->accept();
		return true;
	}
	case Qt::PanNativeGesture: {
		const QPointF d = event->delta();
		if ((d.x() != 0.0 || d.y() != 0.0) &&
			this->kit_.pan(x, y, float(d.x()), float(d.y())))
			request_render();
		event->accept();
		return true;
	}
	default:
		return false;
	}
}

bool
Window::handle_touch(QTouchEvent *event)
{
	const auto pts = event->points();
	int n = 0;
	int id0 = -1, id1 = -1;
	float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	for (const QEventPoint &p : pts) {
		if (p.state() == QEventPoint::Released ||
			p.state() == QEventPoint::Stationary ||
			p.state() == QEventPoint::Pressed ||
			p.state() == QEventPoint::Updated) {
			if (p.state() == QEventPoint::Released)
				continue;
			if (n == 0) {
				id0 = int(p.id());
				x0 = float(p.position().x());
				y0 = float(p.position().y());
			} else if (n == 1) {
				id1 = int(p.id());
				x1 = float(p.position().x());
				y1 = float(p.position().y());
			}
			++n;
		}
	}
	if (event->type() == QEvent::TouchCancel || n < 2) {
		this->touch_pinch_ = false;
		return n >= 2;
	}
	if (!this->touch_pinch_ || this->touch_id0_ != id0 ||
		this->touch_id1_ != id1) {
		this->touch_pinch_ = true;
		this->touch_id0_ = id0;
		this->touch_id1_ = id1;
		this->touch_x0_ = x0;
		this->touch_y0_ = y0;
		this->touch_x1_ = x1;
		this->touch_y1_ = y1;
		event->accept();
		return true;
	}
	const float odx = this->touch_x1_ - this->touch_x0_;
	const float ody = this->touch_y1_ - this->touch_y0_;
	const float ndx = x1 - x0;
	const float ndy = y1 - y0;
	const float olen = hypotf(odx, ody);
	const float nlen = hypotf(ndx, ndy);
	float factor = 1.0f;
	if (olen > 1.0f && nlen > 1.0f)
		factor = nlen / olen;
	const float oang = atan2f(ody, odx);
	const float nang = atan2f(ndy, ndx);
	float dangle = nang - oang;
	if (dangle > numbers::pi_v<float>)
		dangle -= 2.0f * numbers::pi_v<float>;
	if (dangle < -numbers::pi_v<float>)
		dangle += 2.0f * numbers::pi_v<float>;
	const float cx = 0.5f * (x0 + x1);
	const float cy = 0.5f * (y0 + y1);
	this->kit_.gesture(cx, cy, factor, dangle);
	this->touch_x0_ = x0;
	this->touch_y0_ = y0;
	this->touch_x1_ = x1;
	this->touch_y1_ = y1;
	request_render();
	event->accept();
	return true;
}

}  // namespace dn
