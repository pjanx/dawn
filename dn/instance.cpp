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

// One event-loop watch on an ipc::Waitable. Qt watches sockets on Unix and
// overlapped completion events on Windows; nothing below cares which.
#ifdef Q_OS_WIN
using Watch = QWinEventNotifier;

Watch *
make_watch(ipc::Waitable w, bool, QObject *parent)
{
	return new QWinEventNotifier((Qt::HANDLE) w, parent);
}
#else
using Watch = QSocketNotifier;

Watch *
make_watch(ipc::Waitable w, bool write, QObject *parent)
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

ipc::instance::ErrorCode
map_open_error(OpenResult r)
{
	switch (r) {
	case OpenResult::NotFound:
		return ipc::instance::ErrorCode::NotFound;
	case OpenResult::PermissionDenied:
		return ipc::instance::ErrorCode::PermissionDenied;
	case OpenResult::InvalidArgument:
		return ipc::instance::ErrorCode::InvalidArgument;
	case OpenResult::Ok:
	case OpenResult::Internal:
		break;
	}
	return ipc::instance::ErrorCode::Internal;
}

}  // namespace

struct InstanceHost::Impl {
	Impl(ipc::Listener listener, App &app, const QString &session,
		InstanceHost *host);
	void watch_read(uint64_t id, ipc::Waitable w);
	void watch_write(uint64_t id, ipc::Waitable w, bool enable);
	void unwatch(uint64_t id);
	void on_request(const ipc::instance::RequestView &req,
		ipc::instance::Response &response);

	App &app_;
	InstanceHost *host_;
	unordered_map<uint64_t, Watch *> reads_;
	unordered_map<uint64_t, Watch *> writes_;
	unique_ptr<ipc::Server> server_;
};

InstanceHost::Impl::Impl(ipc::Listener listener, App &app,
	const QString &session, InstanceHost *host)
	: app_(app), host_(host)
{
	ipc::Server::Config cfg;
	cfg.session = session.toUtf8().toStdString();
	cfg.on_request = [this](const ipc::instance::RequestView &req,
						 ipc::instance::Response &response) {
		on_request(req, response);
	};
	cfg.watch_read = [this](uint64_t id, ipc::Waitable w) {
		watch_read(id, w);
	};
	cfg.unwatch = [this](uint64_t id) { unwatch(id); };
	cfg.watch_write = [this](uint64_t id, ipc::Waitable w, bool enable) {
		watch_write(id, w, enable);
	};
	this->server_ =
		make_unique<ipc::Server>(std::move(listener), std::move(cfg));

	auto *n = make_watch(this->server_->listen_waitable(), false, this->host_);
	QObject::connect(n, &Watch::activated, this->host_,
		[this] { this->server_->poll_listen(); });
}

void
InstanceHost::Impl::watch_read(uint64_t id, ipc::Waitable w)
{
	if (this->reads_.contains(id))
		return;
	auto *n = make_watch(w, false, this->host_);
	QObject::connect(n, &Watch::activated, this->host_,
		[this, id] { this->server_->poll_read(id); });
	this->reads_[id] = n;
}

void
InstanceHost::Impl::watch_write(uint64_t id, ipc::Waitable w, bool enable)
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

void
InstanceHost::Impl::on_request(
	const ipc::instance::RequestView &req, ipc::instance::Response &response)
{
	const auto *open_body =
		get_if<ipc::instance::RequestBodyOpenView>(&req.body.value);
	if (!open_body) {
		response.result.value = ipc::instance::ResultError{
			ipc::instance::Error{
				ipc::instance::ErrorCode::InvalidArgument,
				{},
			},
		};
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
			response.result.value = ipc::instance::ResultError{
				ipc::instance::Error{map_open_error(r), {}},
			};
			return;
		}
	}
	response.result.value = ipc::instance::ResultDone{};
}

InstanceHost::InstanceHost(
	ipc::Listener listener, App &app, QString session, QObject *parent)
	: QObject(parent),
	  impl_(make_unique<Impl>(std::move(listener), app, session, this))
{
}

InstanceHost::~InstanceHost() = default;

}  // namespace dn
