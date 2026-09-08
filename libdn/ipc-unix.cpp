//
// ipc-unix.cpp: readiness-based IPC transport over Unix sockets
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/ucred.h>
#endif

#include <string>
#include <utility>
#include <vector>

using namespace std;

namespace dawn
{
namespace ipc
{

#ifdef __APPLE__
constexpr int kRecvFlags = 0;
constexpr int kSendFlags = 0;
#else
constexpr int kRecvFlags = MSG_CMSG_CLOEXEC;
constexpr int kSendFlags = MSG_NOSIGNAL;
#endif

void
close_handle(Handle handle)
{
	if (handle >= 0)
		::close(int(handle));
}

// Abstract name: "\0dawn-<uid>-<service>".
static string
endpoint_name(string_view service)
{
	string n;
#ifdef __APPLE__
	const char *tmp = getenv("TMPDIR");
	n = tmp && *tmp ? tmp : "/tmp/";
	if (n.back() != '/')
		n.push_back('/');
#else
	n.push_back('\0');
#endif
	n += "dawn-";
	n += to_string(::getuid());
	n += '-';
	n.append(service);
	return n;
}

static bool
fill_addr(string_view service, sockaddr_un &addr, socklen_t &len)
{
	const string n = endpoint_name(service);
	if (n.empty() || n.size() > sizeof addr.sun_path ||
		(n.front() != '\0' && n.size() == sizeof addr.sun_path))
		return false;
	addr = {};
	addr.sun_family = AF_UNIX;
	memcpy(addr.sun_path, n.data(), n.size());
	if (n.front() != '\0')
		addr.sun_path[n.size()] = '\0';
	len = socklen_t(offsetof(sockaddr_un, sun_path) + n.size());
	if (n.front() != '\0')
		len++;
	return true;
}

static bool
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

static int
unix_socket(int extra_fl)
{
	const int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (!set_flags(fd, extra_fl)) {
		::close(fd);
		return -1;
	}
#ifdef __APPLE__
	const int one = 1;
	if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
		::close(fd);
		return -1;
	}
#endif
	return fd;
}

// Rejects peers running as another user, and reports the peer's PID.
static bool
peer_ok(int fd, uint32_t &pid)
{
#ifdef __APPLE__
	xucred cred{};
	socklen_t n = sizeof cred;
	if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERCRED, &cred, &n) != 0 ||
		n != sizeof cred || cred.cr_uid != ::getuid())
		return false;
	pid_t peer = 0;
	n = sizeof peer;
	if (::getsockopt(fd, SOL_LOCAL, LOCAL_PEERPID, &peer, &n) != 0)
		return false;
	pid = uint32_t(peer);
#else
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
#endif
	return true;
}

// --- Connection --------------------------------------------------------------

struct Connection::Impl {
	int fd = -1;
	bool ok = false;
	bool eof = false;
	uint32_t peer_pid = 0;
	FrameReader reader;
	FrameWriter writer;
	vector<Handle> received;

	~Impl();
};

