//
// test-ipc.cpp: framing, and the transport each platform provides
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/ipc-instance.hpp"
#include "libdn/ipc-loop.hpp"
#include "libdn/ipc-shm.hpp"
#include "libdn/ipc.hpp"
#include "test.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#ifndef _WIN32
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#else
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

using namespace std;
namespace inst = dawn::ipc::instance;

#ifndef _WIN32
namespace
{

struct SocketPair {
	int fds[2] = {-1, -1};

	SocketPair()
	{
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0)
			test::fail("socketpair: %s", strerror(errno));
	}
	~SocketPair()
	{
		for (int fd : fds)
			if (fd >= 0)
				::close(fd);
	}
	int take(int index) { return exchange(fds[index], -1); }
	explicit operator bool() const { return fds[0] >= 0; }
};

}  // namespace

static bool
write_all(int fd, const void *p, size_t n)
{
	const auto *b = (const uint8_t *) p;
	size_t off = 0;
	while (off < n) {
		const ssize_t w = ::write(fd, b + off, n - off);
		if (w < 0) {
			if (errno == EINTR)
				continue;
			return false;
		}
		if (w == 0)
			return false;
		off += size_t(w);
	}
	return true;
}

static bool
send_fd_with_bytes(int socket, int fd, span<const uint8_t> bytes)
{
	iovec iov{const_cast<uint8_t *>(bytes.data()), bytes.size()};
	alignas(cmsghdr) char control[CMSG_SPACE(sizeof fd)]{};
	msghdr message{};
	message.msg_iov = &iov;
	message.msg_iovlen = 1;
	message.msg_control = control;
	message.msg_controllen = sizeof control;
	cmsghdr *header = CMSG_FIRSTHDR(&message);
	header->cmsg_level = SOL_SOCKET;
	header->cmsg_type = SCM_RIGHTS;
	header->cmsg_len = CMSG_LEN(sizeof fd);
	memcpy(CMSG_DATA(header), &fd, sizeof fd);
	return sendmsg(socket, &message, 0) == ssize_t(bytes.size());
}

static void
put_u32be(uint8_t *out, uint32_t n)
{
	out[0] = uint8_t((n >> 24) & 0xff);
	out[1] = uint8_t((n >> 16) & 0xff);
	out[2] = uint8_t((n >> 8) & 0xff);
	out[3] = uint8_t(n & 0xff);
}

#endif

static bool
payload_eq(const vector<byte> &got, span<const uint8_t> want)
{
	if (got.size() != want.size())
		return false;
	for (size_t i = 0; i < got.size(); i++) {
		if (uint8_t(got[i]) != want[i])
			return false;
	}
	return true;
}

#ifndef _WIN32
static void
test_fragmented()
{
	SocketPair pair;
	if (!pair)
		return;
	dawn::ipc::Connection conn(pair.take(0));
	const int peer = pair.fds[1];
	CHECK(conn.ok());

	static constexpr uint8_t kPayload[] = {1, 2, 3, 4, 5};
	uint8_t wire[4 + sizeof kPayload];
	put_u32be(wire, uint32_t(sizeof kPayload));
	memcpy(wire + 4, kPayload, sizeof kPayload);

	for (size_t i = 0; i < sizeof wire; i++) {
		CHECK(write_all(peer, &wire[i], 1));
		const auto st = conn.read();
		if (i + 1 < sizeof wire)
			CHECK(st == dawn::ipc::Connection::Status::NeedMore);
		else
			CHECK(st == dawn::ipc::Connection::Status::Frame);
	}

	vector<byte> got;
	CHECK(conn.take_plain_payload(got));
	CHECK(payload_eq(got, kPayload));
}

static void
test_two_frames_one_write()
{
	SocketPair pair;
	if (!pair)
		return;
	dawn::ipc::Connection conn(pair.take(0));
	const int peer = pair.fds[1];
	CHECK(conn.ok());

	static constexpr uint8_t kA[] = {1, 2, 3, 4, 5};
	static constexpr uint8_t kB[] = {6, 7, 8, 9, 10};
	uint8_t wire[18];
	put_u32be(wire, 5);
	memcpy(wire + 4, kA, 5);
	put_u32be(wire + 9, 5);
	memcpy(wire + 13, kB, 5);
	CHECK(write_all(peer, wire, sizeof wire));

	CHECK(conn.read() == dawn::ipc::Connection::Status::Frame);
	vector<byte> first;
	CHECK(conn.take_plain_payload(first));
	CHECK(payload_eq(first, kA));

	CHECK(conn.read() == dawn::ipc::Connection::Status::Frame);
	vector<byte> second;
	CHECK(conn.take_plain_payload(second));
	CHECK(payload_eq(second, kB));
}

