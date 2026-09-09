//
// test-lxdr.cpp: LibertyXDR encode/decode and generated instance types
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn/ipc.hpp"

#include "ipc/imaged.lxdr.hpp"
#include "ipc/instance.lxdr.hpp"
#include "ipc/thumbd.lxdr.hpp"
#include "test.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

using namespace std;
namespace inst = dawn::ipc::instance;
namespace imaged = dawn::ipc::imaged;
namespace thumbd = dawn::ipc::thumbd;

template <typename T>
static vector<byte>
wire(const T &value)
{
	vector<byte> buf;
	dawn::ipc::Encoder enc(buf);
	encode(value, enc);
	CHECK(enc.ok());
	return buf;
}

static bool
bytes_eq(span<const byte> got, span<const uint8_t> want)
{
	if (got.size() != want.size())
		return false;
	for (size_t i = 0; i < got.size(); i++) {
		if (uint8_t(got[i]) != want[i])
			return false;
	}
	return true;
}

static void
check_golden(
	const char *label, const vector<byte> &got, span<const uint8_t> want)
{
	if (bytes_eq(got, want))
		return;
	test::fail("golden %s: got %llu bytes, want %llu", label,
		(unsigned long long) got.size(), (unsigned long long) want.size());
	for (size_t i = 0; i < got.size(); i++)
		fprintf(stderr, " %02x", uint8_t(got[i]));
	fprintf(stderr, "\n");
}

static void
test_round_trip_hello_reply()
{
	{
		dawn::ipc::HelloReply own;
		own.value =
			dawn::ipc::HelloReplyAccepted{dawn::ipc::Limits{0x00100000u}};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		dawn::ipc::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<dawn::ipc::HelloReplyAcceptedView>(view.value));
		CHECK(get<dawn::ipc::HelloReplyAcceptedView>(view.value)
				  .limits.max_payload_size == 0x00100000u);
	}
	{
		dawn::ipc::HelloReply own;
		own.value = dawn::ipc::HelloReplyVersionMismatch{2};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		dawn::ipc::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<dawn::ipc::HelloReplyVersionMismatchView>(
			view.value));
		CHECK(get<dawn::ipc::HelloReplyVersionMismatchView>(view.value)
				  .server_protocol_version == 2);
	}
	{
		dawn::ipc::HelloReply own;
		own.value = dawn::ipc::HelloReplySessionMismatch{};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		dawn::ipc::HelloReplyView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<dawn::ipc::HelloReplySessionMismatchView>(
			view.value));
	}
}

static void
test_round_trip_result()
{
	{
		inst::Result own;
		own.value = inst::ResultDone{};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		inst::ResultView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::ResultDoneView>(view.value));
	}
	{
		inst::Result own;
		own.value = inst::ResultError{
			dawn::ipc::Error{dawn::ipc::ErrorCode::Busy, "nope"}};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		inst::ResultView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::ResultErrorView>(view.value));
		const auto &err = get<inst::ResultErrorView>(view.value).error;
		CHECK(err.code == dawn::ipc::ErrorCode::Busy);
		CHECK(err.message == "nope");
	}
}

static void
test_round_trip_frames()
{
	{
		dawn::ipc::Hello hello{1, "sess"};
		const vector<byte> buf = wire(hello);
		dawn::ipc::Decoder dec(buf);
		dawn::ipc::HelloView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(view.protocol_version == 1);
		CHECK(view.session == "sess");
	}
	{
		inst::OpenRequest open;
		open.urls = {"/one", "/two"};
		open.activation_token = "tok";
		open.mode = "browse";
		const vector<byte> buf = wire(open);
		dawn::ipc::Decoder dec(buf);
		inst::OpenRequestView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(view.urls.size() == 2);
		CHECK(view.urls[0] == "/one");
		CHECK(view.urls[1] == "/two");
		CHECK(view.activation_token == "tok");
		CHECK(view.mode == "browse");
	}

	{
		inst::Frame own;
		own.payload.value = inst::PayloadHello{dawn::ipc::Hello{1, "b"}};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadHelloView>(view.payload.value));
		const auto &h = get<inst::PayloadHelloView>(view.payload.value).hello;
		CHECK(h.protocol_version == 1);
		CHECK(h.session == "b");
	}
	{
		dawn::ipc::HelloReply reply;
		reply.value = dawn::ipc::HelloReplyAccepted{dawn::ipc::Limits{4096}};
		inst::Frame own;
		own.payload.value = inst::PayloadHelloReply{reply};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(
			holds_alternative<inst::PayloadHelloReplyView>(view.payload.value));
		const auto &hr =
			get<inst::PayloadHelloReplyView>(view.payload.value).hello_reply;
		CHECK(holds_alternative<dawn::ipc::HelloReplyAcceptedView>(hr.value));
		CHECK(get<dawn::ipc::HelloReplyAcceptedView>(hr.value)
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
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
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
		CHECK(o.mode.empty());
	}
	{
		inst::Response resp;
		resp.id = 9;
		resp.result.value = inst::ResultDone{};
		inst::Frame own;
		own.payload.value = inst::PayloadResponse{resp};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
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
		own.payload.value =
			inst::PayloadCancel{dawn::ipc::Cancel{0x100000002ull}};
		const vector<byte> buf = wire(own);
		dawn::ipc::Decoder dec(buf);
		inst::FrameView view{};
		CHECK(decode(dec, view));
		CHECK(dec.remaining() == 0);
		CHECK(holds_alternative<inst::PayloadCancelView>(view.payload.value));
		CHECK(get<inst::PayloadCancelView>(view.payload.value).cancel.id ==
			0x100000002ull);
	}
}

