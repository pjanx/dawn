//
// thumbnailer.cpp: process-wide thumbnail execution
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "thumbnailer.hpp"
#include "thumbnail-cache.hpp"

#include <QMetaObject>
#include <QThread>
#include <QTimer>
#include <QtLogging>

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

using namespace std;

namespace dn
{

namespace
{

constexpr uint64_t kThumbRingBytes = 256ull * 1024 * 1024;
constexpr size_t kPendingBundleBytes = 1ull << 30;
constexpr size_t kPriorityCount = 4;
constexpr size_t kGuiBatch = 32;

// A worker may queue GPU work before returning the GUI completion which
// records that work in its client. Hold such GPU callbacks until the CPU
// completion has at least been placed on the GUI queue.
thread_local shared_ptr<atomic_bool> current_cpu_gate;

bool
same_source(const ThumbnailSource &a, const ThumbnailSource &b)
{
	return a.uri == b.uri && a.mtime == b.mtime && a.size == b.size;
}

size_t
priority_index(Thumbnailer::Priority priority)
{
	return size_t(priority);
}

dawn::ThumbScaler::Priority
scaler_priority(Thumbnailer::Priority priority)
{
	switch (priority) {
	case Thumbnailer::Priority::Visible:
		return dawn::ThumbScaler::Priority::Interactive;
	case Thumbnailer::Priority::Prefetch:
		return dawn::ThumbScaler::Priority::Prefetch;
	case Thumbnailer::Priority::Dimensions:
		return dawn::ThumbScaler::Priority::Dimensions;
	case Thumbnailer::Priority::Maintenance:
		return dawn::ThumbScaler::Priority::Maintenance;
	}
	return dawn::ThumbScaler::Priority::Maintenance;
}

}  // namespace

struct Thumbnailer::Impl {
	struct CpuTask;
	struct ClientState {
		uint64_t epoch = 0;
		size_t queued = 0;
		size_t running = 0;
		size_t gpu = 0;
		size_t gui = 0;
		unordered_map<string, shared_ptr<CpuTask>> keyed;
		Completion activity;
		bool activity_pending = false;
	};
	struct CpuTask {
		Client client = 0;
		uint64_t epoch = 0;
		Priority priority = Priority::Maintenance;
		Priority running_priority = Priority::Maintenance;
		Work work;
		string key;
		shared_ptr<atomic_bool> gate;
		bool queued = true;
	};
	struct GuiTask {
		Client client = 0;
		uint64_t epoch = 0;
		Priority priority = Priority::Maintenance;
		Completion completion;
	};
	struct GpuTask {
		Client client = 0;
		uint64_t epoch = 0;
		Priority priority = Priority::Maintenance;
		string key;
		GpuCompletion completion;
		shared_ptr<atomic_bool> gate;
		optional<dawn::ThumbScaler::Result> result;
	};
	struct BundleSlot {
		Reservation id = 0;
		Client client = 0;
		uint64_t epoch = 0;
		ThumbnailSource source;
		int top_tier = 0;
		size_t reserved_bytes = 0;
		Priority priority = Priority::Maintenance;
		shared_ptr<const ThumbnailBundle> bundle;
	};
	struct EncodeJob {
		Reservation reservation = 0;
		shared_ptr<const ThumbnailBundle> bundle;
	};

	Thumbnailer *owner = nullptr;
	mutable mutex mu;
	condition_variable cv;
	bool stop = false;
	array<deque<shared_ptr<CpuTask>>, kPriorityCount> cpu;
	deque<GuiTask> gui;
	unordered_map<Client, ClientState> clients;
	unordered_map<uint64_t, GpuTask> gpu_tasks;
	vector<std::thread> workers;
	vector<std::thread> encoders;
	deque<EncodeJob> encode;
	deque<Reservation> encoded;
	unordered_map<Reservation, BundleSlot> bundles;
	size_t worker_count = 1;
	size_t bundle_bytes = 0;
	unique_ptr<dawn::ThumbScaler> scaler;
	QTimer timer;
	atomic_bool pump_posted = false;
	uint64_t next_client = 1;
	uint64_t next_gpu = 1;
	uint64_t next_reservation = 1;
	size_t cpu_running = 0;
	size_t background_running = 0;
	size_t background_max = 1;

