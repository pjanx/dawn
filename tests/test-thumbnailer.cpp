//
// test-thumbnailer.cpp: process-wide thumbnail scheduling
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "thumbnailer.hpp"

#include <QCoreApplication>
#include <QTimer>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <functional>
#include <mutex>
#include <vector>

using namespace std;
using namespace std::chrono_literals;

namespace
{

bool
wait_for(condition_variable &cv, unique_lock<mutex> &lock,
	const function<bool()> &predicate)
{
	return cv.wait_for(lock, 2s, predicate);
}

bool
test_background_reserve()
{
	dn::Thumbnailer thumbnailer(nullptr, 4);
	if (thumbnailer.background_limit() != 1) {
		fprintf(stderr, "unexpected four-worker background limit: %zu\n",
			thumbnailer.background_limit());
		return false;
	}
	const auto client = thumbnailer.add_client();
	mutex mu;
	condition_variable cv;
	bool release = false;
	int background_started = 0;
	bool visible_started = false;
	auto background = [&] {
		unique_lock lock(mu);
		background_started++;
		cv.notify_all();
		cv.wait(lock, [&] { return release; });
		return dn::Thumbnailer::Completion{};
	};
	for (int i = 0; i < 2; ++i) {
		if (!thumbnailer.submit(
				client, 0, dn::Thumbnailer::Priority::Dimensions, background)) {
			lock_guard lock(mu);
			release = true;
			cv.notify_all();
			return false;
		}
	}
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return background_started == 1; })) {
			release = true;
			cv.notify_all();
			fprintf(stderr, "background work did not start\n");
			return false;
		}
	}
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			lock_guard lock(mu);
			visible_started = true;
			cv.notify_all();
			return dn::Thumbnailer::Completion{};
		}))
		return false;
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return visible_started; })) {
			release = true;
			cv.notify_all();
			fprintf(stderr, "visible work was starved by background work\n");
			return false;
		}
		if (background_started != 1) {
			release = true;
			cv.notify_all();
			fprintf(stderr, "background admission exceeded its limit\n");
			return false;
		}
		release = true;
		cv.notify_all();
	}
	thumbnailer.remove_client(client);
	return true;
}

bool
test_visible_reserve()
{
	dn::Thumbnailer thumbnailer(nullptr, 4);
	const auto client = thumbnailer.add_client();
	mutex mu;
	condition_variable cv;
	bool release = false;
	int prefetch_started = 0;
	bool visible_started = false;
	auto prefetch = [&] {
		unique_lock lock(mu);
		prefetch_started++;
		cv.notify_all();
		cv.wait(lock, [&] { return release; });
		return dn::Thumbnailer::Completion{};
	};
	for (int i = 0; i < 4; ++i) {
		if (!thumbnailer.submit(
				client, 0, dn::Thumbnailer::Priority::Prefetch, prefetch)) {
			lock_guard lock(mu);
			release = true;
			cv.notify_all();
			return false;
		}
	}
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return prefetch_started == 3; })) {
			release = true;
			cv.notify_all();
			fprintf(stderr, "prefetch did not fill the non-visible workers\n");
			return false;
		}
	}
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			lock_guard lock(mu);
			visible_started = true;
			cv.notify_all();
			return dn::Thumbnailer::Completion{};
		})) {
		lock_guard lock(mu);
		release = true;
		cv.notify_all();
		return false;
	}
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return visible_started; }) ||
			prefetch_started != 3) {
			release = true;
			cv.notify_all();
			fprintf(stderr, "prefetch consumed the visible worker reserve\n");
			return false;
		}
		release = true;
		cv.notify_all();
	}
	thumbnailer.remove_client(client);
	return true;
}

bool
test_reprioritization_order()
{
	dn::Thumbnailer thumbnailer(nullptr, 1);
	const auto client = thumbnailer.add_client();
	mutex mu;
	condition_variable cv;
	bool release = false;
	bool blocker_started = false;
	vector<int> order;
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			unique_lock lock(mu);
			blocker_started = true;
			cv.notify_all();
			cv.wait(lock, [&] { return release; });
			return dn::Thumbnailer::Completion{};
		}))
		return false;
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return blocker_started; })) {
			release = true;
			cv.notify_all();
			return false;
		}
	}
	for (int id : {1, 2}) {
		if (!thumbnailer.submit(
				client, 0,
				id == 1 ? dn::Thumbnailer::Priority::Visible
						: dn::Thumbnailer::Priority::Dimensions,
				[&, id] {
					lock_guard lock(mu);
					order.push_back(id);
					cv.notify_all();
					return dn::Thumbnailer::Completion{};
				},
				to_string(id))) {
			lock_guard lock(mu);
			release = true;
			cv.notify_all();
			return false;
		}
	}
	if (!thumbnailer.reprioritize(
			client, 0, dn::Thumbnailer::Priority::Dimensions, "1") ||
		!thumbnailer.reprioritize(
			client, 0, dn::Thumbnailer::Priority::Visible, "2")) {
		lock_guard lock(mu);
		release = true;
		cv.notify_all();
		return false;
	}
	{
		lock_guard lock(mu);
		release = true;
		cv.notify_all();
	}
	{
		unique_lock lock(mu);
		if (!wait_for(cv, lock, [&] { return order.size() == 2; }))
			return false;
	}
	thumbnailer.remove_client(client);
	if (order != vector<int>{2, 1}) {
		fprintf(stderr, "reprioritized order was %d,%d\n", order[0], order[1]);
		return false;
	}
	return true;
}

}  // namespace

int
main(int argc, char **argv)
{
	QCoreApplication app(argc, argv);
	if (!test_background_reserve() || !test_visible_reserve() ||
		!test_reprioritization_order())
		return 1;
	dn::Thumbnailer thumbnailer;

	mutex mu;
	condition_variable cv;
	bool release = false;
	bool saw_busy = false;
	bool saw_idle = false;
	dn::Thumbnailer::Client client = 0;
	client = thumbnailer.add_client(0, [&] {
		if (thumbnailer.busy(client)) {
			saw_busy = true;
			{
				lock_guard lock(mu);
				release = true;
			}
			cv.notify_one();
		} else if (saw_busy) {
			saw_idle = true;
			app.quit();
		}
	});
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			unique_lock lock(mu);
			cv.wait(lock, [&] { return release; });
			return dn::Thumbnailer::Completion{};
		})) {
		fprintf(stderr, "could not submit thumbnail work\n");
		return 1;
	}

	QTimer::singleShot(2000, &app, [&] {
		{
			lock_guard lock(mu);
			release = true;
		}
		cv.notify_one();
		app.quit();
	});
	app.exec();
	thumbnailer.remove_client(client);
	if (!saw_busy || !saw_idle) {
		fprintf(stderr,
			"missing thumbnail activity transition: busy=%d idle=%d\n",
			int(saw_busy), int(saw_idle));
		return 1;
	}
	return 0;
}
