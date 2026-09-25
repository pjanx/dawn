//
// display-profile-macos.mm: display ICC via ColorSync
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <QScreen>
#include <QWindow>
#include <QtGui/qscreen_platform.h>
#include <QtLogging>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <functional>
#include <utility>

using namespace std;

namespace dn
{

static DisplayProfile
load_display_profile(QScreen *screen)
{
	DisplayProfile result;
	if (!screen)
		return result;

	auto *native = screen->nativeInterface<QNativeInterface::QCocoaScreen>();
	NSScreen *native_screen = native ? native->nativeScreen() : nil;
	NSNumber *number = native_screen.deviceDescription[@"NSScreenNumber"];
	if (!number)
		return result;

	CGColorSpaceRef color_space = CGDisplayCopyColorSpace(
		static_cast<CGDirectDisplayID>(number.unsignedIntValue));
	if (!color_space)
		return result;

	CFDataRef data = CGColorSpaceCopyICCData(color_space);
	CGColorSpaceRelease(color_space);
	if (!data)
		return result;

	const auto *bytes = CFDataGetBytePtr(data);
	const CFIndex size = CFDataGetLength(data);
	if (bytes && size > 0)
		result.icc.assign(bytes, bytes + size);
	CFRelease(data);
	if (result.icc.empty())
		return {};

	result.source = "ColorSync";
	result.label = screen->name().toUtf8().toStdString();
	qInfo("ICC source: ColorSync (%s)", result.label.c_str());
	return result;
}

namespace
{

struct CocoaSource final : DisplayProfileSource {
	function<void()> on_change;
	id observer = nil;

	~CocoaSource() override;
	void start(function<void()> fn) override;
	DisplayProfile load(QScreen *screen) override
	{
		return load_display_profile(screen);
	}
};

}  // namespace

CocoaSource::~CocoaSource()
{
	if (!this->observer)
		return;

	[[NSNotificationCenter defaultCenter] removeObserver:this->observer];
	[this->observer release];
}

void
CocoaSource::start(function<void()> fn)
{
	this->on_change = std::move(fn);
	if (this->observer)
		return;

	this->observer = [[[NSNotificationCenter defaultCenter]
		addObserverForName:NSWindowDidChangeBackingPropertiesNotification
					object:nil
					 queue:nil
				usingBlock:^(NSNotification *) {
				  this->on_change();
				}] retain];
}

unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<CocoaSource>();
}

DisplayRange
macos_display_range(QScreen *screen)
{
	DisplayRange range;
	auto *native = screen
		? screen->nativeInterface<QNativeInterface::QCocoaScreen>()
		: nullptr;
	NSScreen *native_screen = native ? native->nativeScreen() : nil;
	if (!native_screen)
		return range;

	// Non-XDR MacBooks often have potential headroom too, from backlight.
	range.hdr =
		native_screen.maximumPotentialExtendedDynamicRangeColorComponentValue >
		1;
	range.headroom =
		float(native_screen.maximumExtendedDynamicRangeColorComponentValue);
	return range;
}

// This fires at about frame rate during an EDR ramp, so it needs no poll.
shared_ptr<void>
macos_watch_screen_parameters(function<void()> fn)
{
	id observer = [[[NSNotificationCenter defaultCenter]
		addObserverForName:NSApplicationDidChangeScreenParametersNotification
					object:nil
					 queue:nil
				usingBlock:^(NSNotification *) {
				  fn();
				}] retain];
	return shared_ptr<void>(static_cast<void *>(observer), [](void *p) {
		id observer = static_cast<id>(p);
		[[NSNotificationCenter defaultCenter] removeObserver:observer];
		[observer release];
	});
}

// Qt wraps its Metal layer in a container, unless QT_MAC_NO_CONTAINER_LAYER.
void
macos_tag_for_display(QWindow *window)
{
	auto *view = reinterpret_cast<NSView *>(window->winId());
	id layer = view.layer;
	if (![layer respondsToSelector:@selector(setColorspace:)])
		layer = [layer sublayers].firstObject;
	if ([layer respondsToSelector:@selector(setColorspace:)])
		[layer setColorspace:view.window.colorSpace.CGColorSpace];
}

}  // namespace dn
