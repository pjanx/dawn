//
// main.cpp: dn image viewer entry point
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "app.hpp"
#include "dawn-config.h"
#include "thumbnail-cache.hpp"

#if DN_WITH_SINGLE_INSTANCE
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
#include <QGuiApplication>

#include <cstdio>
#include <cstring>

#if DN_WITH_SINGLE_INSTANCE
namespace
{

QString
instance_session()
{
	QString session = qEnvironmentVariable("WAYLAND_DISPLAY");
	if (session.isEmpty())
		session = qEnvironmentVariable("DISPLAY");
	if (session.isEmpty()) {
		session = QStringLiteral("none-%1").arg(
			QCoreApplication::applicationPid());
	}
	return session;
}

std::vector<std::string>
paths_utf8(const QStringList &paths)
{
	std::vector<std::string> out;
	out.reserve(size_t(paths.size()));
	for (const QString &path : paths)
		out.push_back(path.toUtf8().toStdString());
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
handoff_open(dn::ipc::BlockingClient &client, const QStringList &paths)
{
	const std::string token =
		qEnvironmentVariable("XDG_ACTIVATION_TOKEN")
			.toUtf8()
			.toStdString();
	dn::ipc::instance::Error error;
	if (client.open(paths_utf8(paths), token, &error))
		return true;
	if (!error.message.empty())
		fprintf(stderr, "dn: %s\n", error.message.c_str());
	else
		fprintf(stderr, "dn: %s\n", error_fallback(error.code));
	return false;
}

void
report_mismatch(dn::ipc::BlockingClient::HelloStatus status,
	bool &reported)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	if (reported)
		return;
	if (status == HelloStatus::VersionMismatch) {
		fprintf(stderr,
			"dn: running isolated (version mismatch)\n");
		reported = true;
	} else if (status == HelloStatus::SessionMismatch) {
		fprintf(stderr,
			"dn: running isolated (session mismatch)\n");
		reported = true;
	}
}

enum class Remote : std::uint8_t { Done, Failed, Isolated };

Remote
try_remote_open(const QString &build_id, const QString &session,
	const QStringList &paths, bool &reported_mismatch)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	HelloStatus status = HelloStatus::Unavailable;
	auto client = dn::ipc::BlockingClient::connect(
		build_id.toUtf8().toStdString(),
		session.toUtf8().toStdString(), &status);
	if (client) {
		if (handoff_open(*client, paths))
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
	for (int i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "--invalidate-cache") != 0)
			continue;
		QCoreApplication application(argc, argv);
		QCoreApplication::setApplicationName(QStringLiteral("dn"));
		dn::thumbnail_cache_invalidate();
		return 0;
	}
	QGuiApplication application(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("dn"));
	QGuiApplication::setDesktopFileName(QStringLiteral("dn"));

	QCommandLineParser parser;
	parser.setApplicationDescription(QStringLiteral(
		"Display images or browse directories. Esc switches or quits; "
		"q quits; wheel or +/- zooms; left- or middle-drag pans; Ctrl+middle-drag zooms; Alt+middle-drag rotates; pinch or Alt+wheel rotates."));
	parser.addHelpOption();
	const QCommandLineOption new_instance_opt(
		QStringLiteral("new-instance"),
		QStringLiteral(
			"Do not connect to a running dn; start a new process."));
	parser.addOption(new_instance_opt);
	parser.addOption({QStringLiteral("invalidate-cache"),
		QStringLiteral("Remove invalid wide thumbnails and exit.")});
	parser.addPositionalArgument(QStringLiteral("path"),
		QStringLiteral(
			"Image file or directory. Repeat to open multiple windows. "
			"Defaults to the current directory."),
		QStringLiteral("[path...]"));
	parser.process(application);

	const QStringList raw = parser.positionalArguments();
	const QDir cwd = QDir::current();
	QStringList to_open;
	if (raw.isEmpty())
		to_open.append(cwd.absoluteFilePath(QStringLiteral(".")));
	else {
		to_open.reserve(raw.size());
		for (const QString &path : raw)
			to_open.append(cwd.absoluteFilePath(path));
	}

	dn::App app;
#if DN_WITH_SINGLE_INSTANCE
	std::unique_ptr<dn::InstanceHost> host;
	const bool new_instance = parser.isSet(new_instance_opt);
	if (!new_instance) {
		const QString build_id =
			QString::fromUtf8(DN_IPC_BUILD_ID);
		const QString session = instance_session();
		bool reported_mismatch = false;
		switch (try_remote_open(
			build_id, session, to_open, reported_mismatch)) {
		case Remote::Done:
			return 0;
		case Remote::Failed:
			return 1;
		case Remote::Isolated:
			break;
		}

		const auto listen =
			dn::ipc::Endpoint::listen("instance");
		if (listen.status ==
			dn::ipc::Endpoint::ListenStatus::InUse) {
			switch (try_remote_open(build_id, session,
				to_open, reported_mismatch)) {
			case Remote::Done:
				return 0;
			case Remote::Failed:
				return 1;
			case Remote::Isolated:
				break;
			}
		} else if (listen.status ==
			dn::ipc::Endpoint::ListenStatus::Ok) {
			// Notifiers armed; Qt delivers them only in
			// exec(). A Hello during init may time out
			// (250ms) and isolate.
			host = std::make_unique<dn::InstanceHost>(
				listen.fd, app, build_id, session);
		}
	}
#endif
	if (!app.init())
		return 1;
	for (const QString &path : to_open) {
		if (app.open(path) != dn::OpenResult::Ok)
			return 1;
	}
	return application.exec();
}
