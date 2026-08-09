//
// app-menu-macos.hpp: native macOS application menu
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

namespace dn
{

class App;

#if defined(__APPLE__)
void install_macos_app_menu(App *app);
void sync_macos_app_menu(App *app);
#else
inline void install_macos_app_menu(App *) {}
inline void sync_macos_app_menu(App *) {}
#endif

}  // namespace dn
