//
// ipc-rpc.cpp: service-independent RPC core
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-rpc.hpp"

#include <climits>

#include <thread>
#include <utility>
#include <variant>

using namespace std;

namespace dawn
{
namespace ipc
{

static void
consume_elapsed(chrono::milliseconds &left, chrono::milliseconds dt)
{
	if (dt >= left)
		left = chrono::milliseconds{0};
	else
		left -= dt;
}

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
	consume_elapsed(
		budget, chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
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
	const int ms = budget.count() > INT_MAX ? INT_MAX : int(budget.count());

	const auto t0 = clock::now();
	const Connection::Ready r = conn_.wait(dir, ms);
	consume_elapsed(
		budget, chrono::duration_cast<chrono::milliseconds>(clock::now() - t0));
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
Channel::send(span<const byte> payload, chrono::milliseconds &budget,
	span<const Handle> attachments)
{
	if (!conn_.write_payload(payload, attachments))
		return false;
	return this->flush(budget);
}

bool
Channel::recv(vector<byte> &payload, chrono::milliseconds &budget,
	vector<Handle> *attachments)
{
	while (true) {
		switch (conn_.read()) {
		case Connection::Status::Frame:
			if (attachments)
				return conn_.take_payload(payload, *attachments);
			return conn_.take_plain_payload(payload);
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
	while (true) {
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
		if (cfg_.watch_read &&
			!cfg_.watch_read(id, it->second.conn.read_waitable())) {
			trace("refused connection %llu: event loop is full",
				(unsigned long long) id);
			this->drop(id);
		}
	}
}

void
ServerCore::poll_read(uint64_t id)
{
	// on_payload may send, close, or drop this very connection, so
	// nothing about it survives across the call.
	while (true) {
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
		vector<Handle> attachments;
		if (!c.conn.take_payload(payload, attachments)) {
			this->drop(id);
			return;
		}
		if (cfg_.on_payload) {
			if (!cfg_.on_payload(id, payload, std::move(attachments))) {
				this->drop(id);
				return;
			}
		} else {
			OwnedHandles owned(std::move(attachments));
			(void) owned;
		}

		const auto after = conns_.find(id);
		if (after == conns_.end())
			return;
		if (cfg_.watch_write)
			cfg_.watch_write(id, after->second.conn.write_waitable(),
				after->second.conn.wants_write());
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
ServerCore::send(
	uint64_t id, span<const byte> payload, span<const Handle> attachments)
{
	const auto it = conns_.find(id);
	if (it == conns_.end())
		return false;

	Conn &c = it->second;
	if (!c.conn.write_payload(payload, attachments)) {
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

// --- Daemons -----------------------------------------------------------------

const char *
place_blob(Blob &blob, span<const byte> bytes, uint32_t limit,
	const function<vector<byte>()> &encode, vector<byte> &out,
	SharedMemory &attachment)
{
	blob.value = BlobInline{};
	const size_t overhead = encode().size();
	if (overhead <= limit && bytes.size() <= limit - overhead) {
		blob.value = BlobInline{vector<byte>(bytes.begin(), bytes.end())};
		out = encode();
		return nullptr;
	}

	attachment = SharedMemory::copy(bytes.data(), bytes.size());
	if (!attachment.ok())
		return "shared memory creation failed";

	blob.value = BlobShared{bytes.size()};
	out = encode();
	if (out.size() <= limit)
		return nullptr;

	attachment.close();
	return "frame metadata is too large";
}

const char *
take_blob(const BlobView &blob, OwnedHandles &handles, uint64_t limit,
	span<const byte> &bytes, SharedMemory &memory)
{
	if (auto in = get_if<BlobInlineView>(&blob.value)) {
		if (!handles.empty())
			return "inline blob has attachments";
		if (in->bytes.empty() || in->bytes.size() > limit)
			return "invalid inline blob size";

		bytes = in->bytes;
		return nullptr;
	}

	const auto shared = get<BlobSharedView>(blob.value);
	if (handles.size() != 1 || !shared.size || shared.size > limit)
		return "invalid shared blob";

	memory = SharedMemory::map(handles.take(0), shared.size);
	if (!memory.ok())
		return "cannot map shared blob";

	bytes = {reinterpret_cast<const byte *>(memory.data()), memory.size()};
	return nullptr;
}

DaemonHelloReply
daemon_hello(const HelloView &hello, uint32_t version, uint64_t max_blob_size)
{
	DaemonHelloReply reply;
	if (hello.protocol_version != version)
		reply.value = DaemonHelloReplyVersionMismatch{version};
	else if (!hello.session.empty())
		reply.value = DaemonHelloReplySessionMismatch{};
	else
		reply.value = DaemonHelloReplyAccepted{
			DaemonLimits{Connection::kMaxPayload, max_blob_size}};
	return reply;
}

DaemonHost::DaemonHost(
	Listener listener, ServerCore::Config cfg, unsigned workers)
	: workers_(workers)
{
	cfg.watch_read = [this](uint64_t id, Waitable w) {
		return loop_.watch_read(id, w);
	};
	cfg.unwatch = [this](uint64_t id) { loop_.unwatch(id); };
	cfg.watch_write = [this](uint64_t id, Waitable w, bool enable) {
		loop_.watch_write(id, w, enable);
	};
	core_ = make_unique<ServerCore>(std::move(listener), std::move(cfg));

	loop_.on_read = [this](uint64_t id) {
		if (id == Loop::kListener)
			core_->poll_listen();
		else
			core_->poll_read(id);
	};
	loop_.on_write = [this](uint64_t id) { core_->poll_write(id); };
	loop_.on_wake = [this] { this->flush_done(); };
}

bool
DaemonHost::submit(uint64_t id, size_t bytes, Work work)
{
	{
		lock_guard lock(mutex_);
		if (admitted_.size() >= kMaxJobs ||
			admitted_.count(id) >= kMaxJobsPerConnection ||
			bytes > kMaxRetainedBytes ||
			retained_bytes_ > kMaxRetainedBytes - bytes)
			return false;

		admitted_.insert(id);
		retained_bytes_ += bytes;
		jobs_.push_back(Job{id, bytes, std::move(work)});
	}
	cv_.notify_one();
	return true;
}

void
DaemonHost::work()
{
	while (true) {
		Job job;
		{
			unique_lock lock(mutex_);
			cv_.wait(lock, [&] { return stop_ || !jobs_.empty(); });
			if (stop_ && jobs_.empty())
				return;
			job = std::move(jobs_.front());
			jobs_.pop_front();
		}

		Reply reply = job.work();

		// Dropped before the accounting, because whatever input the work
		// was holding on to goes with it.
		const uint64_t id = job.connection;
		const size_t bytes = job.bytes;
		job = {};
		{
			lock_guard lock(mutex_);
			retained_bytes_ -= bytes;
			admitted_.erase(admitted_.find(id));
			done_.emplace_back(id, std::move(reply));
		}
		loop_.wake();
	}
}

void
DaemonHost::flush_done()
{
	deque<pair<uint64_t, Reply>> done;
	{
		lock_guard lock(mutex_);
		swap(done, done_);
	}
	for (auto &[id, reply] : done) {
		const Handle handle = reply.attachment.handle();
		core_->send(id, reply.payload,
			reply.attachment.ok() ? span(&handle, 1) : span<const Handle>());
	}
}

bool
DaemonHost::run()
{
	if (!loop_.watch_read(Loop::kListener, core_->listen_waitable()))
		return false;

	vector<thread> workers;
	for (unsigned i = 0; i < workers_; i++)
		workers.emplace_back([this] { this->work(); });
	loop_.run();
	{
		lock_guard lock(mutex_);
		stop_ = true;
	}
	cv_.notify_all();
	for (auto &worker : workers)
		worker.join();
	return true;
}

}  // namespace ipc
}  // namespace dawn
