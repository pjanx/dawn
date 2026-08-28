//
// libdnvk.h: Vulkan scaler API
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dawn {

/// Soft cap on device-local bytes for one scale() (tiles + mid + dest).
/// Tweak in source; no CLI yet.
inline constexpr uint64_t kMaxDeviceBytes = 4ull << 30;

} // namespace dawn

#include "scale-engine.hpp"

namespace dawn {

struct ScaleOutput {
	uint32_t width = 0;
	uint32_t height = 0;
	/// Straight (non-premultiplied) RGBA8, stride = width * 4.
	std::vector<uint8_t> rgba8;
};

/// Headless Vulkan H→V scaler. `scale()` is internally mutex-serialized.
class ScaleScaler {
	struct Impl;
	Impl *impl_ = nullptr;

public:
	ScaleScaler();
	~ScaleScaler();

	ScaleScaler(const ScaleScaler &) = delete;
	ScaleScaler &operator=(const ScaleScaler &) = delete;

	bool init(std::string *error = nullptr);
	void destroy();

	/// `pixels` is BGRA_PREMUL_4X16LE; `stride` bytes per row.
	/// Scales to exactly `out_w`×`out_h` with sRGB-aware filtering.
	bool scale(uint32_t src_w, uint32_t src_h, const uint8_t *pixels,
		size_t stride, uint32_t out_w, uint32_t out_h, ScaleOutput *out,
		std::string *error = nullptr);
};

} // namespace dawn
