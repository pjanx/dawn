//
// display-profile-macos.mm: display ICC via ColorSync
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <QScreen>
#include <QtGui/qscreen_platform.h>

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>

#include <cstdio>
#include <functional>
#include <utility>

using namespace std;

namespace dn
{
namespace
{

DisplayProfile
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
	printf("ICC source: ColorSync (%s)\n", result.label.c_str());
	return result;
}

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

}  // namespace
unique_ptr<DisplayProfileSource>
make_display_profile_source()
{
	return make_unique<CocoaSource>();
}

}  // namespace dn
