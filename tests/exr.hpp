//
// exr.hpp: the least OpenEXR writer that loaders take
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

namespace test
{

namespace exr_detail
{

inline void
append(std::vector<uint8_t> &o, const void *data, size_t size)
{
	const auto *p = static_cast<const uint8_t *>(data);
	o.insert(o.end(), p, p + size);
}

// OpenEXR is little-endian throughout, and so is every host Dawn runs on.
template <typename T>
inline void
append_le(std::vector<uint8_t> &o, T value)
{
	append(o, &value, sizeof value);
}

inline void
append_attribute(std::vector<uint8_t> &o, std::string_view name,
	std::string_view type, const std::vector<uint8_t> &value)
{
	append(o, name.data(), name.size());
	o.push_back(0);
	append(o, type.data(), type.size());
	o.push_back(0);
	append_le(o, int32_t(value.size()));
	append(o, value.data(), value.size());
}

}  // namespace exr_detail

/// An uncompressed scanline file of FLOAT channels.  `rgb` holds a triplet
/// per pixel, `alpha`, unless empty, a value, which OpenEXR takes for
/// associated.
inline std::vector<uint8_t>
write_exr(uint32_t width, uint32_t height, std::span<const float> rgb,
	std::span<const float> alpha)
{
	using namespace exr_detail;

	// Channels go in alphabetical order, in the header and in each line.
	std::vector<char> names = {'B', 'G', 'R'};
	if (!alpha.empty())
		names.insert(names.begin(), 'A');

	std::vector<uint8_t> o = {0x76, 0x2f, 0x31, 0x01, 2, 0, 0, 0};
	std::vector<uint8_t> value;
	for (char name : names) {
		value.push_back(uint8_t(name));
		value.push_back(0);
		append_le(value, int32_t(2));   // FLOAT
		append_le(value, uint32_t(0));  // pLinear, reserved
		append_le(value, int32_t(1));
		append_le(value, int32_t(1));
	}
	value.push_back(0);
	append_attribute(o, "channels", "chlist", value);
	append_attribute(o, "compression", "compression", {0});

	value.clear();
	for (int32_t v : {0, 0, int32_t(width) - 1, int32_t(height) - 1})
		append_le(value, v);
	append_attribute(o, "dataWindow", "box2i", value);
	append_attribute(o, "displayWindow", "box2i", value);
	append_attribute(o, "lineOrder", "lineOrder", {0});

	value.clear();
	append_le(value, 1.f);
	append_attribute(o, "pixelAspectRatio", "float", value);
	append_attribute(o, "screenWindowWidth", "float", value);
	value.clear();
	append_le(value, 0.f);
	append_le(value, 0.f);
	append_attribute(o, "screenWindowCenter", "v2f", value);
	o.push_back(0);

	const size_t line = size_t(width) * names.size() * sizeof(float);
	const size_t table = o.size();
	for (uint32_t y = 0; y < height; y++)
		append_le(o, uint64_t(table + height * 8 + y * (8 + line)));
	for (uint32_t y = 0; y < height; y++) {
		append_le(o, int32_t(y));
		append_le(o, int32_t(line));
		for (char name : names) {
			const size_t channel = std::string_view("RGB").find(name);
			for (uint32_t x = 0; x < width; x++) {
				const size_t i = size_t(y) * width + x;
				append_le(o, name == 'A' ? alpha[i] : rgb[i * 3 + channel]);
			}
		}
	}
	return o;
}

}  // namespace test
