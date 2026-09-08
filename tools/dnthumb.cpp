//
// dnthumb.cpp: command-line client for dnthumbd
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-rpc.hpp"
#include "ipc-shm.hpp"
#include "ipc/thumbd.lxdr.hpp"
#include "libdn.h"
#include "png-io.hpp"

#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <variant>
#include <vector>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

using namespace std;
namespace proto = dawn::ipc::thumbd;

// --- Helpers -----------------------------------------------------------------

static vector<byte>
encoded(const proto::Frame &frame)
{
	vector<byte> out;
	dawn::ipc::Encoder encoder(out);
	encode(frame, encoder);
	return out;
}
static bool
parse_size(string_view value, uint32_t &w, uint32_t &h)
{
	const size_t x = value.find_first_of("xX");
	if (x == string_view::npos)
		return false;
	auto a = from_chars(value.data(), value.data() + x, w);
	auto b = from_chars(value.data() + x + 1, value.data() + value.size(), h);
	return a.ec == errc{} && a.ptr == value.data() + x && b.ec == errc{} &&
		b.ptr == value.data() + value.size() && w && h;
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
main(int argc, char **argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif

	// TODO(p): getopt_long
	uint32_t bound_w = 512, bound_h = 256;
	if (argc == 3 && string_view(argv[1]) == "-s") {
		if (!parse_size(argv[2], bound_w, bound_h)) {
			fprintf(stderr, "dnthumb: invalid size\n");
			return 1;
		}
	} else if (argc != 1) {
		fprintf(stderr, "Usage: dnthumb [-s WxH]\n");
		return 1;
	}

	vector<uint8_t> input;
	uint8_t chunk[65536];
	while (size_t n = fread(chunk, 1, sizeof chunk, stdin)) {
		if (uint64_t(input.size()) > uint64_t(proto::kThumbdMaxBlobSize) - n) {
			fprintf(stderr, "dnthumb: input is too large\n");
			return 1;
		}
		input.insert(input.end(), chunk, chunk + n);
	}
	if (ferror(stdin) || input.empty()) {
		fprintf(stderr, "dnthumb: cannot read input\n");
		return 1;
	}

	auto cmm = make_shared<dawn::Cmm>();
	dawn::OpenContext context;
	context.cmm = cmm;
	context.screen_profile = cmm->get_profile_sRGB(false);
	context.first_frame_only = true;
	dawn::Error open_error;
	auto image = dawn::open_from_data(input, context, &open_error);
	if (!image) {
		fprintf(stderr, "dnthumb: %s\n",
			open_error.message.empty() ? "invalid PNG"
									   : open_error.message.c_str());
		return 1;
	}

	int32_t orientation = int32_t(image->orientation);
	if (orientation < 0 || orientation > 8)
		orientation = 0;
	if (auto it = image->text.find("dawn:orientation");
		it != image->text.end()) {
		int32_t parsed = 0;
		auto result = from_chars(
			it->second.data(), it->second.data() + it->second.size(), parsed);
		if (result.ec == errc{} &&
			result.ptr == it->second.data() + it->second.size() &&
			parsed >= 0 && parsed <= 8)
			orientation = parsed;
	}
	uint32_t display_w, display_h;
	dawn::orientation_display_size(image->width, image->height,
		dawn::orientation_or_0(dawn::Orientation(orientation)), &display_w,
		&display_h);
	const double scale =
		min(1.0, min(double(bound_w) / display_w, double(bound_h) / display_h));
	const uint32_t out_w = max(1u, uint32_t(display_w * scale + .5));
	const uint32_t out_h = max(1u, uint32_t(display_h * scale + .5));

	chrono::milliseconds budget = chrono::seconds(10);
	auto channel = dawn::ipc::Channel::connect("thumbd", budget);
	if (!channel.ok()) {
		fprintf(stderr, "dnthumb: dnthumbd is not running\n");
		return 1;
	}

	proto::Frame hello;
	hello.payload.value =
		proto::PayloadHello{proto::Hello{proto::kThumbdProtocolVersion, ""}};
	if (!channel.send(encoded(hello), budget, {}))
		return 1;
	vector<byte> wire;
	vector<dawn::ipc::Handle> reply_handles;
	proto::FrameView reply;
	const bool received = receive(channel, reply, wire, reply_handles, budget);
	dawn::ipc::OwnedHandles owned_reply_handles(std::move(reply_handles));
	if (!received || !owned_reply_handles.empty() ||
		!holds_alternative<proto::PayloadHelloReplyView>(reply.payload.value)) {
		fprintf(stderr, "dnthumb: incompatible daemon\n");
		return 1;
	}

	const auto *accepted = get_if<proto::DaemonHelloReplyAcceptedView>(
		&get<proto::PayloadHelloReplyView>(reply.payload.value)
			.hello_reply.value);
	if (!accepted || !accepted->limits.max_payload_size ||
		accepted->limits.max_payload_size >
			dawn::ipc::Connection::kMaxPayload ||
		!accepted->limits.max_blob_size ||
		accepted->limits.max_blob_size > proto::kThumbdMaxBlobSize) {
		fprintf(stderr, "dnthumb: invalid daemon limits\n");
		return 1;
	}

	const uint32_t max_payload = accepted->limits.max_payload_size;
	const uint64_t max_blob = accepted->limits.max_blob_size;
	channel.set_max_payload(max_payload);

	proto::ScaleRequest scale_request;
	scale_request.source.width = image->width;
	scale_request.source.height = image->height;
	scale_request.source.stride = image->stride;
	scale_request.source.orientation = orientation;
	scale_request.out_width = out_w;
	scale_request.out_height = out_h;
	dawn::ipc::SharedMemory request_memory;
	dawn::ipc::Handle request_handle;
	span<const dawn::ipc::Handle> request_handles;
	if (image->data.empty() || image->data.size() > max_blob) {
		fprintf(stderr, "dnthumb: image is too large\n");
		return 1;
	}
	scale_request.source.pixels.value = proto::BlobInline{};
	proto::Frame request;
	request.payload.value =
		proto::PayloadRequest{proto::Request{1, scale_request}};
	const size_t inline_overhead = encoded(request).size();
	vector<byte> request_wire;
	if (inline_overhead <= max_payload &&
		image->data.size() <= max_payload - inline_overhead) {
		proto::BlobInline inline_blob;
		inline_blob.bytes.assign(
			reinterpret_cast<const byte *>(image->data.data()),
			reinterpret_cast<const byte *>(
				image->data.data() + image->data.size()));
		scale_request.source.pixels.value = std::move(inline_blob);
		request.payload.value =
			proto::PayloadRequest{proto::Request{1, std::move(scale_request)}};
		request_wire = encoded(request);
	} else {
		request_memory = dawn::ipc::SharedMemory::copy(
			image->data.data(), image->data.size());
		if (!request_memory.ok())
			return 1;
		scale_request.source.pixels.value =
			proto::BlobShared{image->data.size()};
		request_handle = request_memory.handle();
		request_handles = span(&request_handle, 1);
		request.payload.value =
			proto::PayloadRequest{proto::Request{1, std::move(scale_request)}};
		request_wire = encoded(request);
	}
	if (request_wire.size() > max_payload ||
		!channel.send(request_wire, budget, request_handles))
		return 1;

	vector<dawn::ipc::Handle> handles;
	proto::FrameView response;
	const bool response_received =
		receive(channel, response, wire, handles, budget);
	dawn::ipc::OwnedHandles owned_handles(std::move(handles));
	if (!response_received)
		return 1;
	auto payload = get_if<proto::PayloadResponseView>(&response.payload.value);
	if (!payload)
		return 1;
	if (payload->response.id != 1) {
		fprintf(stderr, "dnthumb: response request ID mismatch\n");
		return 1;
	}

	auto scaled =
		get_if<proto::ResultScaledView>(&payload->response.result.value);
	if (!scaled) {
		if (!owned_handles.empty())
			return 1;
		auto error =
			get_if<proto::ResultErrorView>(&payload->response.result.value);
		fprintf(stderr, "dnthumb: %.*s\n",
			error ? int(error->error.message.size()) : 12,
			error ? error->error.message.data() : "scale failed");
		return 1;
	}
	const uint64_t rgba_size =
		uint64_t(scaled->scaled.width) * scaled->scaled.height * 4;
	if (!scaled->scaled.width || !scaled->scaled.height ||
		scaled->scaled.width != out_w || scaled->scaled.height != out_h ||
		scaled->scaled.width > dawn::kMaxDimension ||
		scaled->scaled.height > dawn::kMaxDimension || rgba_size > max_blob) {
		fprintf(stderr, "dnthumb: invalid scaled image geometry\n");
		return 1;
	}

	span<const uint8_t> rgba;
	dawn::ipc::SharedMemory response_memory;
	if (auto in = get_if<proto::BlobInlineView>(&scaled->scaled.rgba8.value)) {
		if (!owned_handles.empty() || in->bytes.size() != rgba_size ||
			in->bytes.size() > max_blob)
			return 1;
		rgba = {reinterpret_cast<const uint8_t *>(in->bytes.data()),
			in->bytes.size()};
	} else {
		auto shared = get<proto::BlobSharedView>(scaled->scaled.rgba8.value);
		if (owned_handles.size() != 1 || shared.size != rgba_size ||
			shared.size > max_blob)
			return 1;
		response_memory =
			dawn::ipc::SharedMemory::map(owned_handles.take(0), shared.size);
		if (!response_memory.ok())
			return 1;
		rgba = {response_memory.data(), response_memory.size()};
	}

	vector<uint8_t> icc = image->effective_profile
		? image->effective_profile->to_bytes()
		: vector<uint8_t>{};
	string error;
	if (!write_png8(stdout, scaled->scaled.width, scaled->scaled.height, rgba,
			icc, &error)) {
		fprintf(stderr, "dnthumb: %s\n", error.c_str());
		return 1;
	}
}