static void
test_empty_payload()
{
	SocketPair pair;
	if (!pair)
		return;
	dawn::ipc::Connection conn(pair.take(0));
	const int peer = pair.fds[1];
	CHECK(conn.ok());

	uint8_t len[4];
	put_u32be(len, 0);
	CHECK(write_all(peer, len, sizeof len));
	CHECK(conn.read() == dawn::ipc::Connection::Status::Error);
}

static void
test_oversize_length()
{
	SocketPair pair;
	if (!pair)
		return;
	dawn::ipc::Connection conn(pair.take(0));
	const int peer = pair.fds[1];
	CHECK(conn.ok());

	uint8_t len[4];
	put_u32be(len, dawn::ipc::Connection::kMaxPayload + 1);
	CHECK(write_all(peer, len, sizeof len));
	CHECK(conn.read() == dawn::ipc::Connection::Status::Error);
}

static void
test_write_read_pair()
{
	SocketPair pair;
	if (!pair)
		return;
	dawn::ipc::Connection a(pair.take(0));
	dawn::ipc::Connection b(pair.take(1));
	CHECK(a.ok());
	CHECK(b.ok());

	static constexpr uint8_t kPayload[] = {1, 2, 3, 4, 5};
	const auto payload = as_bytes(span(kPayload));
	CHECK(a.write_payload(payload, {}));
	CHECK(a.flush());
	CHECK(b.read() == dawn::ipc::Connection::Status::Frame);
	vector<byte> got;
	CHECK(b.take_plain_payload(got));
	CHECK(payload_eq(got, kPayload));
}

static void
test_attachment_pair()
{
	SocketPair pair;
	dawn::ipc::Connection a(pair.take(0)), b(pair.take(1));
	static constexpr char kShared[] = "dawn";
	auto memory = dawn::ipc::SharedMemory::copy(kShared, 4);
	CHECK(memory.ok());
	static constexpr byte payload[]{byte{42}};
	const dawn::ipc::Handle handle = memory.handle();
	CHECK(a.write_payload(payload, span(&handle, 1)));
	CHECK(a.flush());
	CHECK(b.read() == dawn::ipc::Connection::Status::Frame);
	vector<byte> got;
	vector<dawn::ipc::Handle> handles;
	CHECK(b.take_payload(got, handles));
	CHECK(got.size() == 1 && got[0] == byte{42});
	CHECK(handles.size() == 1);
	if (handles.size() == 1) {
		auto mapped = dawn::ipc::SharedMemory::map(handles[0], 4);
		CHECK(mapped.ok() && memcmp(mapped.data(), "dawn", 4) == 0);
	}
}

static vector<int>
open_fds()
{
	vector<int> out;
	for (int fd = 0; fd < 256; fd++)
		if (fcntl(fd, F_GETFD) >= 0)
			out.push_back(fd);
	return out;
}

static void
test_close_discards_attachments()
{
	SocketPair pair;
	dawn::ipc::Connection a(pair.take(0)), b(pair.take(1));
	const byte data[4]{};
	auto memory = dawn::ipc::SharedMemory::copy(data, sizeof data);
	const dawn::ipc::Handle handle = memory.handle();
	static constexpr byte payload[]{byte{1}};
	CHECK(a.write_payload(payload, span(&handle, 1)));
	CHECK(a.flush());
	const vector<int> before = open_fds();
	CHECK(b.read() == dawn::ipc::Connection::Status::Frame);
	const vector<int> after = open_fds();
	vector<int> received;
	for (int fd : after)
		if (find(before.begin(), before.end(), fd) == before.end())
			received.push_back(fd);
	CHECK(received.size() == 1);
	b.close();
	if (received.size() == 1)
		CHECK(fcntl(received[0], F_GETFD) < 0 && errno == EBADF);
}

static void
test_rejects_undeclared_attachment()
{
	SocketPair pair;
	dawn::ipc::Connection receiver(pair.take(0));
	const byte data[4]{};
	auto memory = dawn::ipc::SharedMemory::copy(data, sizeof data);
	const uint8_t wire[]{0, 0, 0, 1, 42};
	CHECK(send_fd_with_bytes(pair.fds[1], int(memory.handle()), wire));
	CHECK(receiver.read() == dawn::ipc::Connection::Status::Frame);
	vector<byte> payload;
	vector<dawn::ipc::Handle> handles;
	CHECK(!receiver.take_payload(payload, handles));
	CHECK(!receiver.ok());
	CHECK(handles.empty());
}
#endif

