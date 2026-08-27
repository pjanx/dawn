//
// ipc-rpc.cpp: service-independent RPC core
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-rpc.hpp"

#include <climits>

#include <utility>

using namespace std;

namespace dn
{
namespace ipc
{
namespace
{

void
consume_elapsed(chrono::milliseconds &left, chrono::milliseconds dt)
{
	if (dt >= left)
		left = chrono::milliseconds{0};
	else
		left -= dt;
}

}  // namespace

// --- Channel -----------------------------------------------------------------

Channel::Channel() = default;

Channel::Channel(Connection conn) : conn_(std::move(conn))
{
}

Channel::Channel(Channel &&) noexcept = default;
Channel &Channel::operator=(Channel &&) noexcept = default;
Channel::~Channel() = default;

Channel
Channel::connect(string_view service, chrono::milliseconds &budget)
{
	using clock = chrono::steady_clock;
	if (budget.count() < 0)
		budget = chrono::milliseconds{0};

	const auto t0 = clock::now();
	Endpoint::Connect ep = Endpoint::connect(service);
	consume_elapsed(budget,
		chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
	if (ep.status != Endpoint::ConnectStatus::Ok)
		return Channel();
	return Channel(std::move(ep.conn));
}

bool
Channel::ok() const
{
	return conn_.ok();
}

uint32_t
Channel::peer_pid() const
{
	return conn_.peer_pid();
}

void
Channel::set_max_payload(uint32_t limit)
{
	conn_.set_max_payload(limit);
}

bool
Channel::wait(Connection::Direction dir, chrono::milliseconds &budget)
{
	using clock = chrono::steady_clock;
	if (budget.count() < 0)
		budget = chrono::milliseconds{0};
	const int ms =
		budget.count() > INT_MAX ? INT_MAX : int(budget.count());

	const auto t0 = clock::now();
	const Connection::Ready r = conn_.wait(dir, ms);
	consume_elapsed(budget,
		chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
	return r == Connection::Ready::Ok;
}

bool
Channel::flush(chrono::milliseconds &budget)
{
	while (!conn_.flush()) {
		if (!conn_.ok())
			return false;
		if (!conn_.wants_write())
			break;
		if (!this->wait(Connection::Direction::Write, budget))
			return false;
	}
	return conn_.ok();
}

bool
Channel::send(span<const byte> payload, chrono::milliseconds &budget)
{
	if (!conn_.write_payload(payload))
		return false;
	return this->flush(budget);
}

bool
Channel::recv(vector<byte> &payload, chrono::milliseconds &budget)
{
	for (;;) {
		switch (conn_.read()) {
		case Connection::Status::Frame:
			return conn_.take_payload(payload);
		case Connection::Status::NeedMore:
			if (!this->wait(Connection::Direction::Read, budget))
				return false;
			break;
		default:
			return false;
		}
	}
}

// --- ServerCore --------------------------------------------------------------

ServerCore::Conn::Conn(Connection c) : conn(std::move(c))
{
}

ServerCore::ServerCore(Listener listener, Config cfg)
	: listener_(std::move(listener)), cfg_(std::move(cfg))
{
}

ServerCore::~ServerCore()
{
	while (!conns_.empty())
		this->drop(conns_.begin()->first);
}

Waitable
ServerCore::listen_waitable() const
{
	return listener_.waitable();
}

uint32_t
ServerCore::peer_pid(uint64_t id) const
{
	const auto it = conns_.find(id);
	return it == conns_.end() ? 0 : it->second.conn.peer_pid();
}

void
ServerCore::poll_listen()
{
	for (;;) {
		Connection c = listener_.accept();
		if (!c.ok())
			return;

		// Before the handshake a peer is only a peer; hold it to the
		// same frame size the service will admit to afterwards.
		c.set_max_payload(cfg_.max_payload_size);
		const uint64_t id = ++next_id_;
		auto [it, inserted] = conns_.try_emplace(id, std::move(c));
		if (!inserted)
			return;
		trace("accepted %llu from pid %lu", (unsigned long long) id,
			(unsigned long) it->second.conn.peer_pid());
		if (cfg_.watch_read)
			cfg_.watch_read(id, it->second.conn.read_waitable());
	}
}

void
ServerCore::poll_read(uint64_t id)
{
	// on_payload may send, close, or drop this very connection, so
	// nothing about it survives across the call.
	for (;;) {
		const auto it = conns_.find(id);
		if (it == conns_.end())
			return;
		Conn &c = it->second;
		if (c.closing)
			return;

		switch (c.conn.read()) {
		case Connection::Status::NeedMore:
			return;
		case Connection::Status::Frame:
			break;
		default:
			this->drop(id);
			return;
		}

		vector<byte> payload;
		if (!c.conn.take_payload(payload)) {
			this->drop(id);
			return;
		}
		if (cfg_.on_payload && !cfg_.on_payload(id, payload)) {
			this->drop(id);
			return;
		}

		const auto after = conns_.find(id);
		if (after == conns_.end())
			return;
		if (after->second.closing) {
			if (!after->second.conn.wants_write())
				this->drop(id);
			return;
		}
	}
}

void
ServerCore::poll_write(uint64_t id)
{
	const auto it = conns_.find(id);
	if (it == conns_.end())
		return;
	Conn &c = it->second;
	if (c.conn.flush()) {
		if (cfg_.watch_write)
			cfg_.watch_write(id, c.conn.write_waitable(), false);
		if (c.closing)
			this->drop(id);
		return;
	}
	if (!c.conn.ok()) {
		this->drop(id);
		return;
	}
	if (cfg_.watch_write)
		cfg_.watch_write(id, c.conn.write_waitable(), c.conn.wants_write());
}

bool
ServerCore::send(uint64_t id, span<const byte> payload)
{
	const auto it = conns_.find(id);
	if (it == conns_.end())
		return false;

	Conn &c = it->second;
	if (!c.conn.write_payload(payload)) {
		this->drop(id);
		return false;
	}
	(void) c.conn.flush();
	if (!c.conn.ok()) {
		this->drop(id);
		return false;
	}
	if (cfg_.watch_write)
		cfg_.watch_write(id, c.conn.write_waitable(), c.conn.wants_write());
	return true;
}

void
ServerCore::close_after_flush(uint64_t id)
{
	const auto it = conns_.find(id);
	if (it == conns_.end())
		return;
	it->second.closing = true;
	if (!it->second.conn.wants_write())
		this->drop(id);
}

void
ServerCore::drop(uint64_t id)
{
	const auto it = conns_.find(id);
	if (it == conns_.end())
		return;

	// Erased before anyone is told, so that a handler reacting to the
	// loss cannot find the connection it is being told about.
	conns_.erase(it);
	if (cfg_.unwatch)
		cfg_.unwatch(id);
	if (cfg_.on_closed)
		cfg_.on_closed(id);
}

}  // namespace ipc
}  // namespace dn
