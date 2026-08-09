//
// test-thumbnailer.cpp: process-wide thumbnail scheduling
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "qt-test.hpp"
#include "test.hpp"
#include "thumbnailer.hpp"

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

struct WorkGate {
	mutex mu;
	condition_variable changed;
	bool released = false;

	~WorkGate() { unblock(); }
	void unblock()
	{
		{
			lock_guard lock(mu);
			released = true;
		}
		changed.notify_all();
	}
	template <typename Predicate>
	bool wait(unique_lock<mutex> &lock, Predicate predicate)
	{
		return changed.wait_for(lock, 2s, predicate);
	}
};

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
	WorkGate gate;
	int background_started = 0;
	bool visible_started = false;
	auto background = [&] {
		unique_lock lock(gate.mu);
		background_started++;
		gate.changed.notify_all();
		gate.changed.wait(lock, [&] { return gate.released; });
		return dn::Thumbnailer::Completion{};
	};
	for (int i = 0; i < 2; ++i) {
		if (!thumbnailer.submit(
				client, 0, dn::Thumbnailer::Priority::Dimensions, background)) {
			gate.unblock();
			return false;
		}
	}
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return background_started == 1; })) {
			fprintf(stderr, "background work did not start\n");
			return false;
		}
	}
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			lock_guard lock(gate.mu);
			visible_started = true;
			gate.changed.notify_all();
			return dn::Thumbnailer::Completion{};
		}))
		return false;
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return visible_started; })) {
			fprintf(stderr, "visible work was starved by background work\n");
			return false;
		}
		if (background_started != 1) {
			fprintf(stderr, "background admission exceeded its limit\n");
			return false;
		}
	}
	gate.unblock();
	thumbnailer.remove_client(client);
	return true;
}

bool
test_visible_reserve()
{
	dn::Thumbnailer thumbnailer(nullptr, 4);
	const auto client = thumbnailer.add_client();
	WorkGate gate;
	int prefetch_started = 0;
	bool visible_started = false;
	auto prefetch = [&] {
		unique_lock lock(gate.mu);
		prefetch_started++;
		gate.changed.notify_all();
		gate.changed.wait(lock, [&] { return gate.released; });
		return dn::Thumbnailer::Completion{};
	};
	for (int i = 0; i < 4; ++i) {
		if (!thumbnailer.submit(
				client, 0, dn::Thumbnailer::Priority::Prefetch, prefetch)) {
			gate.unblock();
			return false;
		}
	}
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return prefetch_started == 3; })) {
			fprintf(stderr, "prefetch did not fill the non-visible workers\n");
			return false;
		}
	}
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			lock_guard lock(gate.mu);
			visible_started = true;
			gate.changed.notify_all();
			return dn::Thumbnailer::Completion{};
		})) {
		gate.unblock();
		return false;
	}
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return visible_started; }) ||
			prefetch_started != 3) {
			fprintf(stderr, "prefetch consumed the visible worker reserve\n");
			return false;
		}
	}
	gate.unblock();
	thumbnailer.remove_client(client);
	return true;
}

bool
test_reprioritization_order()
{
	dn::Thumbnailer thumbnailer(nullptr, 1);
	const auto client = thumbnailer.add_client();
	WorkGate gate;
	bool blocker_started = false;
	vector<int> order;
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			unique_lock lock(gate.mu);
			blocker_started = true;
			gate.changed.notify_all();
			gate.changed.wait(lock, [&] { return gate.released; });
			return dn::Thumbnailer::Completion{};
		}))
		return false;
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return blocker_started; })) {
			return false;
		}
	}
	for (int id : {1, 2}) {
		if (!thumbnailer.submit(
				client, 0,
				id == 1 ? dn::Thumbnailer::Priority::Visible
						: dn::Thumbnailer::Priority::Dimensions,
				[&, id] {
					lock_guard lock(gate.mu);
					order.push_back(id);
					gate.changed.notify_all();
					return dn::Thumbnailer::Completion{};
				},
				to_string(id))) {
			gate.unblock();
			return false;
		}
	}
	if (!thumbnailer.reprioritize(
			client, 0, dn::Thumbnailer::Priority::Dimensions, "1") ||
		!thumbnailer.reprioritize(
			client, 0, dn::Thumbnailer::Priority::Visible, "2")) {
		gate.unblock();
		return false;
	}
	gate.unblock();
	{
		unique_lock lock(gate.mu);
		if (!gate.wait(lock, [&] { return order.size() == 2; }))
			return false;
	}
	thumbnailer.remove_client(client);
	if (order != vector<int>{2, 1}) {
		fprintf(stderr, "reprioritized order was %d,%d\n", order[0], order[1]);
		return false;
	}
	return true;
}

