//
// thumb-scaler.hpp: batched GPU thumbnail rescale
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "libdn.h"

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dn
{

/// Worker-fed, GUI-submitted thumbnail scaler. Job pixels are copied into a
/// bounded staging ring before queue() returns; Vulkan work stays on the GUI
/// thread in flush()/poll().
class ThumbScaler
{
public:
	enum class Priority : uint8_t {
		Interactive,
		Prefetch,
		Dimensions,
		Maintenance,
	};

	struct Job {
		const uint16_t *pixels = nullptr;  // BGRA16, premultiplied
		size_t stride = 0;
		uint32_t src_w = 0, src_h = 0;
		uint32_t out_w = 0, out_h = 0;
		Orientation orientation = Orientation::Rotate0;
		Transfer transfer = Transfer::Srgb;
		Priority priority = Priority::Maintenance;
		uint64_t user = 0;
		std::string path;
	};
	struct Result {
		uint64_t user = 0;
		std::string path;
		uint32_t out_w = 0, out_h = 0;
		bool failed = false;
		std::vector<uint16_t> data;  // destination-sized BGRA16
	};

	ThumbScaler();
	~ThumbScaler();

	ThumbScaler(const ThumbScaler &) = delete;
	ThumbScaler &operator=(const ThumbScaler &) = delete;

	bool init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
		uint32_t queue_family, uint64_t ring_bytes, std::string *error);
	/// Worker. Copies and queues one image, blocking only for staging space.
	/// True guarantees one eventual success or failure Result.
	bool queue(const Job &job);
	/// Change the priority of a job waiting for staging or submission.
	bool reprioritize(uint64_t user, Priority priority);
	void flush();                          // GUI: submit one bounded batch
	void poll(std::vector<Result> *done);  // GUI: reap signalled batches
	void shutdown();                       // unblock workers permanently
	void destroy();

	[[nodiscard]] bool busy() const;

private:
	struct Impl;
	Impl *impl_ = nullptr;
};

}  // namespace dn
