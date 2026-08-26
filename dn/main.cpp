//
// main.cpp: dn image viewer entry point
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "app.hpp"
#include "dawn-config.h"
#include "libdn.h"
#include "thumbnail-cache.hpp"
#include "url.hpp"
#include "window.hpp"
#include "xdg.hpp"

#ifndef Q_OS_MACOS
#include "instance.hpp"
#include "ipc-instance.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#endif

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QEvent>
#include <QFileOpenEvent>
#include <QGuiApplication>
#include <QUrl>
#include <QtLogging>
#include <qcommandlineoption.h>
#include <qcoreapplication.h>

#include <cstdio>
#include <cstring>
#include <vector>

using namespace std;

namespace
{

// Finder delivers a document to open as a QFileOpenEvent, not an argument;
// app_ is set before exec(), which is the earliest this can be delivered.
struct GuiApplication : public QGuiApplication
{
	dn::App *app_ = nullptr;

	GuiApplication(int &argc, char **argv) : QGuiApplication(argc, argv) {}

protected:
	bool event(QEvent *event) override;
};

bool
GuiApplication::event(QEvent *event)
{
	if (event->type() == QEvent::FileOpen) {
		this->app_->open(dn::url_normalized(((QFileOpenEvent *) event)->url()));
		return true;
	}
	return QGuiApplication::event(event);
}

}  // namespace

#ifndef Q_OS_MACOS
#ifdef Q_OS_WIN
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace
{

QString
instance_session()
{
#ifdef Q_OS_WIN
	// Endpoint names are already scoped to the Windows session, so there
	// is nothing left here for the handshake to catch.
	return {};
#else
	QString session = qEnvironmentVariable("WAYLAND_DISPLAY");
	if (session.isEmpty())
		session = qEnvironmentVariable("DISPLAY");
	if (session.isEmpty()) {
		session =
			QStringLiteral("none-%1").arg(QCoreApplication::applicationPid());
	}
	return session;
#endif
}

vector<string>
urls_utf8(const QList<QUrl> &urls)
{
	vector<string> out;
	out.reserve(size_t(urls.size()));
	for (const QUrl &url : urls)
		out.push_back(url.toEncoded().toStdString());
	return out;
}

const char *
error_fallback(dn::ipc::instance::ErrorCode code)
{
	using dn::ipc::instance::ErrorCode;
	switch (code) {
	case ErrorCode::NotFound:
		return "not found";
	case ErrorCode::PermissionDenied:
		return "permission denied";
	case ErrorCode::InvalidArgument:
		return "invalid argument";
	case ErrorCode::Internal:
		return "internal error";
	default:
		return "open failed";
	}
}

bool
handoff_open(
	dn::ipc::BlockingClient &client, const QList<QUrl> &urls, bool browse)
{
	const string token =
		qEnvironmentVariable("XDG_ACTIVATION_TOKEN").toUtf8().toStdString();
#ifdef Q_OS_WIN
	// Windows only lets the foreground process pass that right on, and
	// the shell just launched us. There is no token to send; the running
	// instance raises its own window once it has the permission.
	if (const uint32_t pid = client.server_pid())
		AllowSetForegroundWindow(DWORD(pid));
#endif
	dn::ipc::instance::Error error;
	if (client.open(urls_utf8(urls), token, browse, &error))
		return true;
	if (!error.message.empty())
		qWarning("%s", error.message.c_str());
	else
		qWarning("%s", error_fallback(error.code));
	return false;
}

void
report_mismatch(dn::ipc::BlockingClient::HelloStatus status, bool &reported)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	if (reported)
		return;
	if (status == HelloStatus::VersionMismatch) {
		qWarning("running isolated (version mismatch)");
		reported = true;
	} else if (status == HelloStatus::SessionMismatch) {
		qWarning("running isolated (session mismatch)");
		reported = true;
	}
}

enum class Remote : uint8_t { Done, Failed, Isolated };

Remote
try_remote_open(const QString &session, const QList<QUrl> &urls, bool browse,
	bool &reported_mismatch)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	HelloStatus status = HelloStatus::Unavailable;
	auto client = dn::ipc::BlockingClient::connect(
		session.toUtf8().toStdString(), &status);
	if (client) {
		if (handoff_open(*client, urls, browse))
			return Remote::Done;
		return Remote::Failed;
	}
	report_mismatch(status, reported_mismatch);
	return Remote::Isolated;
}

}  // namespace
#endif

