//
// thumbnailer.cpp: process-wide thumbnail execution
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "thumbnailer.hpp"

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
constexpr size_t kPriorityCount = 4;
constexpr size_t kGuiBatch = 32;

// A worker may queue GPU work before returning the GUI completion which
// records that work in its client. Hold such GPU callbacks until the CPU
// completion has at least been placed on the GUI queue.
thread_local shared_ptr<atomic_bool> current_cpu_gate;

size_t
priority_index(Thumbnailer::Priority priority)
{
	return size_t(priority);
}

ThumbScaler::Priority
scaler_priority(Thumbnailer::Priority priority)
{
	switch (priority) {
	case Thumbnailer::Priority::Visible:
		return ThumbScaler::Priority::Interactive;
	case Thumbnailer::Priority::Prefetch:
		return ThumbScaler::Priority::Prefetch;
	case Thumbnailer::Priority::Dimensions:
		return ThumbScaler::Priority::Dimensions;
	case Thumbnailer::Priority::Maintenance:
		return ThumbScaler::Priority::Maintenance;
	}
	return ThumbScaler::Priority::Maintenance;
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
		Completion completion;
	};
	struct GpuTask {
		Client client = 0;
		uint64_t epoch = 0;
		Priority priority = Priority::Maintenance;
		string key;
		GpuCompletion completion;
		shared_ptr<atomic_bool> gate;
		optional<ThumbScaler::Result> result;
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
	size_t worker_count = 1;
	unique_ptr<ThumbScaler> scaler;
	QTimer timer;
	atomic_bool pump_posted = false;
	uint64_t next_client = 1;
	uint64_t next_gpu = 1;
	size_t cpu_running = 0;
	size_t background_running = 0;
	size_t background_max = 1;

	explicit Impl(Thumbnailer *thumbnailer, unsigned worker_count);
	~Impl();
	bool have_cpu() const;
	bool pop_cpu(shared_ptr<CpuTask> *task);
	void worker_loop();
	void erase_queued(Client id, ClientState &state);
	void erase_gui(Client id, ClientState &state);
	void erase_gpu(Client id, ClientState &state);
	void drop_gpu(uint64_t gpu_id);
	void fail_gpu(uint64_t gpu_id, string path);
	bool scaler_queue(const ThumbScaler::Job &job);
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
}

Thumbnailer::Impl::~Impl()
{
	{
		lock_guard lock(mu);
		stop = true;
		for (auto &queue : cpu)
			queue.clear();
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
	for (;;) {
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
				gui.push_back(
					{task->client, task->epoch, std::move(completion)});
			}
		}
		task->gate->store(true, memory_order_release);
		cv.notify_all();
		owner->schedule_pump();
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
	ThumbScaler::Result result;
	result.user = gpu_id;
	result.path = std::move(path);
	result.failed = true;
	found->second.result = std::move(result);
}

bool
Thumbnailer::Impl::scaler_queue(const ThumbScaler::Job &job)
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

Thumbnailer::~Thumbnailer() = default;

bool
Thumbnailer::init(const GpuContext &gpu)
{
	if (impl_->scaler)
		return true;
	if (!gpu.phys() || !gpu.device() || !gpu.queue())
		return false;
	auto scaler = make_unique<ThumbScaler>();
	string error;
	if (!scaler->init(gpu.phys(), gpu.device(), gpu.queue(), gpu.queue_family(),
			kThumbRingBytes, &error)) {
		qWarning("ThumbScaler init failed: %s", error.c_str());
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
	impl_->cv.notify_one();
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
	ThumbScaler::Job job, GpuCompletion completion, string key)
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

	vector<ThumbScaler::Result> results;
	if (impl_->scaler)
		impl_->scaler->poll(&results);
	for (ThumbScaler::Result &result : results) {
		lock_guard lock(impl_->mu);
		if (auto found = impl_->gpu_tasks.find(result.user);
			found != impl_->gpu_tasks.end())
			found->second.result = std::move(result);
	}

	vector<pair<GpuCompletion, ThumbScaler::Result>> callbacks;
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
