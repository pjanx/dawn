//
// ipc-loop-unix.cpp: poll-based service event loop
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-loop.hpp"

#include <cerrno>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

#include <unordered_map>
#include <vector>

using namespace std;

namespace dawn::ipc
{

struct Loop::Impl {
	struct Watch {
		int fd = -1;
		bool read = false;
		bool write = false;
	};
	unordered_map<uint64_t, Watch> watches;
	int wake_pipe[2] = {-1, -1};
};

Loop::Loop() : impl_(make_unique<Impl>())
{
#if defined __linux__
	if (pipe2(impl_->wake_pipe, O_CLOEXEC | O_NONBLOCK) != 0)
		impl_->wake_pipe[0] = impl_->wake_pipe[1] = -1;
#else
	if (pipe(impl_->wake_pipe) == 0)
		for (int fd : impl_->wake_pipe) {
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
		}
#endif
}

Loop::~Loop()
{
	for (int fd : impl_->wake_pipe)
		if (fd >= 0)
			::close(fd);
}

bool
Loop::watch_read(uint64_t id, Waitable w)
{
	auto &watch = impl_->watches[id];
	watch.fd = int(w);
	watch.read = true;
	return true;
}

void
Loop::watch_write(uint64_t id, Waitable w, bool enable)
{
	auto &watch = impl_->watches[id];
	watch.fd = int(w);
	watch.write = enable;
}

void
Loop::unwatch(uint64_t id)
{
	impl_->watches.erase(id);
}

void
Loop::wake()
{
	if (impl_->wake_pipe[1] >= 0) {
		const char byte = 0;
		(void) ::write(impl_->wake_pipe[1], &byte, 1);
	}
}

void
Loop::run()
{
	while (true) {
		vector<pollfd> fds{{impl_->wake_pipe[0], POLLIN, 0}};
		vector<uint64_t> ids{0};
		for (const auto &[id, watch] : impl_->watches) {
			short events =
				(watch.read ? POLLIN : 0) | (watch.write ? POLLOUT : 0);
			fds.push_back({watch.fd, events, 0});
			ids.push_back(id);
		}
		const int n = poll(fds.data(), fds.size(), -1);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;
		}
		if (fds[0].revents) {
			char buf[64];
			while (::read(fds[0].fd, buf, sizeof buf) > 0) {
			}
			if (on_wake)
				on_wake();
		}
		for (size_t i = 1; i < fds.size(); i++) {
			const short re = fds[i].revents;
			if ((re & (POLLIN | POLLHUP | POLLERR)) && on_read)
				on_read(ids[i]);
			if ((re & (POLLOUT | POLLERR)) && on_write &&
				impl_->watches.contains(ids[i]))
				on_write(ids[i]);
		}
	}
}

}  // namespace dawn::ipc