static void
test_attachment_length_prefix()
{
	dawn::ipc::FrameWriter writer;
	static constexpr byte payload[]{byte{1}, byte{2}};
	CHECK(writer.push(payload, {}));
	auto plain = writer.pending();
	CHECK(plain.size() == 6);
	CHECK(plain[0] == byte{0} && plain[1] == byte{0} && plain[2] == byte{0} &&
		plain[3] == byte{2});
	writer.clear();
	const dawn::ipc::Handle handles[]{3, 4};
	CHECK(writer.push(payload, handles));
	auto attached = writer.pending();
	CHECK(attached.size() == 6);
	CHECK(attached[0] == byte{4} && attached[3] == byte{2});
}

static void
test_attachment_reader()
{
	dawn::ipc::FrameReader reader;
	uint8_t wire[] = {6, 0, 0, 2, 9, 8};
	memcpy(reader.buffer().data(), wire, 4);
	CHECK(reader.advance(4) == dawn::ipc::FrameReader::Status::NeedMore);
	CHECK(reader.attachment_count() == 3);
	memcpy(reader.buffer().data(), wire + 4, 2);
	CHECK(reader.advance(2) == dawn::ipc::FrameReader::Status::Frame);
	vector<byte> payload;
	vector<dawn::ipc::Handle> handles{99};
	CHECK(reader.take_payload(payload, handles));
	CHECK(handles.empty());
	CHECK(payload.size() == 2);
}

static void
test_attachment_boundaries()
{
	dawn::ipc::FrameWriter writer;
	static constexpr byte payload[]{byte{1}};
	const dawn::ipc::Handle first[]{10};
	const dawn::ipc::Handle second[]{20, 21};
	CHECK(writer.push(payload, first));
	CHECK(writer.push(payload, {}));
	CHECK(writer.push(payload, second));
	CHECK(writer.pending_attachments().size() == 1);
	CHECK(writer.pending().size() == 10);
	writer.attachments_sent();
	writer.consume(10);
	CHECK(writer.pending_attachments().size() == 2);
	CHECK(writer.pending().size() == 5);
	(void) writer.take_all_attachments();
}

static void
test_clear_discards_attachment_points()
{
	dawn::ipc::FrameWriter writer;
	static constexpr byte attached[]{byte{9}};
	const dawn::ipc::Handle handle[]{10};
	CHECK(writer.push(attached, handle));
	CHECK(writer.pending_attachments().size() == 1);
	writer.clear();
	static constexpr byte plain[]{byte{1}, byte{2}};
	CHECK(writer.push(plain, {}));
	CHECK(writer.pending_attachments().empty());
	const auto pending = writer.pending();
	CHECK(pending.size() == 6 && pending[0] == byte{0} &&
		pending[1] == byte{0} && pending[2] == byte{0} &&
		pending[3] == byte{2} && pending[4] == byte{1} &&
		pending[5] == byte{2});
}

static void
test_inline_attachment_encoding()
{
	dawn::ipc::FrameWriter writer;
	writer.set_inline_handles(true);
	const byte payload[]{byte{7}, byte{8}};
	const dawn::ipc::Handle sent[]{0x01020304};
	CHECK(writer.push(payload, sent));
	const auto application = writer.pending();
	CHECK(application.size() == 14 && application[0] == byte{2} &&
		application[1] == byte{0} && application[2] == byte{0} &&
		application[3] == byte{2} && application[12] == byte{7} &&
		application[13] == byte{8});

	dawn::ipc::FrameReader reader;
	reader.set_inline_handles(true);
	memcpy(reader.buffer().data(), application.data(), 4);
	CHECK(reader.advance(4) == dawn::ipc::FrameReader::Status::NeedMore);
	memcpy(reader.buffer().data(), application.data() + 4, 10);
	CHECK(reader.advance(10) == dawn::ipc::FrameReader::Status::Frame);
	vector<byte> received;
	vector<dawn::ipc::Handle> handles;
	CHECK(reader.take_payload(received, handles));
	CHECK(received.size() == 2 && received[0] == byte{7} &&
		received[1] == byte{8});
	CHECK(handles.size() == 1 && handles[0] == sent[0]);
}

static void
test_reader_rejects_reserved_bit()
{
	dawn::ipc::FrameReader reader;
	const byte reserved[]{byte{0x80}, byte{0}, byte{0}, byte{0}};
	memcpy(reader.buffer().data(), reserved, sizeof reserved);
	CHECK(reader.advance(sizeof reserved) ==
		dawn::ipc::FrameReader::Status::Error);
}

