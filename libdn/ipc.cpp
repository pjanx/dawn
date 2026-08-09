//
// ipc.cpp: LibertyXDR encoder/decoder and framed connection
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include "ipc/instance.lxdr.hpp"

#ifndef _WIN32
#include <cerrno>
#include <fcntl.h>
#include <unistd.h>
#endif

#include <utility>

namespace dn {
namespace ipc {

Encoder::Encoder(std::vector<std::byte> &buf)
	: buf_(buf)
{
}

void
Encoder::put_be(uint64_t v, size_t width)
{
	if (!ok_)
		return;
	for (size_t i = width; i > 0; --i)
		buf_.push_back(std::byte((v >> ((i - 1) * 8)) & 0xff));
}

void
Encoder::i8(int8_t v)
{
	put_be(uint8_t(v), 1);
}

void
Encoder::i16(int16_t v)
{
	put_be(uint16_t(v), 2);
}

void
Encoder::i32(int32_t v)
{
	put_be(uint32_t(v), 4);
}

void
Encoder::i64(int64_t v)
{
	put_be(uint64_t(v), 8);
}

void
Encoder::u8(uint8_t v)
{
	put_be(v, 1);
}

void
Encoder::u16(uint16_t v)
{
	put_be(v, 2);
}

void
Encoder::u32(uint32_t v)
{
	put_be(v, 4);
}

void
Encoder::u64(uint64_t v)
{
	put_be(v, 8);
}

void
Encoder::boolean(bool v)
{
	u8(v ? 1 : 0);
}

void
Encoder::string(std::string_view s)
{
	if (s.size() > UINT32_MAX) {
		ok_ = false;
		return;
	}
	u32(uint32_t(s.size()));
	const auto *p = reinterpret_cast<const std::byte *>(s.data());
	buf_.insert(buf_.end(), p, p + s.size());
}

void
Encoder::bytes(std::span<const std::byte> s)
{
	if (!ok_)
		return;
	buf_.insert(buf_.end(), s.begin(), s.end());
}

void
Encoder::i8s(std::span<const int8_t> s)
{
	if (!ok_)
		return;
	const auto *p = reinterpret_cast<const std::byte *>(s.data());
	buf_.insert(buf_.end(), p, p + s.size());
}

Decoder::Decoder(std::span<const std::byte> in)
	: in_(in)
{
}

bool
Decoder::fail(DecodeError e)
{
	error_ = e;
	return false;
}

bool
Decoder::take_be(uint64_t &v, size_t width)
{
	if (error_ != DecodeError::Ok)
		return false;
	if (remaining() < width)
		return fail(DecodeError::Truncated);
	uint64_t x = 0;
	for (size_t i = 0; i < width; ++i)
		x = (x << 8) | uint8_t(in_[off_ + i]);
	off_ += width;
	v = x;
	return true;
}

bool
Decoder::take_raw(size_t count)
{
	if (error_ != DecodeError::Ok)
		return false;
	if (count > remaining())
		return fail(DecodeError::Truncated);
	if (count > kMaxElements)
		return fail(DecodeError::Limit);
	return true;
}

bool
Decoder::i8(int8_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 1))
		return false;
	out = int8_t(uint8_t(v));
	return true;
}

bool
Decoder::i16(int16_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 2))
		return false;
	out = int16_t(uint16_t(v));
	return true;
}

bool
Decoder::i32(int32_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 4))
		return false;
	out = int32_t(uint32_t(v));
	return true;
}

bool
Decoder::i64(int64_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 8))
		return false;
	out = int64_t(v);
	return true;
}

bool
Decoder::u8(uint8_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 1))
		return false;
	out = uint8_t(v);
	return true;
}

bool
Decoder::u16(uint16_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 2))
		return false;
	out = uint16_t(v);
	return true;
}

bool
Decoder::u32(uint32_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 4))
		return false;
	out = uint32_t(v);
	return true;
}

bool
Decoder::u64(uint64_t &out)
{
	uint64_t v = 0;
	if (!take_be(v, 8))
		return false;
	out = v;
	return true;
}

bool
Decoder::boolean(bool &out)
{
	uint8_t v = 0;
	if (!u8(v))
		return false;
	out = v != 0;
	return true;
}

