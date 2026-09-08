//
// main.cpp: out-of-process image decoder probe
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-loop.hpp"
#include "ipc-rpc.hpp"
#include "ipc-shm.hpp"
#include "ipc/imaged.lxdr.hpp"
#include "libdn.h"

#include <algorithm>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <variant>
#include <vector>

using namespace std;
namespace proto = dawn::ipc::imaged;

// --- Jobs --------------------------------------------------------------------

static constexpr char kService[] = "imaged";
static constexpr uint64_t kMaxBlob = 1024ull * 1024 * 1024;
// Four decoder workers share a bounded retained-input pool. Per-client
// admission keeps one connection from occupying the whole queue.
static constexpr size_t kMaxJobs = 8, kMaxJobsPerConnection = 2;
static constexpr size_t kMaxRetainedBytes =
	1024ull * 1024 * 1024 + dawn::ipc::Connection::kMaxPayload;

static vector<byte>
encode_frame(const proto::Frame &frame)
{
	vector<byte> out;
	dawn::ipc::Encoder encoder(out);
	encode(frame, encoder);
	return out;
}

namespace
{

struct Job {
	uint64_t connection = 0, request = 0;
	size_t retained_bytes = 0;
	vector<byte> data, target_icc;
	dawn::ipc::SharedMemory shared;
	bool first_frame_only = false;
};

struct Done {
	uint64_t connection = 0;
	proto::Frame frame;
	dawn::ipc::SharedMemory memory;
};

struct State {
	mutex mu;
	condition_variable cv;
	deque<Job> jobs;
	deque<Done> done;
	unordered_multiset<uint64_t> admitted;
	unordered_set<uint64_t> greeted;
	size_t retained_bytes = 0;
	bool stop = false;
	dawn::ipc::Loop loop;
	dawn::ipc::ServerCore *server = nullptr;
};

}  // namespace

static bool
admit(State &state, Job &job, size_t bytes)
{
	lock_guard lock(state.mu);
	if (state.admitted.size() >= kMaxJobs ||
		state.admitted.count(job.connection) >= kMaxJobsPerConnection ||
		bytes > kMaxRetainedBytes ||
		state.retained_bytes > kMaxRetainedBytes - bytes)
		return false;

	state.admitted.insert(job.connection);
	state.retained_bytes += bytes;
	job.retained_bytes = bytes;
	return true;
}

static void
release(State &state, uint64_t connection, size_t bytes)
{
	lock_guard lock(state.mu);
	state.retained_bytes -= bytes;
	state.admitted.erase(state.admitted.find(connection));
}

static proto::Frame
error_frame(uint64_t id, string message, proto::ErrorCode code)
{
	proto::Frame frame;
	frame.payload.value = proto::PayloadResponse{proto::Response{id,
		proto::Result{
			proto::ResultError{proto::Error{code, std::move(message)}}}}};
	return frame;
}