#ifdef _WIN32
static void
test_inline_attachment_discard()
{
	HANDLE handle = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	CHECK(handle != nullptr);
	if (!handle)
		return;

	dawn::ipc::FrameWriter writer;
	writer.set_inline_handles(true);
	const dawn::ipc::Handle sent[]{(dawn::ipc::Handle) handle};
	const byte payload[]{byte{1}, byte{2}};
	CHECK(writer.push(payload, sent));
	const auto frame = writer.pending();

	dawn::ipc::FrameReader reader;
	reader.set_inline_handles(true);
	memcpy(reader.buffer().data(), frame.data(), 4);
	CHECK(reader.advance(4) == dawn::ipc::FrameReader::Status::NeedMore);
	memcpy(reader.buffer().data(), frame.data() + 4, 9);
	CHECK(reader.advance(9) == dawn::ipc::FrameReader::Status::NeedMore);
	reader.discard();

	DWORD flags = 0;
	CHECK(!::GetHandleInformation(handle, &flags));
	CHECK(::GetLastError() == ERROR_INVALID_HANDLE);
}

static void
test_loop_refuses_excess_connections()
{
	dawn::ipc::Loop loop;
	CHECK(loop.watch_read(dawn::ipc::Loop::kListener, 1));
	for (uint64_t id = 1; id <= 31; id++)
		CHECK(loop.watch_read(id, dawn::ipc::Waitable(id + 1)));
	CHECK(!loop.watch_read(32, 33));
}
#endif

// --- Endpoint ----------------------------------------------------------------

// Whatever the platform transport is, binding it has to arbitrate and a
// frame has to survive the trip. This is what dn's single instance rests
// on, and neither half is exercised by the loopback tests above.
constexpr char kService[] = "test";

static void
test_listen_arbitrates()
{
	auto first = dawn::ipc::Endpoint::listen(kService);
	CHECK(first.status == dawn::ipc::Endpoint::ListenStatus::Ok);
	CHECK(first.listener.ok());
	if (first.status != dawn::ipc::Endpoint::ListenStatus::Ok)
		return;

	const auto second = dawn::ipc::Endpoint::listen(kService);
	CHECK(second.status == dawn::ipc::Endpoint::ListenStatus::InUse);
}

static void
test_endpoint_roundtrip()
{
	auto listen = dawn::ipc::Endpoint::listen(kService);
	CHECK(listen.status == dawn::ipc::Endpoint::ListenStatus::Ok);
	if (listen.status != dawn::ipc::Endpoint::ListenStatus::Ok)
		return;

	auto connect = dawn::ipc::Endpoint::connect(kService);
	CHECK(connect.status == dawn::ipc::Endpoint::ConnectStatus::Ok);
	if (connect.status != dawn::ipc::Endpoint::ConnectStatus::Ok)
		return;

	// The connect completion may not have been posted yet; an event loop
	// would be woken by the listener instead of spinning like this.
	dawn::ipc::Connection server;
	for (int i = 0; i < 100 && !server.ok(); i++)
		server = listen.listener.accept();
	CHECK(server.ok());
	if (!server.ok())
		return;

	static constexpr uint8_t kPayload[] = {9, 8, 7};
	const auto payload = as_bytes(span(kPayload));
	CHECK(connect.conn.write_payload(payload, {}));
	CHECK(connect.conn.flush());

	while (server.read() == dawn::ipc::Connection::Status::NeedMore) {
		if (server.wait(dawn::ipc::Connection::Direction::Read, 2000) !=
			dawn::ipc::Connection::Ready::Ok)
			break;
	}
	vector<byte> got;
	CHECK(server.take_plain_payload(got));
	CHECK(payload_eq(got, kPayload));

	// The peer identity is what stands in for a same-user check.
	CHECK(server.peer_pid() != 0);
	CHECK(connect.conn.peer_pid() != 0);
}

#ifdef _WIN32
static dawn::ipc::Connection
accept_connection(dawn::ipc::Listener &listener)
{
	for (int i = 0; i < 100; i++) {
		auto connection = listener.accept();
		if (connection.ok())
			return connection;
		::WaitForSingleObject((HANDLE) listener.waitable(), 20);
	}
	return {};
}

static bool
flush_connection(dawn::ipc::Connection &connection)
{
	for (int i = 0; i < 100; i++) {
		if (connection.flush())
			return true;
		if (!connection.ok() ||
			connection.wait(dawn::ipc::Connection::Direction::Write, 2000) !=
				dawn::ipc::Connection::Ready::Ok)
			return false;
	}
	return false;
}

static bool
read_frame(dawn::ipc::Connection &connection)
{
	for (int i = 0; i < 100; i++) {
		const auto status = connection.read();
		if (status == dawn::ipc::Connection::Status::Frame)
			return true;
		if (status != dawn::ipc::Connection::Status::NeedMore ||
			connection.wait(dawn::ipc::Connection::Direction::Read, 2000) !=
				dawn::ipc::Connection::Ready::Ok)
			return false;
	}
	return false;
}

