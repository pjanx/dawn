//
// assoc-macos.mm: Launch Services Open With (UTI from the file, not MIME)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "assoc.hpp"

#include <QFileInfo>
#include <QUrl>

using namespace std;

#import <AppKit/AppKit.h>
#import <CoreServices/CoreServices.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

namespace dn
{
namespace
{

NSURL *
file_url(const QString &path)
{
	if (path.isEmpty())
		return nil;
	return [NSURL fileURLWithPath:path.toNSString()];
}

QString
from_ns(NSString *s)
{
	return s ? QString::fromNSString(s) : QString();
}

Handler
app_from_url(NSURL *url)
{
	Handler a;
	if (!url)
		return a;
	a.id = from_ns(url.path);
	NSBundle *bundle = [NSBundle bundleWithURL:url];
	if (bundle) {
		NSString *bid = bundle.bundleIdentifier;
		if (bid.length)
			a.id = from_ns(bid);
		NSString *name =
			[bundle objectForInfoDictionaryKey:@"CFBundleDisplayName"];
		if (!name)
			name = [bundle objectForInfoDictionaryKey:@"CFBundleName"];
		a.name = from_ns(name);
		a.icon = from_ns(bundle.bundlePath);
	}
	if (a.name.isEmpty())
		a.name = from_ns(url.lastPathComponent);
	return a;
}

NSString *
uti_from_file(NSURL *url)
{
	if (!url)
		return nil;
	NSString *uti = nil;
	[url getResourceValue:&uti forKey:NSURLTypeIdentifierKey error:nil];
	if (uti.length)
		return uti;

	NSString *ext = url.pathExtension;
	if (ext.length)
		return [UTType typeWithFilenameExtension:ext].identifier;
	return nil;
}

NSURL *
app_url_for_bundle_id(NSString *bid)
{
	if (!bid.length)
		return nil;
	CFErrorRef err = nullptr;
	CFArrayRef urls = LSCopyApplicationURLsForBundleIdentifier(
		(__bridge CFStringRef) bid, &err);
	if (err)
		CFRelease(err);
	if (!urls)
		return nil;
	NSURL *url = nil;
	if (CFArrayGetCount(urls) > 0)
		url = (__bridge NSURL *) CFArrayGetValueAtIndex(urls, 0);
	NSURL *copy = url ? [url copy] : nil;
	CFRelease(urls);
	return copy;
}

}  // namespace

Handler
default_for(const QString &path)
{
	NSURL *url = file_url(path);
	if (!url)
		return {};
	CFErrorRef err = nullptr;
	CFURLRef app = LSCopyDefaultApplicationURLForURL(
		(__bridge CFURLRef) url, kLSRolesAll, &err);
	if (err)
		CFRelease(err);
	if (!app)
		return {};
	Handler a = app_from_url((__bridge NSURL *) app);
	CFRelease(app);
	return a;
}
vector<Handler>
recommended_for(const QString &path)
{
	NSURL *url = file_url(path);
	NSString *uti = uti_from_file(url);
	if (!uti)
		return {};
	CFArrayRef handlers = LSCopyAllRoleHandlersForContentType(
		(__bridge CFStringRef) uti, kLSRolesAll);
	if (!handlers)
		return {};

	const Handler def = default_for(path);
	vector<Handler> out;
	const CFIndex n = CFArrayGetCount(handlers);
	for (CFIndex i = 0; i < n; ++i) {
		NSString *bid =
			(__bridge NSString *) CFArrayGetValueAtIndex(handlers, i);
		NSURL *app_url = app_url_for_bundle_id(bid);
		Handler a = app_from_url(app_url);
		if (a.id.isEmpty()) {
			a.id = from_ns(bid);
			a.name = from_ns(bid);
		}
		if (a.id.isEmpty())
			continue;
		if (!def.id.isEmpty() && a.id == def.id)
			continue;
		out.push_back(std::move(a));
	}
	CFRelease(handlers);
	return out;
}
vector<Handler>
fallback_for(const QString &)
{
	return {};
}

bool
launch(const Handler &app, const QString &path)
{
	if (app.id.isEmpty() || path.isEmpty())
		return false;
	NSURL *file = file_url(path);
	if (!file)
		return false;

	NSURL *app_url = nil;
	NSString *ident = app.id.toNSString();
	NSBundle *bundle = [NSBundle bundleWithIdentifier:ident];
	if (bundle)
		app_url = bundle.bundleURL;
	if (!app_url)
		app_url = app_url_for_bundle_id(ident);
	if (!app_url)
		app_url = [NSURL fileURLWithPath:ident];
	if (!app_url)
		return false;

	// The launch is asynchronous, so failures can only be reported later,
	// and the caller has nothing to do with them anyway.
	[[NSWorkspace sharedWorkspace] openURLs:@[ file ]
					   withApplicationAtURL:app_url
							  configuration:[NSWorkspaceOpenConfiguration
												configuration]
						  completionHandler:nil];
	return true;
}

void
set_last_used(const Handler &, const QString &)
{
}

}  // namespace dn
