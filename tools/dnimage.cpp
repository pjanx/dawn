//
// dnimage.cpp: command-line client for dnimaged
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-rpc.hpp"
#include "ipc-shm.hpp"
#include "ipc/imaged.lxdr.hpp"
#include "libdn.h"
#include "png-io.hpp"

#include <chrono>
#include <cstdio>
#include <variant>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std;
namespace proto = dawn::ipc::imaged;

// --- IPC ---------------------------------------------------------------------

static vector<byte>
encoded(const proto::Frame &frame)
{
	vector<byte> out;
	dawn::ipc::Encoder encoder(out);
	encode(frame, encoder);
	return out;
}
static bool
receive(dawn::ipc::Channel &channel, proto::FrameView &view,
	vector<byte> &storage, vector<dawn::ipc::Handle> &handles,
	chrono::milliseconds &budget)
{
	if (!channel.recv(storage, budget, &handles))
		return false;
	dawn::ipc::Decoder decoder(storage);
	return decode(decoder, view) && !decoder.remaining();
}

// --- Main --------------------------------------------------------------------

int
main()
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	vector<byte> input;
	byte chunk[65536];
	while (size_t n = fread(chunk, 1, sizeof chunk, stdin)) {
		if (uint64_t(input.size()) > uint64_t(proto::kImagedMaxBlobSize) - n) {
			fprintf(stderr, "dnimage: input is too large\n");
			return 1;
		}
		input.insert(input.end(), chunk, chunk + n);
	}
	if (ferror(stdin) || input.empty()) {
		fprintf(stderr, "dnimage: cannot read input\n");
		return 1;
	}

	chrono::milliseconds budget = chrono::seconds(10);
	auto channel = dawn::ipc::Channel::connect("imaged", budget);
	if (!channel.ok()) {
		fprintf(stderr, "dnimage: dnimaged is not running\n");
		return 1;
	}

	proto::Frame hello;
	hello.payload.value =
		proto::PayloadHello{proto::Hello{proto::kImagedProtocolVersion, ""}};
	if (!channel.send(encoded(hello), budget, {})) {
		fprintf(stderr, "dnimage: handshake failed\n");
		return 1;
	}
	vector<byte> wire;
	vector<dawn::ipc::Handle> reply_handles;
	proto::FrameView reply;
	const bool received = receive(channel, reply, wire, reply_handles, budget);
	dawn::ipc::OwnedHandles owned_reply_handles(std::move(reply_handles));
	if (!received || !owned_reply_handles.empty() ||
		!holds_alternative<proto::PayloadHelloReplyView>(reply.payload.value)) {
		fprintf(stderr, "dnimage: incompatible daemon\n");
		return 1;
	}

	const auto *accepted = get_if<proto::DaemonHelloReplyAcceptedView>(
		&get<proto::PayloadHelloReplyView>(reply.payload.value)
			.hello_reply.value);
	if (!accepted || !accepted->limits.max_payload_size ||
		accepted->limits.max_payload_size >
			dawn::ipc::Connection::kMaxPayload ||
		!accepted->limits.max_blob_size ||
		accepted->limits.max_blob_size > proto::kImagedMaxBlobSize) {
		fprintf(stderr, "dnimage: invalid daemon limits\n");
		return 1;
	}
	const uint32_t max_payload = accepted->limits.max_payload_size;
	const uint64_t max_blob = accepted->limits.max_blob_size;
	channel.set_max_payload(max_payload);
	if (input.size() > max_blob) {
		fprintf(stderr, "dnimage: input is too large\n");
		return 1;
	}

	proto::DecodeRequest decode;
	dawn::ipc::SharedMemory request_memory;
	dawn::ipc::Handle request_handle;
	span<const dawn::ipc::Handle> request_handles;
	decode.first_frame_only = true;
	decode.data.value = proto::BlobInline{};
	proto::Frame request;
	request.payload.value = proto::PayloadRequest{proto::Request{1, decode}};
	const size_t inline_overhead = encoded(request).size();
	vector<byte> request_wire;
	if (inline_overhead <= max_payload &&
		input.size() <= max_payload - inline_overhead) {
		decode.data.value = proto::BlobInline{std::move(input)};
		request.payload.value =
			proto::PayloadRequest{proto::Request{1, std::move(decode)}};
		request_wire = encoded(request);
	} else {
		request_memory =
			dawn::ipc::SharedMemory::copy(input.data(), input.size());
		if (!request_memory.ok())
			return 1;
		decode.data.value = proto::BlobShared{input.size()};
		request_handle = request_memory.handle();
		request_handles = span(&request_handle, 1);
		request.payload.value =
			proto::PayloadRequest{proto::Request{1, std::move(decode)}};
		request_wire = encoded(request);
	}
	if (request_wire.size() > max_payload ||
		!channel.send(request_wire, budget, request_handles)) {
		fprintf(stderr, "dnimage: request failed\n");
		return 1;
	}

	vector<dawn::ipc::Handle> handles;
	proto::FrameView response_frame;
	const bool response_received =
		receive(channel, response_frame, wire, handles, budget);
	dawn::ipc::OwnedHandles owned_handles(std::move(handles));
	if (!response_received) {
		fprintf(stderr, "dnimage: decode failed\n");
		return 1;
	}
	auto payload =
		get_if<proto::PayloadResponseView>(&response_frame.payload.value);
	if (!payload)
		return 1;
	if (payload->response.id != 1) {
		fprintf(stderr, "dnimage: response request ID mismatch\n");
		return 1;
	}

	auto decoded =
		get_if<proto::ResultDecodedView>(&payload->response.result.value);
	if (!decoded) {
		if (!owned_handles.empty())
			return 1;
		auto error =
			get_if<proto::ResultErrorView>(&payload->response.result.value);
		fprintf(stderr, "dnimage: %.*s\n",
			error ? int(error->error.message.size()) : 13,
			error ? error->error.message.data() : "decode failed");
		return 1;
	}
	const auto &pixmap = decoded->decoded.pixmap;
	const uint64_t row = uint64_t(pixmap.width) * dawn::kBytesPerPixel;
	if (!pixmap.width || !pixmap.height || pixmap.width > dawn::kMaxDimension ||
		pixmap.height > dawn::kMaxDimension || pixmap.stride < row) {
		fprintf(stderr, "dnimage: invalid pixmap geometry\n");
		return 1;
	}

	const uint8_t *pixels = nullptr;
	dawn::ipc::SharedMemory response_memory;
	if (auto in = get_if<proto::BlobInlineView>(&pixmap.pixels.value)) {
		if (!owned_handles.empty() || in->bytes.size() > max_blob ||
			uint64_t(pixmap.stride) * pixmap.height > in->bytes.size())
			return 1;
		pixels = reinterpret_cast<const uint8_t *>(in->bytes.data());
	} else {
		auto shared = get<proto::BlobSharedView>(pixmap.pixels.value);
		if (owned_handles.size() != 1 || !shared.size ||
			shared.size > max_blob ||
			uint64_t(pixmap.stride) * pixmap.height > shared.size)
			return 1;
		response_memory =
			dawn::ipc::SharedMemory::map(owned_handles.take(0), shared.size);
		if (!response_memory.ok())
			return 1;
		pixels = response_memory.data();
	}

	auto cmm = make_shared<dawn::Cmm>();
	auto icc = cmm->get_profile_sRGB(false)->to_bytes();
	string error;
	if (!write_png16(stdout, pixmap.width, pixmap.height, pixmap.stride, pixels,
			icc, pixmap.orientation, &error)) {
		fprintf(stderr, "dnimage: %s\n", error.c_str());
		return 1;
	}
}