static void
test_windows_attachment_protocol()
{
	auto endpoint = dawn::ipc::Endpoint::listen("test-windows-attachments");
	CHECK(endpoint.status == dawn::ipc::Endpoint::ListenStatus::Ok);
	if (endpoint.status != dawn::ipc::Endpoint::ListenStatus::Ok)
		return;
	auto client = dawn::ipc::Endpoint::connect("test-windows-attachments");
	CHECK(client.status == dawn::ipc::Endpoint::ConnectStatus::Ok);
	auto server = accept_connection(endpoint.listener);
	CHECK(client.conn.ok() && server.ok());
	if (!client.conn.ok() || !server.ok())
		return;

	const uint8_t first_value = 17, second_value = 29;
	auto first = dawn::ipc::SharedMemory::copy(&first_value, 1);
	auto second = dawn::ipc::SharedMemory::copy(&second_value, 1);
	CHECK(first.ok() && second.ok());
	const dawn::ipc::Handle first_handle[]{first.handle()};
	const dawn::ipc::Handle second_handle[]{second.handle()};
	const byte first_payload[]{byte{1}};
	const byte second_payload[]{byte{2}};
	CHECK(client.conn.write_payload(first_payload, first_handle));
	CHECK(client.conn.write_payload(second_payload, second_handle));
	first.close();
	second.close();
	CHECK(flush_connection(client.conn));

	for (uint8_t expected : {uint8_t(17), uint8_t(29)}) {
		CHECK(read_frame(server));
		vector<byte> payload;
		vector<dawn::ipc::Handle> handles;
		CHECK(server.take_payload(payload, handles));
		CHECK(handles.size() == 1);
		if (handles.size() == 1) {
			auto memory = dawn::ipc::SharedMemory::map(handles[0], 1);
			handles.clear();
			CHECK(memory.ok() && memory.data()[0] == expected);
		}
	}
}

static void
test_windows_attachment_teardown()
{
	auto endpoint = dawn::ipc::Endpoint::listen("test-windows-handle-close");
	CHECK(endpoint.status == dawn::ipc::Endpoint::ListenStatus::Ok);
	if (endpoint.status != dawn::ipc::Endpoint::ListenStatus::Ok)
		return;
	auto client = dawn::ipc::Endpoint::connect("test-windows-handle-close");
	auto server = accept_connection(endpoint.listener);
	CHECK(client.conn.ok() && server.ok());
	const byte value{};
	auto memory = dawn::ipc::SharedMemory::copy(&value, 1);
	const dawn::ipc::Handle handle[]{memory.handle()};
	const byte payload[]{byte{1}};
	CHECK(client.conn.write_payload(payload, handle));
	CHECK(flush_connection(client.conn));
	client.conn.close();
	CHECK(read_frame(server));
	vector<byte> received;
	vector<dawn::ipc::Handle> handles;
	CHECK(server.take_payload(received, handles));
	CHECK(handles.size() == 1);
	if (handles.size() == 1) {
		auto mapped = dawn::ipc::SharedMemory::map(handles[0], 1);
		handles.clear();
		CHECK(mapped.ok());
	}
}
#endif

// --- Instance service --------------------------------------------------------

// Everything below drives dawn::ipc::instance::Server through a hand-built
// peer: dn's own Client cannot serve here, because it always connects to
// the real "instance" endpoint, which a running dn may well hold.
//
// dn answers every request inline on the Qt thread, so nothing in the
// application exercises deferred completion, out-of-order responses, or
// cancellation. This does.

constexpr char kInstanceService[] = "test-instance";
constexpr char kSession[] = "test-session";
constexpr uint32_t kMaxPayload = 4096;

static vector<byte>
frame_bytes(const inst::Frame &frame)
{
	vector<byte> buf;
	dawn::ipc::Encoder enc(buf);
	encode(frame, enc);
	CHECK(enc.ok());
	return buf;
}

static inst::Frame
hello_frame(uint32_t version, string_view session)
{
	inst::Hello hello;
	hello.protocol_version = version;
	hello.session = string(session);
	inst::Frame frame;
	frame.payload.value = inst::PayloadHello{std::move(hello)};
	return frame;
}

static inst::Frame
open_frame(uint64_t id)
{
	inst::OpenRequest open;
	open.urls = {"file:///tmp"};
	inst::Request req;
	req.id = id;
	req.body.value = inst::RequestBodyOpen{std::move(open)};
	inst::Frame frame;
	frame.payload.value = inst::PayloadRequest{std::move(req)};
	return frame;
}

static inst::Frame
cancel_frame(uint64_t id)
{
	inst::Frame frame;
	frame.payload.value = inst::PayloadCancel{inst::Cancel{id}};
	return frame;
}

