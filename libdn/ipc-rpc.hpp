//
// ipc-rpc.hpp: service-independent RPC core
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc.hpp"

#include <chrono>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dawn
{
namespace ipc
{

// Nothing here knows any schema: payloads go in and out whole. A service
// puts its own handshake and envelope dispatch on top.

// Timeouts are asymmetric: a handshake that does not complete promptly means
// running a second process, while a request already sent must be given time
// to finish rather than risk opening a duplicate window.
inline constexpr auto kHelloTimeout = std::chrono::milliseconds(250);
inline constexpr auto kRequestTimeout = std::chrono::milliseconds(10000);

enum class HelloStatus : uint8_t {
	Ok,
	Unavailable,  // connect failed or handshake I/O failed
	VersionMismatch,
	SessionMismatch,
};

// A connection driven by blocking waits against a shrinking time budget.
// This is what a one-shot client wants; an event loop drives Connection
// directly instead.
class Channel
{
public:
	static Channel connect(
		std::string_view service, std::chrono::milliseconds &budget);

	Channel();
	explicit Channel(Connection conn);
	~Channel();
	Channel(Channel &&) noexcept;
	Channel &operator=(Channel &&) noexcept;
	Channel(const Channel &) = delete;
	Channel &operator=(const Channel &) = delete;

	[[nodiscard]] bool ok() const;
	[[nodiscard]] uint32_t peer_pid() const;
	void set_max_payload(uint32_t limit);

	// Both consume budget as time passes, and fail once it runs out.
	bool send(
		std::span<const std::byte> payload, std::chrono::milliseconds &budget);
	bool recv(
		std::vector<std::byte> &payload, std::chrono::milliseconds &budget);

private:
	Connection conn_;

	bool wait(Connection::Direction dir, std::chrono::milliseconds &budget);
	bool flush(std::chrono::milliseconds &budget);
};

// Accepting, per-connection bookkeeping, and one serialized output queue
// each, driven by an event loop through the watch callbacks below.
class ServerCore
{
public:
	struct Config {
		// Applied to every accepted connection, so that a peer cannot
		// make the server allocate more than the service admits to.
		uint32_t max_payload_size = Connection::kMaxPayload;
		// One whole payload arrived. Returning false is a protocol
		// error and drops the connection.
		std::function<bool(uint64_t id, std::span<const std::byte>)> on_payload;
		// The connection is gone; nothing can be sent on it any more.
		std::function<void(uint64_t id)> on_closed;
		// Event-loop hooks. Connections are named by an opaque id;
		// the Waitable is what the loop has to watch for it.
		std::function<void(uint64_t id, Waitable w)> watch_read;
		std::function<void(uint64_t id)> unwatch;
		std::function<void(uint64_t id, Waitable w, bool enable)> watch_write;
	};

	ServerCore(Listener listener, Config cfg);
	~ServerCore();
	ServerCore(const ServerCore &) = delete;
	ServerCore &operator=(const ServerCore &) = delete;

	[[nodiscard]] Waitable listen_waitable() const;
	[[nodiscard]] uint32_t peer_pid(uint64_t id) const;

	// Call when the listener signals. Accepts until nothing is pending.
	void poll_listen();
	// Call when a connection signals readable / writable / hung up.
	void poll_read(uint64_t id);
	void poll_write(uint64_t id);

	// Queue one payload. False means the connection is gone or refused
	// it, in which case it has already been dropped.
	bool send(uint64_t id, std::span<const std::byte> payload);
	// Write out what is queued, then drop. No further payload is read.
	void close_after_flush(uint64_t id);
	void drop(uint64_t id);

private:
	struct Conn {
		explicit Conn(Connection c);
		Connection conn;
		bool closing = false;
	};

	Listener listener_;
	Config cfg_;
	uint64_t next_id_ = 0;
	std::unordered_map<uint64_t, Conn> conns_;
};

}  // namespace ipc
}  // namespace dawn
