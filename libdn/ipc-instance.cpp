//
// ipc-instance.cpp: the dn single-instance service
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-instance.hpp"

#include <utility>
#include <variant>

using namespace std;

namespace dawn
{
namespace ipc
{
namespace instance
{

static bool
encode_frame(const Frame &frame, vector<byte> &buf)
{
	buf.clear();
	Encoder enc(buf);
	encode(frame, enc);
	return enc.ok() && !buf.empty();
}

static void
set_internal(Error *error)
{
	if (!error)
		return;
	error->code = ErrorCode::Internal;
	error->message.clear();
}

// --- Call --------------------------------------------------------------------

Call::Call(shared_ptr<State> state) : state_(std::move(state))
{
}

Call::Call(Call &&) noexcept = default;

Call::~Call()
{
	// A service that answers nothing still owes the peer a response.
	if (this->state_ && !this->state_->completed)
		this->fail(ErrorCode::Internal, {});
}

Call &
Call::operator=(Call &&other)
{
	if (this != &other) {
		if (this->state_ && !this->state_->completed)
			this->fail(ErrorCode::Internal, {});
		this->state_ = std::move(other.state_);
	}
	return *this;
}

uint64_t
Call::id() const
{
	return this->state_ ? this->state_->id : 0;
}

bool
Call::cancelled() const
{
	return !this->state_ || this->state_->cancelled || !this->state_->core;
}

void
Call::on_cancel(function<void()> fn)
{
	if (!this->state_)
		return;
	if (this->state_->cancelled && fn)
		fn();
	else
		this->state_->on_cancel = std::move(fn);
}

void
Call::done()
{
	this->complete({ResultDone{}});
}

void
Call::fail(ErrorCode code, string_view message)
{
	this->complete({ResultError{Error{code, string(message)}}});
}

void
Call::complete(Result result)
{
	State *s = this->state_.get();
	if (!s || s->completed)
		return;

	s->completed = true;
	if (!s->core)
		return;

	Response response;
	response.id = s->id;
	response.result = std::move(result);

	Frame frame;
	frame.payload.value = PayloadResponse{std::move(response)};
	vector<byte> buf;
	if (encode_frame(frame, buf))
		(void) s->core->send(s->conn, buf);
	s->owner->retire(s->conn, s->id);
}

// --- Server ------------------------------------------------------------------

Server::Server(Listener listener, Config cfg) : cfg_(std::move(cfg))
{
	ServerCore::Config core;
	core.max_payload_size = this->cfg_.max_payload_size;
	core.on_payload = [this](uint64_t id, span<const byte> payload) {
		return this->on_payload(id, payload);
	};
	core.on_closed = [this](uint64_t id) { this->on_closed(id); };
	core.watch_read = this->cfg_.watch_read;
	core.unwatch = this->cfg_.unwatch;
	core.watch_write = this->cfg_.watch_write;
	this->core_ = make_unique<ServerCore>(std::move(listener), std::move(core));
}

Server::~Server()
{
	// Torn down first: it drops every connection, which fails the calls
	// still outstanding on them through on_closed, which reads conns_.
	this->core_.reset();
}

Waitable
Server::listen_waitable() const
{
	return this->core_->listen_waitable();
}

void
Server::poll_listen()
{
	this->core_->poll_listen();
}

void
Server::poll_read(uint64_t id)
{
	this->core_->poll_read(id);
}

void
Server::poll_write(uint64_t id)
{
	this->core_->poll_write(id);
}

bool
Server::send_frame(uint64_t id, const Frame &frame)
{
	vector<byte> buf;
	return encode_frame(frame, buf) && this->core_->send(id, buf);
}

void
Server::on_closed(uint64_t id)
{
	const auto it = this->conns_.find(id);
	if (it == this->conns_.end())
		return;

	// Closing a connection cancels everything outstanding on it. The
	// service may still be holding Calls; they have to find nothing to
	// answer rather than a dangling core.
	for (auto &[request_id, state] : it->second.active) {
		state->core = nullptr;
		state->cancelled = true;
		if (state->on_cancel)
			state->on_cancel();
	}
	this->conns_.erase(it);
}

bool
Server::handshake(uint64_t id, const HelloView &hello)
{
	Frame reply;
	if (hello.protocol_version != uint32_t(kInstanceProtocolVersion)) {
		HelloReplyVersionMismatch mismatch;
		mismatch.server_protocol_version = uint32_t(kInstanceProtocolVersion);
		reply.payload.value = PayloadHelloReply{HelloReply{mismatch}};
		if (!this->send_frame(id, reply))
			return false;
		this->core_->close_after_flush(id);
		return true;
	}
	if (hello.session != this->cfg_.session) {
		reply.payload.value =
			PayloadHelloReply{HelloReply{HelloReplySessionMismatch{}}};
		if (!this->send_frame(id, reply))
			return false;
		this->core_->close_after_flush(id);
		return true;
	}

	trace("hello accepted on %llu", (unsigned long long) id);
	HelloReplyAccepted accepted;
	accepted.limits.max_payload_size = this->cfg_.max_payload_size;
	reply.payload.value = PayloadHelloReply{HelloReply{accepted}};
	if (!this->send_frame(id, reply))
		return false;
	this->conns_[id].handshake_done = true;
	return true;
}

bool
Server::dispatch(uint64_t id, const RequestView &req)
{
	// Zero is reserved, and reusing a live ID would make the two answers
	// indistinguishable. Either is a protocol error.
	if (req.id == 0 || this->conns_[id].active.contains(req.id))
		return false;

	auto state = make_shared<Call::State>();
	state->core = this->core_.get();
	state->owner = this;
	state->conn = id;
	state->id = req.id;
	this->conns_[id].active.emplace(req.id, state);

	// Answering inline retires the entry through retire() below; a Call
	// the service keeps holds its slot until it does answer.
	if (this->cfg_.on_request)
		this->cfg_.on_request(Call(state), req);
	else
		Call(state).fail(ErrorCode::Unsupported, {});
	return true;
}

void
Server::retire(uint64_t id, uint64_t request_id)
{
	const auto it = this->conns_.find(id);
	if (it != this->conns_.end())
		it->second.active.erase(request_id);
}

void
Server::cancel(uint64_t id, uint64_t request_id)
{
	const auto it = this->conns_.find(id);
	if (it == this->conns_.end())
		return;
	const auto call = it->second.active.find(request_id);
	if (call == it->second.active.end())
		return;

	// Best effort: a terminal response may already be racing this, and
	// whichever arrives first is the answer.
	trace("cancel %llu on %llu", (unsigned long long) request_id,
		(unsigned long long) id);
	call->second->cancelled = true;
	if (call->second->on_cancel)
		call->second->on_cancel();
}

bool
Server::on_payload(uint64_t id, span<const byte> payload)
{
	Decoder dec(payload);
	FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0)
		return false;

