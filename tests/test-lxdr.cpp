//
// test-lxdr.cpp: LibertyXDR encode/decode and generated instance types
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"
#include "ipc/instance.lxdr.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace std;
namespace inst = dn::ipc::instance;

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

template <typename T>
vector<byte>
encoded(const T &value)
{
	vector<byte> buf;
	dn::ipc::Encoder enc(buf);
	encode(value, enc);
	CHECK(enc.ok());
	return buf;
}

bool
bytes_eq(span<const byte> got, span<const uint8_t> want)
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
check_golden(
	const char *label, const vector<byte> &got, span<const uint8_t> want)
{
	if (bytes_eq(got, want))
		return;
	fprintf(stderr, "golden %s: got %zu bytes, want %zu\n", label, got.size(),
		want.size());
	for (size_t i = 0; i < got.size(); ++i)
		fprintf(stderr, " %02x", uint8_t(got[i]));
	fprintf(stderr, "\n");
	++g_failures;
}

void
test_round_trip_hello_reply()
{
	{
		inst::HelloReply own;
		own.value = inst::HelloReplyAccepted{inst::Limits{0x00100000u}};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::HelloReplyAcceptedView>(view.value));
		CHECK(get<inst::HelloReplyAcceptedView>(view.value)
				  .limits.max_payload_size == 0x00100000u);
	}
	{
		inst::HelloReply own;
		own.value = inst::HelloReplyVersionMismatch{2};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(
			holds_alternative<inst::HelloReplyVersionMismatchView>(view.value));
		CHECK(get<inst::HelloReplyVersionMismatchView>(view.value)
				  .server_protocol_version == 2);
	}
	{
		inst::HelloReply own;
		own.value = inst::HelloReplySessionMismatch{};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(
			holds_alternative<inst::HelloReplySessionMismatchView>(view.value));
	}
}

void
test_round_trip_result()
{
	{
		inst::Result own;
		own.value = inst::ResultDone{};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::ResultView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::ResultDoneView>(view.value));
	}
	{
		inst::Result own;
		own.value =
			inst::ResultError{inst::Error{inst::ErrorCode::Busy, "nope"}};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::ResultView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::ResultErrorView>(view.value));
		const auto &err = get<inst::ResultErrorView>(view.value).error;
		CHECK(err.code == inst::ErrorCode::Busy);
		CHECK(err.message == "nope");
	}
}

void
test_round_trip_frames()
{
	{
		inst::Hello hello{1, "sess"};
		const vector<byte> buf = encoded(hello);
		dn::ipc::Decoder dec(buf);
		inst::HelloView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(view.protocol_version == 1);
		CHECK(view.session == "sess");
	}
	{
		inst::OpenRequest open;
		open.urls = {"/one", "/two"};
		open.activation_token = "tok";
		open.browse = true;
		const vector<byte> buf = encoded(open);
		dn::ipc::Decoder dec(buf);
		inst::OpenRequestView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(view.urls.size() == 2);
		CHECK(view.urls[0] == "/one");
		CHECK(view.urls[1] == "/two");
		CHECK(view.activation_token == "tok");
		CHECK(view.browse);
	}

	{
		inst::Frame own;
		own.payload.value = inst::PayloadHello{inst::Hello{1, "b"}};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadHelloView>(view.payload.value));
		const auto &h = get<inst::PayloadHelloView>(view.payload.value).hello;
		CHECK(h.protocol_version == 1);
		CHECK(h.session == "b");
	}
	{
		inst::HelloReply reply;
		reply.value = inst::HelloReplyAccepted{inst::Limits{4096}};
		inst::Frame own;
		own.payload.value = inst::PayloadHelloReply{reply};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(
			holds_alternative<inst::PayloadHelloReplyView>(view.payload.value));
		const auto &hr =
			get<inst::PayloadHelloReplyView>(view.payload.value).hello_reply;
		CHECK(holds_alternative<inst::HelloReplyAcceptedView>(hr.value));
		CHECK(get<inst::HelloReplyAcceptedView>(hr.value)
				  .limits.max_payload_size == 4096);
	}
	{
		inst::OpenRequest open;
		open.urls = {"/p", "/q"};
		open.activation_token = "";
		inst::Request req;
		req.id = 7;
		req.body.value = inst::RequestBodyOpen{open};
		inst::Frame own;
		own.payload.value = inst::PayloadRequest{req};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadRequestView>(view.payload.value));
		const auto &r =
			get<inst::PayloadRequestView>(view.payload.value).request;
		CHECK(r.id == 7);
		CHECK(holds_alternative<inst::RequestBodyOpenView>(r.body.value));
		const auto &o = get<inst::RequestBodyOpenView>(r.body.value).open;
		CHECK(o.urls.size() == 2);
		CHECK(o.urls[0] == "/p");
		CHECK(o.urls[1] == "/q");
		CHECK(!o.browse);
	}
	{
		inst::Response resp;
		resp.id = 9;
		resp.result.value = inst::ResultDone{};
		inst::Frame own;
		own.payload.value = inst::PayloadResponse{resp};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadResponseView>(view.payload.value));
		const auto &r =
			get<inst::PayloadResponseView>(view.payload.value).response;
		CHECK(r.id == 9);
		CHECK(holds_alternative<inst::ResultDoneView>(r.result.value));
	}
	{
		inst::Frame own;
		own.payload.value = inst::PayloadCancel{inst::Cancel{0x100000002ull}};
		const vector<byte> buf = encoded(own);
		dn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadCancelView>(view.payload.value));
		CHECK(get<inst::PayloadCancelView>(view.payload.value).cancel.id ==
			0x100000002ull);
	}
}