// A server, and one raw connection standing in for a client. Everything is
// single-threaded: the peer writes, then pump() runs the server by hand
// until an answer comes back.
namespace
{

struct Fixture {
	unique_ptr<inst::Server> server;
	dawn::ipc::Connection peer;
	uint64_t conn_id = 0;
	int requests = 0;
	function<void(inst::Call, const inst::RequestView &)> handler;

	bool start();
	bool send(const inst::Frame &frame);
	// Drive both ends until the peer has a whole frame, or patience runs
	// out. False also means the server closed on us; see closed().
	bool recv(inst::FrameView &view, vector<byte> &storage);
	bool closed();
	void poll();
};

}  // namespace

void
Fixture::poll()
{
	this->server->poll_listen();
	if (this->conn_id) {
		this->server->poll_read(this->conn_id);
		this->server->poll_write(this->conn_id);
	}
}

bool
Fixture::start()
{
	auto listen = dawn::ipc::Endpoint::listen(kInstanceService);
	CHECK(listen.status == dawn::ipc::Endpoint::ListenStatus::Ok);
	if (listen.status != dawn::ipc::Endpoint::ListenStatus::Ok)
		return false;

	inst::Server::Config cfg;
	cfg.session = kSession;
	cfg.max_payload_size = kMaxPayload;
	cfg.watch_read = [this](uint64_t id, dawn::ipc::Waitable) {
		this->conn_id = id;
		return true;
	};
	cfg.on_request = [this](inst::Call call, const inst::RequestView &req) {
		this->requests++;
		if (this->handler)
			this->handler(std::move(call), req);
		else
			call.done();
	};
	this->server =
		make_unique<inst::Server>(std::move(listen.listener), std::move(cfg));

	auto connect = dawn::ipc::Endpoint::connect(kInstanceService);
	CHECK(connect.status == dawn::ipc::Endpoint::ConnectStatus::Ok);
	if (connect.status != dawn::ipc::Endpoint::ConnectStatus::Ok)
		return false;
	this->peer = std::move(connect.conn);

	for (int i = 0; i < 100 && !this->conn_id; i++)
		this->poll();
	CHECK(this->conn_id != 0);
	return this->conn_id != 0;
}

bool
Fixture::send(const inst::Frame &frame)
{
	const vector<byte> buf = frame_bytes(frame);
	return this->peer.write_payload(buf, {}) && this->peer.flush();
}

bool
Fixture::recv(inst::FrameView &view, vector<byte> &storage)
{
	for (int i = 0; i < 200; i++) {
		this->poll();
		const auto st = this->peer.read();
		if (st == dawn::ipc::Connection::Status::Frame) {
			if (!this->peer.take_plain_payload(storage))
				return false;
			dawn::ipc::Decoder dec(storage);
			return decode(dec, view) && dec.remaining() == 0;
		}
		if (st != dawn::ipc::Connection::Status::NeedMore)
			return false;
		// Both a short nap and an early wake-up once bytes land.
		this->peer.wait(dawn::ipc::Connection::Direction::Read, 5);
	}
	return false;
}

bool
Fixture::closed()
{
	for (int i = 0; i < 200; i++) {
		this->poll();
		const auto st = this->peer.read();
		if (st == dawn::ipc::Connection::Status::Eof ||
			st == dawn::ipc::Connection::Status::Error)
			return true;
		if (st == dawn::ipc::Connection::Status::Frame) {
			vector<byte> drop;
			(void) this->peer.take_plain_payload(drop);
			continue;
		}
		this->peer.wait(dawn::ipc::Connection::Direction::Read, 5);
	}
	return false;
}

// Complete the handshake, and report the payload limit it settled on.
static bool
handshake(Fixture &f, uint32_t &limit)
{
	if (!f.send(
			hello_frame(uint32_t(inst::kInstanceProtocolVersion), kSession)))
		return false;

	inst::FrameView view{};
	vector<byte> storage;
	if (!f.recv(view, storage))
		return false;
	const auto *reply =
		get_if<inst::PayloadHelloReplyView>(&view.payload.value);
	if (!reply)
		return false;
	const auto *accepted =
		get_if<inst::HelloReplyAcceptedView>(&reply->hello_reply.value);
	if (!accepted)
		return false;
	limit = accepted->limits.max_payload_size;
	return true;
}

static void
test_instance_handshake()
{
	Fixture f;
	if (!f.start())
		return;

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	// What the server accepts is what the client must hold itself to.
	CHECK(limit == kMaxPayload);

	CHECK(f.send(open_frame(1)));
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *resp = get_if<inst::PayloadResponseView>(&view.payload.value);
	CHECK(resp != nullptr);
	if (!resp)
		return;
	CHECK(resp->response.id == 1);
	CHECK(holds_alternative<inst::ResultDoneView>(resp->response.result.value));
	CHECK(f.requests == 1);
}

