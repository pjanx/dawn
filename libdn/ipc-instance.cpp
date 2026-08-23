//
// ipc-instance.cpp: instance-service blocking client and server
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-instance.hpp"

#include <cerrno>
#include <climits>
#include <poll.h>
#include <unistd.h>

#include <utility>

using namespace std;

namespace dn
{
namespace ipc
{
namespace
{

constexpr char kService[] = "instance";

enum class Wait : uint8_t { Ok, Timeout, Fail };

void
consume_elapsed(chrono::milliseconds &left, chrono::milliseconds dt)
{
	if (dt >= left)
		left = chrono::milliseconds{0};
	else
		left -= dt;
}

Wait
wait_fd(int fd, short events, chrono::milliseconds &left)
{
	using clock = chrono::steady_clock;
	if (fd < 0)
		return Wait::Fail;
	for (;;) {
		if (left.count() < 0)
			left = chrono::milliseconds{0};
		pollfd pfd{};
		pfd.fd = fd;
		pfd.events = events;
		int ms = 0;
		if (left.count() > INT_MAX)
			ms = INT_MAX;
		else
			ms = int(left.count());
		const auto t0 = clock::now();
		const int n = ::poll(&pfd, 1, ms);
		consume_elapsed(left,
			chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			return Wait::Fail;
		}
		if (n == 0)
			return Wait::Timeout;
		if (pfd.revents & (POLLERR | POLLNVAL))
			return Wait::Fail;
		if (pfd.revents & events)
			return Wait::Ok;
		if ((pfd.revents & POLLHUP) && (events & POLLIN))
			return Wait::Ok;
		return Wait::Fail;
	}
}

bool
flush_deadline(Connection &conn, chrono::milliseconds &left)
{
	if (conn.flush())
		return true;
	if (!conn.ok())
		return false;
	while (conn.wants_write()) {
		if (wait_fd(conn.fd(), POLLOUT, left) != Wait::Ok)
			return false;
		if (conn.flush())
			return true;
		if (!conn.ok())
			return false;
	}
	return conn.ok();
}

bool
write_bytes(
	Connection &conn, span<const byte> payload, chrono::milliseconds &left)
{
	if (!conn.write_payload(payload))
		return false;
	return flush_deadline(conn, left);
}

bool
read_one_frame(Connection &conn, vector<byte> &out, chrono::milliseconds &left)
{
	for (;;) {
		switch (conn.read()) {
		case Connection::Status::Frame:
			return conn.take_payload(out);
		case Connection::Status::NeedMore:
			if (wait_fd(conn.fd(), POLLIN, left) != Wait::Ok)
				return false;
			break;
		default:
			return false;
		}
	}
}

bool
encode_frame(const instance::Frame &frame, vector<byte> &buf)
{
	buf.clear();
	Encoder enc(buf);
	encode(frame, enc);
	return enc.ok() && !buf.empty();
}

void
set_internal(instance::Error *error)
{
	if (!error)
		return;
	error->code = instance::ErrorCode::Internal;
	error->message.clear();
}
optional<BlockingClient>
fail_hello(BlockingClient::HelloStatus *status, BlockingClient::HelloStatus s)
{
	if (status)
		*status = s;
	return nullopt;
}

}  // namespace

BlockingClient::BlockingClient(Connection conn) : conn_(std::move(conn))
{
}

BlockingClient::BlockingClient(BlockingClient &&) noexcept = default;
BlockingClient &BlockingClient::operator=(BlockingClient &&) noexcept = default;
BlockingClient::~BlockingClient() = default;
optional<BlockingClient>
BlockingClient::connect(string_view session, HelloStatus *status,
	chrono::milliseconds timeout)
{
	using clock = chrono::steady_clock;
	chrono::milliseconds left = timeout;
	if (left.count() < 0)
		left = chrono::milliseconds{0};

	const auto t0 = clock::now();
	const Endpoint::Connect ep = Endpoint::connect(kService);
	consume_elapsed(
		left, chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
	if (ep.status != Endpoint::ConnectStatus::Ok || ep.fd < 0)
		return fail_hello(status, HelloStatus::Unavailable);

	Connection conn(ep.fd);
	if (!conn.ok())
		return fail_hello(status, HelloStatus::Unavailable);

	instance::Hello hello;
	hello.protocol_version = uint32_t(instance::kInstanceProtocolVersion);
	hello.session = string(session);

	instance::Frame hello_frame;
	hello_frame.payload.value = instance::PayloadHello{std::move(hello)};
	vector<byte> buf;
	if (!encode_frame(hello_frame, buf) || !write_bytes(conn, buf, left))
		return fail_hello(status, HelloStatus::Unavailable);

	if (!read_one_frame(conn, buf, left))
		return fail_hello(status, HelloStatus::Unavailable);

	Decoder dec(buf);
	instance::FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0)
		return fail_hello(status, HelloStatus::Unavailable);

	const auto *reply =
		get_if<instance::PayloadHelloReplyView>(&view.payload.value);
	if (!reply)
		return fail_hello(status, HelloStatus::Unavailable);

	const auto &hr = reply->hello_reply.value;
	if (holds_alternative<instance::HelloReplyAcceptedView>(hr)) {
		if (status)
			*status = HelloStatus::Ok;
		return BlockingClient(std::move(conn));
	}
	if (holds_alternative<instance::HelloReplyVersionMismatchView>(hr))
		return fail_hello(status, HelloStatus::VersionMismatch);
	if (holds_alternative<instance::HelloReplySessionMismatchView>(hr))
		return fail_hello(status, HelloStatus::SessionMismatch);
	return fail_hello(status, HelloStatus::Unavailable);
}

bool
BlockingClient::open(const vector<string> &urls, string_view activation_token,
	instance::Error *error, chrono::milliseconds timeout)
{
	if (!conn_.ok()) {
		set_internal(error);
		return false;
	}
	chrono::milliseconds left = timeout;
	if (left.count() < 0)
		left = chrono::milliseconds{0};

	instance::OpenRequest open_req;
	open_req.urls = urls;
	open_req.activation_token = string(activation_token);

	instance::Request req;
	req.id = 1;
	req.body.value = instance::RequestBodyOpen{std::move(open_req)};

	instance::Frame frame;
	frame.payload.value = instance::PayloadRequest{std::move(req)};
	vector<byte> buf;
	if (!encode_frame(frame, buf) || !write_bytes(conn_, buf, left)) {
		set_internal(error);
		return false;
	}
	if (!read_one_frame(conn_, buf, left)) {
		set_internal(error);
		return false;
	}

	Decoder dec(buf);
	instance::FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0) {
		set_internal(error);
		return false;
	}

