//
// ipc-unix.cpp: readiness-based IPC transport over Unix sockets
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include <cerrno>
#include <cstddef>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#include <string>
#include <utility>

using namespace std;

namespace dn
{
namespace ipc
{
namespace
{

// Abstract name: "\0dawn-<uid>-<service>".
string
endpoint_name(string_view service)
{
	string n;
	n.push_back('\0');
	n += "dawn-";
	n += to_string(::getuid());
	n += '-';
	n.append(service);
	return n;
}

bool
fill_addr(string_view service, sockaddr_un &addr, socklen_t &len)
{
	const string n = endpoint_name(service);
	if (n.empty() || n.size() > sizeof addr.sun_path)
		return false;
	addr = {};
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, n.data(), n.size());
	len = socklen_t(offsetof(sockaddr_un, sun_path) + n.size());
	return true;
}

bool
set_flags(int fd, int extra_fl)
{
	const int fdfl = ::fcntl(fd, F_GETFD, 0);
	if (fdfl < 0 || ::fcntl(fd, F_SETFD, fdfl | FD_CLOEXEC) < 0)
		return false;
	if (extra_fl == 0)
		return true;
	const int fl = ::fcntl(fd, F_GETFL, 0);
	return fl >= 0 && ::fcntl(fd, F_SETFL, fl | extra_fl) == 0;
}

int
unix_socket(int extra_fl)
{
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (!set_flags(fd, extra_fl)) {
		::close(fd);
		return -1;
	}
	return fd;
}

// Rejects peers running as another user, and reports the peer's PID.
bool
peer_ok(int fd, uint32_t &pid)
{
	struct {
		pid_t pid;
		uid_t uid;
		gid_t gid;
	} cred{};
	socklen_t n = sizeof cred;
	if (::getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &n) != 0 ||
		n != sizeof cred || cred.uid != ::getuid())
		return false;
	pid = uint32_t(cred.pid);
	return true;
}

}  // namespace

// --- Connection --------------------------------------------------------------

struct Connection::Impl {
	int fd = -1;
	bool ok = false;
	bool eof = false;
	uint32_t peer_pid = 0;
	FrameReader reader;
	FrameWriter writer;

	~Impl();
};

Connection::Impl::~Impl()
{
	if (fd >= 0)
		::close(fd);
}

Connection::Connection() : impl_(make_unique<Impl>())
{
}

Connection::Connection(Handle h) : impl_(make_unique<Impl>())
{
	const int fd = int(h);
	if (fd < 0)
		return;
	if (!set_flags(fd, O_NONBLOCK)) {
		::close(fd);
		return;
	}
	impl_->fd = fd;
	impl_->ok = true;
}

Connection::~Connection() = default;

Connection::Connection(Connection &&) noexcept = default;
Connection &Connection::operator=(Connection &&) noexcept = default;

bool
Connection::ok() const
{
	return impl_->ok;
}

bool
Connection::wants_write() const
{
	return impl_->ok && !impl_->writer.empty();
}

Waitable
Connection::read_waitable() const
{
	return impl_->fd;
}

Waitable
Connection::write_waitable() const
{
	return impl_->fd;
}

uint32_t
Connection::peer_pid() const
{
	return impl_->peer_pid;
}

void
Connection::close()
{
	if (impl_->fd >= 0) {
		::close(impl_->fd);
		impl_->fd = -1;
	}
	impl_->ok = false;
	impl_->writer.clear();
}

Connection::Status
Connection::read()
{
	Impl &m = *impl_;
	if (!m.ok)
		return m.eof ? Status::Eof : Status::Error;
	if (m.reader.ready())
		return Status::Frame;

	for (;;) {
		const span<byte> buf = m.reader.buffer();
		const ssize_t n = ::read(m.fd, buf.data(), buf.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return Status::NeedMore;
			close();
			return Status::Error;
		}
		if (n == 0) {
			const bool clean = m.reader.idle();
			m.eof = clean;
			close();
			return clean ? Status::Eof : Status::Error;
		}
		switch (m.reader.advance(size_t(n))) {
		case FrameReader::Status::NeedMore:
			break;
		case FrameReader::Status::Frame:
			return Status::Frame;
		default:
			close();
			return Status::Error;
		}
	}
}

