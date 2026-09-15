//
// window-appearance-macos.mm: native window behaviour Qt does not cover
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "window-appearance-macos.hpp"

#include <QWindow>

#import <AppKit/AppKit.h>

using namespace std;

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

void
raise_macos_window(QWindow *window)
{
	// Activation is cooperative: this only succeeds when the active
	// application has yielded it to us, as launcher.swift does.
	if (@available(macOS 14, *))
		[NSApp activate];
	else
		[NSApp activateIgnoringOtherApps:YES];
	window->requestActivate();
}

void
on_macos_launched(function<void()> fn)
{
	if (NSRunningApplication.currentApplication.finishedLaunching) {
		fn();
		return;
	}

	// AppKit hands over the launch's documents before posting this.
	// Its NSApplicationLaunchIsDefaultLaunchKey cannot tell whether there
	// were any: it has been seen to say NO for a plain open-application event.
	NSNotificationCenter *center = NSNotificationCenter.defaultCenter;
	__block id observer = nil;
	observer = [center
		addObserverForName:NSApplicationDidFinishLaunchingNotification
					object:nil
					 queue:nil
				usingBlock:^(NSNotification *) {
					[center removeObserver:observer];
					fn();
				}];
}

}  // namespace dn