bool
Decoder::string(std::string_view &out)
{
	uint32_t n = 0;
	if (!u32(n))
		return false;
	if (n > remaining())
		return fail(DecodeError::Truncated);
	if (n > kMaxElements)
		return fail(DecodeError::Limit);
	auto sv = std::string_view(
		reinterpret_cast<const char *>(in_.data() + off_), n);
	if (!utf8_validate(sv))
		return fail(DecodeError::InvalidUtf8);
	out = sv;
	off_ += n;
	return true;
}

bool
Decoder::bytes(std::span<const std::byte> &out, size_t count)
{
	if (!take_raw(count))
		return false;
	out = in_.subspan(off_, count);
	off_ += count;
	return true;
}

bool
Decoder::i8s(std::span<const int8_t> &out, size_t count)
{
	if (!take_raw(count))
		return false;
	out = std::span<const int8_t>(
		reinterpret_cast<const int8_t *>(in_.data() + off_), count);
	off_ += count;
	return true;
}

bool
utf8_validate(std::string_view s)
{
	const auto *p = reinterpret_cast<const uint8_t *>(s.data());
	const auto *end = p + s.size();
	while (p < end) {
		const uint8_t c = *p;
		if (c <= 0x7F) {
			++p;
			continue;
		}
		int extra = 0;
		uint32_t cp = 0;
		uint32_t min_cp = 0;
		if ((c & 0xE0) == 0xC0) {
			extra = 1;
			cp = c & 0x1F;
			min_cp = 0x80;
		} else if ((c & 0xF0) == 0xE0) {
			extra = 2;
			cp = c & 0x0F;
			min_cp = 0x800;
		} else if ((c & 0xF8) == 0xF0) {
			extra = 3;
			cp = c & 0x07;
			min_cp = 0x10000;
		} else {
			return false;
		}
		if (end - p < extra + 1)
			return false;
		++p;
		for (int i = 0; i < extra; ++i) {
			if ((*p & 0xC0) != 0x80)
				return false;
			cp = (cp << 6) | (*p & 0x3F);
			++p;
		}
		if (cp < min_cp || cp > 0x10FFFF ||
			(cp >= 0xD800 && cp <= 0xDFFF))
			return false;
	}
	return true;
}

#ifndef _WIN32
Connection::Connection(int fd)
	: fd_(fd)
{
	if (fd_ < 0)
		return;
	const int flags = fcntl(fd_, F_GETFL, 0);
	if (flags < 0 || fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
		::close(fd_);
		fd_ = -1;
		return;
	}
	ok_ = true;
}

Connection::~Connection()
{
	if (fd_ >= 0)
		::close(fd_);
}

void
Connection::steal(Connection &other) noexcept
{
	fd_ = other.fd_;
	ok_ = other.ok_;
	eof_ = other.eof_;
	have_frame_ = other.have_frame_;
	read_state_ = other.read_state_;
	length_buf_[0] = other.length_buf_[0];
	length_buf_[1] = other.length_buf_[1];
	length_buf_[2] = other.length_buf_[2];
	length_buf_[3] = other.length_buf_[3];
	length_got_ = other.length_got_;
	payload_ = std::move(other.payload_);
	payload_got_ = other.payload_got_;
	write_q_ = std::move(other.write_q_);
	write_off_ = other.write_off_;

	other.fd_ = -1;
	other.ok_ = false;
	other.eof_ = false;
	other.have_frame_ = false;
	other.read_state_ = ReadState::Length;
	other.length_got_ = 0;
	other.payload_got_ = 0;
	other.write_off_ = 0;
}

Connection::Connection(Connection &&other) noexcept
{
	steal(other);
}

Connection &
Connection::operator=(Connection &&other) noexcept
{
	if (this != &other) {
		close();
		steal(other);
	}
	return *this;
}

void
Connection::close()
{
	if (fd_ >= 0) {
		::close(fd_);
		fd_ = -1;
	}
	ok_ = false;
	write_q_.clear();
	write_off_ = 0;
}

void
Connection::fail()
{
	eof_ = false;
	close();
}