bool
test_bundle_reservations()
{
	dn::Thumbnailer thumbnailer(nullptr, 2);
	const auto client = thumbnailer.add_client(7);
	dn::ThumbnailSource a;
	a.uri = QByteArrayLiteral("file:///a");
	a.mtime = 1;
	a.size = 2;
	dn::ThumbnailSource b = a;
	b.uri = QByteArrayLiteral("file:///b");
	dn::ThumbnailSource c = a;
	c.uri = QByteArrayLiteral("file:///c");
	const auto first = thumbnailer.reserve_bundle(
		client, 7, a, 2, 4096, dn::Thumbnailer::Priority::Dimensions);
	const auto second = thumbnailer.reserve_bundle(
		client, 7, b, 2, 8192, dn::Thumbnailer::Priority::Visible);
	if (!first || !second || thumbnailer.pending_bundle_limit() != 2 ||
		thumbnailer.pending_bundle_bytes() != 12288 ||
		thumbnailer.reserve_bundle(
			client, 7, c, 2, 4096, dn::Thumbnailer::Priority::Prefetch))
		return false;
	thumbnailer.cancel_bundle(first);
	const auto third = thumbnailer.reserve_bundle(
		client, 7, c, 2, 4096, dn::Thumbnailer::Priority::Prefetch);
	if (!third || thumbnailer.pending_bundle_bytes() != 12288)
		return false;
	thumbnailer.set_epoch(client, 8);
	if (thumbnailer.pending_bundle_bytes() != 0)
		return false;
	thumbnailer.remove_client(client);
	return true;
}

void
test_activity_transitions(QCoreApplication &app)
{
	dn::Thumbnailer thumbnailer;
	WorkGate gate;
	bool saw_busy = false;
	bool saw_idle = false;
	dn::Thumbnailer::Client client = 0;
	client = thumbnailer.add_client(0, [&] {
		if (thumbnailer.busy(client)) {
			saw_busy = true;
			gate.unblock();
		} else if (saw_busy) {
			saw_idle = true;
			app.quit();
		}
	});
	if (!thumbnailer.submit(client, 0, dn::Thumbnailer::Priority::Visible, [&] {
			unique_lock lock(gate.mu);
			gate.changed.wait(lock, [&] { return gate.released; });
			return dn::Thumbnailer::Completion{};
		})) {
		test::fail("could not submit thumbnail work");
		return;
	}

	QTimer::singleShot(2000, &app, [&] {
		gate.unblock();
		app.quit();
	});
	app.exec();
	thumbnailer.remove_client(client);
	CHECK(saw_busy);
	CHECK(saw_idle);
}

}  // namespace

int
main(int argc, char **argv)
{
	test::Application application(argc, argv);
	return test::run({
		{"background worker reserve", [] { CHECK(test_background_reserve()); }},
		{"visible worker reserve", [] { CHECK(test_visible_reserve()); }},
		{"reprioritization", [] { CHECK(test_reprioritization_order()); }},
		{"bundle reservations", [] { CHECK(test_bundle_reservations()); }},
		{"activity transitions",
			[&] { test_activity_transitions(application.app()); }},
	});
}
