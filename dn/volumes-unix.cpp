//
// volumes-unix.cpp: mounted filesystems worth showing
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <libdn/gettext.hpp>

#include "volumes.hpp"

#include <filesystem>
#include <fstream>
#include <string_view>

#ifdef __linux__
#include <mntent.h>
#else
// The BSDs want these before <sys/mount.h>, which include sorting would undo.
#include <sys/param.h>
#include <sys/ucred.h>

#include <sys/mount.h>
#endif

using namespace std;

namespace dn
{

static bool
is_network_fs(string_view type)
{
	// FUSE filesystems name their transport in the subtype.
	return type.starts_with("nfs") || type.starts_with("smb") ||
		type == "cifs" || type == "afpfs" || type == "ceph" || type == "9p" ||
		type == "davfs" || type == "fuse.sshfs";
}

// The mount table is mostly the operating system talking to itself.  What is
// left is where automounters and hands put removable media, and whatever the
// user has attached over a network, wherever they have attached it.
static bool
is_interesting(string_view dir, string_view type)
{
	return dir.starts_with("/media/") || dir.starts_with("/run/media/") ||
		is_network_fs(type);
}

#ifdef __linux__

// A partition has no queue of its own; the disk it belongs to does.
static bool
sysfs_flag(string_view device, const char *name)
{
	error_code ec;
	const filesystem::path dev = filesystem::canonical(device, ec);
	if (ec)
		return false;

	const filesystem::path base =
		filesystem::path("/sys/class/block") / dev.filename();
	int value = 0;
	ifstream input(base / name);
	if (!input.is_open())
		input.open(base / ".." / name);
	return input >> value && value;
}

#endif

// The counterpart of the drive type mapping in volumes-windows.cpp.
// Only Linux has anything to say about the device behind the mount.
static const char *
get_mount_icon(string_view device, string_view type)
{
	if (is_network_fs(type) || device.starts_with("//"))
		return "network-server-symbolic";
	if (type == "iso9660" || type == "udf")
		return "drive-optical-symbolic";

#ifdef __linux__
	if (!device.starts_with("/dev/"))
		return "drive-harddisk-symbolic";
	if (sysfs_flag(device, "removable"))
		return "drive-removable-media-symbolic";
	if (!sysfs_flag(device, "queue/rotational"))
		return "drive-ssd-symbolic";
#endif
	return "drive-harddisk-symbolic";
}

static void
push_mount(vector<Volume> &volumes, const char *dir, const char *device,
	const char *type)
{
	if (!is_interesting(dir, type))
		return;

	// Automounters name the directory after the volume label.
	volumes.push_back({dir, filesystem::path(dir).filename().string(),
		get_mount_icon(device, type)});
}

vector<Volume>
list_volumes()
{
	vector<Volume> volumes{{"/", _("Computer"), "computer-symbolic"}};

#ifdef __linux__
	// Neither this nor getmntinfo(MNT_NOWAIT) touches the filesystems
	// themselves, which a hung network mount would make us regret.
	if (FILE *fp = setmntent("/proc/self/mounts", "r")) {
		struct mntent entry = {};
		char buf[4096] = {};
		while (getmntent_r(fp, &entry, buf, sizeof buf))
			push_mount(
				volumes, entry.mnt_dir, entry.mnt_fsname, entry.mnt_type);
		endmntent(fp);
	}
#else
	struct statfs *mounts = nullptr;
	int count = getmntinfo(&mounts, MNT_NOWAIT);
	for (int i = 0; i < count; i++)
		push_mount(volumes, mounts[i].f_mntonname, mounts[i].f_mntfromname,
			mounts[i].f_fstypename);
#endif
	return volumes;
}

}  // namespace dn
