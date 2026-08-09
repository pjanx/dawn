//
// test-ipc.cpp: framed Connection over a Unix socketpair
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

using namespace std;

namespace
{

int g_failures = 0;

#define CHECK(cond)                                                            \
	do {                                                                       \
		if (!(cond)) {                                                         \
			fprintf(stderr, "CHECK failed: %s (%s:%d)\n", #cond, __FILE__,     \
				__LINE__);                                                     \
			++g_failures;                                                      \
		}                                                                      \
	} while (0)

bool
write_all(int fd, const void *p, size_t n)
{
	const auto *b = static_cast<const uint8_t *>(p);
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

void
put_u32be(uint8_t *out, uint32_t n)
{
	out[0] = uint8_t((n >> 24) & 0xff);
	out[1] = uint8_t((n >> 16) & 0xff);
	out[2] = uint8_t((n >> 8) & 0xff);
	out[3] = uint8_t(n & 0xff);
}

bool
payload_eq(const vector<byte> &got, span<const uint8_t> want)
{
	if (got.size() != want.size())
		return false;
	for (size_t i = 0; i < got.size(); ++i) {
		if (uint8_t(got[i]) != want[i])
			return false;
	}
	return true;
}

void
test_fragmented()
{
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "socketpair: %s\n", strerror(errno));
		++g_failures;
		return;
	}
	dn::ipc::Connection conn(fds[0]);
	const int peer = fds[1];
	CHECK(conn.ok());

	static constexpr uint8_t kPayload[] = {1, 2, 3, 4, 5};
	uint8_t wire[4 + sizeof(kPayload)];
	put_u32be(wire, uint32_t(sizeof(kPayload)));
	memcpy(wire + 4, kPayload, sizeof(kPayload));

	for (size_t i = 0; i < sizeof(wire); ++i) {
		CHECK(write_all(peer, &wire[i], 1));
		const auto st = conn.read();
		if (i + 1 < sizeof(wire))
			CHECK(st == dn::ipc::Connection::Status::NeedMore);
		else
			CHECK(st == dn::ipc::Connection::Status::Frame);
	}

	vector<byte> got;
	CHECK(conn.take_payload(got));
	CHECK(payload_eq(got, kPayload));
	::close(peer);
}

void
test_two_frames_one_write()
{
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "socketpair: %s\n", strerror(errno));
		++g_failures;
		return;
	}
	dn::ipc::Connection conn(fds[0]);
	const int peer = fds[1];
	CHECK(conn.ok());

	static constexpr uint8_t kA[] = {1, 2, 3, 4, 5};
	static constexpr uint8_t kB[] = {6, 7, 8, 9, 10};
	uint8_t wire[18];
	put_u32be(wire, 5);
	memcpy(wire + 4, kA, 5);
	put_u32be(wire + 9, 5);
	memcpy(wire + 13, kB, 5);
	CHECK(write_all(peer, wire, sizeof(wire)));

	CHECK(conn.read() == dn::ipc::Connection::Status::Frame);
	vector<byte> first;
	CHECK(conn.take_payload(first));
	CHECK(payload_eq(first, kA));

	CHECK(conn.read() == dn::ipc::Connection::Status::Frame);
	vector<byte> second;
	CHECK(conn.take_payload(second));
	CHECK(payload_eq(second, kB));
	::close(peer);
}

void
test_empty_payload()
{
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "socketpair: %s\n", strerror(errno));
		++g_failures;
		return;
	}
	dn::ipc::Connection conn(fds[0]);
	const int peer = fds[1];
	CHECK(conn.ok());

	uint8_t len[4];
	put_u32be(len, 0);
	CHECK(write_all(peer, len, sizeof(len)));
	CHECK(conn.read() == dn::ipc::Connection::Status::Error);
	::close(peer);
}

void
test_oversize_length()
{
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "socketpair: %s\n", strerror(errno));
		++g_failures;
		return;
	}
	dn::ipc::Connection conn(fds[0]);
	const int peer = fds[1];
	CHECK(conn.ok());

	uint8_t len[4];
	put_u32be(len, dn::ipc::Connection::kMaxPayload + 1);
	CHECK(write_all(peer, len, sizeof(len)));
	CHECK(conn.read() == dn::ipc::Connection::Status::Error);
	::close(peer);
}

void
test_write_read_pair()
{
	int fds[2] = {-1, -1};
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
		fprintf(stderr, "socketpair: %s\n", strerror(errno));
		++g_failures;
		return;
	}
	dn::ipc::Connection a(fds[0]);
	dn::ipc::Connection b(fds[1]);
	CHECK(a.ok());
	CHECK(b.ok());

	static constexpr uint8_t kPayload[] = {1, 2, 3, 4, 5};
	const auto payload = as_bytes(span(kPayload));
	CHECK(a.write_payload(payload));
	CHECK(a.flush());
	CHECK(b.read() == dn::ipc::Connection::Status::Frame);
	vector<byte> got;
	CHECK(b.take_payload(got));
	CHECK(payload_eq(got, kPayload));
}

}  // namespace

int
main()
{
	test_fragmented();
	test_two_frames_one_write();
	test_empty_payload();
	test_oversize_length();
	test_write_read_pair();

	if (g_failures) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	puts("ok");
	return 0;
}
