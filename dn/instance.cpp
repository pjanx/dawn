//
// instance.cpp: Qt adapter for dn single-instance IPC
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "instance.hpp"

#include "app.hpp"
#include "ipc-instance.hpp"

#include <QSocketNotifier>

#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>

using namespace std;

namespace dn
{
namespace
{

QString
from_utf8(string_view s)
{
	return QString::fromUtf8(s.data(), qsizetype(s.size()));
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
	Impl(int listen_fd, App &app, const QString &session, InstanceHost *host);
	void watch_read(int fd);
	void watch_write(int fd, bool enable);
	void unwatch(int fd);
	void on_request(const ipc::instance::RequestView &req,
		ipc::instance::Response &response);

	App &app_;
	InstanceHost *host_;
	unordered_map<int, QSocketNotifier *> reads_;
	unordered_map<int, QSocketNotifier *> writes_;
	unique_ptr<ipc::Server> server_;
};

InstanceHost::Impl::Impl(int listen_fd, App &app, const QString &session,
	InstanceHost *host)
	: app_(app), host_(host)
{
	ipc::Server::Config cfg;
	cfg.session = session.toUtf8().toStdString();
	cfg.on_request = [this](const ipc::instance::RequestView &req,
						 ipc::instance::Response &response) {
		on_request(req, response);
	};
	cfg.watch_read = [this](int fd) { watch_read(fd); };
	cfg.unwatch = [this](int fd) { unwatch(fd); };
	cfg.watch_write = [this](int fd, bool enable) { watch_write(fd, enable); };
	this->server_ = make_unique<ipc::Server>(listen_fd, std::move(cfg));

	auto *n =
		new QSocketNotifier(listen_fd, QSocketNotifier::Read, this->host_);
	QObject::connect(n, &QSocketNotifier::activated, this->host_,
		[this] { this->server_->poll_listen(); });
}

void
InstanceHost::Impl::watch_read(int fd)
{
	if (this->reads_.contains(fd))
		return;
	auto *n = new QSocketNotifier(fd, QSocketNotifier::Read, this->host_);
	QObject::connect(n, &QSocketNotifier::activated, this->host_,
		[this, fd] { this->server_->poll_read(fd); });
	this->reads_[fd] = n;
}

void
InstanceHost::Impl::watch_write(int fd, bool enable)
{
	const auto it = this->writes_.find(fd);
	if (it != this->writes_.end()) {
		it->second->setEnabled(enable);
		return;
	}
	if (!enable)
		return;
	auto *n = new QSocketNotifier(fd, QSocketNotifier::Write, this->host_);
	QObject::connect(n, &QSocketNotifier::activated, this->host_,
		[this, fd] { this->server_->poll_write(fd); });
	this->writes_[fd] = n;
}

void
InstanceHost::Impl::unwatch(int fd)
{
	if (const auto it = this->reads_.find(fd); it != this->reads_.end()) {
		it->second->setEnabled(false);
		it->second->deleteLater();
		this->reads_.erase(it);
	}
	if (const auto it = this->writes_.find(fd); it != this->writes_.end()) {
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

	const QString token = from_utf8(open_body->open.activation_token);
	for (const string_view path : open_body->open.urls) {
		const OpenResult r = this->app_.open(from_utf8(path), token);
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
	int listen_fd, App &app, QString session, QObject *parent)
	: QObject(parent),
	  impl_(make_unique<Impl>(listen_fd, app, session, this))
{
}

InstanceHost::~InstanceHost() = default;

}  // namespace dn
