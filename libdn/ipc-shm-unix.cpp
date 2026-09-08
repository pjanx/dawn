//
// ipc-shm-unix.cpp: POSIX anonymous shared pixel storage
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-shm.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined __linux__
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

#include <cstdlib>
#include <string>

using namespace std;

namespace dawn::ipc
{

static int
create_fd()
{
#if defined __linux__
	return int(syscall(
		SYS_memfd_create, "dawn-pixels", MFD_CLOEXEC | MFD_ALLOW_SEALING));
#elif defined SHM_ANON
	return shm_open(SHM_ANON, O_RDWR | O_CLOEXEC, 0600);
#else
	for (unsigned i = 0; i < 100; i++) {
		const string name = "/dawn-" + to_string(getuid()) + "-" +
			to_string(unsigned(arc4random()));
		const int fd = shm_open(name.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
		if (fd >= 0) {
			shm_unlink(name.c_str());
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			return fd;
		}
		if (errno != EEXIST)
			break;
	}
	return -1;
#endif
}

SharedMemory
SharedMemory::copy(const void *data, size_t size)
{
	SharedMemory out;
	if (!size)
		return out;
	const int fd = create_fd();
	if (fd < 0 || ftruncate(fd, off_t(size)) != 0) {
		if (fd >= 0)
			::close(fd);
		return out;
	}
	void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (p == MAP_FAILED) {
		::close(fd);
		return out;
	}
	memcpy(p, data, size);
	out.handle_ = fd;
	out.data_ = p;
	out.size_ = size;

#if defined __linux__
	munmap(out.data_, out.size_);
	out.data_ = nullptr;
	if (fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_SEAL) != 0)
		return out;
	out.data_ = mmap(nullptr, size, PROT_READ, MAP_SHARED, fd, 0);
	if (out.data_ == MAP_FAILED)
		out.data_ = nullptr;
#endif
	return out;
}

SharedMemory
SharedMemory::map(Handle handle, size_t size)
{
	SharedMemory out;
	if (handle < 0 || !size) {
		close_handle(handle);
		return out;
	}
	struct stat st{};
	if (fstat(int(handle), &st) != 0 || st.st_size < 0 ||
		uint64_t(st.st_size) < size) {
		close_handle(handle);
		return out;
	}
	void *p = mmap(nullptr, size, PROT_READ, MAP_SHARED, int(handle), 0);
	if (p == MAP_FAILED) {
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
		munmap(data_, size_);
	close_handle(handle_);
	handle_ = kInvalidHandle;
	data_ = nullptr;
	size_ = 0;
}

}  // namespace dawn::ipc