bool
Connection::take_payload(vector<byte> &out)
{
	return impl_->reader.take_payload(out);
}

bool
Connection::write_payload(span<const byte> payload)
{
	Impl &m = *impl_;
	if (!m.ok)
		return false;
	if (m.writer.push(payload))
		return true;
	close();
	return false;
}

bool
Connection::flush()
{
	Impl &m = *impl_;
	if (!m.ok)
		return false;
	while (!m.writer.empty()) {
		const span<const byte> p = m.writer.pending();
		const ssize_t n = ::write(m.fd, p.data(), p.size());
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			close();
			return false;
		}
		if (n == 0) {
			close();
			return false;
		}
		m.writer.consume(size_t(n));
	}
	return true;
}

Connection::Ready
Connection::wait(Direction dir, int timeout_ms)
{
	Impl &m = *impl_;
	if (!m.ok || m.fd < 0)
		return Ready::Fail;

	const short events = dir == Direction::Write ? POLLOUT : POLLIN;
	for (;;) {
		pollfd pfd{};
		pfd.fd = m.fd;
		pfd.events = events;
		const int n = ::poll(&pfd, 1, timeout_ms < 0 ? 0 : timeout_ms);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return Ready::Fail;
		}
		if (n == 0)
			return Ready::Timeout;
		if (pfd.revents & (POLLERR | POLLNVAL))
			return Ready::Fail;
		if (pfd.revents & events)
			return Ready::Ok;
		// A half-closed peer is reported to the reader as EOF.
		if ((pfd.revents & POLLHUP) && dir == Direction::Read)
			return Ready::Ok;
		return Ready::Fail;
	}
}

// --- Listener ----------------------------------------------------------------

struct Listener::Impl {
	int fd = -1;

	~Impl();
};

Listener::Impl::~Impl()
{
	if (fd >= 0)
		::close(fd);
}

Listener::Listener() : impl_(make_unique<Impl>())
{
}

Listener::~Listener() = default;

Listener::Listener(Listener &&) noexcept = default;
Listener &Listener::operator=(Listener &&) noexcept = default;

bool
Listener::ok() const
{
	return impl_->fd >= 0;
}

Waitable
Listener::waitable() const
{
	return impl_->fd;
}

void
Listener::close()
{
	if (impl_->fd >= 0) {
		::close(impl_->fd);
		impl_->fd = -1;
	}
}

Connection
Listener::accept()
{
	if (impl_->fd < 0)
		return {};

	for (;;) {
		const int fd = ::accept(impl_->fd, nullptr, nullptr);
		if (fd < 0) {
			if (errno == EINTR)
				continue;
			return {};
		}

		uint32_t pid = 0;
		if (!set_flags(fd, O_NONBLOCK) || !peer_ok(fd, pid)) {
			::close(fd);
			continue;
		}

		Connection conn(fd);
		conn.impl_->peer_pid = pid;
		return conn;
	}
}

// --- Endpoint ----------------------------------------------------------------

Endpoint::Listen
Endpoint::listen(string_view service)
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
		if (::bind(fd, (sockaddr *) &addr, len) == 0)
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

	out.listener.impl_->fd = fd;
	out.status = ListenStatus::Ok;
	return out;
}

Endpoint::Connect
Endpoint::connect(string_view service)
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
		if (::connect(fd, (sockaddr *) &addr, len) == 0)
			break;
		if (errno == EINTR)
			continue;
		const int e = errno;
		::close(fd);
		if (e == ECONNREFUSED || e == ENOENT)
			out.status = ConnectStatus::Refused;
		return out;
	}

	uint32_t pid = 0;
	if (!peer_ok(fd, pid)) {
		::close(fd);
		return out;
	}

	out.conn = Connection(fd);
	if (!out.conn.ok())
		return out;
	out.conn.impl_->peer_pid = pid;
	out.status = ConnectStatus::Ok;
	return out;
}

}  // namespace ipc
}  // namespace dn
