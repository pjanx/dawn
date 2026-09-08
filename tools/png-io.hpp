//
// png-io.hpp: PNG output for daemon command-line clients
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdint>
#include <cstdio>
#include <span>
#include <string>

bool write_png16(FILE *file, uint32_t width, uint32_t height, uint32_t stride,
	const uint8_t *bgra16_premul, std::span<const uint8_t> icc,
	int32_t orientation, std::string *error);
bool write_png8(FILE *file, uint32_t width, uint32_t height,
	std::span<const uint8_t> rgba, std::span<const uint8_t> icc,
	std::string *error);
