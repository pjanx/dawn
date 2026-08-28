//
// ipc-windows.cpp: completion-based IPC transport over named pipes
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
// Needs windows.h first, whatever the usual include ordering says.
#include <sddl.h>

#include <string>
#include <utility>
#include <vector>

// WIN32_LEAN_AND_MEAN keeps rpcndr.h's global ::byte out of the way, but
// this file spells std::byte rather than rely on that.
using namespace std;

namespace dawn
{
namespace ipc
{
namespace
{

// Overlapped writes pin their buffer until completion; keep that bounded
// rather than hand the kernel a whole output queue.
constexpr size_t kWriteChunk = 256 * 1024;

struct LocalFreeDeleter {
	void operator()(void *p) const { ::LocalFree((HLOCAL) p); }
};

// The token user of a process, as an owned SID copy.
vector<std::byte>
process_sid(DWORD pid)
{
	vector<std::byte> out;
	const HANDLE proc =
		::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
	if (!proc)
		return out;

	HANDLE token = nullptr;
	if (!::OpenProcessToken(proc, TOKEN_QUERY, &token)) {
		::CloseHandle(proc);
		return out;
	}

	DWORD need = 0;
	::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
	vector<std::byte> buf(need);
	if (need &&
		::GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
		const auto *user = (const TOKEN_USER *) buf.data();
		const DWORD n = ::GetLengthSid(user->User.Sid);
		out.resize(n);
		if (!::CopySid(n, out.data(), user->User.Sid))
			out.clear();
	}
	::CloseHandle(token);
	::CloseHandle(proc);
	return out;
}

const vector<std::byte> &
own_sid()
{
	static const vector<std::byte> sid = process_sid(::GetCurrentProcessId());
	return sid;
}

bool
sid_is_own(const vector<std::byte> &sid)
{
	return !sid.empty() && !own_sid().empty() &&
		::EqualSid((PSID) sid.data(), (PSID) own_sid().data());
}

// Pipe names are machine-global, so both the user and the Windows session
// have to be in there: one user may be logged into several sessions.
wstring
pipe_name(string_view service)
{
	if (own_sid().empty())
		return {};

	wchar_t *text = nullptr;
	if (!::ConvertSidToStringSidW((PSID) own_sid().data(), &text))
		return {};
	const unique_ptr<wchar_t, LocalFreeDeleter> owned(text);

	DWORD session = 0;
	if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session))
		return {};

	wstring name = L"\\\\.\\pipe\\dawn-";
	name += text;
	name += L'-';
	name += to_wstring(session);
	name += L'-';
	name.append(service.begin(), service.end());
	return name;
}

// Deny everyone but the pipe's creator.
bool
own_user_only(SECURITY_ATTRIBUTES &sa, unique_ptr<void, LocalFreeDeleter> &sd)
{
	wchar_t *text = nullptr;
	if (own_sid().empty() ||
		!::ConvertSidToStringSidW((PSID) own_sid().data(), &text))
		return false;
	const unique_ptr<wchar_t, LocalFreeDeleter> owned(text);

	const wstring sddl = wstring(L"D:P(A;;GA;;;") + text + L")";
	PSECURITY_DESCRIPTOR raw = nullptr;
	if (!::ConvertStringSecurityDescriptorToSecurityDescriptorW(
			sddl.c_str(), SDDL_REVISION_1, &raw, nullptr))
		return false;

	sd.reset(raw);
	sa = {};
	sa.nLength = sizeof sa;
	sa.lpSecurityDescriptor = raw;
	sa.bInheritHandle = FALSE;
	return true;
}

HANDLE
create_instance(const wstring &name, bool first)
{
	SECURITY_ATTRIBUTES sa{};
	unique_ptr<void, LocalFreeDeleter> sd;
	if (!own_user_only(sa, sd))
		return INVALID_HANDLE_VALUE;

	DWORD open_mode = PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED;
	if (first)
		open_mode |= FILE_FLAG_FIRST_PIPE_INSTANCE;
	const HANDLE pipe = ::CreateNamedPipeW(name.c_str(), open_mode,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT |
			PIPE_REJECT_REMOTE_CLIENTS,
		PIPE_UNLIMITED_INSTANCES, 64 * 1024, 64 * 1024, 0, &sa);
	if (pipe == INVALID_HANDLE_VALUE)
		trace("CreateNamedPipe(first=%d) failed: %lu", int(first),
			::GetLastError());
	return pipe;
}

// There is no SO_PEERCRED here. Impersonation gives the server the client's
// real token; a client can only go by the server's process identity.
bool
client_is_own_user(HANDLE pipe)
{
	if (!::ImpersonateNamedPipeClient(pipe)) {
		trace("ImpersonateNamedPipeClient failed: %lu", ::GetLastError());
		return false;
	}

	HANDLE token = nullptr;
	bool same = false;
	if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &token)) {
		DWORD need = 0;
		::GetTokenInformation(token, TokenUser, nullptr, 0, &need);
		vector<std::byte> buf(need);
		if (need &&
			::GetTokenInformation(token, TokenUser, buf.data(), need, &need)) {
			const auto *user = (const TOKEN_USER *) buf.data();
			same = !own_sid().empty() &&
				::EqualSid(user->User.Sid, (PSID) own_sid().data());
		}
		::CloseHandle(token);
	}
	::RevertToSelf();
	return same;
}

}  // namespace