	if (!this->conns_[id].handshake_done) {
		const auto *hello = get_if<PayloadHelloView>(&view.payload.value);
		return hello && this->handshake(id, hello->hello);
	}
	if (const auto *c = get_if<PayloadCancelView>(&view.payload.value)) {
		this->cancel(id, c->cancel.id);
		return true;
	}
	if (const auto *req = get_if<PayloadRequestView>(&view.payload.value))
		return this->dispatch(id, req->request);
	return false;
}

// --- Client ------------------------------------------------------------------

Client::Client(Channel chan) : chan_(std::move(chan))
{
}

uint32_t
Client::server_pid() const
{
	return this->chan_.peer_pid();
}

optional<Client>
Client::connect(
	string_view session, HelloStatus *status, chrono::milliseconds timeout)
{
	const auto fail = [status](HelloStatus s, const char *where) {
		trace("hello failed at %s", where);
		if (status)
			*status = s;
		return optional<Client>();
	};

	chrono::milliseconds left = timeout;
	Channel chan = Channel::connect(kService, left);
	if (!chan.ok())
		return fail(HelloStatus::Unavailable, "connect");

	Hello hello;
	hello.protocol_version = uint32_t(kInstanceProtocolVersion);
	hello.session = string(session);

	Frame frame;
	frame.payload.value = PayloadHello{std::move(hello)};
	vector<byte> buf;
	if (!encode_frame(frame, buf) || !chan.send(buf, left))
		return fail(HelloStatus::Unavailable, "send");
	if (!chan.recv(buf, left))
		return fail(HelloStatus::Unavailable, "no reply");

	Decoder dec(buf);
	FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0)
		return fail(HelloStatus::Unavailable, "decode");

