//
// qt-test.hpp: Qt-specific test harness
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "test.hpp"

#include <QCoreApplication>
#include <QTemporaryDir>

namespace test
{

class Application
{
	QTemporaryDir cache_;
	QTemporaryDir inputs_;
	QCoreApplication app_;

	static int &prepare(
		int &argc, const QTemporaryDir &cache, const QTemporaryDir &inputs)
	{
		CHECK(cache.isValid());
		CHECK(inputs.isValid());
		// XXX: XDG_CACHE_HOME only redirects on Linux/BSD.
		qputenv("XDG_CACHE_HOME", cache.path().toUtf8());
		return argc;
	}

public:
	Application(int &argc, char **argv, const char *name = nullptr)
		: app_(prepare(argc, cache_, inputs_), argv)
	{
		if (name)
			QCoreApplication::setApplicationName(QString::fromLatin1(name));
	}

	QCoreApplication &app() { return app_; }
	const QTemporaryDir &cache() const { return cache_; }
	const QTemporaryDir &inputs() const { return inputs_; }
};

}  // namespace test
