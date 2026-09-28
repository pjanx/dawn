//
// url.hpp: URL identity for files and directories
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QString>
#include <QUrl>

#include <filesystem>
#include <string>

namespace dn
{

// URLs identify files and directories throughout the UI. Filesystem paths are
// derived from them only where the filesystem is actually touched, so that
// non-local URLs fail at that operation rather than silently much earlier.

QUrl path_to_url(const QString &path);
QUrl url_normalized(const QUrl &url);
QUrl url_from_user_input(const QString &input, const QString &working_dir);
QString url_to_path(const QUrl &url);
QString url_basename(const QUrl &url);
QString url_parse_name(const QUrl &url);

// Paths held as std::string are UTF-8 with forward slashes, the form that
// url_to_path() gives, so that they compare equal as strings.  These convert
// at std::filesystem, whose narrow strings are in the ANSI code page on
// Windows.
std::filesystem::path utf8_to_fs_path(const std::string &path);
std::string fs_path_to_utf8(const std::filesystem::path &path);

}  // namespace dn
