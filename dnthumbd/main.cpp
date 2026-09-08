//
// main.cpp: out-of-process Vulkan scaler probe
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-rpc.hpp"
#include "ipc-shm.hpp"
#include "ipc/thumbd.lxdr.hpp"
#include "libdn.h"
#include "libdnvk.h"

#include <cstdio>
#include <memory>
#include <unordered_set>
#include <variant>
#include <vector>

using namespace std;
namespace proto = dawn::ipc::thumbd;

// --- Jobs --------------------------------------------------------------------

static constexpr char kService[] = "thumbd";
static constexpr uint64_t kMaxBlob = 1024ull * 1024 * 1024;

static vector<byte>
error_frame(uint64_t id, string message, dawn::ipc::ErrorCode code)
{
	proto::Frame frame;
	frame.payload.value = proto::PayloadResponse{proto::Response{id,
		proto::Result{
			proto::ResultError{dawn::ipc::Error{code, std::move(message)}}}}};
	return dawn::ipc::encoded(frame);
}

namespace
{

struct Job {
	uint64_t request = 0;
	uint32_t width = 0, height = 0, stride = 0, out_width = 0, out_height = 0;
	int32_t orientation = 0;
	vector<byte> inline_pixels;
	dawn::ipc::SharedMemory shared;
};

struct State {
	unordered_set<uint64_t> greeted;
	dawn::ipc::DaemonHost *host = nullptr;
	dawn::ScaleScaler *scaler = nullptr;
};

}  // namespace

static dawn::ipc::DaemonHost::Reply
scale_job(const Job &job, dawn::ScaleScaler *scaler)
{
	dawn::ipc::DaemonHost::Reply reply;
	dawn::ScaleOutput output;
	string error;
	const uint8_t *pixels = job.shared.ok()
		? job.shared.data()
		: reinterpret_cast<const uint8_t *>(job.inline_pixels.data());
	if (!scaler->scale(job.width, job.height, pixels, job.stride, job.out_width,
			job.out_height,
			dawn::orientation_or_0(dawn::Orientation(job.orientation)), &output,
			&error)) {
		reply.payload =
			error_frame(job.request, error.empty() ? "scale failed" : error,
				dawn::ipc::ErrorCode::InvalidArgument);
		return reply;
	}

	proto::ScaleResponse response;
	response.width = output.width;
	response.height = output.height;
	const auto encode_response = [&] {
		proto::Frame frame;
		frame.payload.value = proto::PayloadResponse{proto::Response{
			job.request, proto::Result{proto::ResultScaled{response}}}};
		return dawn::ipc::encoded(frame);
	};
	if (const char *failure = dawn::ipc::place_blob(response.rgba8,
			as_bytes(span(output.rgba8)), dawn::ipc::Connection::kMaxPayload,
			encode_response, reply.payload, reply.attachment))
		reply.payload =
			error_frame(job.request, failure, dawn::ipc::ErrorCode::Internal);
	return reply;
}

// --- Server ------------------------------------------------------------------

static bool
handle_payload(State &state, uint64_t connection, span<const byte> payload,
	vector<dawn::ipc::Handle> attachments)
{
	dawn::ipc::OwnedHandles owned(std::move(attachments));
	dawn::ipc::Decoder decoder(payload);
	proto::FrameView frame;
	if (!decode(decoder, frame) || decoder.remaining())
		return false;

	if (auto hello = get_if<proto::PayloadHelloView>(&frame.payload.value)) {
		if (!owned.empty() || state.greeted.contains(connection))
			return false;

		const auto reply = dawn::ipc::daemon_hello(hello->hello,
			proto::kThumbdProtocolVersion, proto::kThumbdMaxBlobSize);
		if (holds_alternative<dawn::ipc::DaemonHelloReplyAccepted>(reply.value))
			state.greeted.insert(connection);

		proto::Frame out;
		out.payload.value = proto::PayloadHelloReply{reply};
		return state.host->server().send(
			connection, dawn::ipc::encoded(out), {});
	}
	if (!state.greeted.contains(connection))
		return false;

	auto request = get_if<proto::PayloadRequestView>(&frame.payload.value);
	if (!request)
		return false;

	const auto invalid = [&](string message) {
		return state.host->server().send(connection,
			error_frame(request->request.id, std::move(message),
				dawn::ipc::ErrorCode::InvalidArgument),
			{});
	};
	const auto busy = [&] {
		return state.host->server().send(connection,
			error_frame(request->request.id, "thumbnail service is busy",
				dawn::ipc::ErrorCode::Busy),
			{});
	};

	const auto &scale = request->request.scale;
	auto job = make_shared<Job>();
	job->request = request->request.id;
	job->width = scale.source.width;
	job->height = scale.source.height;
	job->stride = scale.source.stride;
	job->orientation = scale.source.orientation;
	job->out_width = scale.out_width;
	job->out_height = scale.out_height;
	const uint64_t row = uint64_t(job->width) * dawn::kBytesPerPixel;
	if (job->orientation < 0 || job->orientation > 8)
		return invalid("invalid image orientation");
	if (!job->width || !job->height || job->width > dawn::kMaxDimension ||
		job->height > dawn::kMaxDimension || !job->out_width ||
		!job->out_height || job->out_width > dawn::kMaxDimension ||
		job->out_height > dawn::kMaxDimension || job->stride < row ||
		uint64_t(job->out_width) * job->out_height * 4 > kMaxBlob)
		return invalid("invalid image geometry");
	span<const byte> pixels;
	if (const char *failure = dawn::ipc::take_blob(
			scale.source.pixels, owned, kMaxBlob, pixels, job->shared))
		return invalid(failure);
	if (uint64_t(job->stride) * job->height > pixels.size())
		return invalid("pixel blob is too small");
	if (!job->shared.ok())
		job->inline_pixels.assign(pixels.begin(), pixels.end());

	if (!state.host->submit(connection, pixels.size(),
			[job, scaler = state.scaler] { return scale_job(*job, scaler); }))
		return busy();
	return true;
}

int
main()
{
	auto endpoint = dawn::ipc::Endpoint::listen(kService);
	if (endpoint.status != dawn::ipc::Endpoint::ListenStatus::Ok) {
		fprintf(stderr, "dnthumbd: %s\n",
			endpoint.status == dawn::ipc::Endpoint::ListenStatus::InUse
				? "already running"
				: "cannot listen");
		return 1;
	}

	dawn::ScaleScaler scaler;
	string init_error;
	if (!scaler.init(&init_error)) {
		fprintf(stderr, "dnthumbd: %s\n", init_error.c_str());
		return 1;
	}

	State state;
	state.scaler = &scaler;

	dawn::ipc::ServerCore::Config config;
	config.on_payload = [&](uint64_t connection, span<const byte> payload,
							vector<dawn::ipc::Handle> attachments) {
		return handle_payload(
			state, connection, payload, std::move(attachments));
	};
	config.on_closed = [&](uint64_t id) { state.greeted.erase(id); };

	// The scaler serializes internally, so more workers would only queue.
	dawn::ipc::DaemonHost host(
		std::move(endpoint.listener), std::move(config), 1);
	state.host = &host;
	return host.run() ? 0 : 1;
}