// --- Connection --------------------------------------------------------------

struct Connection::Impl {
	HANDLE pipe = INVALID_HANDLE_VALUE;
	bool ok = false;
	bool eof = false;
	uint32_t peer_pid = 0;

	OVERLAPPED rov{};
	bool read_pending = false;
	FrameReader reader;

	OVERLAPPED wov{};
	bool write_pending = false;
	vector<std::byte> inflight;
	FrameWriter writer;

	~Impl();
	void shutdown();
	bool post_read();
};

void
Connection::Impl::shutdown()
{
	if (pipe != INVALID_HANDLE_VALUE) {
		// The kernel owns rov, wov and the buffers they point into until
		// cancellation completes, so wait for it before anything goes away.
		::CancelIoEx(pipe, nullptr);
		DWORD n = 0;
		if (read_pending)
			::GetOverlappedResult(pipe, &rov, &n, TRUE);
		if (write_pending)
			::GetOverlappedResult(pipe, &wov, &n, TRUE);
		::CloseHandle(pipe);
		pipe = INVALID_HANDLE_VALUE;
	}
	read_pending = false;
	write_pending = false;
	if (rov.hEvent)
		::CloseHandle(rov.hEvent);
	if (wov.hEvent)
		::CloseHandle(wov.hEvent);
	rov = {};
	wov = {};
	ok = false;
	inflight.clear();
	writer.clear();
}

Connection::Impl::~Impl()
{
	shutdown();
}

bool
Connection::Impl::post_read()
{
	const span<std::byte> buf = reader.buffer();
	::ResetEvent(rov.hEvent);
	if (::ReadFile(pipe, buf.data(), DWORD(buf.size()), nullptr, &rov) ||
		::GetLastError() == ERROR_IO_PENDING) {
		read_pending = true;
		return true;
	}
	return false;
}

Connection::Connection() : impl_(make_unique<Impl>())
{
}

Connection::Connection(Handle h) : impl_(make_unique<Impl>())
{
	const HANDLE pipe = (HANDLE) h;
	if (pipe == INVALID_HANDLE_VALUE || !pipe)
		return;

	impl_->pipe = pipe;
	impl_->rov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	impl_->wov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!impl_->rov.hEvent || !impl_->wov.hEvent) {
		impl_->shutdown();
		return;
	}

	// A completion event only fires for I/O already issued, so a read has
	// to be outstanding from the start or read_waitable() never signals
	// and an event loop waits forever.
	if (!impl_->post_read()) {
		trace("initial read failed: %lu", ::GetLastError());
		impl_->shutdown();
		return;
	}
	impl_->ok = true;
}

Connection::~Connection() = default;
Connection::Connection(Connection &&) noexcept = default;
Connection &Connection::operator=(Connection &&) noexcept = default;

bool
Connection::ok() const
{
	return impl_->ok;
}

bool
Connection::wants_write() const
{
	const Impl &m = *impl_;
	return m.ok &&
		(m.write_pending || !m.inflight.empty() || !m.writer.empty());
}

void
Connection::set_max_payload(uint32_t limit)
{
	impl_->reader.set_limit(limit);
	impl_->writer.set_limit(limit);
}

Waitable
Connection::read_waitable() const
{
	return (Waitable) impl_->rov.hEvent;
}

Waitable
Connection::write_waitable() const
{
	return (Waitable) impl_->wov.hEvent;
}

uint32_t
Connection::peer_pid() const
{
	return impl_->peer_pid;
}

void
Connection::close()
{
	impl_->shutdown();
}