static void
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
	check_golden("Hello{1,\"b\"}", wire(dawn::ipc::Hello{1, "b"}), kHello);

	// HelloReply SessionMismatch: tag i8 = 3, no payload
	static constexpr uint8_t kSessionMismatch[] = {0x03};
	dawn::ipc::HelloReply mismatch;
	mismatch.value = dawn::ipc::HelloReplySessionMismatch{};
	check_golden(
		"HelloReply SessionMismatch", wire(mismatch), kSessionMismatch);

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
		wire(dawn::ipc::Error{dawn::ipc::ErrorCode::NotFound, "x"}), kError);
}

static void
test_truncation()
{
	const vector<byte> full = wire(dawn::ipc::Hello{1, "b"});
	CHECK(!full.empty());
	for (size_t n = 0; n < full.size(); n++) {
		dawn::ipc::Decoder dec(span<const byte>(full.data(), n));
		dawn::ipc::HelloView view{};
		const bool ok = decode(dec, view);
		CHECK(!ok);
		CHECK(dec.error() == dawn::ipc::DecodeError::Truncated);
	}
	dawn::ipc::Decoder dec(full);
	dawn::ipc::HelloView view{};
	CHECK(decode(dec, view));
	CHECK(dec.remaining() == 0);
	CHECK(dec.error() == dawn::ipc::DecodeError::Ok);
	CHECK(view.protocol_version == 1);
	CHECK(view.session == "b");
}

static void
test_trailing_bytes()
{
	vector<byte> buf = wire(dawn::ipc::Hello{1, "b"});
	buf.push_back(byte{0x00});
	dawn::ipc::Decoder dec(buf);
	dawn::ipc::HelloView view{};
	const bool ok = decode(dec, view);
	// Generated Hello decode succeeds and leaves the extra byte.
	// Connection/Server require exact consumption.
	CHECK(!ok || dec.remaining() != 0);
}

static void
test_invalid_utf8()
{
	{
		// u32be length 2, overlong NUL (C0 80)
		const uint8_t raw[] = {0x00, 0x00, 0x00, 0x02, 0xC0, 0x80};
		dawn::ipc::Decoder dec(as_bytes(span(raw)));
		string_view s;
		CHECK(!dec.string(s));
		CHECK(dec.error() == dawn::ipc::DecodeError::InvalidUtf8);
	}
	{
		// u32be length 1, truncated 2-byte sequence
		const uint8_t raw[] = {0x00, 0x00, 0x00, 0x01, 0xC2};
		dawn::ipc::Decoder dec(as_bytes(span(raw)));
		string_view s;
		CHECK(!dec.string(s));
		CHECK(dec.error() == dawn::ipc::DecodeError::InvalidUtf8);
	}
}

static void
test_zero_and_unknown_enum()
{
	for (byte tag : {byte{0}, byte{99}}) {
		const byte raw[] = {tag};
		dawn::ipc::Decoder dec(raw);
		dawn::ipc::HelloResult value{};
		CHECK(!decode(dec, value));
	}
}

static void
test_unknown_union_tag()
{
	const byte raw[] = {byte{99}};
	dawn::ipc::Decoder dec(raw);
	dawn::ipc::HelloReplyView view{};
	CHECK(!decode(dec, view));
}

static void
test_huge_array_count()
{
	const size_t n = dawn::ipc::Decoder::kMaxElements + 1;
	{
		const uint8_t raw[] = {0xFF, 0xFF, 0xFF, 0xFF};
		dawn::ipc::Decoder dec(as_bytes(span(raw)));
		inst::OpenRequestView view{};
		CHECK(!decode(dec, view));
	}
	{
		vector<byte> raw(n);
		dawn::ipc::Decoder dec(raw);
		span<const byte> out;
		CHECK(!dec.bytes(out, n));
		CHECK(dec.error() == dawn::ipc::DecodeError::Limit);
	}
	{
		vector<byte> raw(4 + n, byte{0});
		raw[0] = byte((n >> 24) & 0xff);
		raw[1] = byte((n >> 16) & 0xff);
		raw[2] = byte((n >> 8) & 0xff);
		raw[3] = byte(n & 0xff);
		dawn::ipc::Decoder dec(raw);
		string_view sv;
		CHECK(!dec.string(sv));
		CHECK(dec.error() == dawn::ipc::DecodeError::Limit);
	}
}

