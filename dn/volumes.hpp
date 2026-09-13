//
// volumes.hpp: mounted filesystems for the browser's places section
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <string>
#include <vector>

namespace dn
{

struct Volume {
	std::string path;
	std::string name;
	const char *icon = nullptr;
};

std::vector<Volume> list_volumes();

}  // namespace dn
