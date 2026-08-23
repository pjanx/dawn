//
// instance.hpp: Qt adapter for dn single-instance IPC
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace dn
{

class App;

class InstanceHost : public QObject {
	Q_OBJECT
public:
	// Takes ownership of listen_fd. Forwards Open requests to
	// app.open.
	InstanceHost(int listen_fd, App &app, QString session,
		QObject *parent = nullptr);
	~InstanceHost() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace dn
