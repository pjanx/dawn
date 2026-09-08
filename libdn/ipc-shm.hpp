//
// ipc-shm.hpp: anonymous shared pixel storage
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "ipc.hpp"

#include <cstddef>
#include <cstdint>

namespace dawn::ipc
{

class SharedMemory
{
	Handle handle_ = kInvalidHandle;
	void *data_ = nullptr;
	size_t size_ = 0;

public:
	SharedMemory() = default;
	~SharedMemory();
	SharedMemory(SharedMemory &&) noexcept;
	SharedMemory &operator=(SharedMemory &&) noexcept;
	SharedMemory(const SharedMemory &) = delete;
	SharedMemory &operator=(const SharedMemory &) = delete;

	static SharedMemory copy(const void *data, size_t size);
	static SharedMemory map(Handle handle, size_t size);
	[[nodiscard]] bool ok() const { return data_ != nullptr; }
	[[nodiscard]] Handle handle() const { return handle_; }
	[[nodiscard]] size_t size() const { return size_; }
	[[nodiscard]] const uint8_t *data() const
	{
		return (const uint8_t *) data_;
	}
	void close();
};

}  // namespace dawn::ipc
