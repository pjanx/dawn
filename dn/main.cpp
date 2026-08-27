//
// main.cpp: dn image viewer entry point
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>

#include "app.hpp"
#include "libdn/libdn.h"
#include "thumbnail-cache.hpp"
#include "url.hpp"
#include "window.hpp"
#include "xdg.hpp"

#ifndef Q_OS_MACOS
#include "instance.hpp"
#include "libdn/ipc-instance.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#endif

#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCoreApplication>
#include <QDir>
#include <QGuiApplication>
#include <QUrl>
#include <QtLogging>

#include <cstdio>
#include <string>
#include <vector>

using namespace std;

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
	dn::ipc::instance::Client &client, const vector<QUrl> &urls, bool browse)
{
	const string token =
		qEnvironmentVariable("XDG_ACTIVATION_TOKEN").toUtf8().toStdString();
	vector<string> encoded;
	for (const QUrl &url : urls)
		encoded.push_back(url.toEncoded().toStdString());
#ifdef Q_OS_WIN
	// Windows only lets the foreground process pass that right on, and
	// the shell just launched us. There is no token to send; the running
	// instance raises its own window once it has the permission.
	if (const uint32_t pid = client.server_pid())
		AllowSetForegroundWindow(DWORD(pid));
#endif
	dn::ipc::instance::Error error;
	if (client.open(
			encoded, token, browse, &error, dn::ipc::kRequestTimeout))
		return true;
	if (!error.message.empty())
		qWarning("%s", error.message.c_str());
	else
		qWarning("%s", error_fallback(error.code));
	return false;
}

// Returns an exit code once a running instance has taken the URLs over.
optional<int>
try_remote_open(const QString &session, const vector<QUrl> &urls, bool browse,
	bool &reported_mismatch)
{
	using HelloStatus = dn::ipc::HelloStatus;
	HelloStatus status = HelloStatus::Unavailable;
	auto client = dn::ipc::instance::Client::connect(
		session.toUtf8().toStdString(), &status, dn::ipc::kHelloTimeout);
	if (client) {
		return handoff_open(*client, urls, browse)
			? EXIT_SUCCESS
			: EXIT_FAILURE;
	}

	const char *mismatch = nullptr;
	if (status == HelloStatus::VersionMismatch)
		mismatch = "version";
	else if (status == HelloStatus::SessionMismatch)
		mismatch = "session";
	if (mismatch && !reported_mismatch) {
		qWarning("running isolated (%s mismatch)", mismatch);
		reported_mismatch = true;
	}
	return {};
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

	{
		// xdg_data_dirs() invokes the static QCoreApplication::instance().
		QCoreApplication bootstrap(argc, argv);
		parser.process(bootstrap);

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
			for (const QString &glob :
				dn::extract_mime_globs(dn::supported_media_types()))
				printf("%s\n", glob.toUtf8().constData());
			return 0;
		}
	}

	dn::App app(argc, argv);
	QStringList raw = parser.positionalArguments();
	const bool bare = raw.isEmpty();
	if (bare)
		raw.push_back(QStringLiteral("."));

	// Without the working directory, relative arguments do not resolve to
	// a local file, and every one of them is rejected as a foreign scheme.
	const QString cwd = QDir::currentPath();

	vector<QUrl> to_open;
	for (const QString &arg : raw) {
		auto url = QUrl::fromUserInput(arg, cwd, QUrl::AssumeLocalFile);
		if (!url.isValid())
			url = dn::path_to_url(QDir(cwd).absoluteFilePath(arg));
		to_open.push_back(dn::url_normalized(url));
	}

	const bool browse = parser.isSet(browse_opt);

#ifndef Q_OS_MACOS
	unique_ptr<dn::InstanceHost> host;
	if (!parser.isSet(new_instance_opt)) {
		const QString session = instance_session();
		bool reported_mismatch = false;
		if (auto code = try_remote_open(
				session, to_open, browse, reported_mismatch))
			return *code;

		auto listen = dn::ipc::Endpoint::listen(dn::ipc::instance::kService);
		if (listen.status == dn::ipc::Endpoint::ListenStatus::InUse) {
			// Someone else bound it in the meantime.
			if (auto code = try_remote_open(
					session, to_open, browse, reported_mismatch))
				return *code;
		} else if (listen.status == dn::ipc::Endpoint::ListenStatus::Ok) {
			// Notifiers armed; Qt delivers them only in exec().
			// A Hello during init may time out (250ms) and isolate.
			host = make_unique<dn::InstanceHost>(
				std::move(listen.listener), app, session);
		}
	}
#endif
	if (!app.init())
		return EXIT_FAILURE;

	for (const QUrl &url : to_open) {
		if (app.open(url, {}, {}, browse) != dn::OpenResult::Ok)
			return EXIT_FAILURE;
	}

	// A bare launch opened the CWD above on a guess; see default_window().
	if (bare)
		app.default_window = app.key_window();

	return app.exec();
}
