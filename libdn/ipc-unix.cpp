//
// ipc-unix.cpp: Linux abstract-namespace IPC endpoint
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>

namespace dn {
namespace ipc {
namespace {

int
unix_socket(int extra_fl)
{
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	const int fdfl = ::fcntl(fd, F_GETFD, 0);
	if (fdfl < 0 || ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0) {
		::close(fd);
		return -1;
	}
	if (extra_fl != 0) {
		const int fl = ::fcntl(fd, F_GETFL, 0);
		if (fl < 0 || ::fcntl(fd, F_SETFL, fl | extra_fl) < 0) {
			::close(fd);
			return -1;
		}
	}
	return fd;
}

bool
fill_addr(std::string_view service, sockaddr_un &addr, socklen_t &len)
{
	const std::string n = Endpoint::name(service);
	if (n.empty() || n.size() > sizeof(addr.sun_path))
		return false;
	addr = {};
	addr.sun_family = AF_UNIX;
	std::memcpy(addr.sun_path, n.data(), n.size());
	len = socklen_t(offsetof(sockaddr_un, sun_path) + n.size());
	return true;
}

bool
set_accept_flags(int fd)
{
	const int fdfl = ::fcntl(fd, F_GETFD, 0);
	if (fdfl < 0 || ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0)
		return false;
	const int fl = ::fcntl(fd, F_GETFL, 0);
	return fl >= 0 && ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) == 0;
}

bool
peer_uid_ok(int fd)
{
	struct {
		pid_t pid;
		uid_t uid;
		gid_t gid;
	} cred{};
	socklen_t n = sizeof(cred);
	return ::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &n) == 0 &&
		n == sizeof(cred) && cred.uid == ::getuid();
}

}  // namespace

std::string
Endpoint::name(std::string_view service)
{
	std::string n;
	n.push_back('\0');
	n += "dawn-";
	n += std::to_string(::getuid());
	n += '-';
	n.append(service);
	return n;
}

Endpoint::Listen
Endpoint::listen(std::string_view service)
{
	Listen out;
	sockaddr_un addr{};
	socklen_t len = 0;
	if (!fill_addr(service, addr, len))
		return out;

	const int fd = unix_socket(O_NONBLOCK);
	if (fd < 0)
		return out;

	for (;;) {
		if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), len) == 0)
			break;
		if (errno == EINTR)
			continue;
		const int e = errno;
		::close(fd);
		if (e == EADDRINUSE)
			out.status = ListenStatus::InUse;
		return out;
	}

	for (;;) {
		if (::listen(fd, SOMAXCONN) == 0)
			break;
		if (errno == EINTR)
			continue;
		::close(fd);
		return out;
	}

	out.fd = fd;
	out.status = ListenStatus::Ok;
	return out;
}

Endpoint::Connect
Endpoint::connect(std::string_view service)
{
	Connect out;
	sockaddr_un addr{};
	socklen_t len = 0;
	if (!fill_addr(service, addr, len))
		return out;

	const int fd = unix_socket(0);
	if (fd < 0)
		return out;

	for (;;) {
		if (::connect(fd, reinterpret_cast<sockaddr *>(&addr),
			len) == 0)
			break;
		if (errno == EINTR)
			continue;
		const int e = errno;
		::close(fd);
		if (e == ECONNREFUSED || e == ENOENT)
			out.status = ConnectStatus::Refused;
		return out;
	}

	if (!peer_uid_ok(fd)) {
		::close(fd);
		return out;
	}

	out.fd = fd;
	out.status = ConnectStatus::Ok;
	return out;
}

int
Endpoint::accept(int listen_fd)
{
	if (listen_fd < 0)
		return -1;

	int fd = -1;
	for (;;) {
		fd = ::accept(listen_fd, nullptr, nullptr);
		if (fd >= 0)
			break;
		if (errno == EINTR)
			continue;
		return -1;
	}

	if (!set_accept_flags(fd) || !peer_uid_ok(fd)) {
		::close(fd);
		return -1;
	}
	return fd;
}

}  // namespace ipc
}  // namespace dn
