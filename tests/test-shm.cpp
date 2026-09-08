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

static void
test_shared_memory()
{
	char data[4096]{};
	memcpy(data, "shared", 6);
	auto owner = dawn::ipc::SharedMemory::copy(data, sizeof data);
	CHECK(owner.ok());
	auto too_large =
		dawn::ipc::SharedMemory::map(duplicate_handle(owner.handle()), 8192);
	CHECK(!too_large.ok());
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

int
main()
{
	return test::run({{"shared memory", test_shared_memory}});
}
