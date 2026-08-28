//
// instance.cpp: Qt adapter for dn single-instance IPC
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "instance.hpp"

#include "app.hpp"
#include "libdn/ipc-instance.hpp"

#include <QByteArray>
#include <QUrl>
#include <QtGlobal>

#ifdef Q_OS_WIN
#include <QWinEventNotifier>
#else
#include <QSocketNotifier>
#endif

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace std;

namespace dn
{
namespace
{

// One event-loop watch on an dawn::ipc::Waitable. Qt watches sockets on Unix
// and overlapped completion events on Windows; nothing below cares which.
#ifdef Q_OS_WIN
using Watch = QWinEventNotifier;

Watch *
make_watch(dawn::ipc::Waitable w, bool, QObject *parent)
{
	return new QWinEventNotifier((Qt::HANDLE) w, parent);
}
#else
using Watch = QSocketNotifier;

Watch *
make_watch(dawn::ipc::Waitable w, bool write, QObject *parent)
{
	return new QSocketNotifier(qintptr(w),
		write ? QSocketNotifier::Write : QSocketNotifier::Read, parent);
}
#endif

QString
from_utf8(string_view s)
{
	return QString::fromUtf8(s);
}

dawn::ipc::instance::ErrorCode
map_open_error(OpenResult r)
{
	switch (r) {
	case OpenResult::NotFound:
		return dawn::ipc::instance::ErrorCode::NotFound;
	case OpenResult::PermissionDenied:
		return dawn::ipc::instance::ErrorCode::PermissionDenied;
	case OpenResult::InvalidArgument:
		return dawn::ipc::instance::ErrorCode::InvalidArgument;
	case OpenResult::Ok:
	case OpenResult::Internal:
		break;
	}
	return dawn::ipc::instance::ErrorCode::Internal;
}

}  // namespace

struct InstanceHost::Impl {
	Impl(dawn::ipc::Listener listener, App &app, const QString &session,
		InstanceHost *host);
	void watch_read(uint64_t id, dawn::ipc::Waitable w);
	void watch_write(uint64_t id, dawn::ipc::Waitable w, bool enable);
	void unwatch(uint64_t id);
	void on_request(dawn::ipc::instance::Call call,
		const dawn::ipc::instance::RequestView &req);

	App &app_;
	InstanceHost *host_;
	unordered_map<uint64_t, Watch *> reads_;
	unordered_map<uint64_t, Watch *> writes_;
	unique_ptr<dawn::ipc::instance::Server> server_;
};

InstanceHost::Impl::Impl(dawn::ipc::Listener listener, App &app,
	const QString &session, InstanceHost *host)
	: app_(app), host_(host)
{
	dawn::ipc::instance::Server::Config cfg;
	cfg.session = session.toUtf8().toStdString();
	cfg.on_request = [this](dawn::ipc::instance::Call call,
						 const dawn::ipc::instance::RequestView &req) {
		on_request(std::move(call), req);
	};
	cfg.watch_read = [this](uint64_t id, dawn::ipc::Waitable w) {
		watch_read(id, w);
	};
	cfg.unwatch = [this](uint64_t id) { unwatch(id); };
	cfg.watch_write = [this](uint64_t id, dawn::ipc::Waitable w, bool enable) {
		watch_write(id, w, enable);
	};
	this->server_ = make_unique<dawn::ipc::instance::Server>(
		std::move(listener), std::move(cfg));

	auto *n = make_watch(this->server_->listen_waitable(), false, this->host_);
	QObject::connect(n, &Watch::activated, this->host_,
		[this] { this->server_->poll_listen(); });
}

void
InstanceHost::Impl::watch_read(uint64_t id, dawn::ipc::Waitable w)
{
	if (this->reads_.contains(id))
		return;
	auto *n = make_watch(w, false, this->host_);
	QObject::connect(n, &Watch::activated, this->host_,
		[this, id] { this->server_->poll_read(id); });
	this->reads_[id] = n;
}

void
InstanceHost::Impl::watch_write(uint64_t id, dawn::ipc::Waitable w, bool enable)
{
	const auto it = this->writes_.find(id);
	if (it != this->writes_.end()) {
		it->second->setEnabled(enable);
		return;
	}
	if (!enable)
		return;
	auto *n = make_watch(w, true, this->host_);
	QObject::connect(n, &Watch::activated, this->host_,
		[this, id] { this->server_->poll_write(id); });
	this->writes_[id] = n;
}

void
InstanceHost::Impl::unwatch(uint64_t id)
{
	if (const auto it = this->reads_.find(id); it != this->reads_.end()) {
		it->second->setEnabled(false);
		it->second->deleteLater();
		this->reads_.erase(it);
	}
	if (const auto it = this->writes_.find(id); it != this->writes_.end()) {
		it->second->setEnabled(false);
		it->second->deleteLater();
		this->writes_.erase(it);
	}
}

// Opening runs to completion right here, so there is no window in which
// a cancellation could arrive: an ID only becomes cancellable when it is
// dispatched, which is this call.
void
InstanceHost::Impl::on_request(
	dawn::ipc::instance::Call call, const dawn::ipc::instance::RequestView &req)
{
	const auto *open_body =
		get_if<dawn::ipc::instance::RequestBodyOpenView>(&req.body.value);
	if (!open_body) {
		call.fail(dawn::ipc::instance::ErrorCode::InvalidArgument, {});
		return;
	}

	// default_window exists so Finder's first document can replace the
	// window dn guessed at startup. A hand-off is not that: it is another
	// invocation with its own arguments, and gets its own window.
	this->app_.default_window.clear();

	const QString token = from_utf8(open_body->open.activation_token);
	const bool browse = open_body->open.browse;
	for (const string_view url : open_body->open.urls) {
		const OpenResult r = this->app_.open(
			QUrl::fromEncoded(QByteArray(url.data(), qsizetype(url.size()))),
			token, {}, browse);
		if (r != OpenResult::Ok) {
			call.fail(map_open_error(r), {});
			return;
		}
	}
	call.done();
}

InstanceHost::InstanceHost(
	dawn::ipc::Listener listener, App &app, QString session, QObject *parent)
	: QObject(parent),
	  impl_(make_unique<Impl>(std::move(listener), app, session, this))
{
}

InstanceHost::~InstanceHost() = default;

}  // namespace dn
