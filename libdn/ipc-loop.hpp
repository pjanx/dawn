//
// ipc-loop.hpp: minimal event loop for IPC services
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc.hpp"

#include <cstdint>
#include <functional>
#include <memory>

namespace dawn::ipc
{

class Loop
{
	struct Impl;
	std::unique_ptr<Impl> impl_;

public:
	static constexpr uint64_t kListener = UINT64_MAX;
	std::function<void(uint64_t)> on_read, on_write;
	std::function<void()> on_wake;

	Loop();
	~Loop();
	Loop(const Loop &) = delete;
	Loop &operator=(const Loop &) = delete;
	[[nodiscard]] bool watch_read(uint64_t id, Waitable w);
	void watch_write(uint64_t id, Waitable w, bool enable);
	void unwatch(uint64_t id);
	void wake();
	void run();
};

}  // namespace dawn::ipc
