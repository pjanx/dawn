//
// thumbnailer.hpp: process-wide thumbnail execution
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "gpu.hpp"
#include "thumbnail-cache.hpp"

#include "libdn/thumb-scaler.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dn
{

struct ThumbnailTierPixels {
	int tier = 0;
	uint32_t width = 0;
	uint32_t height = 0;
	std::shared_ptr<const std::vector<uint16_t>> pixels;

	[[nodiscard]] size_t bytes() const
	{
		return pixels ? pixels->size() * sizeof(uint16_t) : 0;
	}
};

struct ThumbnailBundle {
	ThumbnailSource source;
	uint32_t image_width = 0;
	uint32_t image_height = 0;
	int top_tier = 0;
	std::vector<ThumbnailTierPixels> tiers;

	[[nodiscard]] const ThumbnailTierPixels *find(int wanted) const
	{
		for (const ThumbnailTierPixels &tier : this->tiers)
			if (tier.tier == wanted && tier.pixels && !tier.pixels->empty())
				return &tier;
		return nullptr;
	}

	[[nodiscard]] size_t bytes() const
	{
		size_t total = 0;
		for (const ThumbnailTierPixels &tier : this->tiers)
			total += tier.bytes();
		return total;
	}
};

class Thumbnailer final : public QObject
{
	struct Impl;
	std::unique_ptr<Impl> impl_;

	void schedule_pump();
	void pump();

public:
	using Client = uint64_t;
	using Reservation = uint64_t;
	using Completion = std::function<void()>;
	using Work = std::function<Completion()>;
	using GpuCompletion = std::function<void(dawn::ThumbScaler::Result)>;

	enum class Priority : uint8_t {
		Visible,
		Prefetch,
		Dimensions,
		Maintenance,
	};

	explicit Thumbnailer(QObject *parent = nullptr, unsigned workers = 0);
	~Thumbnailer() override;

	Thumbnailer(const Thumbnailer &) = delete;
	Thumbnailer &operator=(const Thumbnailer &) = delete;

	bool init(const GpuContext &gpu);
	Client add_client(uint64_t epoch = 0, Completion activity = {});
	void remove_client(Client client);
	void set_epoch(Client client, uint64_t epoch);

	bool submit(Client client, uint64_t epoch, Priority priority, Work work,
		std::string key = {});
	bool reprioritize(Client client, uint64_t epoch, Priority priority,
		const std::string &key);
	/// Copies owned job pixels on a worker. Safe on the GUI thread.
	bool submit_gpu(Client client, uint64_t epoch, Priority priority,
		dawn::ThumbScaler::Job job, GpuCompletion completion,
		std::string key = {});
	Reservation reserve_bundle(Client client, uint64_t epoch,
		const ThumbnailSource &source, int top_tier, size_t bytes,
		Priority priority);
	void cancel_bundle(Reservation reservation);
	bool publish_bundle(
		Reservation reservation, std::shared_ptr<const ThumbnailBundle> bundle);
	[[nodiscard]] std::shared_ptr<const ThumbnailBundle> pending_bundle(
		const ThumbnailSource &source, int tier) const;
	[[nodiscard]] size_t pending_bundle_limit() const;
	[[nodiscard]] size_t pending_bundle_bytes() const;
	[[nodiscard]] bool busy(Client client) const;
	[[nodiscard]] bool foreground_busy(Client client) const;
	[[nodiscard]] size_t background_limit() const;
};

}  // namespace dn
