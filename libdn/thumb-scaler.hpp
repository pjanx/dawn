//
// thumb-scaler.hpp: batched GPU thumbnail rescale
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dawn
{

/// Worker-fed, GUI-submitted thumbnail scaler. Job pixels are copied into a
/// bounded staging ring before queue() returns; Vulkan work stays on the GUI
/// thread in flush()/poll().
class ThumbScaler
{
	struct Impl;
	Impl *impl_ = nullptr;

public:
	enum class Priority : uint8_t {
		Interactive,
		Prefetch,
		Dimensions,
		Maintenance,
	};

	struct Job {
		struct Output {
			uint32_t width = 0;
			uint32_t height = 0;
			int tag = 0;
		};

		std::shared_ptr<const std::vector<uint16_t>> pixels;
		// BGRA16, premultiplied. Ownership lasts through staging.
		size_t stride = 0;
		uint32_t src_w = 0, src_h = 0;
		std::vector<Output> outputs;
		Orientation orientation = Orientation::Rotate0;
		Transfer transfer = Transfer::Srgb;
		Priority priority = Priority::Maintenance;
		uint64_t user = 0;
		std::string path;
	};
	struct Result {
		struct Output {
			uint32_t width = 0;
			uint32_t height = 0;
			int tag = 0;
			std::vector<uint16_t> data;
		};

		uint64_t user = 0;
		std::string path;
		bool failed = false;
		std::vector<Output> outputs;
	};

	ThumbScaler();
	~ThumbScaler();

	ThumbScaler(const ThumbScaler &) = delete;
	ThumbScaler &operator=(const ThumbScaler &) = delete;

	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
		uint32_t queue_family, uint64_t ring_bytes, std::string *error);
	/// Worker. Copies and queues one image, blocking only for staging space.
	/// Every output is independently filtered from that common input.
	/// True guarantees one eventual success or failure Result.
	bool queue(const Job &job);
	/// Cancel work that has not been submitted to Vulkan. Submitted work may
	/// finish, but its result is discarded by the caller's generation gate.
	bool cancel(uint64_t user);
	/// Change the priority of a job waiting for staging or submission.
	bool reprioritize(uint64_t user, Priority priority);
	void flush();                          // GUI: submit one bounded batch
	void poll(std::vector<Result> *done);  // GUI: reap signalled batches
	void shutdown();                       // unblock workers permanently
	void destroy();

	[[nodiscard]] bool busy() const;
};

}  // namespace dawn
