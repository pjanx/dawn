//
// volumes-windows.cpp: logical drives
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "volumes.hpp"

#include <QString>

#include <windows.h>

using namespace std;

namespace dn
{

static bool
is_drive_ssd(wchar_t letter)
{
	wchar_t path[] = {L'\\', L'\\', L'.', L'\\', letter, L':', 0};

	HANDLE h = CreateFileW(path,
		0,  // no access rights needed
		FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
	if (h == INVALID_HANDLE_VALUE)
		return false;

	STORAGE_PROPERTY_QUERY query = {};
	query.PropertyId = StorageDeviceSeekPenaltyProperty;
	query.QueryType = PropertyStandardQuery;

	DEVICE_SEEK_PENALTY_DESCRIPTOR result = {};
	DWORD bytesReturned = 0;

	BOOL ok = DeviceIoControl(h, IOCTL_STORAGE_QUERY_PROPERTY, &query,
		sizeof query, &result, sizeof result, &bytesReturned, nullptr);
	CloseHandle(h);

	return ok && bytesReturned >= sizeof result && !result.IncursSeekPenalty;
}

// It's not entirely clear how to map here, so we'll do our best.
//
// x drive-harddisk.svg          HDD        DRIVE_FIXED, default
// x drive-optical.svg           CD/DVD     DRIVE_CDROM
//   drive-multidisk.svg         RAID/NAS?  BusType?
// x drive-removable-media.svg   pen drive  DRIVE_REMOVABLE
// x drive-ssd.svg               SSD        DRIVE_FIXED + seek penalty
//   memory.svg                  ramdisk    DRIVE_RAMDISK (rare!)
// x network-server.svg          remote     DRIVE_REMOTE
//
// This will eventually need to be part of the VFS API.
static const char *
get_drive_icon(wchar_t letter)
{
	wchar_t root[] = {letter, ':', '\\', 0};
	switch (GetDriveTypeW(root)) {
	case DRIVE_CDROM:
		return "drive-optical-symbolic";
	case DRIVE_REMOVABLE:
		return "drive-removable-media-symbolic";
	case DRIVE_REMOTE:
		return "network-server-symbolic";
	case DRIVE_FIXED:
		if (is_drive_ssd(letter))
			return "drive-ssd-symbolic";
		// Fall-through
	default:
		return "drive-harddisk-symbolic";
	}
}

static wstring
get_drive_label(const wchar_t *root)
{
	wchar_t buf[33] = {};
	if (!GetVolumeInformationW(root, buf, sizeof buf / sizeof *buf, nullptr,
			nullptr, nullptr, nullptr, 0) ||
		!*buf)
		return root;
	return buf;
}

static string
narrow(const wstring &w)
{
	return QString::fromStdWString(w).toStdString();
}

// TODO(p): Set up a watch so that we reload on drive change.
// The window should receive WM_DEVICECHANGE:
// respond to DBT_DEVICEARRIVAL, DBT_DEVICEREMOVECOMPLETE,
// and/or maybe just DBT_DEVNODES_CHANGED (trivially reload here).
vector<Volume>
list_volumes()
{
	vector<Volume> volumes;
	DWORD mask = GetLogicalDrives();
	for (int i = 0; i < 26; i++) {
		wchar_t letter = wchar_t(L'A' + i);
		if (!(mask & (1 << i)))
			continue;

		wchar_t drive[] = {letter, L':', L'\\', 0};
		volumes.push_back({narrow(drive), narrow(get_drive_label(drive)),
			get_drive_icon(letter)});
	}
	return volumes;
}

}  // namespace dn