Connection::Status
Connection::read()
{
	if (fd_ < 0 || !ok_)
		return eof_ ? Status::Eof : Status::Error;
	if (have_frame_)
		return Status::Frame;

	for (;;) {
		if (read_state_ == ReadState::Length) {
			const ssize_t n = ::read(fd_,
				length_buf_ + length_got_, 4 - length_got_);
			if (n < 0) {
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return Status::NeedMore;
				fail();
				return Status::Error;
			}
			if (n == 0) {
				if (length_got_ == 0) {
					eof_ = true;
					close();
					return Status::Eof;
				}
				fail();
				return Status::Error;
			}
			length_got_ += size_t(n);
			if (length_got_ < 4)
				return Status::NeedMore;
			const uint32_t size =
				(uint32_t(length_buf_[0]) << 24) |
				(uint32_t(length_buf_[1]) << 16) |
				(uint32_t(length_buf_[2]) << 8) |
				uint32_t(length_buf_[3]);
			if (size == 0 || size > kMaxPayload) {
				fail();
				return Status::Error;
			}
			payload_.resize(size);
			payload_got_ = 0;
			read_state_ = ReadState::Payload;
		}

		const ssize_t n = ::read(fd_, payload_.data() + payload_got_,
			payload_.size() - payload_got_);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return Status::NeedMore;
			fail();
			return Status::Error;
		}
		if (n == 0) {
			fail();
			return Status::Error;
		}
		payload_got_ += size_t(n);
		if (payload_got_ < payload_.size())
			return Status::NeedMore;
		have_frame_ = true;
		length_got_ = 0;
		read_state_ = ReadState::Length;
		return Status::Frame;
	}
}

bool
Connection::take_payload(std::vector<std::byte> &out)
{
	if (!have_frame_)
		return false;
	out = std::move(payload_);
	have_frame_ = false;
	payload_got_ = 0;
	return true;
}

bool
Connection::write_payload(std::span<const std::byte> payload)
{
	if (!ok_ || fd_ < 0)
		return false;
	if (payload.empty() || payload.size() > kMaxPayload) {
		fail();
		return false;
	}
	if (write_off_ > 0) {
		write_q_.erase(write_q_.begin(),
			write_q_.begin() + write_off_);
		write_off_ = 0;
	}
	const size_t add = 4 + payload.size();
	if (add > kMaxWriteQueue ||
		write_q_.size() > kMaxWriteQueue - add) {
		fail();
		return false;
	}
	const uint32_t n = uint32_t(payload.size());
	write_q_.push_back(std::byte((n >> 24) & 0xff));
	write_q_.push_back(std::byte((n >> 16) & 0xff));
	write_q_.push_back(std::byte((n >> 8) & 0xff));
	write_q_.push_back(std::byte(n & 0xff));
	write_q_.insert(write_q_.end(), payload.begin(), payload.end());
	return true;
}

bool
Connection::flush()
{
	if (!ok_ || fd_ < 0)
		return false;
	while (write_off_ < write_q_.size()) {
		const ssize_t n = ::write(fd_, write_q_.data() + write_off_,
			write_q_.size() - write_off_);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return false;
			fail();
			return false;
		}
		if (n == 0) {
			fail();
			return false;
		}
		write_off_ += size_t(n);
	}
	write_q_.clear();
	write_off_ = 0;
	return true;
}
#endif

}  // namespace ipc
}  // namespace dn

namespace {

[[maybe_unused]] void
compile_check_instance_lxdr()
{
	dn::ipc::instance::Hello hello{};
	std::vector<std::byte> hello_buf;
	dn::ipc::Encoder hello_enc(hello_buf);
	encode(hello, hello_enc);
	dn::ipc::Decoder hello_dec(hello_buf);
	dn::ipc::instance::HelloView view{};
	(void)decode(hello_dec, view);

	dn::ipc::instance::OpenRequest open{};
	std::vector<std::byte> open_buf;
	dn::ipc::Encoder open_enc(open_buf);
	encode(open, open_enc);
	dn::ipc::Decoder open_dec(open_buf);
	dn::ipc::instance::OpenRequestView open_view{};
	(void)decode(open_dec, open_view);
}

[[maybe_unused]] auto *const kCompileCheckInstanceLxdr =
	&compile_check_instance_lxdr;

}  // namespace
