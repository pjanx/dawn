//
// url.cpp: URL identity for files and directories
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "url.hpp"

#include <QDir>
#include <QFileInfo>

using namespace std;

namespace dn
{

// Absolute and cleaned, so that URLs built from equivalent paths compare equal.
QUrl
path_to_url(const QString &path)
{
	if (path.isEmpty())
		return {};
	const QString abs = QFileInfo(path).absoluteFilePath();
	return QUrl::fromLocalFile(QDir::cleanPath(abs.isEmpty() ? path : abs));
}

// Command lines and drops deliver URLs in whatever shape the sender chose.
QUrl
url_normalized(const QUrl &url)
{
	if (url.isLocalFile())
		return path_to_url(url.toLocalFile());
	return url.adjusted(QUrl::NormalizePathSegments);
}

// Empty for anything the local filesystem cannot name.
QString
url_to_path(const QUrl &url)
{
	if (!url.isLocalFile())
		return {};
	return url.toLocalFile();
}

QString
url_basename(const QUrl &url)
{
	return url.fileName(QUrl::FullyDecoded);
}

// GFile parse name: a local URL is an absolute native path with $HOME as ~,
// anything else is the URL itself.
QString
url_parse_name(const QUrl &url)
{
	const QString path = url_to_path(url);
	if (path.isEmpty())
		return url.toString(QUrl::PrettyDecoded);

	const QString abs = QDir::toNativeSeparators(QDir::cleanPath(path));
	const QString home =
		QDir::toNativeSeparators(QDir::cleanPath(QDir::homePath()));
	if (home.isEmpty())
		return abs;
	const QChar sep = QDir::separator();
#ifdef Q_OS_WIN
	const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
	const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
	if (abs.compare(home, cs) == 0)
		return QStringLiteral("~");
	if (abs.startsWith(home + sep, cs))
		return QChar(u'~') + abs.mid(home.size());
	return abs;
}

}  // namespace dn