	const auto *presp =
		get_if<instance::PayloadResponseView>(&view.payload.value);
	if (!presp || presp->response.id != 1) {
		set_internal(error);
		return false;
	}

	const auto &result = presp->response.result.value;
	if (holds_alternative<instance::ResultDoneView>(result))
		return true;
	if (const auto *err = get_if<instance::ResultErrorView>(&result)) {
		if (error) {
			error->code = err->error.code;
			error->message = string(err->error.message);
		}
		return false;
	}
	set_internal(error);
	return false;
}

Server::Conn::Conn(int fd) : conn(fd)
{
}

Server::Server(int listen_fd, Config cfg)
	: listen_fd_(listen_fd), cfg_(std::move(cfg))
{
}

Server::~Server()
{
	while (!conns_.empty())
		drop(conns_.begin()->first);
	if (listen_fd_ >= 0) {
		::close(listen_fd_);
		listen_fd_ = -1;
	}
}

void
Server::poll_listen()
{
	for (;;) {
		const int fd = Endpoint::accept(listen_fd_);
		if (fd < 0)
			return;
		auto [it, inserted] = conns_.try_emplace(fd, fd);
		if (!inserted || !it->second.conn.ok()) {
			if (inserted)
				conns_.erase(it);
			else
				::close(fd);
			continue;
		}
		if (cfg_.watch_read)
			cfg_.watch_read(fd);
	}
}

void
Server::poll_read(int fd)
{
	const auto it = conns_.find(fd);
	if (it == conns_.end())
		return;
	Conn *c = &it->second;
	if (c->drop_after_flush)
		return;
	for (;;) {
		switch (c->conn.read()) {
		case Connection::Status::NeedMore:
			return;
		case Connection::Status::Frame: {
			vector<byte> payload;
			if (!c->conn.take_payload(payload) ||
				!handle_payload(*c, payload)) {
				drop(fd);
				return;
			}
			if (c->drop_after_flush) {
				if (!c->conn.wants_write())
					drop(fd);
				return;
			}
			break;
		}
		default:
			drop(fd);
			return;
		}
	}
}

void
Server::poll_write(int fd)
{
	const auto it = conns_.find(fd);
	if (it == conns_.end())
		return;
	Conn &c = it->second;
	if (c.conn.flush()) {
		if (cfg_.watch_write)
			cfg_.watch_write(fd, false);
		if (c.drop_after_flush)
			drop(fd);
		return;
	}
	if (!c.conn.ok()) {
		drop(fd);
		return;
	}
	if (cfg_.watch_write)
		cfg_.watch_write(fd, c.conn.wants_write());
}

void
Server::drop(int fd)
{
	const auto it = conns_.find(fd);
	if (it == conns_.end())
		return;
	if (cfg_.unwatch)
		cfg_.unwatch(fd);
	conns_.erase(it);
}

bool
Server::write_frame(Conn &c, const instance::Frame &frame)
{
	vector<byte> buf;
	if (!encode_frame(frame, buf) || !c.conn.write_payload(buf))
		return false;
	(void) c.conn.flush();
	if (!c.conn.ok())
		return false;
	if (cfg_.watch_write)
		cfg_.watch_write(c.conn.fd(), c.conn.wants_write());
	return true;
}

bool
Server::handle_payload(Conn &c, span<const byte> payload)
{
	Decoder dec(payload);
	instance::FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0)
		return false;