static void
test_instance_version_mismatch()
{
	Fixture f;
	if (!f.start())
		return;

	CHECK(f.send(
		hello_frame(uint32_t(inst::kInstanceProtocolVersion) + 1, kSession)));
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *reply =
		get_if<inst::PayloadHelloReplyView>(&view.payload.value);
	CHECK(reply != nullptr);
	if (!reply)
		return;
	const auto *mismatch =
		get_if<inst::HelloReplyVersionMismatchView>(&reply->hello_reply.value);
	CHECK(mismatch != nullptr);
	if (mismatch) {
		CHECK(mismatch->server_protocol_version ==
			uint32_t(inst::kInstanceProtocolVersion));
	}
	// Reported once, then the connection goes; there is nothing to say.
	CHECK(f.closed());
}

static void
test_instance_session_mismatch()
{
	Fixture f;
	if (!f.start())
		return;

	CHECK(f.send(hello_frame(
		uint32_t(inst::kInstanceProtocolVersion), "somebody-elses-display")));
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *reply =
		get_if<inst::PayloadHelloReplyView>(&view.payload.value);
	CHECK(reply != nullptr);
	if (reply) {
		CHECK(holds_alternative<inst::HelloReplySessionMismatchView>(
			reply->hello_reply.value));
	}
	CHECK(f.closed());
}

static void
test_instance_request_before_hello()
{
	Fixture f;
	if (!f.start())
		return;

	// Nothing is served before the handshake, not even a well-formed
	// request.
	CHECK(f.send(open_frame(1)));
	CHECK(f.closed());
	CHECK(f.requests == 0);
}

static void
test_instance_zero_id()
{
	Fixture f;
	if (!f.start())
		return;

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	CHECK(f.send(open_frame(0)));
	CHECK(f.closed());
	CHECK(f.requests == 0);
}

static void
test_instance_duplicate_id()
{
	Fixture f;
	if (!f.start())
		return;

	vector<inst::Call> held;
	f.handler = [&held](inst::Call call, const inst::RequestView &) {
		held.push_back(std::move(call));
	};

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	CHECK(f.send(open_frame(7)));
	// Reusing a live ID would make the two answers indistinguishable.
	CHECK(f.send(open_frame(7)));
	CHECK(f.closed());
	CHECK(f.requests == 1);

	// Losing the connection cancels what was outstanding on it.
	CHECK(held.size() == 1);
	if (held.size() == 1)
		CHECK(held[0].cancelled());
}

static void
test_instance_out_of_order()
{
	Fixture f;
	if (!f.start())
		return;

	vector<inst::Call> held;
	f.handler = [&held](inst::Call call, const inst::RequestView &) {
		held.push_back(std::move(call));
	};

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	CHECK(f.send(open_frame(11)));
	CHECK(f.send(open_frame(12)));
	for (int i = 0; i < 100 && f.requests < 2; i++)
		f.poll();
	CHECK(f.requests == 2);
	CHECK(held.size() == 2);
	if (held.size() != 2)
		return;

	// Answered back to front; each response still carries its own ID.
	held[1].done();
	held[0].fail(inst::ErrorCode::NotFound, "gone");

	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *first = get_if<inst::PayloadResponseView>(&view.payload.value);
	CHECK(first != nullptr);
	if (first) {
		CHECK(first->response.id == 12);
		CHECK(holds_alternative<inst::ResultDoneView>(
			first->response.result.value));
	}

	inst::FrameView view2{};
	vector<byte> storage2;
	CHECK(f.recv(view2, storage2));
	const auto *second =
		get_if<inst::PayloadResponseView>(&view2.payload.value);
	CHECK(second != nullptr);
	if (!second)
		return;
	CHECK(second->response.id == 11);
	const auto *err =
		get_if<inst::ResultErrorView>(&second->response.result.value);
	CHECK(err != nullptr);
	if (err) {
		CHECK(err->error.code == inst::ErrorCode::NotFound);
		CHECK(err->error.message == "gone");
	}
}