Connection::Impl::~Impl()
{
	for (Handle h : received)
		::close(int(h));
	for (Handle h : writer.take_all_attachments())
		::close(int(h));
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

void
Connection::set_max_payload(uint32_t limit)
{
	impl_->reader.set_limit(limit);
	impl_->writer.set_limit(limit);
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
	for (Handle h : impl_->received)
		::close(int(h));
	impl_->received.clear();
	for (Handle h : impl_->writer.take_all_attachments())
		::close(int(h));
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

	while (true) {
		const span<byte> buf = m.reader.buffer();
		iovec iov{buf.data(), buf.size()};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 63)]{};
		msghdr msg{};
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		msg.msg_control = control;
		msg.msg_controllen = sizeof control;
		const ssize_t n = ::recvmsg(m.fd, &msg, kRecvFlags);
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
		const bool truncated = msg.msg_flags & MSG_CTRUNC;
		for (cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
			if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS ||
				c->cmsg_len < CMSG_LEN(0))
				continue;
			const size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
			const auto *fds = reinterpret_cast<const int *>(CMSG_DATA(c));
			for (size_t i = 0; i < count; i++) {
				if (truncated)
					::close(fds[i]);
				else {
					(void) set_flags(fds[i], 0);
					m.received.push_back(fds[i]);
				}
			}
		}
		if (truncated) {
			close();
			return Status::Error;
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
Connection::take_plain_payload(vector<byte> &out)
{
	vector<Handle> attachments;
	if (!take_payload(out, attachments))
		return false;
	for (Handle h : attachments)
		::close(int(h));
	return true;
}

bool
Connection::take_payload(vector<byte> &out, vector<Handle> &attachments)
{
	Impl &m = *impl_;
	const size_t count = m.reader.attachment_count();
	if (m.received.size() != count) {
		close();
		return false;
	}
	if (!m.reader.take_plain_payload(out))
		return false;
	attachments.assign(
		m.received.begin(), m.received.begin() + ptrdiff_t(count));
	m.received.erase(m.received.begin(), m.received.begin() + ptrdiff_t(count));
	return true;
}

bool
Connection::write_payload(
	span<const byte> payload, span<const Handle> attachments)
{
	Impl &m = *impl_;
	if (!m.ok)
		return false;
	vector<Handle> owned;
	for (Handle handle : attachments) {
		const int copy = fcntl(int(handle), F_DUPFD_CLOEXEC, 0);
		if (copy < 0) {
			for (Handle h : owned)
				::close(int(h));
			return false;
		}
		owned.push_back(copy);
	}
	if (m.writer.push(payload, owned))
		return true;
	for (Handle h : owned)
		::close(int(h));
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
		const span<const Handle> attachments = m.writer.pending_attachments();
		iovec iov{const_cast<byte *>(p.data()), p.size()};
		alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 63)]{};
		msghdr msg{};
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;
		if (!attachments.empty()) {
			msg.msg_control = control;
			msg.msg_controllen =
				socklen_t(CMSG_SPACE(sizeof(int) * attachments.size()));
			cmsghdr *c = CMSG_FIRSTHDR(&msg);
			c->cmsg_level = SOL_SOCKET;
			c->cmsg_type = SCM_RIGHTS;
			c->cmsg_len = socklen_t(CMSG_LEN(sizeof(int) * attachments.size()));
			auto *fds = reinterpret_cast<int *>(CMSG_DATA(c));
			for (size_t i = 0; i < attachments.size(); i++)
				fds[i] = int(attachments[i]);
		}
		const ssize_t n = ::sendmsg(m.fd, &msg, kSendFlags);
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
		if (!attachments.empty()) {
			for (Handle h : attachments)
				::close(int(h));
			m.writer.attachments_sent();
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
	while (true) {
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
	string path;

	~Impl();
};

Listener::Impl::~Impl()
{
	if (fd >= 0)
		::close(fd);
	if (!path.empty())
		::unlink(path.c_str());
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
	if (!impl_->path.empty()) {
		::unlink(impl_->path.c_str());
		impl_->path.clear();
	}
}

Connection
Listener::accept()
{
	if (impl_->fd < 0)
		return {};

	while (true) {
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
#ifdef __APPLE__
		const int one = 1;
		if (::setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one) != 0) {
			::close(fd);
			continue;
		}
#endif

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

	while (true) {
		if (::bind(fd, (sockaddr *) &addr, len) == 0)
			break;
		if (errno == EINTR)
			continue;
		const int e = errno;
		trace("bind failed: %s", strerror(e));
#ifdef __APPLE__
		if (e == EADDRINUSE) {
			const int probe = unix_socket(0);
			const int result =
				probe < 0 ? -1 : ::connect(probe, (sockaddr *) &addr, len);
			const int connect_error = errno;
			if (probe >= 0)
				::close(probe);
			if (result != 0 && connect_error == ECONNREFUSED) {
				::unlink(addr.sun_path);
				continue;
			}
		}
#endif
		::close(fd);
		if (e == EADDRINUSE)
			out.status = ListenStatus::InUse;
		return out;
	}

	while (true) {
		if (::listen(fd, SOMAXCONN) == 0)
			break;
		if (errno == EINTR)
			continue;
		::close(fd);
		return out;
	}

	out.listener.impl_->fd = fd;
#ifdef __APPLE__
	out.listener.impl_->path = addr.sun_path;
#endif
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

	while (true) {
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
}  // namespace dawn
