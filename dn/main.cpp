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
#include <QUrl>
#include <qcommandlineoption.h>

#include <cstdio>
#include <cstring>

using namespace std;

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
		session =
			QStringLiteral("none-%1").arg(QCoreApplication::applicationPid());
	}
	return session;
}

vector<string>
paths_utf8(const QStringList &paths)
{
	vector<string> out;
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
	const string token =
		qEnvironmentVariable("XDG_ACTIVATION_TOKEN").toUtf8().toStdString();
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
report_mismatch(dn::ipc::BlockingClient::HelloStatus status, bool &reported)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	if (reported)
		return;
	if (status == HelloStatus::VersionMismatch) {
		fprintf(stderr, "dn: running isolated (version mismatch)\n");
		reported = true;
	} else if (status == HelloStatus::SessionMismatch) {
		fprintf(stderr, "dn: running isolated (session mismatch)\n");
		reported = true;
	}
}

enum class Remote : uint8_t { Done, Failed, Isolated };

Remote
try_remote_open(const QString &session, const QStringList &paths,
	bool &reported_mismatch)
{
	using HelloStatus = dn::ipc::BlockingClient::HelloStatus;
	HelloStatus status = HelloStatus::Unavailable;
	auto client = dn::ipc::BlockingClient::connect(
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
	QCoreApplication::setApplicationName(QStringLiteral("dn"));
	QCoreApplication::setApplicationVersion(QStringLiteral(DAWN_VERSION));
	QGuiApplication::setDesktopFileName(QStringLiteral("dn"));

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

	parser.addPositionalArgument(QStringLiteral("path | URI"),
		QStringLiteral(
			"Image file or directory. Repeat to open multiple windows. "
			"Defaults to the current directory."),
		QStringLiteral("[path | URI]..."));

	// I suppose this could very well be passed a QStringList; this is shorter.
	parser.process(QCoreApplication(argc, argv));

	if (parser.isSet(invalidate_opt)) {
		dn::thumbnail_cache_invalidate();
		return 0;
	}
	if (parser.isSet(list_supported_opt)) {
		for (const string &type : dn::supported_media_types())
			printf("%s\n", type.c_str());
		return 0;
	}

	QGuiApplication application(argc, argv);
	const QStringList raw = parser.positionalArguments();
	const QDir cwd = QDir::current();

	QStringList to_open;
	if (raw.isEmpty()) {
		to_open.append(QUrl::fromUserInput(".",
			{}, QUrl::AssumeLocalFile).toString());
	} else {
		for (const QString &arg : raw) {
			auto url = QUrl::fromUserInput(arg, {}, QUrl::AssumeLocalFile);
			if (!url.isValid())
				url = QUrl::fromLocalFile(arg);
			to_open.append(url.toString());
		}
	}

	// TODO(p): Process browse_opt: pass through IPC, or to this instance.

	dn::App app;
#if DN_WITH_SINGLE_INSTANCE
	unique_ptr<dn::InstanceHost> host;
	if (!parser.isSet(new_instance_opt)) {
		const QString session = instance_session();
		bool reported_mismatch = false;
		switch (
			try_remote_open(session, to_open, reported_mismatch)) {
		case Remote::Done:
			return 0;
		case Remote::Failed:
			return 1;
		case Remote::Isolated:
			break;
		}

		const auto listen = dn::ipc::Endpoint::listen("instance");
		if (listen.status == dn::ipc::Endpoint::ListenStatus::InUse) {
			switch (try_remote_open(session, to_open, reported_mismatch)) {
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
			host = make_unique<dn::InstanceHost>(listen.fd, app, session);
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
