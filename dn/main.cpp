//
// main.cpp: dn image viewer entry point
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-config.h>
#include <dawn-gettext.h>

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
#include <QLibraryInfo>
#include <QLocale>
#include <QTranslator>
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

static QString
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

static const char *
error_fallback(dawn::ipc::ErrorCode code)
{
	using dawn::ipc::ErrorCode;
	switch (code) {
	case ErrorCode::NotFound:
		return _("not found");
	case ErrorCode::PermissionDenied:
		return _("permission denied");
	case ErrorCode::InvalidArgument:
		return _("invalid argument");
	case ErrorCode::Internal:
		return _("internal error");
	default:
		return _("open failed");
	}
}

static bool
handoff_open(dawn::ipc::instance::Client &client, const vector<QUrl> &urls,
	dn::Mode mode)
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
	dawn::ipc::Error error;
	if (client.open(encoded, token, dn::mode_def(mode).name, &error,
			dawn::ipc::kRequestTimeout))
		return true;
	if (!error.message.empty())
		qWarning("%s", error.message.c_str());
	else
		qWarning("%s", error_fallback(error.code));
	return false;
}

// Returns an exit code once a running instance has taken the URLs over.
static optional<int>
try_remote_open(const QString &session, const vector<QUrl> &urls, dn::Mode mode,
	bool &reported_mismatch)
{
	using HelloStatus = dawn::ipc::HelloStatus;
	HelloStatus status = HelloStatus::Unavailable;
	auto client = dawn::ipc::instance::Client::connect(
		session.toUtf8().toStdString(), &status, dawn::ipc::kHelloTimeout);
	if (client) {
		return handoff_open(*client, urls, mode) ? EXIT_SUCCESS : EXIT_FAILURE;
	}

	const char *mismatch = nullptr;
	if (status == HelloStatus::VersionMismatch)
		mismatch = N_("running isolated (version mismatch)");
	else if (status == HelloStatus::SessionMismatch)
		mismatch = N_("running isolated (session mismatch)");
	if (mismatch && !reported_mismatch) {
		qWarning("%s", _(mismatch));
		reported_mismatch = true;
	}
	return {};
}

#endif

// Qt's own text -- the generic command-line options, the items macOS adds to
// the application menu -- lives in its catalogues, which it will not load on
// its own.  Qt reads the environment itself, and does not know about LANGUAGE.
static void
install_qt_translations(QCoreApplication &app)
{
	static QTranslator translations;
	if (translations.isEmpty() &&
		!translations.load(QLocale(), QStringLiteral("qtbase"),
			QStringLiteral("_"),
			QLibraryInfo::path(QLibraryInfo::TranslationsPath)))
		return;

	app.installTranslator(&translations);
}

