//
// cie-diagram.hpp: CIE 1931 xy sidebar widget
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit.hpp"

#include <libdn.h>

namespace dn
{

struct CieDiagram : Widget {
	Chromaticities image{};
	Chromaticities screen{};
	bool show_screen = false;
	bool screen_dashed = false;
	bool image_dashed = false;

	void measure(Kit &kit, float max_w, float max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void prepare(Kit &kit) override;
	void paint(Kit &kit) const override;

private:
	Kit::Packed slot_{};
	uint32_t epoch_ = 0;
	Chromaticities packed_image_{};
	Chromaticities packed_screen_{};
	bool packed_show_screen_ = false;
	bool packed_screen_dashed_ = false;
	bool packed_image_dashed_ = false;
};

}  // namespace dn