void
test_goldens()
{
	// Hello{1, "b"}: u32be version, u32be len+"b"
	static constexpr uint8_t kHello[] = {
		0x00,
		0x00,
		0x00,
		0x01,  // protocol_version = 1
		0x00,
		0x00,
		0x00,
		0x01,  // session length
		0x62,  // 'b'
	};
	check_golden("Hello{1,\"b\"}", encoded(inst::Hello{1, "b"}), kHello);

	// HelloReply SessionMismatch: tag i8 = 3, no payload
	static constexpr uint8_t kSessionMismatch[] = {0x03};
	inst::HelloReply mismatch;
	mismatch.value = inst::HelloReplySessionMismatch{};
	check_golden(
		"HelloReply SessionMismatch", encoded(mismatch), kSessionMismatch);

	// Error{NotFound, "x"}: i8 code = 3, u32be len+"x"
	static constexpr uint8_t kError[] = {
		0x03,  // ErrorCode::NotFound
		0x00,
		0x00,
		0x00,
		0x01,  // message length
		0x78,  // 'x'
	};
	check_golden("Error{NotFound,\"x\"}",
		encoded(inst::Error{inst::ErrorCode::NotFound, "x"}), kError);
}

void
test_truncation()
{
	const vector<byte> full = encoded(inst::Hello{1, "b"});
	CHECK(!full.empty());
	for (size_t n = 0; n < full.size(); ++n) {
		dn::ipc::Decoder dec(span<const byte>(full.data(), n));
		inst::HelloView view{};
		const bool ok = decode(dec, view);
		CHECK(!ok);
		CHECK(dec.error() == dn::ipc::DecodeError::Truncated);
	}
	dn::ipc::Decoder dec(full);
	inst::HelloView view{};
	CHECK(decode(dec, view));
	CHECK(dec.remaining() == 0);
	CHECK(dec.error() == dn::ipc::DecodeError::Ok);
	CHECK(view.protocol_version == 1);
	CHECK(view.session == "b");
}

void
test_trailing_bytes()
{
	vector<byte> buf = encoded(inst::Hello{1, "b"});
	buf.push_back(byte{0x00});
	dn::ipc::Decoder dec(buf);
	inst::HelloView view{};
	const bool ok = decode(dec, view);
	// Generated Hello decode succeeds and leaves the extra byte.
	// Connection/Server require exact consumption.
	CHECK(!ok || dec.remaining() != 0);
}

void
test_invalid_utf8()
{
	{
		// u32be length 2, overlong NUL (C0 80)
		const uint8_t raw[] = {0x00, 0x00, 0x00, 0x02, 0xC0, 0x80};
		dn::ipc::Decoder dec(as_bytes(span(raw)));
		string_view s;
		CHECK(!dec.string(s));
		CHECK(dec.error() == dn::ipc::DecodeError::InvalidUtf8);
	}
	{
		// u32be length 1, truncated 2-byte sequence
		const uint8_t raw[] = {0x00, 0x00, 0x00, 0x01, 0xC2};
		dn::ipc::Decoder dec(as_bytes(span(raw)));
		string_view s;
		CHECK(!dec.string(s));
		CHECK(dec.error() == dn::ipc::DecodeError::InvalidUtf8);
	}
}

void
test_zero_and_unknown_enum()
{
	{
		const byte raw[] = {byte{0}};
		dn::ipc::Decoder dec(raw);
		inst::HelloResult value{};
		CHECK(!decode(dec, value));
	}
	{
		const byte raw[] = {byte{99}};
		dn::ipc::Decoder dec(raw);
		inst::HelloResult value{};
		CHECK(!decode(dec, value));
	}
}

void
test_unknown_union_tag()
{
	const byte raw[] = {byte{99}};
	dn::ipc::Decoder dec(raw);
	inst::HelloReplyView view{};
	CHECK(!decode(dec, view));
}

void
test_huge_array_count()
{
	const size_t n = dn::ipc::Decoder::kMaxElements + 1;
	{
		const uint8_t raw[] = {0xFF, 0xFF, 0xFF, 0xFF};
		dn::ipc::Decoder dec(as_bytes(span(raw)));
		inst::OpenRequestView view{};
		CHECK(!decode(dec, view));
	}
	{
		vector<byte> raw(n);
		dn::ipc::Decoder dec(raw);
		span<const byte> out;
		CHECK(!dec.bytes(out, n));
		CHECK(dec.error() == dn::ipc::DecodeError::Limit);
	}
	{
		vector<byte> raw(4 + n, byte{0});
		raw[0] = byte((n >> 24) & 0xff);
		raw[1] = byte((n >> 16) & 0xff);
		raw[2] = byte((n >> 8) & 0xff);
		raw[3] = byte(n & 0xff);
		dn::ipc::Decoder dec(raw);
		string_view sv;
		CHECK(!dec.string(sv));
		CHECK(dec.error() == dn::ipc::DecodeError::Limit);
	}
}

}  // namespace

int
main()
{
	test_round_trip_hello_reply();
	test_round_trip_result();
	test_round_trip_frames();
	test_goldens();
	test_truncation();
	test_trailing_bytes();
	test_invalid_utf8();
	test_zero_and_unknown_enum();
	test_unknown_union_tag();
	test_huge_array_count();

	if (g_failures) {
		fprintf(stderr, "%d check(s) failed\n", g_failures);
		return 1;
	}
	puts("ok");
	return 0;
}
