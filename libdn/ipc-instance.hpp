//
// ipc-instance.hpp: instance-service blocking client and server
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc.hpp"
#include "ipc/instance.lxdr.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dn {
namespace ipc {

class BlockingClient {
public:
	static constexpr auto kHelloTimeout = std::chrono::milliseconds(250);
	static constexpr auto kRequestTimeout =
		std::chrono::milliseconds(10000);

	enum class HelloStatus {
		Ok,
		Unavailable,  // connect failed or handshake I/O failed
		VersionMismatch,
		SessionMismatch,
	};

	// Connect to the "instance" endpoint and complete Hello.
	// protocol_version sent is kInstanceProtocolVersion.
	// The session is compared exactly by the server.
	// timeout covers connect plus handshake (default kHelloTimeout).
	// On failure returns nullopt and writes *status when non-null.
	static std::optional<BlockingClient> connect(
		std::string_view session, HelloStatus *status,
		std::chrono::milliseconds timeout = kHelloTimeout);

	// The process ID behind the endpoint, or 0. Windows needs it to hand
	// over the right to raise a window; it is never authentication.
	[[nodiscard]] uint32_t server_pid() const;

	// Send one Open request (id = 1). Wait for a terminal Response
	// (DONE or ERROR) with the same id. browse routes file arguments
	// to their parent directory instead of the viewer. timeout
	// defaults to kRequestTimeout.
	// On DONE: return true.
	// On ERROR: fill *error if non-null, return false.
	// On timeout, protocol, or I/O failure: return false and, if
	// error is non-null, set code Internal (empty message).
	// Do not retry; after the request is sent the server may already
	// have opened windows.
	bool open(const std::vector<std::string> &paths,
		std::string_view activation_token, bool browse,
		instance::Error *error = nullptr,
		std::chrono::milliseconds timeout = kRequestTimeout);

	BlockingClient(BlockingClient &&) noexcept;
	BlockingClient &operator=(BlockingClient &&) noexcept;
	~BlockingClient();
	BlockingClient(const BlockingClient &) = delete;
	BlockingClient &operator=(const BlockingClient &) = delete;

private:
	explicit BlockingClient(Connection conn);
	Connection conn_;
};

class Server {
public:
	struct Config {
		std::string session;
		uint32_t protocol_version =
			instance::kInstanceProtocolVersion;
		uint32_t max_payload_size = Connection::kMaxPayload;
		// Called on the thread that calls poll_read. Fill
		// `response` (id already copied from the request).
		// Return and the server writes one terminal Response.
		// If result is left unset, the server sends ERROR
		// Internal (result is pre-filled with that).
		std::function<void(const instance::RequestView &req,
			instance::Response &response)>
			on_request;
		// Event-loop hooks. Connections are named by an opaque id;
		// the Waitable is what the loop has to watch for it.
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

	// Call when the listener signals. Accepts until nothing is pending.
	void poll_listen();
	// Call when a connection signals readable / writable / hung up.
	void poll_read(uint64_t id);
	void poll_write(uint64_t id);
	void drop(uint64_t id);

private:
	struct Conn {
		explicit Conn(Connection c);
		Connection conn;
		bool handshake_done = false;
		bool drop_after_flush = false;
	};

	bool handle_payload(
		uint64_t id, Conn &c, std::span<const std::byte> payload);
	bool write_frame(uint64_t id, Conn &c, const instance::Frame &frame);

	Listener listener_;
	Config cfg_;
	uint64_t next_id_ = 0;
	std::unordered_map<uint64_t, Conn> conns_;
};

}  // namespace ipc
}  // namespace dn
