//
// thumbnailer.hpp: process-wide thumbnail execution
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "gpu.hpp"

#include "libdn/thumb-scaler.hpp"

#include <QObject>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace dn
{

class Thumbnailer final : public QObject
{
public:
	using Client = uint64_t;
	using Completion = std::function<void()>;
	using Work = std::function<Completion()>;
	using GpuCompletion = std::function<void(ThumbScaler::Result)>;

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
	/// Copies job pixels on a worker. Safe on the GUI thread; keep `job.pixels`
	/// alive until the completion runs.
	bool submit_gpu(Client client, uint64_t epoch, Priority priority,
		ThumbScaler::Job job, GpuCompletion completion, std::string key = {});
	[[nodiscard]] bool busy(Client client) const;
	[[nodiscard]] size_t background_limit() const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;

	void schedule_pump();
	void pump();
};

}  // namespace dn
