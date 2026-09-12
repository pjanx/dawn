//
// main.cpp: out-of-process image decoder probe
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "ipc-rpc.hpp"
#include "ipc-shm.hpp"
#include "ipc/imaged.lxdr.hpp"
#include "libdn.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

using namespace std;
namespace proto = dawn::ipc::imaged;

// --- Jobs --------------------------------------------------------------------

static constexpr char kService[] = "imaged";
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
	vector<byte> data, target_icc;
	dawn::ipc::SharedMemory shared;
	bool first_frame_only = false;
};

struct State {
	unordered_set<uint64_t> greeted;
	dawn::ipc::DaemonHost *host = nullptr;
};

}  // namespace

static dawn::ipc::DaemonHost::Reply
decode_job(const Job &job)
{
	// libdn wants a Cmm per thread, and this only ever runs on workers.
	static thread_local auto cmm = make_shared<dawn::Cmm>();

	dawn::ipc::DaemonHost::Reply reply;
	dawn::OpenContext context;
	context.cmm = cmm;
	context.first_frame_only = job.first_frame_only;
	context.screen_profile = job.target_icc.empty()
		? cmm->get_profile_sRGB()
		: cmm->get_profile(span<const uint8_t>(
			  reinterpret_cast<const uint8_t *>(job.target_icc.data()),
			  job.target_icc.size()));
	if (!context.screen_profile) {
		reply.payload = error_frame(job.request, "invalid target ICC profile",
			dawn::ipc::ErrorCode::InvalidArgument);
		return reply;
	}

	dawn::Error error;
	const uint8_t *data = job.shared.ok()
		? job.shared.data()
		: reinterpret_cast<const uint8_t *>(job.data.data());
	const size_t size = job.shared.ok() ? job.shared.size() : job.data.size();
	auto image = dawn::open_from_data(span(data, size), context, &error);
	if (!image) {
		reply.payload = error_frame(job.request,
			error.message.empty() ? "decode failed" : error.message,
			dawn::ipc::ErrorCode::InvalidArgument);
		return reply;
	}
	if (image->data.empty() || image->data.size() > kMaxBlob) {
		reply.payload = error_frame(job.request, "decoded pixmap is too large",
			dawn::ipc::ErrorCode::InvalidArgument);
		return reply;
	}

	proto::DecodeResponse response;
	response.pixmap.width = image->width;
	response.pixmap.height = image->height;
	response.pixmap.stride = image->stride;
	response.pixmap.orientation = int32_t(image->orientation);
	response.loader = image->loader ? image->loader : "";
	response.icc.assign(reinterpret_cast<const byte *>(image->icc.data()),
		reinterpret_cast<const byte *>(image->icc.data() + image->icc.size()));
	response.profile_assumed = image->profile_assumed;
	const auto encode_response = [&] {
		proto::Frame frame;
		frame.payload.value = proto::PayloadResponse{proto::Response{
			job.request, proto::Result{proto::ResultDecoded{response}}}};
		return dawn::ipc::encoded(frame);
	};
	if (const char *failure = dawn::ipc::place_blob(response.pixmap.pixels,
			as_bytes(span(image->data)), dawn::ipc::Connection::kMaxPayload,
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
			proto::kImagedProtocolVersion, proto::kImagedMaxBlobSize);
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
			error_frame(request->request.id, "image service is busy",
				dawn::ipc::ErrorCode::Busy),
			{});
	};

	const auto &request_decode = request->request.decode;
	auto job = make_shared<Job>();
	job->request = request->request.id;
	span<const byte> data;
	if (const char *failure = dawn::ipc::take_blob(
			request_decode.data, owned, kMaxBlob, data, job->shared))
		return invalid(failure);
	if (!job->shared.ok())
		job->data.assign(data.begin(), data.end());
	job->target_icc.assign(
		request_decode.target_icc.begin(), request_decode.target_icc.end());
	job->first_frame_only = request_decode.first_frame_only;

	if (!state.host->submit(connection, data.size() + job->target_icc.size(),
			[job] { return decode_job(*job); }))
		return busy();
	return true;
}

int
main()
{
	// The client displays what we send it, already rendered, so these
	// diagnostics speak whatever language this process started in.
	dawn::gettext_init();

	auto endpoint = dawn::ipc::Endpoint::listen(kService);
	if (endpoint.status != dawn::ipc::Endpoint::ListenStatus::Ok) {
		fprintf(stderr, "dnimaged: %s\n",
			endpoint.status == dawn::ipc::Endpoint::ListenStatus::InUse
				? "already running"
				: "cannot listen");
		return 1;
	}

	State state;

	dawn::ipc::ServerCore::Config config;
	config.on_payload = [&](uint64_t connection, span<const byte> payload,
							vector<dawn::ipc::Handle> attachments) {
		return handle_payload(
			state, connection, payload, std::move(attachments));
	};
	config.on_closed = [&](uint64_t id) { state.greeted.erase(id); };

	dawn::ipc::DaemonHost host(std::move(endpoint.listener), std::move(config),
		min(4u, max(1u, thread::hardware_concurrency())));
	state.host = &host;
	return host.run() ? 0 : 1;
}
