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
	// build_id and session are compared exactly by the server.
	// timeout covers connect plus handshake (default kHelloTimeout).
	// On failure returns nullopt and writes *status when non-null.
	static std::optional<BlockingClient> connect(
		std::string_view build_id, std::string_view session,
		HelloStatus *status,
		std::chrono::milliseconds timeout = kHelloTimeout);

	// Send one Open request (id = 1). Wait for a terminal Response
	// (DONE or ERROR) with the same id. timeout defaults to
	// kRequestTimeout.
	// On DONE: return true.
	// On ERROR: fill *error if non-null, return false.
	// On timeout, protocol, or I/O failure: return false and, if
	// error is non-null, set code Internal (empty message).
	// Do not retry; after the request is sent the server may already
	// have opened windows.
	bool open(const std::vector<std::string> &paths,
		std::string_view activation_token,
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
		std::string build_id;
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
		// Event-loop hooks. All optional except you need them
		// to watch fds.
		std::function<void(int fd)> watch_read;
		std::function<void(int fd)> unwatch;
		std::function<void(int fd, bool enable)> watch_write;
	};

	// Takes ownership of listen_fd (from Endpoint::listen).
	Server(int listen_fd, Config cfg);
	~Server();
	Server(const Server &) = delete;
	Server &operator=(const Server &) = delete;

	[[nodiscard]] int listen_fd() const { return listen_fd_; }

	// Call when the listen fd is readable. Accepts until EAGAIN.
	void poll_listen();
	// Call when a connection fd is readable / writable / hung up.
	void poll_read(int fd);
	void poll_write(int fd);
	void drop(int fd);

private:
	struct Conn {
		explicit Conn(int fd);
		Connection conn;
		bool handshake_done = false;
		bool drop_after_flush = false;
	};

	bool handle_payload(Conn &c, std::span<const std::byte> payload);
	bool write_frame(Conn &c, const instance::Frame &frame);

	int listen_fd_ = -1;
	Config cfg_;
	std::unordered_map<int, Conn> conns_;
};

}  // namespace ipc
}  // namespace dn
