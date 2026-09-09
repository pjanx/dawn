//
// kit-crop-jpeg.cpp: JPEG Cropper subapplication
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "kit-crop-jpeg.hpp"

using namespace std;

namespace dn
{

unique_ptr<Page>
make_crop_jpeg_page(Kit &kit, const HostActions &host, Cropper **out)
{
	auto content = make_unique<Cropper>();
	if (out)
		*out = content.get();

	PageSetup setup;
	setup.mode = Mode::CropJpeg;
	setup.content = std::move(content);
	setup.toolbar = make_toolbar({}, {});
	setup.actor = chain_actor(
		host, {}, [](Action a) { return a != Action::Reload; },
		[&kit](Action a) {
			return a == Action::DarkMode
				? kit.dark_
				: a == Action::Fullscreen && kit.fullscreen_;
		});
	return make_page(kit, host, std::move(setup));
}

}  // namespace dn