	const auto *reply = get_if<PayloadHelloReplyView>(&view.payload.value);
	if (!reply)
		return fail(HelloStatus::Unavailable, "not a reply");

	const auto &hr = reply->hello_reply.value;
	if (holds_alternative<HelloReplyVersionMismatchView>(hr))
		return fail(HelloStatus::VersionMismatch, "version");
	if (holds_alternative<HelloReplySessionMismatchView>(hr))
		return fail(HelloStatus::SessionMismatch, "session");

	const auto *accepted = get_if<HelloReplyAcceptedView>(&hr);
	if (!accepted)
		return fail(HelloStatus::Unavailable, "bad reply");

	// Hold ourselves to what the server admits it will read, so that an
	// oversize request fails here instead of killing the connection.
	chan.set_max_payload(accepted->limits.max_payload_size);
	if (status)
		*status = HelloStatus::Ok;
	return Client(std::move(chan));
}

void
Client::cancel(uint64_t id)
{
	if (!this->chan_.ok())
		return;

	// Best effort on the way out, and briefly: the answer is not coming,
	// but the work behind it may still be running.
	chrono::milliseconds left = kHelloTimeout;
	Frame frame;
	frame.payload.value = PayloadCancel{Cancel{id}};
	vector<byte> buf;
	if (encode_frame(frame, buf))
		(void) this->chan_.send(buf, left);
}

bool
Client::call(const Request &req, Received<ResponseView> &out, Error *error,
	chrono::milliseconds timeout)
{
	if (!this->chan_.ok()) {
		set_internal(error);
		return false;
	}

	chrono::milliseconds left = timeout;
	Frame frame;
	frame.payload.value = PayloadRequest{req};
	vector<byte> buf;
	if (!encode_frame(frame, buf) || !this->chan_.send(buf, left)) {
		set_internal(error);
		return false;
	}
	if (!this->chan_.recv(buf, left)) {
		this->cancel(req.id);
		set_internal(error);
		return false;
	}

	Decoder dec(buf);
	FrameView view{};
	if (!decode(dec, view) || dec.remaining() != 0) {
		set_internal(error);
		return false;
	}

	const auto *resp = get_if<PayloadResponseView>(&view.payload.value);
	if (!resp || resp->response.id != req.id) {
		set_internal(error);
		return false;
	}

	const auto &result = resp->response.result.value;
	if (const auto *err = get_if<ResultErrorView>(&result)) {
		if (error) {
			error->code = err->error.code;
			error->message = string(err->error.message);
		}
		return false;
	}
	if (!holds_alternative<ResultDoneView>(result)) {
		set_internal(error);
		return false;
	}

	out.storage = std::move(buf);
	out.view = resp->response;
	return true;
}

bool
Client::open(const vector<string> &urls, string_view activation_token,
	bool browse, Error *error, chrono::milliseconds timeout)
{
	OpenRequest open_req;
	open_req.urls = urls;
	open_req.activation_token = string(activation_token);
	open_req.browse = browse;

	Request req;
	req.id = ++this->last_id_;
	req.body.value = RequestBodyOpen{std::move(open_req)};

	Received<ResponseView> response;
	return this->call(req, response, error, timeout);
}

}  // namespace instance
}  // namespace ipc
}  // namespace dawn
