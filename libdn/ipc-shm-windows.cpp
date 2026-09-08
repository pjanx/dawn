//
// ipc-shm-windows.cpp: Windows anonymous shared pixel storage
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-shm.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <cstring>

using namespace std;

namespace dawn::ipc
{

SharedMemory
SharedMemory::copy(const void *data, size_t size)
{
	SharedMemory out;
	const HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
		PAGE_READWRITE, DWORD(uint64_t(size) >> 32), DWORD(size), nullptr);
	if (!h)
		return out;
	void *p = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, size);
	if (!p) {
		CloseHandle(h);
		return out;
	}
	memcpy(p, data, size);
	out.handle_ = (Handle) h;
	out.data_ = p;
	out.size_ = size;
	return out;
}

SharedMemory
SharedMemory::map(Handle handle, size_t size)
{
	SharedMemory out;
	void *p = MapViewOfFile((HANDLE) handle, FILE_MAP_READ, 0, 0, size);
	if (!p) {
		close_handle(handle);
		return out;
	}
	out.handle_ = handle;
	out.data_ = p;
	out.size_ = size;
	return out;
}

void
SharedMemory::close()
{
	if (data_)
		UnmapViewOfFile(data_);
	close_handle(handle_);
	handle_ = kInvalidHandle;
	data_ = nullptr;
	size_ = 0;
}

}  // namespace dawn::ipc