int
main(int argc, char **argv)
{
	// Before the first translated literal, which the parser below builds.
	dawn::gettext_init();

	QCoreApplication::setApplicationName(QStringLiteral("dn"));
	QCoreApplication::setApplicationVersion(QStringLiteral(DAWN_VERSION));
	QGuiApplication::setDesktopFileName(QStringLiteral(DAWN_NAMESPACE));

	// Qt logs bare messages by default, which is unhelpful in a terminal.
	// QT_MESSAGE_PATTERN still overrides this.
	qSetMessagePattern(QStringLiteral(
		"%{appname}: %{if-category}%{category}: %{endif}%{message}"));

	QCommandLineParser parser;
	parser.setApplicationDescription(
		QString::fromUtf8(_("Display images or browse directories.")));
	parser.addHelpOption();
	parser.addVersionOption();

	// Option names are what the user types, and stay as they are.
	const QCommandLineOption new_instance_opt(QStringLiteral("new-instance"),
		QString::fromUtf8(
			_("Do not connect to a running dn; start a new process.")));
	parser.addOption(new_instance_opt);

	const QCommandLineOption invalidate_opt(QStringLiteral("invalidate-cache"),
		QString::fromUtf8(_("Remove invalid wide thumbnails and exit.")));
	parser.addOption(invalidate_opt);

	const QCommandLineOption mode_opt(QStringLiteral("mode"),
		QString::fromUtf8(_("Application: view, browse, cropjpeg, commander.")),
		QStringLiteral("mode"));
	parser.addOption(mode_opt);

	const QCommandLineOption list_supported_opt(
		QStringLiteral("list-supported-media-types"),
		QString::fromUtf8(_("Output supported media types and exit.")));
	parser.addOption(list_supported_opt);

	const QCommandLineOption list_extensions_opt(
		QStringLiteral("list-supported-extensions"),
		QString::fromUtf8(_("Output supported filename globs and exit.")));
	parser.addOption(list_extensions_opt);

	// TRANSLATORS: The name of the positional argument, and its syntax.
	parser.addPositionalArgument(QString::fromUtf8(_("path | URL")),
		QString::fromUtf8(
			_("Image file or directory. Repeat to open multiple windows. "
			  "Defaults to an empty cropper or the current directory.")),
		QString::fromUtf8(_("[path | URL]...")));

	{
		// xdg_data_dirs() invokes the static QCoreApplication::instance().
		QCoreApplication bootstrap(argc, argv);
		install_qt_translations(bootstrap);
		parser.process(bootstrap);

		if (parser.isSet(invalidate_opt)) {
			dn::thumbnail_cache_invalidate();
			return 0;
		}
		if (parser.isSet(list_supported_opt)) {
			for (const string &type : dawn::supported_media_types())
				printf("%s\n", type.c_str());
			return 0;
		}
		if (parser.isSet(list_extensions_opt)) {
			for (const QString &glob :
				dn::extract_mime_globs(dawn::supported_media_types()))
				printf("%s\n", glob.toUtf8().constData());
			return 0;
		}
	}

	dn::Mode mode = dn::Mode::View;
	if (parser.isSet(mode_opt)) {
		auto parsed = dn::parse_mode(parser.value(mode_opt).toStdString());
		if (!parsed) {
			qWarning("%s",
				qUtf8Printable(QString::fromUtf8(_("unknown mode: %1"))
						.arg(parser.value(mode_opt))));
			return EXIT_FAILURE;
		}
		mode = *parsed;
	}
#if !DAWN_WIP
	if (!dn::viewer_mode(mode)) {
		qWarning("%s",
			qUtf8Printable(QString::fromUtf8(_("unsupported mode: %1"))
					.arg(QLatin1String(dn::mode_def(mode).name))));
		return EXIT_FAILURE;
	}
#endif

	dn::App app(argc, argv, mode);
	install_qt_translations(app);
	QStringList raw = parser.positionalArguments();
	const bool bare = raw.isEmpty();
	const QString cwd = QDir::currentPath();
	vector<QUrl> to_open;
	for (const QString &arg : raw)
		to_open.push_back(dn::url_from_user_input(arg, cwd));
	if (bare)
		to_open.push_back(
			mode == dn::Mode::CropJpeg ? QUrl{} : dn::path_to_url(cwd));

#ifndef Q_OS_MACOS
	unique_ptr<dn::InstanceHost> host;
	if (!parser.isSet(new_instance_opt)) {
		const QString session = instance_session();
		bool reported_mismatch = false;
		if (auto code =
				try_remote_open(session, to_open, mode, reported_mismatch))
			return *code;

		auto listen =
			dawn::ipc::Endpoint::listen(dawn::ipc::instance::kService);
		if (listen.status == dawn::ipc::Endpoint::ListenStatus::InUse) {
			// Someone else bound it in the meantime.
			if (auto code =
					try_remote_open(session, to_open, mode, reported_mismatch))
				return *code;
		} else if (listen.status == dawn::ipc::Endpoint::ListenStatus::Ok) {
			// Notifiers armed; Qt delivers them only in exec().
			// A Hello during init may time out (250ms) and isolate.
			host = make_unique<dn::InstanceHost>(
				std::move(listen.listener), app, session, nullptr);
		}
	}
#endif
	if (!app.init())
		return EXIT_FAILURE;

	for (const QUrl &url : to_open) {
		if (app.open(url, {}, {}, mode) != dn::OpenResult::Ok)
			return EXIT_FAILURE;
	}

	// Finder may replace an untouched window from a documentless launch.
	if (bare)
		app.default_window = app.key_window();
	app.accept_files();

	return app.exec();
}
