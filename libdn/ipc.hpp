//
// ipc.hpp: LibertyXDR encoder/decoder and framed connection
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <cstddef>
#include <cstdint>
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

// Two-state nonblocking reader: READ_LENGTH (4-byte prefix) then
// READ_PAYLOAD into the final storage. One serialized write queue.
#ifndef _WIN32
class Connection {
public:
	static constexpr uint32_t kMaxPayload = 16 * 1024 * 1024;
	static constexpr size_t kMaxWriteQueue = 64 * 1024 * 1024;

	explicit Connection(int fd);
	~Connection();
	Connection(Connection &&) noexcept;
	Connection &operator=(Connection &&) noexcept;
	Connection(const Connection &) = delete;
	Connection &operator=(const Connection &) = delete;

	[[nodiscard]] int fd() const { return fd_; }
	[[nodiscard]] bool ok() const { return ok_; }
	[[nodiscard]] bool wants_write() const
	{
		return ok_ && write_q_.size() > write_off_;
	}

	enum class Status { NeedMore, Frame, Error, Eof };
	Status read();
	bool take_payload(std::vector<std::byte> &out);

	bool write_payload(std::span<const std::byte> payload);
	bool flush();

	void close();

private:
	enum class ReadState { Length, Payload };

	void fail();
	void steal(Connection &other) noexcept;

	int fd_ = -1;
	bool ok_ = false;
	bool eof_ = false;
	bool have_frame_ = false;
	ReadState read_state_ = ReadState::Length;
	uint8_t length_buf_[4]{};
	size_t length_got_ = 0;
	std::vector<std::byte> payload_;
	size_t payload_got_ = 0;
	std::vector<std::byte> write_q_;
	size_t write_off_ = 0;
};
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
class Endpoint {
public:
	// Abstract name: "\0dawn-<uid>-<service>"
	// service is a short token such as "instance".
	static std::string name(std::string_view service);

	enum class ListenStatus { Ok, InUse, Error };
	struct Listen {
		int fd = -1;  // owned listening socket, or -1
		ListenStatus status = ListenStatus::Error;
	};
	static Listen listen(std::string_view service);

	enum class ConnectStatus { Ok, Refused, Error };
	struct Connect {
		int fd = -1;  // owned connected socket, or -1
		ConnectStatus status = ConnectStatus::Error;
	};
	static Connect connect(std::string_view service);

	// Accept one client on a listening fd from listen().
	// EAGAIN/EWOULDBLOCK -> fd -1, not a failure of the listener.
	// Other errors -> fd -1.
	// On success the new fd is O_NONBLOCK|FD_CLOEXEC.
	// SO_PEERCRED uid must equal getuid(); otherwise close and return -1.
	static int accept(int listen_fd);
};
#endif

}  // namespace ipc
}  // namespace dn
