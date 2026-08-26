//
// xdg.hpp: shared-mime-info glob filtering (filename, not content)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QString>

#include <string>
#include <vector>

namespace dn
{

std::vector<QString> extract_mime_globs(
	const std::vector<std::string> &media_types);

// MIME types whose shared-mime-info globs match the filename (suffix/glob).
std::vector<QString> types_for_filename(const QString &path);

std::vector<QString> xdg_data_dirs();
std::vector<QString> xdg_config_dirs();

}  // namespace dn