static void
test_daemon_schemas()
{
	dawn::ipc::DaemonHelloReply hello;
	hello.value = dawn::ipc::DaemonHelloReplyAccepted{
		dawn::ipc::DaemonLimits{4096, imaged::kImagedMaxBlobSize}};
	auto bytes = wire(hello);
	dawn::ipc::Decoder hello_decoder(bytes);
	dawn::ipc::DaemonHelloReplyView hello_view;
	CHECK(decode(hello_decoder, hello_view));
	const auto *accepted =
		get_if<dawn::ipc::DaemonHelloReplyAcceptedView>(&hello_view.value);
	CHECK(accepted != nullptr);
	if (accepted)
		CHECK(accepted->limits.max_blob_size == imaged::kImagedMaxBlobSize);

	dawn::ipc::Pixmap pixmap;
	pixmap.width = 2;
	pixmap.height = 3;
	pixmap.stride = 16;
	pixmap.orientation = 6;
	pixmap.pixels.value = dawn::ipc::BlobShared{48};
	imaged::DecodeResponse response{pixmap, "wuffs", {}, true};
	bytes = wire(response);
	dawn::ipc::Decoder decoder(bytes);
	imaged::DecodeResponseView view;
	CHECK(decode(decoder, view));
	CHECK(
		holds_alternative<dawn::ipc::BlobSharedView>(view.pixmap.pixels.value));
	CHECK(get<dawn::ipc::BlobSharedView>(view.pixmap.pixels.value).size == 48);

	thumbd::ScaleResponse scaled;
	scaled.width = 1;
	scaled.height = 1;
	scaled.rgba8.value =
		dawn::ipc::BlobInline{{byte{1}, byte{2}, byte{3}, byte{4}}};
	bytes = wire(scaled);
	dawn::ipc::Decoder decoder2(bytes);
	thumbd::ScaleResponseView scaled_view;
	CHECK(decode(decoder2, scaled_view));
	CHECK(
		holds_alternative<dawn::ipc::BlobInlineView>(scaled_view.rgba8.value));
	CHECK(
		get<dawn::ipc::BlobInlineView>(scaled_view.rgba8.value).bytes.size() ==
		4);
}

static void
test_blob_envelope_sizes()
{
	imaged::DecodeRequest decode;
	decode.data.value = dawn::ipc::BlobInline{};
	imaged::Frame request;
	request.payload.value = imaged::PayloadRequest{imaged::Request{1, decode}};
	const size_t imaged_overhead = wire(request).size();
	get<imaged::PayloadRequest>(request.payload.value)
		.request.decode.data.value =
		dawn::ipc::BlobInline{vector<byte>(123, byte{1})};
	CHECK(wire(request).size() == imaged_overhead + 123);
	imaged::DecodeResponse decoded;
	decoded.pixmap.width = 2;
	decoded.pixmap.height = 2;
	decoded.pixmap.stride = 16;
	decoded.pixmap.pixels.value = dawn::ipc::BlobInline{};
	decoded.loader = "variable-loader";
	decoded.icc = vector<byte>(31, byte{2});
	imaged::Frame decoded_frame;
	decoded_frame.payload.value = imaged::PayloadResponse{
		imaged::Response{2, imaged::Result{imaged::ResultDecoded{decoded}}}};
	const size_t decoded_overhead = wire(decoded_frame).size();
	get<imaged::ResultDecoded>(
		get<imaged::PayloadResponse>(decoded_frame.payload.value)
			.response.result.value)
		.decoded.pixmap.pixels.value =
		dawn::ipc::BlobInline{vector<byte>(89, byte{3})};
	CHECK(wire(decoded_frame).size() == decoded_overhead + 89);

	thumbd::ScaleResponse scale;
	scale.width = 1;
	scale.height = 1;
	scale.rgba8.value = dawn::ipc::BlobInline{};
	thumbd::Frame response;
	response.payload.value = thumbd::PayloadResponse{
		thumbd::Response{1, thumbd::Result{thumbd::ResultScaled{scale}}}};
	const size_t thumbd_overhead = wire(response).size();
	get<thumbd::PayloadResponse>(response.payload.value).response.result.value =
		thumbd::ResultScaled{thumbd::ScaleResponse{
			1, 1, dawn::ipc::Blob{dawn::ipc::BlobInline{vector<byte>(77)}}}};
	CHECK(wire(response).size() == thumbd_overhead + 77);
}

int
main()
{
	return test::run({
		{"Hello reply round trips", test_round_trip_hello_reply},
		{"result round trips", test_round_trip_result},
		{"frame round trips", test_round_trip_frames},
		{"golden encodings", test_goldens},
		{"truncated input", test_truncation},
		{"trailing input", test_trailing_bytes},
		{"invalid UTF-8", test_invalid_utf8},
		{"invalid enum", test_zero_and_unknown_enum},
		{"unknown union tag", test_unknown_union_tag},
		{"size limits", test_huge_array_count},
		{"daemon schemas", test_daemon_schemas},
		{"blob envelope sizes", test_blob_envelope_sizes},
	});
}
