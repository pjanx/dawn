//
// ipc-rpc.hpp: service-independent RPC core
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc-loop.hpp"
#include "ipc-shm.hpp"
#include "ipc.hpp"
#include "ipc/common.lxdr.hpp"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace dawn
{
namespace ipc
{

// Nothing here knows any service's schema: payloads go in and out whole,
// and only the types every service shares are understood. A service puts
// its own handshake and envelope dispatch on top.

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
	bool send(std::span<const std::byte> payload,
		std::chrono::milliseconds &budget, std::span<const Handle> attachments);
	bool recv(std::vector<std::byte> &payload,
		std::chrono::milliseconds &budget, std::vector<Handle> *attachments);

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
		std::function<bool(
			uint64_t id, std::span<const std::byte>, std::vector<Handle>)>
			on_payload;
		// The connection is gone; nothing can be sent on it any more.
		std::function<void(uint64_t id)> on_closed;
		// Event-loop hooks. Connections are named by an opaque id;
		// the Waitable is what the loop has to watch for it.
		std::function<bool(uint64_t id, Waitable w)> watch_read;
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
	bool send(uint64_t id, std::span<const std::byte> payload,
		std::span<const Handle> attachments);
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

// --- Daemons -----------------------------------------------------------------

// Hand bytes to a blob: inline while the frame that encode() renders
// around it stays within limit, in shared memory otherwise.  Leaves the
// encoded frame in out, and the memory in attachment when it took that
// route.  Returns what went wrong, or null.
const char *place_blob(Blob &blob, std::span<const std::byte> bytes,
	uint32_t limit, const std::function<std::vector<std::byte>()> &encode,
	std::vector<std::byte> &out, SharedMemory &attachment);

// Where a blob's bytes are, mapping the frame's one attachment into
// memory when that is the route they took.  Bounds the size by limit,
// but says nothing about what the service considers a valid one.
// Returns what went wrong, or null.
const char *take_blob(const BlobView &blob, OwnedHandles &handles,
	uint64_t limit, std::span<const std::byte> &bytes, SharedMemory &memory);

// The one answer a daemon gives to a Hello.  Anything but Accepted means
// the peer must not go on to make requests.
DaemonHelloReply daemon_hello(
	const HelloView &hello, uint32_t version, uint64_t max_blob_size);

// A daemon: a ServerCore, the event loop that drives it, and worker
// threads fed from a bounded queue.  It knows no service's schema -- the
// service decodes a request, submits the work as a closure, and gets to
// encode whatever comes back.
class DaemonHost
{
public:
	// One finished unit of work: the response payload, and the shared
	// memory it points at, which rides along as its one attachment.
	struct Reply {
		std::vector<std::byte> payload;
		SharedMemory attachment;
	};

	// Runs on a worker thread.  It has to be copyable, which is what
	// keeping the job itself in a shared_ptr is for.
	using Work = std::function<Reply()>;

	// One worker is fed from a short queue.  Two jobs per client prevent
	// one connection monopolizing the retained-input cap, which admits
	// one advertised maximum-size request, never several of them.
	static constexpr size_t kMaxJobs = 8, kMaxJobsPerConnection = 2;
	static constexpr size_t kMaxRetainedBytes =
		1024ull * 1024 * 1024 + Connection::kMaxPayload;

	// The event-loop hooks in cfg are the host's own, and are ignored.
	DaemonHost(Listener listener, ServerCore::Config cfg, unsigned workers);
	DaemonHost(const DaemonHost &) = delete;
	DaemonHost &operator=(const DaemonHost &) = delete;

	// What to answer requests with, on the loop thread.
	[[nodiscard]] ServerCore &server() { return *core_; }

	// Queue one unit of work, charging bytes against the retained-input
	// budget.  False means the daemon is busy and nothing was queued.
	bool submit(uint64_t id, size_t bytes, Work work);

	// Serve until the loop gives up, then drain the workers.
	bool run();

private:
	struct Job {
		uint64_t connection = 0;
		size_t bytes = 0;
		Work work;
	};

	unsigned workers_ = 1;
	Loop loop_;
	std::unique_ptr<ServerCore> core_;

	std::mutex mutex_;
	std::condition_variable cv_;
	std::deque<Job> jobs_;
	std::deque<std::pair<uint64_t, Reply>> done_;
	std::unordered_multiset<uint64_t> admitted_;
	size_t retained_bytes_ = 0;
	bool stop_ = false;

	void work();
	void flush_done();
};

}  // namespace ipc
}  // namespace dawn
