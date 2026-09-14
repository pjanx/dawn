//
// window-appearance-macos.hpp: native window behaviour Qt does not cover
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

class QWindow;

namespace dn
{

// QStyleHints::setColorScheme() is process-wide, and each dn::Window keeps
// its own independent dark/light state, so the native titlebar has to be
// driven per window instead.
#ifdef __APPLE__
void sync_macos_window_appearance(QWindow *window, bool dark);
#else
inline void
sync_macos_window_appearance(QWindow *, bool)
{
}
#endif

// QWindow::requestActivate() leaves the window behind the active application.
#ifdef __APPLE__
void raise_macos_window(QWindow *window);
#else
inline void
raise_macos_window(QWindow *)
{
}
#endif

}  // namespace dn