int
main(int argc, char **argv)
{
	QCoreApplication::setApplicationName(QStringLiteral("dn"));
	QCoreApplication::setApplicationVersion(QStringLiteral(DAWN_VERSION));
	QGuiApplication::setDesktopFileName(QStringLiteral("dn"));

	// Qt logs bare messages by default, which is unhelpful in a terminal.
	// QT_MESSAGE_PATTERN still overrides this.
	qSetMessagePattern(QStringLiteral(
		"%{appname}: %{if-category}%{category}: %{endif}%{message}"));

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral(
		"Display images or browse directories."));
	parser.addHelpOption();
	parser.addVersionOption();

	const QCommandLineOption new_instance_opt(QStringLiteral("new-instance"),
		QStringLiteral("Do not connect to a running dn; start a new process."));
	parser.addOption(new_instance_opt);

	const QCommandLineOption invalidate_opt(QStringLiteral("invalidate-cache"),
		QStringLiteral("Remove invalid wide thumbnails and exit."));
	parser.addOption(invalidate_opt);

	const QCommandLineOption browse_opt(QStringLiteral("browse"),
		QStringLiteral("Start in filesystem browsing mode."));
	parser.addOption(browse_opt);

	const QCommandLineOption list_supported_opt(
		QStringLiteral("list-supported-media-types"),
		QStringLiteral("Output supported media types and exit."));
	parser.addOption(list_supported_opt);

	const QCommandLineOption list_extensions_opt(
		QStringLiteral("list-supported-extensions"),
		QStringLiteral("Output supported filename globs and exit."));
	parser.addOption(list_extensions_opt);

	parser.addPositionalArgument(QStringLiteral("path | URI"),
		QStringLiteral(
			"Image file or directory. Repeat to open multiple windows. "
			"Defaults to the current directory."),
		QStringLiteral("[path | URI]..."));

	// xdg_data_dirs() invokes the static QCoreApplication::instance().
	auto application = make_unique<QCoreApplication>(argc, argv);
	parser.process(*application);

	if (parser.isSet(invalidate_opt)) {
		dn::thumbnail_cache_invalidate();
		return 0;
	}
	if (parser.isSet(list_supported_opt)) {
		for (const string &type : dn::supported_media_types())
			printf("%s\n", type.c_str());
		return 0;
	}
	if (parser.isSet(list_extensions_opt)) {
		vector<QString> types;
		for (const string &type : dn::supported_media_types())
			types.push_back(QString::fromStdString(type));
		for (const QString &glob : dn::extract_mime_globs(types))
			printf("%s\n", glob.toUtf8().constData());
		return 0;
	}

	application.reset();
	application = make_unique<GuiApplication>(argc, argv);
	const QStringList raw = parser.positionalArguments();

	// Without the working directory, relative arguments do not resolve to
	// a local file, and every one of them is rejected as a foreign scheme.
	const QString cwd = QDir::currentPath();

	QList<QUrl> to_open;
	if (raw.isEmpty()) {
		to_open.append(dn::url_normalized(QUrl::fromUserInput(
			QStringLiteral("."), cwd, QUrl::AssumeLocalFile)));
	} else {
		for (const QString &arg : raw) {
			auto url = QUrl::fromUserInput(arg, cwd, QUrl::AssumeLocalFile);
			if (!url.isValid())
				url = dn::path_to_url(QDir(cwd).absoluteFilePath(arg));
			to_open.append(dn::url_normalized(url));
		}
	}

	const bool browse = parser.isSet(browse_opt);

	dn::App app;
#ifndef Q_OS_MACOS
	unique_ptr<dn::InstanceHost> host;
	if (!parser.isSet(new_instance_opt)) {
		const QString session = instance_session();
		bool reported_mismatch = false;
		switch (
			try_remote_open(session, to_open, browse, reported_mismatch)) {
		case Remote::Done:
			return 0;
		case Remote::Failed:
			return 1;
		case Remote::Isolated:
			break;
		}

		auto listen = dn::ipc::Endpoint::listen("instance");
		if (listen.status == dn::ipc::Endpoint::ListenStatus::InUse) {
			switch (
				try_remote_open(session, to_open, browse, reported_mismatch)) {
			case Remote::Done:
				return 0;
			case Remote::Failed:
				return 1;
			case Remote::Isolated:
				break;
			}
		} else if (listen.status == dn::ipc::Endpoint::ListenStatus::Ok) {
			// Notifiers armed; Qt delivers them only in exec().
			// A Hello during init may time out (250ms) and isolate.
			host = make_unique<dn::InstanceHost>(
				std::move(listen.listener), app, session);
		}
	}
#endif
	if (!app.init())
		return 1;
	for (const QUrl &url : to_open) {
		if (app.open(url, {}, {}, browse) != dn::OpenResult::Ok)
			return 1;
	}
	// A bare launch opened the CWD above on a guess; see default_window().
	if (raw.isEmpty())
		app.default_window = app.key_window();

	((GuiApplication *) application.get())->app_ = &app;
	return application->exec();
}
