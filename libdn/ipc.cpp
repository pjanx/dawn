//
// ipc.cpp: LibertyXDR encoder/decoder and framed connection
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <utility>

using namespace std;

namespace dawn
{
namespace ipc
{

Encoder::Encoder(vector<byte> &buf) : buf_(buf)
{
}

void
Encoder::put_be(uint64_t v, size_t width)
{
	if (!ok_)
		return;
	for (size_t i = width; i > 0; --i)
		buf_.push_back(byte((v >> ((i - 1) * 8)) & 0xff));
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
Encoder::string(string_view s)
{
	if (s.size() > UINT32_MAX) {
		ok_ = false;
		return;
	}
	u32(uint32_t(s.size()));
	const auto *p = reinterpret_cast<const byte *>(s.data());
	buf_.insert(buf_.end(), p, p + s.size());
}

void
Encoder::bytes(span<const byte> s)
{
	if (!ok_)
		return;
	buf_.insert(buf_.end(), s.begin(), s.end());
}

void
Encoder::i8s(span<const int8_t> s)
{
	if (!ok_)
		return;
	const auto *p = reinterpret_cast<const byte *>(s.data());
	buf_.insert(buf_.end(), p, p + s.size());
}

Decoder::Decoder(span<const byte> in) : in_(in)
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
Decoder::string(string_view &out)
{
	uint32_t n = 0;
	if (!u32(n))
		return false;
	if (n > remaining())
		return fail(DecodeError::Truncated);
	if (n > kMaxElements)
		return fail(DecodeError::Limit);
	auto sv = string_view(reinterpret_cast<const char *>(in_.data() + off_), n);
	if (!utf8_validate(sv))
		return fail(DecodeError::InvalidUtf8);
	out = sv;
	off_ += n;
	return true;
}

bool
Decoder::bytes(span<const byte> &out, size_t count)
{
	if (!take_raw(count))
		return false;
	out = in_.subspan(off_, count);
	off_ += count;
	return true;
}

bool
Decoder::i8s(span<const int8_t> &out, size_t count)
{
	if (!take_raw(count))
		return false;
	out = span<const int8_t>(
		reinterpret_cast<const int8_t *>(in_.data() + off_), count);
	off_ += count;
	return true;
}

bool
utf8_validate(string_view s)
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
		if (cp < min_cp || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF))
			return false;
	}
	return true;
}

// --- Transport diagnostics ---------------------------------------------------

void
trace(const char *fmt, ...)
{
	static const bool on = getenv("DN_IPC_DEBUG") != nullptr;
	if (!on)
		return;

	va_list ap;
	va_start(ap, fmt);
	fputs("dn: ipc: ", stderr);
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

// --- Framing -----------------------------------------------------------------

span<byte>
FrameReader::buffer()
{
	if (have_frame_)
		return {};
	if (state_ == State::Length)
		return {length_ + got_, 4 - got_};
	return {payload_.data() + got_, payload_.size() - got_};
}

bool
FrameReader::ready() const
{
	return have_frame_;
}

bool
FrameReader::idle() const
{
	return !have_frame_ && state_ == State::Length && got_ == 0;
}

FrameReader::Status
FrameReader::advance(size_t n)
{
	if (have_frame_ || n == 0 || n > buffer().size())
		return Status::Error;

	got_ += n;
	if (state_ == State::Length) {
		if (got_ < 4)
			return Status::NeedMore;
		const uint32_t size = (uint32_t(length_[0]) << 24) |
			(uint32_t(length_[1]) << 16) | (uint32_t(length_[2]) << 8) |
			uint32_t(length_[3]);
		if (size == 0 || size > limit_)
			return Status::Error;
		payload_.resize(size);
		got_ = 0;
		state_ = State::Payload;
		return Status::NeedMore;
	}

	if (got_ < payload_.size())
		return Status::NeedMore;
	have_frame_ = true;
	got_ = 0;
	state_ = State::Length;
	return Status::Frame;
}

void
FrameReader::set_limit(uint32_t limit)
{
	limit_ = limit > 0 && limit < kMaxPayload ? limit : kMaxPayload;
}

bool
FrameReader::take_payload(vector<byte> &out)
{
	if (!have_frame_)
		return false;
	out = std::move(payload_);
	payload_.clear();
	have_frame_ = false;
	return true;
}

bool
FrameWriter::empty() const
{
	return q_.size() <= off_;
}

span<const byte>
FrameWriter::pending() const
{
	if (empty())
		return {};
	return {q_.data() + off_, q_.size() - off_};
}

void
FrameWriter::set_limit(uint32_t limit)
{
	limit_ = limit > 0 && limit < FrameReader::kMaxPayload
		? limit
		: FrameReader::kMaxPayload;
}

bool
FrameWriter::push(span<const byte> payload)
{
	if (payload.empty() || payload.size() > limit_)
		return false;
	if (off_ > 0) {
		q_.erase(q_.begin(), q_.begin() + off_);
		off_ = 0;
	}
	const size_t add = 4 + payload.size();
	if (add > kMaxQueue || q_.size() > kMaxQueue - add)
		return false;

	const uint32_t n = uint32_t(payload.size());
	q_.push_back(byte((n >> 24) & 0xff));
	q_.push_back(byte((n >> 16) & 0xff));
	q_.push_back(byte((n >> 8) & 0xff));
	q_.push_back(byte(n & 0xff));
	q_.insert(q_.end(), payload.begin(), payload.end());
	return true;
}

void
FrameWriter::consume(size_t n)
{
	off_ += n;
	if (off_ < q_.size())
		return;
	q_.clear();
	off_ = 0;
}

void
FrameWriter::clear()
{
	q_.clear();
	off_ = 0;
}

}  // namespace ipc
}  // namespace dawn
