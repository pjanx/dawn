//
// assoc.hpp: native Open With handlers for a file path
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include <QString>

#include <vector>

namespace dn
{

// Open With entry. Not named App: that is the process object in app.hpp.
struct Handler {
	QString id;
	QString name;
};

// The Open With groups, in menu order.
struct Handlers {
	Handler preferred;
	std::vector<Handler> recommended;
	std::vector<Handler> fallback;
};

Handlers handlers_for(const QString &path);
bool launch(const Handler &app, const QString &path);
void set_last_used(const Handler &app, const QString &path);

}  // namespace dn
