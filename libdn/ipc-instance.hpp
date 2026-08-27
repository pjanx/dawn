//
// ipc-instance.hpp: the dn single-instance service
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc-rpc.hpp"
#include "ipc/instance.lxdr.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dn {
namespace ipc {
namespace instance {

// The endpoint name, private to the current user, and on Windows to the
// current session too.
inline constexpr char kService[] = "instance";

// --- Server ------------------------------------------------------------------

class Server;

// A request the service has been handed and has not yet answered. It may
// be completed inline or kept and completed later; either way exactly one
// terminal response goes out, and one goes out even if it is dropped.
class Call {
public:
	struct State {
		// Both null once the peer is gone, which is also what says the
		// server is still there to be called back into.
		ServerCore *core = nullptr;
		Server *owner = nullptr;
		uint64_t conn = 0;
		uint64_t id = 0;
		bool cancelled = false;
		bool completed = false;
		std::function<void()> on_cancel;
	};

	explicit Call(std::shared_ptr<State> state);
	~Call();
	Call(Call &&) noexcept;
	// Not defaulted: whatever is being overwritten still owes an answer.
	Call &operator=(Call &&other);
	Call(const Call &) = delete;
	Call &operator=(const Call &) = delete;

	[[nodiscard]] uint64_t id() const;
	// The peer asked for this to stop, or went away. Long work should
	// look at it more than once.
	[[nodiscard]] bool cancelled() const;
	// Run when a cancellation arrives, for work that cannot poll.
	void on_cancel(std::function<void()> fn);

	void complete(Result result);
	void done();
	void fail(ErrorCode code, std::string_view message);

private:
	std::shared_ptr<State> state_;
};

// The handshake, request dispatch, and cancellation, over a ServerCore.
class Server {
public:
	struct Config {
		std::string session;
		uint32_t max_payload_size = Connection::kMaxPayload;
		// Called on the thread that polls. req borrows from the frame
		// and is only valid until this returns; call is not.
		std::function<void(Call call, const RequestView &req)> on_request;
		// Event-loop hooks, passed straight through to ServerCore.
		std::function<void(uint64_t id, Waitable w)> watch_read;
		std::function<void(uint64_t id)> unwatch;
		std::function<void(uint64_t id, Waitable w, bool enable)>
			watch_write;
	};

	Server(Listener listener, Config cfg);
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	[[nodiscard]] Waitable listen_waitable() const;
	// Call when the listener signals, or a connection does.
	void poll_listen();
	void poll_read(uint64_t id);
	void poll_write(uint64_t id);

private:
	struct Conn {
		bool handshake_done = false;
		std::unordered_map<uint64_t, std::shared_ptr<Call::State>> active;
	};

	Config cfg_;
	std::unique_ptr<ServerCore> core_;
	std::unordered_map<uint64_t, Conn> conns_;

	// A completed Call reports back so its slot does not sit in active
	// for as long as the connection lives.
	friend class Call;
	void retire(uint64_t id, uint64_t request_id);

	bool on_payload(uint64_t id, std::span<const std::byte> payload);
	void on_closed(uint64_t id);
	bool handshake(uint64_t id, const HelloView &hello);
	bool dispatch(uint64_t id, const RequestView &req);
	void cancel(uint64_t id, uint64_t request_id);
	bool send_frame(uint64_t id, const Frame &frame);
};

// --- Client ------------------------------------------------------------------

// One request at a time over one connection, waiting for its answer. The
// request ID is still allocated and checked, so a server that answers the
// wrong one is caught rather than believed.
class Client {
	explicit Client(Channel chan);

	Channel chan_;
	uint64_t last_id_ = 0;

	// Send one request and wait for its terminal response.
	bool call(const Request &req, Received<ResponseView> &out, Error *error,
		std::chrono::milliseconds timeout);
	void cancel(uint64_t id);

public:
	// Connect to the endpoint and complete Hello. The session is compared
	// exactly by the server; timeout covers connect plus handshake. On
	// failure returns nullopt and writes *status when non-null.
	static std::optional<Client> connect(std::string_view session,
		HelloStatus *status, std::chrono::milliseconds timeout);

	// The process ID behind the endpoint, or 0. Windows needs it to hand
	// over the right to raise a window; it is never authentication.
	[[nodiscard]] uint32_t server_pid() const;

	// Ask the running dn to open these URLs, and wait for it to say
	// whether it did. browse routes a file argument to its parent
	// directory, with the file selected, rather than to the viewer.
	//
	// On a refusal *error is filled in when non-null; on a timeout,
	// protocol, or I/O failure it gets Internal and an empty message.
	// Do not retry: once the request is out, the server may already have
	// opened windows.
	bool open(const std::vector<std::string> &urls,
		std::string_view activation_token, bool browse, Error *error,
		std::chrono::milliseconds timeout);
};

}  // namespace instance
}  // namespace ipc
}  // namespace dn
