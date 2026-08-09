//
// orient.hpp: EXIF orientation bake-in
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

namespace dnthumbd {

/// Bake EXIF orientation into image.data (BGRA16 premul); set orientation Rotate0.
bool bake_orientation(dn::Image &image);

} // namespace dnthumbd
