//
// kit-commander.hpp: orthodox file manager subapplication
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit-chrome.hpp"

namespace dn
{

struct Commander : Widget {
	QUrl dir_url_;
	Commander() { hittable = true; }
	Size measure_content(Kit &, int max_w, int max_h) override
	{
		return {max_w, max_h};
	}
	void arrange_content(Kit &, Rect alloc) override { r = alloc; }
};

std::unique_ptr<Page> make_commander_page(
	Kit &kit, const HostActions &host, Commander **out);

}  // namespace dn