	if (!c.handshake_done) {
		const auto *hello =
			get_if<instance::PayloadHelloView>(&view.payload.value);
		if (!hello)
			return false;

		instance::Frame reply;
		const instance::HelloView &h = hello->hello;
		if (h.protocol_version != cfg_.protocol_version) {
			instance::HelloReplyVersionMismatch mismatch;
			mismatch.server_protocol_version = cfg_.protocol_version;
			reply.payload.value = instance::PayloadHelloReply{
				instance::HelloReply{mismatch},
			};
			if (!write_frame(c, reply))
				return false;
			c.drop_after_flush = true;
			return true;
		}
		if (h.session != cfg_.session) {
			reply.payload.value = instance::PayloadHelloReply{
				instance::HelloReply{
					instance::HelloReplySessionMismatch{},
				},
			};
			if (!write_frame(c, reply))
				return false;
			c.drop_after_flush = true;
			return true;
		}

		instance::HelloReplyAccepted accepted;
		accepted.limits.max_payload_size = cfg_.max_payload_size;
		reply.payload.value = instance::PayloadHelloReply{
			instance::HelloReply{accepted},
		};
		if (!write_frame(c, reply))
			return false;
		c.handshake_done = true;
		return true;
	}

	if (holds_alternative<instance::PayloadCancelView>(view.payload.value))
		return true;

	const auto *preq =
		get_if<instance::PayloadRequestView>(&view.payload.value);
	if (!preq)
		return false;

	instance::Response response;
	response.id = preq->request.id;
	response.result.value = instance::ResultError{
		instance::Error{instance::ErrorCode::Internal, {}},
	};
	if (cfg_.on_request)
		cfg_.on_request(preq->request, response);

	instance::Frame out;
	out.payload.value = instance::PayloadResponse{std::move(response)};
	return write_frame(c, out);
}

}  // namespace ipc
}  // namespace dn