static void
test_instance_cancel()
{
	Fixture f;
	if (!f.start())
		return;

	vector<inst::Call> held;
	int cancels = 0;
	f.handler = [&held, &cancels](inst::Call call, const inst::RequestView &) {
		call.on_cancel([&cancels] { cancels++; });
		held.push_back(std::move(call));
	};

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	CHECK(f.send(open_frame(3)));
	for (int i = 0; i < 100 && f.requests < 1; i++)
		f.poll();
	CHECK(held.size() == 1);
	if (held.size() != 1)
		return;
	CHECK(!held[0].cancelled());

	CHECK(f.send(cancel_frame(3)));
	for (int i = 0; i < 100 && cancels == 0; i++)
		f.poll();
	CHECK(cancels == 1);
	CHECK(held[0].cancelled());

	// Cancellation is not itself an answer; the service still sends one.
	held[0].fail(inst::ErrorCode::Cancelled, {});
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *resp = get_if<inst::PayloadResponseView>(&view.payload.value);
	CHECK(resp != nullptr);
	if (!resp)
		return;
	CHECK(resp->response.id == 3);
	const auto *err =
		get_if<inst::ResultErrorView>(&resp->response.result.value);
	CHECK(err != nullptr);
	if (err)
		CHECK(err->error.code == inst::ErrorCode::Cancelled);
}

static void
test_instance_cancel_unknown()
{
	Fixture f;
	if (!f.start())
		return;

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	// A cancellation racing a completed request is normal, not an error.
	CHECK(f.send(cancel_frame(999)));
	CHECK(f.send(open_frame(1)));
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	CHECK(holds_alternative<inst::PayloadResponseView>(view.payload.value));
}

static void
test_instance_dropped_call()
{
	Fixture f;
	if (!f.start())
		return;

	// A handler that answers nothing still owes the peer a response.
	f.handler = [](inst::Call, const inst::RequestView &) {};

	uint32_t limit = 0;
	CHECK(handshake(f, limit));
	CHECK(f.send(open_frame(5)));
	inst::FrameView view{};
	vector<byte> storage;
	CHECK(f.recv(view, storage));
	const auto *resp = get_if<inst::PayloadResponseView>(&view.payload.value);
	CHECK(resp != nullptr);
	if (!resp)
		return;
	CHECK(resp->response.id == 5);
	const auto *err =
		get_if<inst::ResultErrorView>(&resp->response.result.value);
	CHECK(err != nullptr);
	if (err)
		CHECK(err->error.code == inst::ErrorCode::Internal);
}

static void
test_instance_oversize()
{
	Fixture f;
	if (!f.start())
		return;

	uint32_t limit = 0;
	CHECK(handshake(f, limit));

	// One URL past the negotiated frame size. A peer that ignores the
	// limit gets dropped rather than served.
	inst::OpenRequest open;
	open.urls = {string(kMaxPayload + 64, 'x')};
	inst::Request req;
	req.id = 1;
	req.body.value = inst::RequestBodyOpen{std::move(open)};
	inst::Frame frame;
	frame.payload.value = inst::PayloadRequest{std::move(req)};

	const vector<byte> buf = frame_bytes(frame);
	CHECK(buf.size() > kMaxPayload);
	CHECK(f.peer.write_payload(buf, {}));
	(void) f.peer.flush();
	CHECK(f.closed());
	CHECK(f.requests == 0);
}

int
main()
{
	return test::run({
#ifndef _WIN32
		{"fragmented frame", test_fragmented},
		{"coalesced frames", test_two_frames_one_write},
		{"empty payload", test_empty_payload},
		{"oversize payload", test_oversize_length},
		{"connection pair", test_write_read_pair},
		{"attachment pair", test_attachment_pair},
		{"close discards attachments", test_close_discards_attachments},
		{"reject undeclared attachment", test_rejects_undeclared_attachment},
#endif
		{"attachment length prefix", test_attachment_length_prefix},
		{"attachment reader", test_attachment_reader},
		{"attachment boundaries", test_attachment_boundaries},
		{"clear attachment points", test_clear_discards_attachment_points},
		{"inline attachment encoding", test_inline_attachment_encoding},
		{"reserved bit rejection", test_reader_rejects_reserved_bit},
#ifdef _WIN32
		{"inline attachment discard", test_inline_attachment_discard},
		{"loop capacity refusal", test_loop_refuses_excess_connections},
		{"Windows attachment protocol", test_windows_attachment_protocol},
		{"Windows attachment teardown", test_windows_attachment_teardown},
#endif
		{"listener arbitration", test_listen_arbitrates},
		{"endpoint round trip", test_endpoint_roundtrip},
		{"instance handshake", test_instance_handshake},
		{"version mismatch", test_instance_version_mismatch},
		{"session mismatch", test_instance_session_mismatch},
		{"request before hello", test_instance_request_before_hello},
		{"zero request ID", test_instance_zero_id},
		{"duplicate request ID", test_instance_duplicate_id},
		{"out-of-order responses", test_instance_out_of_order},
		{"request cancellation", test_instance_cancel},
		{"unknown cancellation", test_instance_cancel_unknown},
		{"dropped call", test_instance_dropped_call},
		{"instance payload limit", test_instance_oversize},
	});
}
