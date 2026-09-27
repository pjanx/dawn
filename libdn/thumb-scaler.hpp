//
// thumb-scaler.hpp: batched GPU thumbnail rescale
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.hpp"

#include <vulkan/vulkan.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dawn
{

/// Worker-fed, GUI-submitted thumbnail scaler. Job pixels are copied into a
/// bounded staging ring before queue() returns; Vulkan work stays on the GUI
/// thread in flush()/poll().
struct ThumbScaler
{
	struct Impl;
	Impl *impl_ = nullptr;

	enum class Priority : uint8_t {
		Interactive,
		Prefetch,
		Dimensions,
		Maintenance,
	};

	/// Reaches a job whose queue() may not have started or returned yet,
	/// which cancel() and reprioritize() by user cannot.  Change it first,
	/// then call those for work already queued.
	struct Control {
		std::atomic_bool canceled = false;
		std::atomic<Priority> priority = Priority::Maintenance;
	};

	struct Job {
		struct Output {
			uint32_t width = 0;
			uint32_t height = 0;
			int tag = 0;
		};

		// BGRA16, premultiplied. Ownership lasts through staging.
		std::shared_ptr<const Image> image;
		std::vector<Output> outputs;
		Orientation orientation = Orientation::Rotate0;
		Transfer transfer = Transfer::Srgb;
		Priority priority = Priority::Maintenance;
		uint64_t user = 0;
		std::string path;
		// Optional; when present, its priority replaces the one above.
		std::shared_ptr<const Control> control;
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
	/// A producer still inside queue() only stops for its Job::control.
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
