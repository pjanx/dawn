//
// volumes-macos.mm: mounted volumes, as Finder's sidebar lists them
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "volumes.hpp"

#include <QString>

#include <cstring>

using namespace std;

#import <Foundation/Foundation.h>

#include <IOKit/IOBSD.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/storage/IOStorageDeviceCharacteristics.h>

#include <sys/mount.h>

namespace dn
{

static string
narrow(NSString *s)
{
	return s ? QString::fromNSString(s).toStdString() : string();
}

// Whether a volume sits on flash is a property of the device, several
// IORegistry hops above the partition the filesystem was mounted from.
static bool
is_device_ssd(const char *bsdname)
{
	io_service_t media = IOServiceGetMatchingService(
		kIOMainPortDefault, IOBSDNameMatching(kIOMainPortDefault, 0, bsdname));
	if (!media)
		return false;

	CFTypeRef characteristics =
		IORegistryEntrySearchCFProperty(media, kIOServicePlane,
			CFSTR(kIOPropertyDeviceCharacteristicsKey), kCFAllocatorDefault,
			kIORegistryIterateRecursively | kIORegistryIterateParents);
	IOObjectRelease(media);
	if (!characteristics)
		return false;

	bool ssd = false;
	if (CFGetTypeID(characteristics) == CFDictionaryGetTypeID()) {
		CFTypeRef medium = CFDictionaryGetValue(
			CFDictionaryRef(characteristics), CFSTR(kIOPropertyMediumTypeKey));
		ssd = medium && CFGetTypeID(medium) == CFStringGetTypeID() &&
			!CFStringCompare(CFStringRef(medium),
				CFSTR(kIOPropertyMediumTypeSolidStateKey), 0);
	}
	CFRelease(characteristics);
	return ssd;
}

// The counterpart of the drive type mapping in volumes-windows.cpp;
// drive-multidisk.svg and memory.svg have no test here, either.
static const char *
get_volume_icon(NSURL *url, NSDictionary<NSURLResourceKey, id> *values)
{
	// A volume that will not say is presumed to be a plain local disk.
	NSNumber *local = values[NSURLVolumeIsLocalKey];
	if (local && !local.boolValue)
		return "network-server-symbolic";

	struct statfs fs = {};
	if (statfs(url.fileSystemRepresentation, &fs))
		return "drive-harddisk-symbolic";

	// Neither filesystem is ever seen off optical media in practice.
	if (!strcmp(fs.f_fstypename, "cd9660") || !strcmp(fs.f_fstypename, "udf"))
		return "drive-optical-symbolic";
	if ([values[NSURLVolumeIsRemovableKey] boolValue] ||
		[values[NSURLVolumeIsEjectableKey] boolValue])
		return "drive-removable-media-symbolic";

	// Disk images and synthesised volumes have no device to ask about.
	const char prefix[] = "/dev/";
	if (!strncmp(fs.f_mntfromname, prefix, sizeof prefix - 1) &&
		is_device_ssd(fs.f_mntfromname + sizeof prefix - 1))
		return "drive-ssd-symbolic";
	return "drive-harddisk-symbolic";
}

// TODO(p): Set up a watch so that we reload on mount change.
// NSWorkspace has a notification centre of its own, which posts
// NSWorkspaceDidMountNotification, NSWorkspaceDidUnmountNotification
// and NSWorkspaceDidRenameVolumeNotification (trivially reload here).
vector<Volume>
list_volumes()
{
	// Prefetching is what keeps the loop from stat()ing each volume anew,
	// which matters for the ones that live on the other side of a network.
	NSArray<NSURLResourceKey> *keys = @[
		NSURLVolumeLocalizedNameKey,
		NSURLVolumeIsBrowsableKey,
		NSURLVolumeIsLocalKey,
		NSURLVolumeIsRemovableKey,
		NSURLVolumeIsEjectableKey,
	];

	// Skipping hidden volumes, and then the unbrowsable ones, is what leaves
	// the APFS system and data volumes out: the boot volume appears once,
	// as "/" under its localized name.
	NSFileManager *fm = NSFileManager.defaultManager;
	const NSVolumeEnumerationOptions options =
		NSVolumeEnumerationSkipHiddenVolumes;
	NSArray<NSURL *> *urls =
		[fm mountedVolumeURLsIncludingResourceValuesForKeys:keys
													options:options];

	vector<Volume> volumes;
	for (NSURL *url in urls) {
		// Unbrowsable volumes are the likes of /System/Volumes/Preboot.
		NSDictionary<NSURLResourceKey, id> *values =
			[url resourceValuesForKeys:keys error:nil];
		NSNumber *browsable = values[NSURLVolumeIsBrowsableKey];
		if (browsable && !browsable.boolValue)
			continue;

		Volume volume;
		volume.path = narrow(url.path);
		if (volume.path.empty())
			continue;

		volume.name = narrow(values[NSURLVolumeLocalizedNameKey]);
		if (volume.name.empty())
			volume.name = volume.path;

		volume.icon = get_volume_icon(url, values);
		volumes.push_back(std::move(volume));
	}
	return volumes;
}

}  // namespace dn
