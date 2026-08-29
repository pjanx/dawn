//
// window-appearance-macos.mm: sync a window's native titlebar to dark mode
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "window-appearance-macos.hpp"

#include <QWindow>

#import <AppKit/AppKit.h>

namespace dn
{

void
sync_macos_window_appearance(QWindow *window, bool dark)
{
	if (!window)
		return;

	// QWindow::winId() is documented to return the backing NSView* on Cocoa.
	auto *view = reinterpret_cast<NSView *>(window->winId());
	if (!view.window)
		return;

	view.window.appearance = dark
		? [NSAppearance appearanceNamed:NSAppearanceNameDarkAqua]
		: [NSAppearance appearanceNamed:NSAppearanceNameAqua];
}

}  // namespace dn
