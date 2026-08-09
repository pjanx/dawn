//
// assoc.hpp: native Open With handlers for a file path
//
// Copyright The dawn Authors
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
	QString icon;
};

Handler default_for(const QString &path);
std::vector<Handler> recommended_for(const QString &path);
std::vector<Handler> fallback_for(const QString &path);
bool launch(const Handler &app, const QString &path);
void set_last_used(const Handler &app, const QString &path);

}  // namespace dn
