//
// ipc.hpp: LibertyXDR encoder/decoder and framed connection
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dn {
namespace ipc {

class Encoder {
public:
	explicit Encoder(std::vector<std::byte> &buf);
	void i8(int8_t);
	void i16(int16_t);
	void i32(int32_t);
	void i64(int64_t);
	void u8(uint8_t);
	void u16(uint16_t);
	void u32(uint32_t);
	void u64(uint64_t);
	void boolean(bool);
	void string(std::string_view);
	void bytes(std::span<const std::byte>);
	void i8s(std::span<const int8_t>);
	[[nodiscard]] bool ok() const { return ok_; }

private:
	std::vector<std::byte> &buf_;
	bool ok_ = true;

	void put_be(uint64_t v, size_t width);
};

enum class DecodeError : uint8_t {
	Ok = 0,
	Truncated,
	InvalidUtf8,
	Limit,
};

class Decoder {
public:
	static constexpr size_t kMaxElements = 16 * 1024 * 1024;
	explicit Decoder(std::span<const std::byte> in);
	bool i8(int8_t &);
	bool i16(int16_t &);
	bool i32(int32_t &);
	bool i64(int64_t &);
	bool u8(uint8_t &);
	bool u16(uint16_t &);
	bool u32(uint32_t &);
	bool u64(uint64_t &);
	bool boolean(bool &);
	bool string(std::string_view &);
	bool bytes(std::span<const std::byte> &, size_t count);
	bool i8s(std::span<const int8_t> &, size_t count);
	[[nodiscard]] size_t remaining() const { return in_.size() - off_; }
	[[nodiscard]] DecodeError error() const { return error_; }

private:
	std::span<const std::byte> in_;
	size_t off_ = 0;
	DecodeError error_ = DecodeError::Ok;

	bool fail(DecodeError e);
	bool take_be(uint64_t &v, size_t width);
	bool take_raw(size_t count);
};

bool utf8_validate(std::string_view);

template<typename TView>
struct Received {
	std::vector<std::byte> storage;
	TView view;
};

// --- Framing -----------------------------------------------------------------

// Two-state frame codec: a big-endian u32 length prefix, then exactly that
// many payload bytes. It performs no I/O of its own -- the transport asks
// where to put arriving bytes and reports how many landed -- which suits
// readiness-based and completion-based backends equally.
class FrameReader {
public:
	static constexpr uint32_t kMaxPayload = 16 * 1024 * 1024;

	enum class Status { NeedMore, Frame, Error };

	// Where to place the next bytes read; empty while a completed frame
	// is waiting to be taken.
	std::span<std::byte> buffer();
	// Report bytes written into buffer(); n must be within its size.
	Status advance(size_t n);
	// True once a whole frame is available to take.
	[[nodiscard]] bool ready() const;
	// True at a frame boundary, the only place a peer may close cleanly.
	[[nodiscard]] bool idle() const;
	bool take_payload(std::vector<std::byte> &out);
	// Lower the accepted frame size; values above kMaxPayload are clamped.
	// A length prefix is believed before the payload arrives, so this is
	// what bounds the allocation an unauthenticated peer can ask for.
	void set_limit(uint32_t limit);

private:
	enum class State { Length, Payload };

	State state_ = State::Length;
	bool have_frame_ = false;
	std::byte length_[4]{};
	size_t got_ = 0;
	uint32_t limit_ = kMaxPayload;
	std::vector<std::byte> payload_;
};

// One serialized output queue. pending() stays valid until the next
// consume() or push(), so a completion-based backend takes its own copy
// of whatever it hands to the operating system.
class FrameWriter {
public:
	static constexpr size_t kMaxQueue = 64 * 1024 * 1024;

	// Prefix payload with its length and enqueue it.
	bool push(std::span<const std::byte> payload);
	[[nodiscard]] bool empty() const;
	[[nodiscard]] std::span<const std::byte> pending() const;
	void consume(size_t n);
	void clear();
	// Refuse to enqueue frames the peer would refuse to read.
	void set_limit(uint32_t limit);

private:
	std::vector<std::byte> q_;
	size_t off_ = 0;
	uint32_t limit_ = FrameReader::kMaxPayload;
};

// --- Transport ---------------------------------------------------------------

// Set DN_IPC_DEBUG to have the transport explain itself on stderr.
// Everything here fails by quietly running a second dn instead.
void trace(const char *fmt, ...);

// A file descriptor on POSIX, a HANDLE on Windows. Both compare equal to
// -1 when invalid.
using Handle = std::intptr_t;
inline constexpr Handle kInvalidHandle = -1;

// What an event loop waits on: the connection itself on POSIX, an
// overlapped completion event on Windows.
using Waitable = Handle;

// A framed full-duplex byte stream. POSIX drives it from readiness on a
// nonblocking Unix socket, Windows from completion of overlapped named
// pipe I/O; both present this same polled interface.
class Connection {
public:
	static constexpr uint32_t kMaxPayload = FrameReader::kMaxPayload;
	static constexpr size_t kMaxWriteQueue = FrameWriter::kMaxQueue;

	enum class Status { NeedMore, Frame, Error, Eof };
	enum class Direction { Read, Write };
	enum class Ready { Ok, Timeout, Fail };

	Connection();
	// Takes ownership of h and puts it in the mode the backend needs.
	explicit Connection(Handle h);
	~Connection();
	Connection(Connection &&) noexcept;
	Connection &operator=(Connection &&) noexcept;
	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;

	[[nodiscard]] bool ok() const;
	[[nodiscard]] bool wants_write() const;
	// Bound both directions to what the handshake agreed on.
	void set_max_payload(uint32_t limit);
	[[nodiscard]] Waitable read_waitable() const;
	[[nodiscard]] Waitable write_waitable() const;
	// Peer process ID as reported by the operating system, or 0.
	// Diagnostics and window activation only; never authentication.
	[[nodiscard]] uint32_t peer_pid() const;

	// Harvest input. Frame means take_payload() will succeed.
	Status read();
	bool take_payload(std::vector<std::byte> &out);

	// Queue one payload; it is written by flush(), which returns true
	// once the queue has drained.
	bool write_payload(std::span<const std::byte> payload);
	bool flush();

	// Block until the direction can make progress. For BlockingClient;
	// an event loop waits on the waitables instead.
	Ready wait(Direction dir, int timeout_ms);
	void close();

private:
	friend class Endpoint;
	friend class Listener;

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

// A bound service endpoint. Binding it is what arbitrates between
// instances starting simultaneously.
class Listener {
public:
	Listener();
	~Listener();
	Listener(Listener &&) noexcept;
	Listener &operator=(Listener &&) noexcept;
	Listener(const Listener &) = delete;
	Listener &operator=(const Listener &) = delete;

	[[nodiscard]] bool ok() const;
	[[nodiscard]] Waitable waitable() const;
	// One accepted client, or a closed Connection when none is pending.
	// Peers running as another user are rejected.
	Connection accept();
	void close();

private:
	friend class Endpoint;

	struct Impl;
	std::unique_ptr<Impl> impl_;
};

class Endpoint {
public:
	enum class ListenStatus { Ok, InUse, Error };
	struct Listen {
		Listener listener;
		ListenStatus status = ListenStatus::Error;
	};

	// service is a short token such as "instance". The name is private
	// to the current user, and on Windows to the current session too.
	static Listen listen(std::string_view service);

	enum class ConnectStatus { Ok, Refused, Error };
	struct Connect {
		Connection conn;
		ConnectStatus status = ConnectStatus::Error;
	};
	static Connect connect(std::string_view service);
};

}  // namespace ipc
}  // namespace dn
