//
// window-appearance-macos.hpp: native window behaviour Qt does not cover
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <functional>

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

// Qt delivers the documents a launch brought only from within the event loop,
// and says nothing about when that is over.  Call before exec(), and fn runs
// once AppKit has finished launching, after any such documents were opened.
#ifdef __APPLE__
void on_macos_launched(std::function<void()> fn);
#endif

}  // namespace dn
