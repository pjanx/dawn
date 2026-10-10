//
// kit-cie-diagram.hpp: CIE 1931 xy sidebar widget
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit.hpp"

#include "libdn/libdn.hpp"

namespace dn
{

struct CieDiagram : Widget {
	// After changing any of these, or the screen's colour, call redraw().
	dawn::Chromaticities image{};
	dawn::Chromaticities screen{};
	bool show_screen = false;
	bool screen_dashed = false;
	bool image_dashed = false;

	// Drops the raster, to be made anew once the diagram is next arranged.
	void redraw(Kit &kit);

	Size measure_content(Kit &kit, int max_w, int max_h) override;
	void arrange_content(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;

private:
	Kit::Packed slot_{};
	// The fonts of the atlas that slot_ is in; new fonts rebuild the atlas.
	uint64_t epoch_ = 0;
};

}  // namespace dn
