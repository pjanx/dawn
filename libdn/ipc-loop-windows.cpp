//
// ipc-loop-windows.cpp: WaitForMultipleObjects service event loop
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "ipc-loop.hpp"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <unordered_map>
#include <vector>

using namespace std;

namespace dawn::ipc
{

struct Loop::Impl {
	struct Watch {
		HANDLE read = nullptr;
		HANDLE write = nullptr;
		bool write_on = false;
	};
	unordered_map<uint64_t, Watch> watches;
	HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
};

Loop::Loop() : impl_(make_unique<Impl>())
{
}

Loop::~Loop()
{
	if (impl_->wake)
		CloseHandle(impl_->wake);
}

bool
Loop::watch_read(uint64_t id, Waitable w)
{
	if (!impl_->watches.contains(id)) {
		size_t reserved = 1;
		for (const auto &[watch_id, watch] : impl_->watches)
			if (watch.read || watch.write)
				reserved += watch_id == kListener ? 1 : 2;
		reserved += id == kListener ? 1 : 2;
		if (reserved > MAXIMUM_WAIT_OBJECTS)
			return false;
	}
	impl_->watches[id].read = (HANDLE) w;
	return true;
}

void
Loop::watch_write(uint64_t id, Waitable w, bool enable)
{
	auto &watch = impl_->watches[id];
	watch.write = (HANDLE) w;
	watch.write_on = enable;
}

void
Loop::unwatch(uint64_t id)
{
	impl_->watches.erase(id);
}

void
Loop::wake()
{
	SetEvent(impl_->wake);
}

void
Loop::run()
{
	while (true) {
		vector<HANDLE> handles{impl_->wake};
		vector<pair<uint64_t, bool>> keys{{0, false}};
		for (const auto &[id, watch] : impl_->watches) {
			if (watch.read) {
				handles.push_back(watch.read);
				keys.emplace_back(id, false);
			}
			if (watch.write && watch.write_on) {
				handles.push_back(watch.write);
				keys.emplace_back(id, true);
			}
		}
		if (handles.size() > MAXIMUM_WAIT_OBJECTS) {
			trace("too many IPC watches");
			break;
		}
		const DWORD result = WaitForMultipleObjects(
			DWORD(handles.size()), handles.data(), FALSE, INFINITE);
		if (result < WAIT_OBJECT_0 || result >= WAIT_OBJECT_0 + handles.size())
			break;
		const size_t index = result - WAIT_OBJECT_0;
		if (!index) {
			ResetEvent(impl_->wake);
			if (on_wake)
				on_wake();
		} else if (keys[index].second) {
			if (on_write)
				on_write(keys[index].first);
		} else if (on_read)
			on_read(keys[index].first);
	}
}

}  // namespace dawn::ipc
