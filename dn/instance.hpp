//
// instance.hpp: Qt adapter for dn single-instance IPC
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc.hpp"

#include <QObject>
#include <QString>

#include <memory>

namespace dn
{

class App;

class InstanceHost : public QObject {
public:
	// Takes over the bound listener. Forwards Open requests to app.open.
	InstanceHost(ipc::Listener listener, App &app, QString session,
		QObject *parent = nullptr);
	~InstanceHost() override;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

}  // namespace dn