Connection::Status
Connection::read()
{
	Impl &m = *impl_;
	if (!m.ok)
		return m.eof ? Status::Eof : Status::Error;
	if (m.reader.ready())
		return Status::Frame;

	// A peer that hangs up between frames is an ordinary end of
	// stream; anywhere else it is a truncated frame.
	const auto finish = [&](bool at_boundary) {
		m.eof = at_boundary;
		close();
		return at_boundary ? Status::Eof : Status::Error;
	};

	for (;;) {
		if (!m.read_pending && !m.post_read()) {
			const DWORD e = ::GetLastError();
			return finish(m.reader.idle() &&
				(e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED));
		}

		DWORD n = 0;
		if (!::GetOverlappedResult(m.pipe, &m.rov, &n, FALSE)) {
			const DWORD e = ::GetLastError();
			if (e == ERROR_IO_INCOMPLETE)
				return Status::NeedMore;
			m.read_pending = false;
			return finish(m.reader.idle() &&
				(e == ERROR_BROKEN_PIPE || e == ERROR_PIPE_NOT_CONNECTED));
		}

		m.read_pending = false;
		::ResetEvent(m.rov.hEvent);
		if (n == 0)
			return finish(m.reader.idle());
		switch (m.reader.advance(n)) {
		case FrameReader::Status::NeedMore:
			break;
		case FrameReader::Status::Frame:
			return Status::Frame;
		default:
			close();
			return Status::Error;
		}
	}
}

bool
Connection::take_payload(vector<std::byte> &out)
{
	return impl_->reader.take_payload(out);
}

bool
Connection::write_payload(span<const std::byte> payload)
{
	Impl &m = *impl_;
	if (!m.ok)
		return false;
	if (m.writer.push(payload))
		return true;
	close();
	return false;
}

bool
Connection::flush()
{
	Impl &m = *impl_;
	if (!m.ok)
		return false;

	for (;;) {
		if (m.write_pending) {
			DWORD n = 0;
			if (!::GetOverlappedResult(m.pipe, &m.wov, &n, FALSE)) {
				if (::GetLastError() != ERROR_IO_INCOMPLETE)
					close();
				return false;
			}
			m.write_pending = false;
			::ResetEvent(m.wov.hEvent);
			if (n == 0) {
				close();
				return false;
			}
			m.inflight.erase(m.inflight.begin(),
				m.inflight.begin() + min(size_t(n), m.inflight.size()));
		}

		if (m.inflight.empty()) {
			if (m.writer.empty())
				return true;
			const span<const std::byte> p = m.writer.pending();
			const size_t take = min(p.size(), kWriteChunk);
			m.inflight.assign(p.begin(), p.begin() + take);
			m.writer.consume(take);
		}

		::ResetEvent(m.wov.hEvent);
		if (!::WriteFile(m.pipe, m.inflight.data(), DWORD(m.inflight.size()),
				nullptr, &m.wov) &&
			::GetLastError() != ERROR_IO_PENDING) {
			close();
			return false;
		}
		m.write_pending = true;
		// Loop only while the OS keeps taking data without blocking.
		if (::WaitForSingleObject(m.wov.hEvent, 0) != WAIT_OBJECT_0)
			return false;
	}
}

Connection::Ready
Connection::wait(Direction dir, int timeout_ms)
{
	Impl &m = *impl_;
	if (!m.ok)
		return Ready::Fail;

	// Nothing outstanding means the caller has to drive it first.
	const bool pending =
		dir == Direction::Write ? m.write_pending : m.read_pending;
	if (!pending)
		return Ready::Ok;

	const HANDLE ev = dir == Direction::Write ? m.wov.hEvent : m.rov.hEvent;
	const DWORD ms = timeout_ms < 0 ? 0 : DWORD(timeout_ms);
	switch (::WaitForSingleObject(ev, ms)) {
	case WAIT_OBJECT_0:
		return Ready::Ok;
	case WAIT_TIMEOUT:
		return Ready::Timeout;
	default:
		return Ready::Fail;
	}
}

// --- Listener ----------------------------------------------------------------

struct Listener::Impl {
	wstring name;
	HANDLE pipe = INVALID_HANDLE_VALUE;
	OVERLAPPED ov{};
	bool pending = false;
	bool ready = false;

	~Impl();
	void shutdown();
	bool arm(bool first);
};

void
Listener::Impl::shutdown()
{
	if (pipe != INVALID_HANDLE_VALUE) {
		::CancelIoEx(pipe, nullptr);
		DWORD n = 0;
		if (pending)
			::GetOverlappedResult(pipe, &ov, &n, TRUE);
		::CloseHandle(pipe);
		pipe = INVALID_HANDLE_VALUE;
	}
	pending = false;
	ready = false;
	if (ov.hEvent)
		::CloseHandle(ov.hEvent);
	ov = {};
}

Listener::Impl::~Impl()
{
	shutdown();
}

// Creates the next free pipe instance and leaves a connect outstanding on
// it. Only the very first one arbitrates.
bool
Listener::Impl::arm(bool first)
{
	pipe = create_instance(name, first);
	if (pipe == INVALID_HANDLE_VALUE)
		return false;

	if (!ov.hEvent)
		ov.hEvent = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) {
		::CloseHandle(pipe);
		pipe = INVALID_HANDLE_VALUE;
		return false;
	}

	::ResetEvent(ov.hEvent);
	pending = true;
	ready = false;
	if (::ConnectNamedPipe(pipe, &ov))
		return true;
	switch (::GetLastError()) {
	case ERROR_IO_PENDING:
		return true;
	case ERROR_PIPE_CONNECTED:
		// A client won the race to the instance; the OVERLAPPED was
		// never handed to the kernel, so complete it by hand.
		ready = true;
		::SetEvent(ov.hEvent);
		return true;
	default:
		pending = false;
		::CloseHandle(pipe);
		pipe = INVALID_HANDLE_VALUE;
		return false;
	}
}

