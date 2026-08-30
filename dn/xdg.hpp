//
// xdg.hpp: shared-mime-info glob filtering (filename, not content)
//
// Copyright The Dawn Authors
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

std::vector<QString> xdg_data_dirs();
std::vector<QString> xdg_config_dirs();

}  // namespace dn
