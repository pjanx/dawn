//
// display-profile.cpp: process-wide display ICC watch
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "display-profile.hpp"

#include <QCoreApplication>
#include <QMetaObject>

#include <utility>

using namespace std;

namespace dn
{

DisplayProfileWatch::DisplayProfileWatch()
	: source_(make_display_profile_source())
{
}

DisplayProfileWatch::~DisplayProfileWatch() = default;

void
DisplayProfileWatch::start()
{
	this->source_->start([this] { this->notify(); });
}

DisplayProfile
DisplayProfileWatch::load(QScreen *screen)
{
	return this->source_->load(screen);
}

void
DisplayProfileWatch::listen(void *key, function<void()> fn)
{
	unlisten(key);
	this->listeners_.emplace_back(key, std::move(fn));
}

void
DisplayProfileWatch::unlisten(const void *key)
{
	erase_if(this->listeners_,
		[key](const auto &item) { return item.first == key; });
}

void
DisplayProfileWatch::notify() const
{
	QObject *app = QCoreApplication::instance();
	if (!app)
		return;
	QMetaObject::invokeMethod(
		app,
		[this] {
			const auto copy = this->listeners_;
			for (const auto &item : copy)
				if (item.second)
					item.second();
		},
		Qt::QueuedConnection);
}

}  // namespace dn