Listener::Listener() : impl_(make_unique<Impl>())
{
}

Listener::~Listener() = default;
Listener::Listener(Listener &&) noexcept = default;
Listener &Listener::operator=(Listener &&) noexcept = default;

bool
Listener::ok() const
{
	return impl_->pipe != INVALID_HANDLE_VALUE;
}

Waitable
Listener::waitable() const
{
	return (Waitable) impl_->ov.hEvent;
}

void
Listener::close()
{
	impl_->shutdown();
}

Connection
Listener::accept()
{
	Impl &m = *impl_;
	if (m.pipe == INVALID_HANDLE_VALUE || !m.pending) {
		trace("accept: nothing armed");
		return {};
	}

	DWORD n = 0;
	if (!m.ready && !::GetOverlappedResult(m.pipe, &m.ov, &n, FALSE)) {
		const DWORD e = ::GetLastError();
		if (e != ERROR_IO_INCOMPLETE) {
			trace("accept: connect failed: %lu", e);
			m.shutdown();
		}
		return {};
	}

	const HANDLE pipe = m.pipe;
	m.pipe = INVALID_HANDLE_VALUE;
	m.pending = false;
	// Without a fresh armed instance nobody else can connect; this client
	// is still served, but the service is finished.
	if (!m.arm(false)) {
		trace("accept: could not re-arm: %lu", ::GetLastError());
		m.shutdown();
	}

	if (!client_is_own_user(pipe)) {
		trace("accept: rejected client: %lu", ::GetLastError());
		::DisconnectNamedPipe(pipe);
		::CloseHandle(pipe);
		return {};
	}

	ULONG client_pid = 0;
	::GetNamedPipeClientProcessId(pipe, &client_pid);

	Connection conn((Handle) pipe);
	if (conn.ok())
		conn.impl_->peer_pid = uint32_t(client_pid);
	return conn;
}

// --- Endpoint ----------------------------------------------------------------

Endpoint::Listen
Endpoint::listen(string_view service)
{
	Listen out;
	Listener::Impl &m = *out.listener.impl_;
	m.name = pipe_name(service);
	if (m.name.empty()) {
		trace("no endpoint name for \"%.*s\"", int(service.size()),
			service.data());
		return out;
	}

	if (!m.arm(true)) {
		// FILE_FLAG_FIRST_PIPE_INSTANCE is how instances arbitrate.
		const DWORD e = ::GetLastError();
		if (e == ERROR_ACCESS_DENIED || e == ERROR_PIPE_BUSY)
			out.status = ListenStatus::InUse;
		trace("listen failed: %lu", e);
		return out;
	}
	trace("listening on %ls", m.name.c_str());
	out.status = ListenStatus::Ok;
	return out;
}

Endpoint::Connect
Endpoint::connect(string_view service)
{
	Connect out;
	const wstring name = pipe_name(service);
	if (name.empty())
		return out;

	HANDLE pipe = INVALID_HANDLE_VALUE;
	for (;;) {
		// Without an explicit quality of service the server receives an
		// anonymous token and cannot check who it is talking to.
		pipe = ::CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
			nullptr, OPEN_EXISTING,
			FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT |
				SECURITY_IMPERSONATION,
			nullptr);
		if (pipe != INVALID_HANDLE_VALUE)
			break;
		const DWORD e = ::GetLastError();
		if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) {
			trace("connect: nobody listening on %ls", name.c_str());
			out.status = ConnectStatus::Refused;
			return out;
		}
		// Every instance is busy for as long as the server takes to arm
		// the next one.
		if (e != ERROR_PIPE_BUSY || !::WaitNamedPipeW(name.c_str(), 100)) {
			trace("connect failed: %lu", e);
			return out;
		}
	}

	ULONG server_pid = 0;
	if (!::GetNamedPipeServerProcessId(pipe, &server_pid) ||
		!sid_is_own(process_sid(server_pid))) {
		trace("rejected server pid %lu: %lu", server_pid, ::GetLastError());
		::CloseHandle(pipe);
		return out;
	}

	out.conn = Connection((Handle) pipe);
	if (!out.conn.ok())
		return out;
	out.conn.impl_->peer_pid = uint32_t(server_pid);
	out.status = ConnectStatus::Ok;
	return out;
}

}  // namespace ipc
}  // namespace dawn
