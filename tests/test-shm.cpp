//
// test-shm.cpp: anonymous shared-memory tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/ipc-shm.hpp"
#include "test.hpp"

#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

using namespace std;

static dawn::ipc::Handle
duplicate_handle(dawn::ipc::Handle handle)
{
#ifdef _WIN32
	HANDLE copy = nullptr;
	DuplicateHandle(GetCurrentProcess(), (HANDLE) handle, GetCurrentProcess(),
		&copy, FILE_MAP_READ, FALSE, 0);
	return dawn::ipc::Handle(copy);
#else
	return dup(int(handle));
#endif
}

// Rejected mappings consume the handle; ask the platform if it is gone.
static bool
handle_is_open(dawn::ipc::Handle handle)
{
#ifdef _WIN32
	DWORD flags = 0;
	return GetHandleInformation((HANDLE) handle, &flags);
#else
	return fcntl(int(handle), F_GETFD) != -1;
#endif
}

static void
test_shared_memory()
{
	char data[4096]{};
	memcpy(data, "shared", 6);
	auto owner = dawn::ipc::SharedMemory::copy(data, sizeof data);
	CHECK(owner.ok());
	const dawn::ipc::Handle spare = duplicate_handle(owner.handle());
	CHECK(handle_is_open(spare));
	CHECK(!dawn::ipc::SharedMemory::map(spare, 8192).ok());
	CHECK(!handle_is_open(spare));
#ifdef __linux__
	errno = 0;
	CHECK(ftruncate(int(owner.handle()), 2048) != 0);
	CHECK(errno == EPERM);
	void *writable =
		mmap(nullptr, 4096, PROT_WRITE, MAP_SHARED, int(owner.handle()), 0);
	CHECK(writable == MAP_FAILED);
#endif
	auto reader =
		dawn::ipc::SharedMemory::map(duplicate_handle(owner.handle()), 4096);
	CHECK(reader.ok() && memcmp(reader.data(), "shared", 6) == 0);
}

static void
test_empty()
{
	char data[4096]{};
	auto owner = dawn::ipc::SharedMemory::copy(data, sizeof data);
	CHECK(owner.ok());

	// Nothing to share, nothing to map it with, and nothing to map.
	CHECK(!dawn::ipc::SharedMemory::copy(data, 0).ok());
	CHECK(!dawn::ipc::SharedMemory::map(dawn::ipc::kInvalidHandle, 4096).ok());

	const dawn::ipc::Handle spare = duplicate_handle(owner.handle());
	CHECK(handle_is_open(spare));
	CHECK(!dawn::ipc::SharedMemory::map(spare, 0).ok());
	CHECK(!handle_is_open(spare));
}

int
main()
{
	return test::run({
		{"shared memory", test_shared_memory},
		{"empty mappings", test_empty},
	});
}