	explicit Impl(Thumbnailer *thumbnailer, unsigned worker_count);
	~Impl();
	void shutdown();
	bool have_cpu() const;
	bool pop_cpu(shared_ptr<CpuTask> *task);
	void worker_loop();
	void encoder_loop();
	void erase_queued(Client id, ClientState &state);
	void erase_gui(Client id, ClientState &state);
	void erase_gpu(Client id, ClientState &state);
	void drop_gpu(uint64_t gpu_id);
	void fail_gpu(uint64_t gpu_id, string path);
	bool scaler_queue(const dawn::ThumbScaler::Job &job);
	void erase_unpublished(Client id);
};

Thumbnailer::Impl::Impl(Thumbnailer *thumbnailer, unsigned worker_count)
	: owner(thumbnailer)
{
	timer.setSingleShot(true);
	QObject::connect(&timer, &QTimer::timeout, owner,
		[thumbnailer] { thumbnailer->pump(); });

	unsigned count =
		worker_count ? worker_count : std::thread::hardware_concurrency();
	if (!count)
		count = 1;
	this->worker_count = count;
	background_max = min<size_t>(4, max<size_t>(1, count / 4));
	workers.reserve(count);
	for (unsigned i = 0; i < count; ++i)
		workers.emplace_back([this] { worker_loop(); });
	encoders.reserve(count);
	for (unsigned i = 0; i < count; ++i)
		encoders.emplace_back([this] { encoder_loop(); });
}

Thumbnailer::Impl::~Impl()
{
	shutdown();
}

void
Thumbnailer::Impl::shutdown()
{
	{
		lock_guard lock(mu);
		stop = true;
		for (auto &queue : cpu)
			queue.clear();
		encode.clear();
		gui.clear();
		gpu_tasks.clear();
		clients.clear();
	}
	cv.notify_all();
	if (scaler)
		scaler->shutdown();
	for (std::thread &worker : workers)
		if (worker.joinable())
			worker.join();
	for (std::thread &encoder : encoders)
		if (encoder.joinable())
			encoder.join();
	{
		lock_guard lock(mu);
		bundles.clear();
		encoded.clear();
		bundle_bytes = 0;
	}
	timer.stop();
	scaler.reset();
}

bool
Thumbnailer::Impl::have_cpu() const
{
	const bool reserve = worker_count > 1 && cpu_running >= worker_count - 1;
	for (size_t priority = 0; priority < cpu.size(); ++priority) {
		if (priority == priority_index(Priority::Prefetch) && reserve)
			continue;
		if (priority >= priority_index(Priority::Dimensions) &&
			(reserve || background_running >= background_max))
			continue;
		for (const auto &task : cpu[priority])
			if (task->queued && priority_index(task->priority) == priority)
				return true;
	}
	return false;
}

bool
Thumbnailer::Impl::pop_cpu(shared_ptr<CpuTask> *task)
{
	for (size_t priority = 0; priority < cpu.size(); ++priority) {
		const bool reserve =
			worker_count > 1 && cpu_running >= worker_count - 1;
		if (priority == priority_index(Priority::Prefetch) && reserve)
			continue;
		if (priority >= priority_index(Priority::Dimensions) &&
			(reserve || background_running >= background_max))
			continue;
		auto &queue = cpu[priority];
		while (!queue.empty()) {
			shared_ptr<CpuTask> queued = std::move(queue.front());
			queue.pop_front();
			if (!queued->queued || priority_index(queued->priority) != priority)
				continue;
			queued->queued = false;
			*task = std::move(queued);
			return true;
		}
	}
	return false;
}

void
Thumbnailer::Impl::worker_loop()
{
	while (true) {
		shared_ptr<CpuTask> task;
		{
			unique_lock lock(mu);
			cv.wait(lock, [this] { return stop || have_cpu(); });
			if (stop)
				return;
			if (!pop_cpu(&task))
				continue;
			auto client = clients.find(task->client);
			if (client == clients.end())
				continue;
			client->second.queued--;
			if (client->second.epoch != task->epoch)
				continue;
			client->second.running++;
			task->running_priority = task->priority;
			cpu_running++;
			if (priority_index(task->running_priority) >=
				priority_index(Priority::Dimensions))
				background_running++;
		}
		current_cpu_gate = task->gate;
		Completion completion = task->work ? task->work() : Completion{};
		current_cpu_gate.reset();
		{
			lock_guard lock(mu);
			auto client = clients.find(task->client);
			if (client != clients.end()) {
				client->second.running--;
				client->second.activity_pending = true;
				if (!task->key.empty()) {
					auto keyed = client->second.keyed.find(task->key);
					if (keyed != client->second.keyed.end() &&
						keyed->second == task)
						client->second.keyed.erase(keyed);
				}
			}
			cpu_running--;
			if (priority_index(task->running_priority) >=
				priority_index(Priority::Dimensions))
				background_running--;
			if (client != clients.end() && completion &&
				client->second.epoch == task->epoch) {
				client->second.gui++;
				gui.push_back({task->client, task->epoch, task->running_priority,
					std::move(completion)});
			}
		}
		task->gate->store(true, memory_order_release);
		cv.notify_all();
		owner->schedule_pump();
	}
}

void
Thumbnailer::Impl::encoder_loop()
{
	while (true) {
		EncodeJob job;
		{
			unique_lock lock(mu);
			cv.wait(lock, [this] { return stop || !encode.empty(); });
			if (stop && encode.empty())
				return;
			job = std::move(encode.front());
			encode.pop_front();
		}
		if (job.bundle) {
			for (const ThumbnailTierPixels &tier : job.bundle->tiers) {
				if (!tier.pixels || tier.pixels->empty())
					continue;
				QString error;
				if (!thumbnail_cache_write(job.bundle->source, tier.tier,
						tier.pixels->data(), tier.width, tier.height,
						job.bundle->image_width, job.bundle->image_height,
						&error)) {
					if (error.isEmpty())
						error = QStringLiteral("thumbnail cache write failed");
					qWarning("%s: tier %d: %s",
						qUtf8Printable(job.bundle->source.path), tier.tier,
						qUtf8Printable(error));
				}
			}
		}
		{
			lock_guard lock(mu);
			if (stop)
				continue;
			encoded.push_back(job.reservation);
		}
		owner->schedule_pump();
	}
}

void
Thumbnailer::Impl::erase_unpublished(Client id)
{
	for (auto it = bundles.begin(); it != bundles.end();) {
		if (it->second.client != id || it->second.bundle) {
			++it;
			continue;
		}
		bundle_bytes -= it->second.reserved_bytes;
		it = bundles.erase(it);
	}
}

void
Thumbnailer::Impl::erase_queued(Client id, ClientState &state)
{
	for (auto &queue : cpu)
		erase_if(queue, [id](const auto &task) { return task->client == id; });
	state.queued = 0;
	state.keyed.clear();
}

void
Thumbnailer::Impl::erase_gui(Client id, ClientState &state)
{
	for (auto it = gui.begin(); it != gui.end();) {
		if (it->client == id) {
			state.gui--;
			it = gui.erase(it);
		} else {
			++it;
		}
	}
}

void
Thumbnailer::Impl::erase_gpu(Client id, ClientState &state)
{
	for (auto it = gpu_tasks.begin(); it != gpu_tasks.end();) {
		if (it->second.client == id) {
			if (scaler)
				scaler->cancel(it->first);
			state.gpu--;
			it = gpu_tasks.erase(it);
		} else {
			++it;
		}
	}
}

void
Thumbnailer::Impl::drop_gpu(uint64_t gpu_id)
{
	auto found = gpu_tasks.find(gpu_id);
	if (found == gpu_tasks.end())
		return;
	if (auto client = clients.find(found->second.client);
		client != clients.end()) {
		client->second.gpu--;
		client->second.activity_pending = true;
	}
	gpu_tasks.erase(found);
}

void
Thumbnailer::Impl::fail_gpu(uint64_t gpu_id, string path)
{
	auto found = gpu_tasks.find(gpu_id);
	if (found == gpu_tasks.end())
		return;
	dawn::ThumbScaler::Result result;
	result.user = gpu_id;
	result.path = std::move(path);
	result.failed = true;
	found->second.result = std::move(result);
}

bool
Thumbnailer::Impl::scaler_queue(const dawn::ThumbScaler::Job &job)
{
	if (!scaler)
		return false;
	owner->schedule_pump();
	if (!scaler->queue(job))
		return false;
	owner->schedule_pump();
	return true;
}

Thumbnailer::Thumbnailer(QObject *parent, unsigned workers)
	: QObject(parent), impl_(make_unique<Impl>(this, workers))
{
}

Thumbnailer::~Thumbnailer()
{
	// Keep impl_ published while workers finish: running work schedules GUI
	// pumping through the owning Thumbnailer before it exits.
	impl_->shutdown();
}

bool
Thumbnailer::init(const GpuContext &gpu)
{
	if (impl_->scaler)
		return true;
	if (!gpu.phys() || !gpu.device() || !gpu.queue())
		return false;

	auto scaler = make_unique<dawn::ThumbScaler>();
	string error;
	if (!scaler->init(gpu.phys(), gpu.device(), gpu.queue(), gpu.queue_family(),
			kThumbRingBytes, &error)) {
		qWarning("dawn::ThumbScaler init failed: %s", error.c_str());
		return false;
	}
	impl_->scaler = std::move(scaler);
	return true;
}

Thumbnailer::Client
Thumbnailer::add_client(uint64_t epoch, Completion activity)
{
	lock_guard lock(impl_->mu);
	Client id = impl_->next_client++;
	if (!id)
		id = impl_->next_client++;
	impl_->clients.emplace(
		id, Impl::ClientState{.epoch = epoch, .activity = std::move(activity)});
	return id;
}

void
Thumbnailer::remove_client(Client id)
{
	lock_guard lock(impl_->mu);
	auto found = impl_->clients.find(id);
	if (found == impl_->clients.end())
		return;

	impl_->erase_queued(id, found->second);
	impl_->erase_gui(id, found->second);
	impl_->erase_gpu(id, found->second);
	impl_->erase_unpublished(id);
	impl_->clients.erase(found);
}

void
Thumbnailer::set_epoch(Client id, uint64_t epoch)
{
	lock_guard lock(impl_->mu);
	auto found = impl_->clients.find(id);
	if (found == impl_->clients.end())
		return;
	impl_->erase_queued(id, found->second);
	impl_->erase_gui(id, found->second);
	impl_->erase_gpu(id, found->second);
	impl_->erase_unpublished(id);
	found->second.epoch = epoch;
	found->second.activity_pending = true;
	schedule_pump();
}

bool
Thumbnailer::submit(
	Client id, uint64_t epoch, Priority priority, Work work, string key)
{
	if (!work)
		return false;
	{
		lock_guard lock(impl_->mu);
		auto client = impl_->clients.find(id);
		if (impl_->stop || client == impl_->clients.end() ||
			client->second.epoch != epoch)
			return false;
		auto task = make_shared<Impl::CpuTask>(Impl::CpuTask{
			.client = id,
			.epoch = epoch,
			.priority = priority,
			.running_priority = priority,
			.work = std::move(work),
			.key = std::move(key),
			.gate = make_shared<atomic_bool>(false),
		});
		if (!task->key.empty()) {
			if (client->second.keyed.contains(task->key))
				return false;
			client->second.keyed.emplace(task->key, task);
		}
		impl_->cpu[priority_index(priority)].push_back(std::move(task));
		client->second.queued++;
		client->second.activity_pending = true;
	}
	impl_->cv.notify_all();
	schedule_pump();
	return true;
}

bool
Thumbnailer::reprioritize(
	Client id, uint64_t epoch, Priority priority, const string &key)
{
	vector<uint64_t> gpu;
	unique_lock lock(impl_->mu);
	auto client = impl_->clients.find(id);
	if (impl_->stop || client == impl_->clients.end() ||
		client->second.epoch != epoch)
		return false;

	bool found_any = false;
	if (auto found = client->second.keyed.find(key);
		found != client->second.keyed.end()) {
		found_any = true;
		shared_ptr<Impl::CpuTask> task = found->second;
		if (priority != task->priority) {
			task->priority = priority;
			if (task->queued)
				impl_->cpu[priority_index(priority)].push_back(std::move(task));
		}
	}
	for (auto &[gpu_id, task] : impl_->gpu_tasks) {
		if (task.client != id || task.epoch != epoch || task.key != key)
			continue;
		found_any = true;
		if (priority != task.priority) {
			task.priority = priority;
			if (!task.result)
				gpu.push_back(gpu_id);
		}
	}
	lock.unlock();
	for (uint64_t gpu_id : gpu)
		if (impl_->scaler)
			impl_->scaler->reprioritize(gpu_id, scaler_priority(priority));
	if (found_any)
		impl_->cv.notify_all();
	return found_any;
}

bool
Thumbnailer::submit_gpu(Client id, uint64_t epoch, Priority priority,
	dawn::ThumbScaler::Job job, GpuCompletion completion, string key)
{
	if (!completion)
		return false;
	uint64_t gpu_id = 0;
	{
		lock_guard lock(impl_->mu);
		auto client = impl_->clients.find(id);
		if (impl_->stop || !impl_->scaler || client == impl_->clients.end() ||
			client->second.epoch != epoch)
			return false;
		if (!key.empty()) {
			if (auto found = client->second.keyed.find(key);
				found != client->second.keyed.end())
				priority = found->second->priority;
		}
		gpu_id = impl_->next_gpu++;
		if (!gpu_id)
			gpu_id = impl_->next_gpu++;
		impl_->gpu_tasks.emplace(gpu_id,
			Impl::GpuTask{
				.client = id,
				.epoch = epoch,
				.priority = priority,
				.key = key,
				.completion = std::move(completion),
				.gate = current_cpu_gate,
			});
		client->second.gpu++;
		client->second.activity_pending = true;
	}
	job.user = gpu_id;
	job.priority = scaler_priority(priority);
	// queue() waits for pump() to free ring space.  Workers may block;
	// this thread may not.  Hand GUI callers a CPU task that does the copy.
	if (QThread::currentThread() != thread()) {
		if (impl_->scaler_queue(job))
			return true;
		lock_guard lock(impl_->mu);
		impl_->drop_gpu(gpu_id);
		return false;
	}
	string path = job.path;
	if (!submit(id, epoch, priority,
			[this, job = std::move(job), gpu_id, path]() mutable {
				if (!impl_->scaler_queue(job)) {
					lock_guard lock(impl_->mu);
					impl_->fail_gpu(gpu_id, std::move(path));
				}
				return Completion{};
			})) {
		lock_guard lock(impl_->mu);
		impl_->drop_gpu(gpu_id);
		return false;
	}
	return true;
}

Thumbnailer::Reservation
Thumbnailer::reserve_bundle(Client id, uint64_t epoch,
	const ThumbnailSource &source, int top_tier, size_t bytes, Priority priority)
{
	if (!bytes || bytes > kPendingBundleBytes)
		return 0;

	lock_guard lock(impl_->mu);
	auto client = impl_->clients.find(id);
	if (impl_->stop || client == impl_->clients.end() ||
		client->second.epoch != epoch ||
		impl_->bundles.size() >= impl_->encoders.size() ||
		bytes > kPendingBundleBytes - impl_->bundle_bytes)
		return 0;
	for (auto &[reservation, slot] : impl_->bundles) {
		if (same_source(slot.source, source) && slot.top_tier == top_tier) {
			if (priority_index(priority) < priority_index(slot.priority))
				slot.priority = priority;
			return 0;
		}
	}
	Reservation reservation = impl_->next_reservation++;
	if (!reservation)
		reservation = impl_->next_reservation++;
	impl_->bundles.emplace(reservation,
		Impl::BundleSlot{reservation, id, epoch, source, top_tier, bytes,
			priority, {}});
	impl_->bundle_bytes += bytes;
	return reservation;
}

void
Thumbnailer::cancel_bundle(Reservation reservation)
{
	if (!reservation)
		return;

	lock_guard lock(impl_->mu);
	auto found = impl_->bundles.find(reservation);
	if (found == impl_->bundles.end() || found->second.bundle)
		return;

	impl_->bundle_bytes -= found->second.reserved_bytes;
	impl_->bundles.erase(found);
}

bool
Thumbnailer::publish_bundle(
	Reservation reservation, shared_ptr<const ThumbnailBundle> bundle)
{
	if (!reservation || !bundle || bundle->tiers.empty())
		return false;
	{
		lock_guard lock(impl_->mu);
		auto found = impl_->bundles.find(reservation);
		if (impl_->stop || found == impl_->bundles.end() ||
			found->second.bundle ||
			!same_source(found->second.source, bundle->source) ||
			found->second.top_tier != bundle->top_tier ||
			bundle->bytes() > found->second.reserved_bytes)
			return false;

		found->second.client = 0;
		found->second.epoch = 0;
		found->second.bundle = bundle;
		impl_->encode.push_back({reservation, std::move(bundle)});
	}
	impl_->cv.notify_all();
	return true;
}

shared_ptr<const ThumbnailBundle>
Thumbnailer::pending_bundle(const ThumbnailSource &source, int tier) const
{
	lock_guard lock(impl_->mu);
	for (const auto &[reservation, slot] : impl_->bundles) {
		(void) reservation;
		if (slot.bundle && same_source(slot.source, source) &&
			slot.bundle->find(tier))
			return slot.bundle;
	}
	return {};
}

size_t
Thumbnailer::pending_bundle_limit() const
{
	return impl_->encoders.size();
}

size_t
Thumbnailer::pending_bundle_bytes() const
{
	lock_guard lock(impl_->mu);
	return impl_->bundle_bytes;
}

size_t
Thumbnailer::background_limit() const
{
	return impl_->background_max;
}

bool
Thumbnailer::busy(Client id) const
{
	lock_guard lock(impl_->mu);
	auto found = impl_->clients.find(id);
	if (found == impl_->clients.end())
		return false;
	const Impl::ClientState &state = found->second;
	return state.queued || state.running || state.gpu || state.gui;
}

bool
Thumbnailer::foreground_busy(Client id) const
{
	lock_guard lock(impl_->mu);
	auto client = impl_->clients.find(id);
	if (client == impl_->clients.end())
		return false;
	for (const auto &[key, task] : client->second.keyed) {
		(void) key;
		if (priority_index(task->priority) < priority_index(Priority::Dimensions))
			return true;
	}
	for (const auto &[gpu_id, task] : impl_->gpu_tasks) {
		(void) gpu_id;
		if (task.client == id && priority_index(task.priority) <
			priority_index(Priority::Dimensions))
			return true;
	}
	for (const Impl::GuiTask &task : impl_->gui)
		if (task.client == id && priority_index(task.priority) <
			priority_index(Priority::Dimensions))
			return true;
	return false;
}

void
Thumbnailer::schedule_pump()
{
	if (impl_->pump_posted.exchange(true))
		return;

	QMetaObject::invokeMethod(this, [this] { pump(); }, Qt::QueuedConnection);
}

void
Thumbnailer::pump()
{
	impl_->pump_posted = false;
	vector<Completion> encoder_activity;
	{
		lock_guard lock(impl_->mu);
		bool released = false;
		while (!impl_->encoded.empty()) {
			const Reservation reservation = impl_->encoded.front();
			impl_->encoded.pop_front();
			auto found = impl_->bundles.find(reservation);
			if (found == impl_->bundles.end())
				continue;
			impl_->bundle_bytes -= found->second.reserved_bytes;
			impl_->bundles.erase(found);
			released = true;
		}
		if (released)
			for (auto &entry : impl_->clients)
				if (entry.second.activity)
					encoder_activity.push_back(entry.second.activity);
	}
	for (Completion &notify : encoder_activity)
		notify();

	// CPU completions establish browser state needed by any GPU job they
	// queued. Always apply them before polling those jobs.
	size_t gui_count = 0;
	for (; gui_count < kGuiBatch; ++gui_count) {
		Completion completion;
		{
			lock_guard lock(impl_->mu);
			if (impl_->gui.empty())
				break;
			Impl::GuiTask task = std::move(impl_->gui.front());
			impl_->gui.pop_front();
			auto client = impl_->clients.find(task.client);
			if (client == impl_->clients.end())
				continue;
			client->second.gui--;
			client->second.activity_pending = true;
			if (client->second.epoch == task.epoch)
				completion = std::move(task.completion);
		}
		if (completion)
			completion();
	}

	vector<dawn::ThumbScaler::Result> results;
	if (impl_->scaler)
		impl_->scaler->poll(&results);
	for (dawn::ThumbScaler::Result &result : results) {
		lock_guard lock(impl_->mu);
		if (auto found = impl_->gpu_tasks.find(result.user);
			found != impl_->gpu_tasks.end())
			found->second.result = std::move(result);
	}

	vector<pair<GpuCompletion, dawn::ThumbScaler::Result>> callbacks;
	{
		lock_guard lock(impl_->mu);
		for (auto it = impl_->gpu_tasks.begin();
			it != impl_->gpu_tasks.end();) {
			Impl::GpuTask &task = it->second;
			if (!task.result ||
				(task.gate && !task.gate->load(memory_order_acquire))) {
				++it;
				continue;
			}
			auto client = impl_->clients.find(task.client);
			if (client != impl_->clients.end()) {
				client->second.gpu--;
				client->second.activity_pending = true;
				if (client->second.epoch == task.epoch)
					callbacks.emplace_back(
						std::move(task.completion), std::move(*task.result));
			}
			it = impl_->gpu_tasks.erase(it);
		}
	}
	for (auto &[completion, result] : callbacks)
		if (completion)
			completion(std::move(result));
	if (impl_->scaler)
		impl_->scaler->flush();
	bool gpu_producer = false;
	vector<Completion> activity;
	{
		lock_guard lock(impl_->mu);
		if (!impl_->gui.empty())
			schedule_pump();

		// A registered GPU task can be between copying a tile and publishing it
		// to the scaler, in which case scaler->busy() is momentarily false.
		// Keep polling until queue() has returned and the task has been
		// retired.
		gpu_producer = !impl_->gpu_tasks.empty();
		for (auto &entry : impl_->clients) {
			Impl::ClientState &client = entry.second;
			if (!client.activity_pending)
				continue;
			client.activity_pending = false;
			if (client.activity)
				activity.push_back(client.activity);
		}
	}
	for (Completion &notify : activity)
		notify();
	if (impl_->scaler && (gpu_producer || impl_->scaler->busy()))
		impl_->timer.start(1);
}

}  // namespace dn
