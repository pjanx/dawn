//
// encode-webp.hpp: WebP thumbnail encoding
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace dnthumbd {

bool encode_webp_rgba8(uint32_t w, uint32_t h, const uint8_t *rgba8,
	std::vector<uint8_t> *out, std::string *error);

} // namespace dnthumbd