static Done
decode_job(Job &job, const shared_ptr<dawn::Cmm> &cmm)
{
	Done done;
	done.connection = job.connection;
	dawn::OpenContext context;
	context.cmm = cmm;
	context.first_frame_only = job.first_frame_only;
	context.screen_profile = job.target_icc.empty()
		? cmm->get_profile_sRGB(false)
		: cmm->get_profile(span<const uint8_t>(
			  reinterpret_cast<const uint8_t *>(job.target_icc.data()),
			  job.target_icc.size()));
	if (!context.screen_profile) {
		done.frame = error_frame(job.request, "invalid target ICC profile",
			proto::ErrorCode::InvalidArgument);
		return done;
	}

	dawn::Error error;
	const uint8_t *data = job.shared.ok()
		? job.shared.data()
		: reinterpret_cast<const uint8_t *>(job.data.data());
	const size_t size = job.shared.ok() ? job.shared.size() : job.data.size();
	auto image = dawn::open_from_data(span(data, size), context, &error);
	if (!image) {
		done.frame = error_frame(job.request,
			error.message.empty() ? "decode failed" : error.message,
			proto::ErrorCode::InvalidArgument);
		return done;
	}
	if (image->data.empty() || image->data.size() > kMaxBlob) {
		done.frame = error_frame(job.request, "decoded pixmap is too large",
			proto::ErrorCode::InvalidArgument);
		return done;
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
	response.pixmap.pixels.value = proto::BlobInline{};

	proto::Frame measurement;
	measurement.payload.value = proto::PayloadResponse{proto::Response{
		job.request, proto::Result{proto::ResultDecoded{response}}}};

	const size_t inline_overhead = encode_frame(measurement).size();
	if (inline_overhead <= dawn::ipc::Connection::kMaxPayload &&
		image->data.size() <=
			dawn::ipc::Connection::kMaxPayload - inline_overhead) {
		proto::BlobInline pixels;
		pixels.bytes.assign(reinterpret_cast<const byte *>(image->data.data()),
			reinterpret_cast<const byte *>(
				image->data.data() + image->data.size()));
		response.pixmap.pixels.value = std::move(pixels);
	} else {
		done.memory = dawn::ipc::SharedMemory::copy(
			image->data.data(), image->data.size());
		if (!done.memory.ok()) {
			done.frame =
				error_frame(job.request, "shared memory creation failed",
					proto::ErrorCode::InvalidArgument);
			return done;
		}
		response.pixmap.pixels.value = proto::BlobShared{image->data.size()};
	}
	done.frame.payload.value = proto::PayloadResponse{proto::Response{
		job.request, proto::Result{proto::ResultDecoded{std::move(response)}}}};
	if (done.memory.ok() &&
		encode_frame(done.frame).size() > dawn::ipc::Connection::kMaxPayload) {
		done.memory.close();
		done.frame = error_frame(job.request, "response metadata is too large",
			proto::ErrorCode::Internal);
	}
	return done;
}

static void
worker(State *state)
{
	auto cmm = make_shared<dawn::Cmm>();
	while (true) {
		Job job;
		{
			unique_lock lock(state->mu);
			state->cv.wait(
				lock, [&] { return state->stop || !state->jobs.empty(); });
			if (state->stop && state->jobs.empty())
				return;
			job = std::move(state->jobs.front());
			state->jobs.pop_front();
		}

		Done done = decode_job(job, cmm);
		const uint64_t connection = job.connection;
		const size_t retained_bytes = job.retained_bytes;
		job = {};
		release(*state, connection, retained_bytes);
		{
			lock_guard lock(state->mu);
			state->done.push_back(std::move(done));
		}
		state->loop.wake();
	}
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

		proto::DaemonHelloReply reply;
		if (hello->hello.protocol_version != proto::kImagedProtocolVersion)
			reply.value = proto::DaemonHelloReplyVersionMismatch{
				proto::kImagedProtocolVersion};
		else if (!hello->hello.session.empty())
			reply.value = proto::DaemonHelloReplySessionMismatch{};
		else {
			reply.value = proto::DaemonHelloReplyAccepted{proto::DaemonLimits{
				dawn::ipc::Connection::kMaxPayload, proto::kImagedMaxBlobSize}};
			state.greeted.insert(connection);
		}

		proto::Frame out;
		out.payload.value = proto::PayloadHelloReply{reply};
		return state.server->send(connection, encode_frame(out), {});
	}
	if (!state.greeted.contains(connection))
		return false;

	auto request = get_if<proto::PayloadRequestView>(&frame.payload.value);
	if (!request)
		return false;

	const auto invalid = [&](string message) {
		return state.server->send(connection,
			encode_frame(error_frame(request->request.id, std::move(message),
				proto::ErrorCode::InvalidArgument)),
			{});
	};
	const auto busy = [&] {
		return state.server->send(connection,
			encode_frame(error_frame(request->request.id,
				"image service is busy", proto::ErrorCode::Busy)),
			{});
	};

	Job job;
	job.connection = connection;
	job.request = request->request.id;
	const size_t icc_size = request->request.decode.target_icc.size();
	auto in =
		get_if<proto::BlobInlineView>(&request->request.decode.data.value);
	uint64_t input_size;
	if (in) {
		if (!owned.empty())
			return invalid("inline blob has attachments");
		if (in->bytes.empty() || in->bytes.size() > kMaxBlob)
			return invalid("invalid input blob size");
		input_size = in->bytes.size();
	} else {
		auto shared =
			get<proto::BlobSharedView>(request->request.decode.data.value);
		if (owned.size() != 1 || !shared.size || shared.size > kMaxBlob)
			return invalid("invalid shared input blob");
		input_size = shared.size;
	}
	if (!admit(state, job, input_size + icc_size))
		return busy();
	if (in)
		job.data.assign(in->bytes.begin(), in->bytes.end());
	else {
		job.shared = dawn::ipc::SharedMemory::map(owned.take(0), input_size);
		if (!job.shared.ok()) {
			release(state, job.connection, job.retained_bytes);
			return invalid("cannot map shared input blob");
		}
	}
	job.target_icc.assign(request->request.decode.target_icc.begin(),
		request->request.decode.target_icc.end());
	job.first_frame_only = request->request.decode.first_frame_only;
	{
		lock_guard lock(state.mu);
		state.jobs.push_back(std::move(job));
	}
	state.cv.notify_one();
	return true;
}

static void
handle_read(State &state, uint64_t id)
{
	if (id == dawn::ipc::Loop::kListener)
		state.server->poll_listen();
	else
		state.server->poll_read(id);
}

static void
handle_wake(State &state)
{
	deque<Done> done;
	{
		lock_guard lock(state.mu);
		swap(done, state.done);
	}
	for (auto &item : done) {
		auto bytes = encode_frame(item.frame);
		dawn::ipc::Handle handle = item.memory.handle();
		state.server->send(item.connection, bytes,
			item.memory.ok() ? span(&handle, 1)
							 : span<const dawn::ipc::Handle>());
	}
}

int
main()
{
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
	config.watch_read = [&](uint64_t id, dawn::ipc::Waitable w) {
		return state.loop.watch_read(id, w);
	};
	config.watch_write = [&](uint64_t id, dawn::ipc::Waitable w, bool on) {
		state.loop.watch_write(id, w, on);
	};
	config.unwatch = [&](uint64_t id) { state.loop.unwatch(id); };

	dawn::ipc::ServerCore server(
		std::move(endpoint.listener), std::move(config));
	state.server = &server;
	state.loop.on_read = [&](uint64_t id) { handle_read(state, id); };
	state.loop.on_write = [&](uint64_t id) { server.poll_write(id); };
	state.loop.on_wake = [&] { handle_wake(state); };
	if (!state.loop.watch_read(
			dawn::ipc::Loop::kListener, server.listen_waitable()))
		return 1;

	const unsigned worker_count =
		min(4u, max(1u, thread::hardware_concurrency()));
	vector<thread> workers(worker_count);
	for (auto &worker_thread : workers)
		worker_thread = thread(worker, &state);
	state.loop.run();

	{
		lock_guard lock(state.mu);
		state.stop = true;
	}
	state.cv.notify_all();
	for (auto &worker_thread : workers)
		worker_thread.join();
}
