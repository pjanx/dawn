//
// thumb-scaler.cpp: batched buffer-backed GPU thumbnail rescale
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "thumb-scaler.hpp"

#include "libdnvk.h"
#include "thumb-reduce-spv.h"
#include "thumb-scale-h-spv.h"
#include "thumb-scale-v-spv.h"
#include "vk-device.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace std;

namespace dawn
{

constexpr uint32_t kBatchSlots = 2;
constexpr uint32_t kMaxBatchReqs = 64;
constexpr uint32_t kMaxDescriptorSets = kMaxBatchReqs * 31;
constexpr uint64_t kReducedBudget = 256ull << 20;

// --- Helpers -----------------------------------------------------------------

static uint64_t
align_up(uint64_t v, uint64_t a)
{
	return a ? (v + a - 1) / a * a : v;
}

static uint32_t
ceil_div(uint32_t a, uint32_t b)
{
	return b ? (a + b - 1) / b : 0;
}

static uint32_t
reduced_dim(uint32_t n, uint32_t k)
{
	return k >= 32 ? 1 : ceil_div(n, 1u << k);
}

static bool
higher(ThumbScaler::Priority a, ThumbScaler::Priority b)
{
	return uint8_t(a) < uint8_t(b);
}

static bool
check_vk(VkResult r, const char *what, string *error)
{
	if (r == VK_SUCCESS)
		return true;
	if (error)
		*error = string(what) + " failed: VkResult " + to_string(int(r));
	return false;
}

#define CALL_VK(name, suffix, ...)                                             \
	check_vk(vk##name(__VA_ARGS__), "vk" #name suffix, error)

namespace
{

struct MemoryType {
	uint32_t index = UINT32_MAX;
	VkMemoryPropertyFlags flags = 0;
};

}  // namespace

static MemoryType
pick_memory(VkPhysicalDevice phys, uint32_t bits,
	VkMemoryPropertyFlags required, VkMemoryPropertyFlags preferred)
{
	VkPhysicalDeviceMemoryProperties props{};
	vkGetPhysicalDeviceMemoryProperties(phys, &props);
	MemoryType best;
	int best_score = -1;
	for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
		if (!(bits & (1u << i)))
			continue;
		const auto flags = props.memoryTypes[i].propertyFlags;
		if ((flags & required) != required)
			continue;
		const int score = __builtin_popcount(flags & preferred);
		if (score > best_score) {
			best = {i, flags};
			best_score = score;
		}
	}
	return best;
}

static VkShaderModule
make_shader(
	VkDevice device, const uint32_t *code, uint32_t words, string *error)
{
	VkShaderModuleCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size_t(words) * sizeof(uint32_t),
		.pCode = code};
	VkShaderModule shader = VK_NULL_HANDLE;
	if (!CALL_VK(CreateShaderModule, " thumbs", device, &ci, nullptr, &shader))
		return VK_NULL_HANDLE;
	return shader;
}

static bool
job_size(const ThumbScaler::Job &job, uint64_t *row, uint64_t *bytes)
{
	if (!job.pixels || job.pixels->empty() || !job.src_w || !job.src_h ||
		job.outputs.empty() || job.outputs.size() > kMaxBatchReqs)
		return false;
	for (const ThumbScaler::Job::Output &output : job.outputs)
		if (!output.width || !output.height)
			return false;
	const uint64_t row_bytes = uint64_t(job.src_w) * kBytesPerPixel;
	if (row_bytes > job.stride || row_bytes > UINT64_MAX / job.src_h)
		return false;
	if (row)
		*row = row_bytes;
	*bytes = row_bytes * job.src_h;
	const uint64_t available = job.pixels->size() * sizeof(uint16_t);
	const uint64_t needed = uint64_t(job.stride) * (job.src_h - 1) + row_bytes;
	return *bytes <= SIZE_MAX && needed <= available;
}

// --- State -------------------------------------------------------------------

namespace
{

struct Slot {
	void *mapped = nullptr;
	uint32_t id = 0;
};

struct Request {
	uint32_t slot = 0;
	uint32_t src_w = 0, src_h = 0;
	Orientation orientation = Orientation::Rotate0;
	Transfer transfer = Transfer::Srgb;
	bool opaque = true;
	uint32_t out_w = 0, out_h = 0;
	int output_tag = 0;
	vector<ThumbScaler::Job::Output> outputs;
	uint64_t user = 0;
	ThumbScaler::Priority priority = ThumbScaler::Priority::Maintenance;
	string path;
	uint32_t session = 0;
	uint32_t tile_ox = 0, tile_oy = 0, tile_w = 0, tile_h = 0;
};

struct Tile {
	uint32_t ox = 0, oy = 0, w = 0, h = 0;
};

struct SessionInfo {
	string path;
	uint64_t user = 0;
	ThumbScaler::Priority priority = ThumbScaler::Priority::Maintenance;
	uint32_t src_w = 0, src_h = 0;
	vector<ThumbScaler::Job::Output> outputs;
	uint32_t k = 0, tile_count = 0;
	Orientation orientation = Orientation::Rotate0;
	Transfer transfer = Transfer::Srgb;
	bool opaque = true;
};

struct Buffer {
	VkBuffer handle = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = nullptr;
	VkDeviceSize size = 0, allocation_size = 0;
	bool coherent = false;
};

struct Range {
	uint64_t off = 0, size = 0;
};

struct Pending {
	Request req;
	Range ring;
};

struct Waiter {
	uint64_t user = 0;
	ThumbScaler::Priority priority = ThumbScaler::Priority::Maintenance;
	uint64_t sequence = 0;
	size_t bytes = 0;
};

struct Session {
	uint32_t id = 0;
	SessionInfo info;
	uint32_t reduced_w = 0, reduced_h = 0;
	uint32_t enqueued = 0, done = 0;
	bool ended = false, failed = false;
	bool fitted = false, emitted = false;
	Buffer reduced;
};

struct Item {
	enum class Kind : uint8_t { Full, Tile, Fit } kind = Kind::Full;
	Request req;
	uint32_t session = 0, slot = 0;
	Session *owner = nullptr;
	Range ring;
	VkDeviceSize source_off = 0, mid_off = 0;
	VkDeviceSize output_off = 0, readback_off = 0;
	uint32_t display_w = 0, display_h = 0;
};

struct Batch {
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	bool in_flight = false;
	vector<Item> items;
	Buffer source, mid, output, readback, ping, pong;
	VkDescriptorPool descriptors = VK_NULL_HANDLE;
};

struct ScalePush {
	uint32_t src_w, src_h, dst_w, dst_h;
	uint32_t src_stride, dst_stride, orientation, transfer;
	uint32_t opaque, linear_source;
};

struct ReducePush {
	uint32_t src_w, src_h, dst_w, dst_h;
	uint32_t src_stride, dst_stride, dst_x, dst_y;
	uint32_t transfer, opaque, linear_source;
};

static_assert(sizeof(ScalePush) == 40);
static_assert(sizeof(ReducePush) == 44);

}  // namespace

struct ThumbScaler::Impl {
	mutex mu;
	condition_variable cv;
	bool stop = false, ready = false;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0, max_image_dim = 0;
	uint64_t max_storage_range = 0, alignment = 256;
	VkCommandPool command_pool = VK_NULL_HANDLE;
	VkDescriptorSetLayout descriptor_layout = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline scale_h = VK_NULL_HANDLE, scale_v = VK_NULL_HANDLE;
	VkPipeline reduce = VK_NULL_HANDLE;
	Buffer ring;
	uint64_t ring_bytes = 0;
	uint32_t next_slot = 1, next_session = 1;
	uint64_t next_waiter = 1;
	vector<Range> free_ranges;
	unordered_map<uint32_t, Range> live;
	vector<Waiter *> waiters;
	unordered_map<uint64_t, Priority> priority_overrides;
	unordered_set<uint64_t> canceled;
	vector<Pending> pending;
	vector<Result> failed_results;
	array<Batch, kBatchSlots> batches;
	unordered_map<uint32_t, unique_ptr<Session>> sessions;
};

// --- Vulkan objects ----------------------------------------------------------

static void
destroy_buffer(ThumbScaler::Impl &e, Buffer &b)
{
	if (!e.device)
		return;
	if (b.mapped)
		vkUnmapMemory(e.device, b.memory);
	if (b.handle)
		vkDestroyBuffer(e.device, b.handle, nullptr);
	if (b.memory)
		vkFreeMemory(e.device, b.memory, nullptr);
	b = {};
}

static bool
create_buffer(ThumbScaler::Impl &e, Buffer &b, VkDeviceSize bytes,
	VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags preferred, bool map, string *error)
{
	destroy_buffer(e, b);
	if (!bytes)
		return true;

	VkBufferCreateInfo ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = usage,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (!CALL_VK(CreateBuffer, " thumbs", e.device, &ci, nullptr, &b.handle))
		return false;

	VkMemoryRequirements mr{};
	vkGetBufferMemoryRequirements(e.device, b.handle, &mr);
	const MemoryType type =
		pick_memory(e.phys, mr.memoryTypeBits, required, preferred);
	if (type.index == UINT32_MAX) {
		if (error)
			*error = "no suitable thumbnail buffer memory";
		destroy_buffer(e, b);
		return false;
	}
	VkMemoryAllocateInfo ai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = type.index};
	if (!CALL_VK(
			AllocateMemory, " thumbs", e.device, &ai, nullptr, &b.memory) ||
		!CALL_VK(
			BindBufferMemory, " thumbs", e.device, b.handle, b.memory, 0)) {
		destroy_buffer(e, b);
		return false;
	}
	b.size = bytes;
	b.allocation_size = mr.size;
	b.coherent = type.flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
	if (map &&
		!CALL_VK(MapMemory, " thumbs", e.device, b.memory, 0, b.allocation_size,
			0, &b.mapped)) {
		destroy_buffer(e, b);
		return false;
	}
	return true;
}

static bool
ensure_buffer(ThumbScaler::Impl &e, Buffer &b, VkDeviceSize bytes,
	VkBufferUsageFlags usage, VkMemoryPropertyFlags required,
	VkMemoryPropertyFlags preferred, bool map, string *error)
{
	if (!bytes || (b.handle && b.size >= bytes))
		return true;
	return create_buffer(e, b, bytes, usage, required, preferred, map, error);
}

static void
destroy_batch(ThumbScaler::Impl &e, Batch &b)
{
	b.items.clear();
	destroy_buffer(e, b.source);
	destroy_buffer(e, b.mid);
	destroy_buffer(e, b.output);
	destroy_buffer(e, b.readback);
	destroy_buffer(e, b.ping);
	destroy_buffer(e, b.pong);
	if (b.descriptors)
		vkDestroyDescriptorPool(e.device, b.descriptors, nullptr);
	b.descriptors = VK_NULL_HANDLE;
}

static void
destroy_all(ThumbScaler::Impl &e)
{
	e.stop = true;
	e.cv.notify_all();
	if (!e.device)
		return;
	vkDeviceWaitIdle(e.device);
	for (Batch &b : e.batches) {
		destroy_batch(e, b);
		if (b.fence)
			vkDestroyFence(e.device, b.fence, nullptr);
		b = {};
	}
	for (auto &entry : e.sessions)
		destroy_buffer(e, entry.second->reduced);
	e.sessions.clear();
	destroy_buffer(e, e.ring);
	for (VkPipeline *p : {&e.scale_h, &e.scale_v, &e.reduce}) {
		if (*p)
			vkDestroyPipeline(e.device, *p, nullptr);
		*p = VK_NULL_HANDLE;
	}
	if (e.pipeline_layout)
		vkDestroyPipelineLayout(e.device, e.pipeline_layout, nullptr);
	if (e.descriptor_layout)
		vkDestroyDescriptorSetLayout(e.device, e.descriptor_layout, nullptr);
	if (e.command_pool)
		vkDestroyCommandPool(e.device, e.command_pool, nullptr);
	e.pipeline_layout = VK_NULL_HANDLE;
	e.descriptor_layout = VK_NULL_HANDLE;
	e.command_pool = VK_NULL_HANDLE;
	e.phys = VK_NULL_HANDLE;
	e.device = VK_NULL_HANDLE;
	e.queue = VK_NULL_HANDLE;
	e.ring_bytes = 0;
	e.free_ranges.clear();
	e.live.clear();
	e.waiters.clear();
	e.priority_overrides.clear();
	e.pending.clear();
	e.failed_results.clear();
	e.ready = false;
}

static bool
make_pipeline(ThumbScaler::Impl &e, const uint32_t *code, uint32_t words,
	VkPipeline *out, string *error)
{
	VkShaderModule shader = make_shader(e.device, code, words, error);
	if (!shader)
		return false;

	VkPipelineShaderStageCreateInfo stage{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
		.stage = VK_SHADER_STAGE_COMPUTE_BIT,
		.module = shader,
		.pName = "main"};
	VkComputePipelineCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
		.stage = stage,
		.layout = e.pipeline_layout};
	const bool ok = CALL_VK(CreateComputePipelines, " thumbs", e.device,
		VK_NULL_HANDLE, 1, &ci, nullptr, out);
	vkDestroyShaderModule(e.device, shader, nullptr);
	return ok;
}

static bool
make_descriptor_pool(ThumbScaler::Impl &e, Batch &b, string *error)
{
	VkDescriptorPoolSize size{
		VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxDescriptorSets * 2};
	VkDescriptorPoolCreateInfo ci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = kMaxDescriptorSets,
		.poolSizeCount = 1,
		.pPoolSizes = &size};
	return CALL_VK(CreateDescriptorPool, " thumbs", e.device, &ci, nullptr,
		&b.descriptors);
}

// --- Staging ring ------------------------------------------------------------

static bool
alloc_range(ThumbScaler::Impl &e, uint64_t bytes, Slot *slot)
{
	bytes = align_up(bytes, e.alignment);
	for (size_t i = 0; i < e.free_ranges.size(); i++) {
		Range &r = e.free_ranges[i];
		if (r.size < bytes)
			continue;
		const uint32_t id = e.next_slot++;
		if (!e.next_slot)
			e.next_slot = 1;
		const Range used{r.off, bytes};
		r.off += bytes;
		r.size -= bytes;
		if (!r.size)
			e.free_ranges.erase(e.free_ranges.begin() + ptrdiff_t(i));
		e.live[id] = used;
		slot->id = id;
		slot->mapped = static_cast<uint8_t *>(e.ring.mapped) + used.off;
		return true;
	}
	return false;
}

static void
release_range(ThumbScaler::Impl &e, uint32_t id)
{
	auto found = e.live.find(id);
	if (found == e.live.end())
		return;

	e.free_ranges.push_back(found->second);
	e.live.erase(found);
	sort(e.free_ranges.begin(), e.free_ranges.end(),
		[](const Range &a, const Range &b) { return a.off < b.off; });
	vector<Range> merged;
	for (const Range &r : e.free_ranges) {
		if (!merged.empty() && merged.back().off + merged.back().size == r.off)
			merged.back().size += r.size;
		else
			merged.push_back(r);
	}
	e.free_ranges = std::move(merged);
	e.cv.notify_all();
}

static bool
claim(ThumbScaler::Impl &e, size_t bytes, uint64_t user,
	ThumbScaler::Priority priority, Slot *slot)
{
	if (!slot)
		return false;

	*slot = {};
	if (!e.ready || !bytes || align_up(bytes, e.alignment) > e.ring_bytes)
		return false;

	unique_lock lock(e.mu);
	if (e.canceled.contains(user))
		return false;

	if (auto found = e.priority_overrides.find(user);
		found != e.priority_overrides.end())
		priority = found->second;
	Waiter waiter{user, priority, e.next_waiter++, bytes};
	if (!e.next_waiter)
		e.next_waiter = 1;
	e.waiters.push_back(&waiter);
	while (!e.stop) {
		if (e.canceled.contains(user)) {
			erase(e.waiters, &waiter);
			e.cv.notify_all();
			return false;
		}
		if (auto found = e.priority_overrides.find(user);
			found != e.priority_overrides.end())
			waiter.priority = found->second;
		Waiter *first = nullptr;
		for (Waiter *candidate : e.waiters)
			if (!first || higher(candidate->priority, first->priority) ||
				(candidate->priority == first->priority &&
					candidate->sequence < first->sequence))
				first = candidate;
		if (first == &waiter && alloc_range(e, bytes, slot)) {
			erase(e.waiters, &waiter);
			e.cv.notify_all();
			return true;
		}
		e.cv.wait(lock);
	}
	erase(e.waiters, &waiter);
	return false;
}

// --- Sessions ----------------------------------------------------------------

static Session *
session(ThumbScaler::Impl &e, uint32_t id)
{
	auto it = e.sessions.find(id);
	return it == e.sessions.end() ? nullptr : it->second.get();
}

static void
fail_result(ThumbScaler::Impl &e, const Request &req)
{
	ThumbScaler::Result r;
	r.user = req.user;
	r.path = req.path;
	r.failed = true;
	e.failed_results.push_back(std::move(r));
}

static void
fail_session(ThumbScaler::Impl &e, Session &s)
{
	s.failed = true;
	if (s.emitted)
		return;

	s.emitted = true;
	ThumbScaler::Result r;
	r.user = s.info.user;
	r.path = s.info.path;
	r.failed = true;
	e.failed_results.push_back(std::move(r));
}

static uint64_t
reduced_budget(const ThumbScaler::Impl &e)
{
	return min({kReducedBudget, e.max_storage_range, kMaxDeviceBytes});
}

static bool
choose_k_impl(const ThumbScaler::Impl &e, uint32_t w, uint32_t h, uint32_t *out)
{
	if (!w || !h || !out || !e.max_image_dim)
		return false;

	for (uint32_t k = 0; k < 32; k++) {
		const uint32_t rw = reduced_dim(w, k), rh = reduced_dim(h, k);
		const uint64_t bytes = uint64_t(rw) * rh * kBytesPerPixel;
		if (rw <= e.max_image_dim && rh <= e.max_image_dim &&
			bytes <= reduced_budget(e)) {
			*out = k;
			return true;
		}
	}
	return false;
}

static bool
choose_k(const ThumbScaler::Impl &e, uint32_t w, uint32_t h, uint32_t *k)
{
	return e.ready && choose_k_impl(e, w, h, k);
}

static bool
plan_tiles_impl(const ThumbScaler::Impl &e, uint32_t sw, uint32_t sh,
	uint32_t k, vector<Tile> *tiles)
{
	if (!sw || !sh || !tiles || k > 31)
		return false;

	tiles->clear();
	const uint32_t cell = k ? 1u << k : 1u;
	const uint64_t max_pixels =
		min<uint64_t>(e.ring_bytes, e.max_storage_range) / kBytesPerPixel;
	auto aligned = [&](uint32_t n) { return n / cell * cell; };

	uint32_t tw = min(sw, e.max_image_dim);
	tw = tw < sw ? aligned(tw) : tw;
	while (tw && uint64_t(tw) > max_pixels)
		tw = aligned(tw / 2);
	if (!tw)
		return false;

	uint32_t th = uint32_t(
		min<uint64_t>(min<uint64_t>(sh, e.max_image_dim), max_pixels / tw));
	th = th < sh ? aligned(th) : th;
	if (!th)
		return false;

	for (uint32_t y = 0; y < sh;) {
		const uint32_t h = min(th, sh - y);
		for (uint32_t x = 0; x < sw;) {
			const uint32_t w = min(tw, sw - x);
			tiles->push_back({x, y, w, h});
			x += w;
		}
		y += h;
	}
	return !tiles->empty();
}

static bool
plan_tiles(const ThumbScaler::Impl &e, uint32_t w, uint32_t h, uint32_t k,
	vector<Tile> *tiles)
{
	return e.ready && plan_tiles_impl(e, w, h, k, tiles);
}

static bool
ensure_reduced(ThumbScaler::Impl &e, Session &s, string *error)
{
	if (s.reduced.handle)
		return true;

	return create_buffer(e, s.reduced,
		VkDeviceSize(s.reduced_w) * s.reduced_h * kBytesPerPixel,
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
		0, false, error);
}

static bool
begin_session(ThumbScaler::Impl &e, const SessionInfo &info, uint32_t *id)
{
	if (!e.ready || !id || !info.src_w || !info.src_h || info.outputs.empty() ||
		!info.tile_count)
		return false;

	auto s = make_unique<Session>();
	s->info = info;
	s->reduced_w = reduced_dim(info.src_w, info.k);
	s->reduced_h = reduced_dim(info.src_h, info.k);
	if (uint64_t(s->reduced_w) * s->reduced_h * kBytesPerPixel >
		reduced_budget(e))
		return false;

	lock_guard lock(e.mu);
	if (auto priority = e.priority_overrides.find(info.user);
		priority != e.priority_overrides.end())
		s->info.priority = priority->second;
	s->id = e.next_session++;
	if (!e.next_session)
		e.next_session = 1;
	*id = s->id;
	e.sessions[*id] = std::move(s);
	return true;
}

static void
end_session(ThumbScaler::Impl &e, uint32_t id)
{
	lock_guard lock(e.mu);
	Session *s = session(e, id);
	if (!s)
		return;

	s->ended = true;
	if (s->enqueued != s->info.tile_count)
		fail_session(e, *s);
}

// --- Recording ---------------------------------------------------------------

static VkDescriptorSet
descriptor(ThumbScaler::Impl &e, Batch &b, const Buffer &in,
	VkDeviceSize in_off, VkDeviceSize in_size, const Buffer &out,
	VkDeviceSize out_off, VkDeviceSize out_size, string *error)
{
	if (!in.handle || !out.handle || !in_size || !out_size ||
		in_size > e.max_storage_range || out_size > e.max_storage_range ||
		in_off + in_size > in.size || out_off + out_size > out.size) {
		if (error)
			*error = "thumbnail storage-buffer range exceeds e.device limits";
		return VK_NULL_HANDLE;
	}
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkDescriptorSetAllocateInfo ai{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = b.descriptors,
		.descriptorSetCount = 1,
		.pSetLayouts = &e.descriptor_layout};
	if (!CALL_VK(AllocateDescriptorSets, " thumbs", e.device, &ai, &set))
		return VK_NULL_HANDLE;

	VkDescriptorBufferInfo info[2] = {
		{in.handle, in_off, in_size}, {out.handle, out_off, out_size}};
	VkWriteDescriptorSet writes[2]{};
	for (uint32_t i = 0; i < 2; i++) {
		writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
		writes[i].dstSet = set;
		writes[i].dstBinding = i;
		writes[i].descriptorCount = 1;
		writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		writes[i].pBufferInfo = &info[i];
	}
	vkUpdateDescriptorSets(e.device, 2, writes, 0, nullptr);
	return set;
}

static void
barrier(VkCommandBuffer cmd, VkAccessFlags src, VkAccessFlags dst,
	VkPipelineStageFlags src_stage, VkPipelineStageFlags dst_stage)
{
	VkMemoryBarrier b{.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
		.srcAccessMask = src,
		.dstAccessMask = dst};
	vkCmdPipelineBarrier(
		cmd, src_stage, dst_stage, 0, 1, &b, 0, nullptr, 0, nullptr);
}

static void
dispatch(ThumbScaler::Impl &e, VkCommandBuffer cmd, VkPipeline pipeline,
	VkDescriptorSet set, const void *push, uint32_t push_size, uint32_t w,
	uint32_t h)
{
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
		e.pipeline_layout, 0, 1, &set, 0, nullptr);
	vkCmdPushConstants(cmd, e.pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
		push_size, push);
	vkCmdDispatch(cmd, ceil_div(w, 16), ceil_div(h, 16), 1);
}

static bool
record_reduce(
	ThumbScaler::Impl &e, Batch &b, Item &item, Session &s, string *error)
{
	uint32_t sw = item.req.tile_w, sh = item.req.tile_h;
	const Buffer *input = &b.source;
	VkDeviceSize input_off = item.source_off;
	bool linear = false;
	for (uint32_t level = 0; level < s.info.k; level++) {
		const uint32_t dw = ceil_div(sw, 2), dh = ceil_div(sh, 2);
		const bool last = level + 1 == s.info.k;
		Buffer *output = last ? &s.reduced : ((level & 1u) ? &b.pong : &b.ping);
		const VkDeviceSize input_bytes = VkDeviceSize(sw) * sh * kBytesPerPixel;
		const VkDeviceSize output_bytes =
			last ? s.reduced.size : VkDeviceSize(dw) * dh * kBytesPerPixel;
		VkDescriptorSet set = descriptor(e, b, *input, input_off, input_bytes,
			*output, 0, output_bytes, error);
		if (!set)
			return false;

		ReducePush push{sw, sh, dw, dh, sw, last ? s.reduced_w : dw,
			last ? item.req.tile_ox >> s.info.k : 0,
			last ? item.req.tile_oy >> s.info.k : 0, uint32_t(s.info.transfer),
			s.info.opaque ? 1u : 0u, linear ? 1u : 0u};
		dispatch(e, b.cmd, e.reduce, set, &push, sizeof push, dw, dh);
		barrier(b.cmd, VK_ACCESS_SHADER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
		input = output;
		input_off = 0;
		sw = dw;
		sh = dh;
		linear = true;
	}
	return true;
}

static bool
record_h(ThumbScaler::Impl &e, Batch &b, Item &item, string *error)
{
	const Buffer *input = &b.source;
	VkDeviceSize input_off = item.source_off;
	VkDeviceSize input_bytes =
		VkDeviceSize(item.req.src_w) * item.req.src_h * kBytesPerPixel;
	bool linear = false;
	if (item.kind == Item::Kind::Fit) {
		Session *s = item.owner;
		if (!s)
			return false;
		input = &s->reduced;
		input_off = 0;
		input_bytes = s->reduced.size;
		linear = true;
	}
	const VkDeviceSize mid_bytes =
		VkDeviceSize(item.req.out_w) * item.display_h * kBytesPerPixel;
	VkDescriptorSet set = descriptor(e, b, *input, input_off, input_bytes,
		b.mid, item.mid_off, mid_bytes, error);
	if (!set)
		return false;

	ScalePush push{item.req.src_w, item.req.src_h, item.req.out_w,
		item.req.out_h, item.req.src_w, item.req.out_w,
		uint32_t(orientation_or_0(item.req.orientation)),
		uint32_t(item.req.transfer), item.req.opaque ? 1u : 0u,
		linear ? 1u : 0u};
	dispatch(e, b.cmd, e.scale_h, set, &push, sizeof push, item.req.out_w,
		item.display_h);
	return true;
}

static bool
record_v(ThumbScaler::Impl &e, Batch &b, Item &item, string *error)
{
	const VkDeviceSize mid_bytes =
		VkDeviceSize(item.req.out_w) * item.display_h * kBytesPerPixel;
	const VkDeviceSize out_bytes =
		VkDeviceSize(item.req.out_w) * item.req.out_h * kBytesPerPixel;
	VkDescriptorSet set = descriptor(e, b, b.mid, item.mid_off, mid_bytes,
		b.output, item.output_off, out_bytes, error);
	if (!set)
		return false;

	ScalePush push{item.req.src_w, item.req.src_h, item.req.out_w,
		item.req.out_h, item.req.out_w, item.req.out_w,
		uint32_t(orientation_or_0(item.req.orientation)),
		uint32_t(item.req.transfer), item.req.opaque ? 1u : 0u, 0};
	dispatch(e, b.cmd, e.scale_v, set, &push, sizeof push, item.req.out_w,
		item.req.out_h);
	return true;
}

static void
fail_items(ThumbScaler::Impl &e, Batch &b)
{
	lock_guard lock(e.mu);
	for (const Item &item : b.items) {
		if (item.slot)
			release_range(e, item.slot);
		if (item.kind == Item::Kind::Full && item.slot)
			fail_result(e, item.req);
		else if (Session *s = session(e, item.session))
			fail_session(e, *s);
	}
}

static bool
build_batch(ThumbScaler::Impl &e, Batch &b, vector<Pending> jobs,
	vector<uint32_t> fits, string *error)
{
	b.items.clear();
	VkDeviceSize source_bytes = 0, mid_bytes = 0, output_bytes = 0;
	VkDeviceSize readback_bytes = 0, scratch_bytes = 0;
	for (Pending &job : jobs) {
		Request request = std::move(job.req);
		const VkDeviceSize source_off = align_up(source_bytes, e.alignment);
		source_bytes = source_off + job.ring.size;
		if (request.session) {
			Item item;
			item.req = std::move(request);
			item.ring = job.ring;
			item.slot = item.req.slot;
			item.source_off = source_off;
			item.kind = Item::Kind::Tile;
			item.session = item.req.session;
			Session *s = nullptr;
			{
				lock_guard lock(e.mu);
				s = session(e, item.session);
				if (!s || s->failed) {
					release_range(e, item.slot);
					if (s)
						fail_session(e, *s);
					continue;
				}
			}
			if (!ensure_reduced(e, *s, error)) {
				lock_guard lock(e.mu);
				release_range(e, item.slot);
				fail_session(e, *s);
				continue;
			}
			item.owner = s;
			const uint32_t rw = ceil_div(item.req.tile_w, 2);
			const uint32_t rh = ceil_div(item.req.tile_h, 2);
			scratch_bytes =
				max(scratch_bytes, VkDeviceSize(rw) * rh * kBytesPerPixel);
			b.items.push_back(std::move(item));
			continue;
		}

		bool owns_slot = true;
		for (const ThumbScaler::Job::Output &output : request.outputs) {
			Item item;
			item.req = request;
			item.req.outputs.clear();
			item.req.out_w = output.width;
			item.req.out_h = output.height;
			item.req.output_tag = output.tag;
			item.ring = job.ring;
			item.slot = owns_slot ? request.slot : 0;
			item.source_off = source_off;
			owns_slot = false;
			item.kind = Item::Kind::Full;
			orientation_display_size(item.req.src_w, item.req.src_h,
				item.req.orientation, &item.display_w, &item.display_h);
			item.mid_off = align_up(mid_bytes, e.alignment);
			mid_bytes = item.mid_off +
				VkDeviceSize(item.req.out_w) * item.display_h * kBytesPerPixel;
			item.output_off = align_up(output_bytes, e.alignment);
			item.readback_off = align_up(readback_bytes, e.alignment);
			const VkDeviceSize n =
				VkDeviceSize(item.req.out_w) * item.req.out_h * kBytesPerPixel;
			output_bytes = item.output_off + n;
			readback_bytes = item.readback_off + n;
			b.items.push_back(std::move(item));
		}
	}
	for (uint32_t id : fits) {
		Session *s = nullptr;
		{
			lock_guard lock(e.mu);
			s = session(e, id);
			if (!s || s->failed)
				continue;
		}
		if (!ensure_reduced(e, *s, error))
			continue;

		for (const ThumbScaler::Job::Output &output : s->info.outputs) {
			Item item;
			item.kind = Item::Kind::Fit;
			item.session = id;
			item.owner = s;
			item.req.user = s->info.user;
			item.req.path = s->info.path;
			item.req.src_w = s->reduced_w;
			item.req.src_h = s->reduced_h;
			item.req.out_w = output.width;
			item.req.out_h = output.height;
			item.req.output_tag = output.tag;
			item.req.orientation = s->info.orientation;
			item.req.transfer = s->info.transfer;
			item.req.opaque = s->info.opaque;
			orientation_display_size(item.req.src_w, item.req.src_h,
				item.req.orientation, &item.display_w, &item.display_h);
			item.mid_off = align_up(mid_bytes, e.alignment);
			mid_bytes = item.mid_off +
				VkDeviceSize(item.req.out_w) * item.display_h * kBytesPerPixel;
			item.output_off = align_up(output_bytes, e.alignment);
			item.readback_off = align_up(readback_bytes, e.alignment);
			const VkDeviceSize n =
				VkDeviceSize(item.req.out_w) * item.req.out_h * kBytesPerPixel;
			output_bytes = item.output_off + n;
			readback_bytes = item.readback_off + n;
			b.items.push_back(std::move(item));
		}
	}
	if (b.items.empty())
		return false;

	const auto storage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
	if (!ensure_buffer(e, b.source, source_bytes,
			storage | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, error) ||
		!ensure_buffer(e, b.mid, mid_bytes, storage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, error) ||
		!ensure_buffer(e, b.output, output_bytes,
			storage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, error) ||
		!ensure_buffer(e, b.readback, readback_bytes,
			VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_MEMORY_PROPERTY_HOST_CACHED_BIT |
				VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
			true, error) ||
		!ensure_buffer(e, b.ping, scratch_bytes, storage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, error) ||
		!ensure_buffer(e, b.pong, scratch_bytes, storage,
			VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, false, error) ||
		!CALL_VK(ResetDescriptorPool, " thumbs", e.device, b.descriptors, 0)) {
		fail_items(e, b);
		return false;
	}

	if (!CALL_VK(ResetCommandBuffer, " thumbs", b.cmd, 0)) {
		fail_items(e, b);
		return false;
	}
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
	if (!CALL_VK(BeginCommandBuffer, " thumbs", b.cmd, &begin)) {
		fail_items(e, b);
		return false;
	}
	for (const Item &item : b.items) {
		if (!item.slot)
			continue;
		VkBufferCopy copy{item.ring.off, item.source_off, item.ring.size};
		vkCmdCopyBuffer(b.cmd, e.ring.handle, b.source.handle, 1, &copy);
	}
	if (source_bytes)
		barrier(b.cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	for (Item &item : b.items) {
		if (item.kind != Item::Kind::Tile)
			continue;
		Session *s = session(e, item.session);
		if (!s || !record_reduce(e, b, item, *s, error)) {
			vkEndCommandBuffer(b.cmd);
			fail_items(e, b);
			return false;
		}
	}
	for (Item &item : b.items) {
		if (item.kind != Item::Kind::Tile && !record_h(e, b, item, error)) {
			vkEndCommandBuffer(b.cmd);
			fail_items(e, b);
			return false;
		}
	}
	if (mid_bytes)
		barrier(b.cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
	for (Item &item : b.items) {
		if (item.kind != Item::Kind::Tile && !record_v(e, b, item, error)) {
			vkEndCommandBuffer(b.cmd);
			fail_items(e, b);
			return false;
		}
	}
	if (output_bytes) {
		barrier(b.cmd, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
			VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT);
		for (const Item &item : b.items) {
			if (item.kind == Item::Kind::Tile)
				continue;
			const VkDeviceSize n =
				VkDeviceSize(item.req.out_w) * item.req.out_h * kBytesPerPixel;
			VkBufferCopy copy{item.output_off, item.readback_off, n};
			vkCmdCopyBuffer(
				b.cmd, b.output.handle, b.readback.handle, 1, &copy);
		}
		barrier(b.cmd, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT,
			VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT);
	}
	if (!CALL_VK(EndCommandBuffer, " thumbs", b.cmd)) {
		fail_items(e, b);
		return false;
	}
	return true;
}

// --- Queueing ----------------------------------------------------------------

static bool
enqueue(ThumbScaler::Impl &e, const Request &req)
{
	lock_guard lock(e.mu);
	auto found = e.live.find(req.slot);
	if (!e.ready || e.stop || found == e.live.end() || !req.src_w ||
		!req.src_h || (!req.session && req.outputs.empty()) ||
		e.canceled.contains(req.user)) {
		release_range(e, req.slot);
		return false;
	}
	if (req.session) {
		Session *s = session(e, req.session);
		if (!s || s->ended || s->failed || !req.tile_w || !req.tile_h) {
			release_range(e, req.slot);
			return false;
		}
		s->enqueued++;
	}
	Request queued = req;
	if (auto priority = e.priority_overrides.find(req.user);
		priority != e.priority_overrides.end())
		queued.priority = priority->second;
	e.pending.push_back({std::move(queued), found->second});
	return true;
}

static bool
queue_full(ThumbScaler::Impl &e, const ThumbScaler::Job &job)
{
	uint64_t row_bytes = 0, bytes = 0;
	if (!job_size(job, &row_bytes, &bytes))
		return false;

	Slot slot;
	if (!claim(e, size_t(bytes), job.user, job.priority, &slot))
		return false;

	bool opaque = true;
	auto *dst = static_cast<uint8_t *>(slot.mapped);
	const auto *src = reinterpret_cast<const uint8_t *>(job.pixels->data());
	for (uint32_t y = 0; y < job.src_h; y++) {
		const auto *row =
			reinterpret_cast<const uint16_t *>(src + y * job.stride);
		memcpy(dst, row, size_t(row_bytes));
		if (opaque) {
			for (uint32_t x = 0; x < job.src_w; x++) {
				if (row[x * 4 + 3] != 65535) {
					opaque = false;
					break;
				}
			}
		}
		dst += row_bytes;
	}

	Request req;
	req.slot = slot.id;
	req.src_w = job.src_w;
	req.src_h = job.src_h;
	req.outputs = job.outputs;
	req.orientation = job.orientation;
	req.transfer = job.transfer;
	req.opaque = opaque;
	req.user = job.user;
	req.priority = job.priority;
	req.path = job.path;
	return enqueue(e, req);
}

// --- Interface ---------------------------------------------------------------

ThumbScaler::ThumbScaler() = default;
ThumbScaler::~ThumbScaler()
{
	destroy();
}

void
ThumbScaler::destroy()
{
	if (!impl_)
		return;

	destroy_all(*impl_);
	delete impl_;
	impl_ = nullptr;
}

bool
ThumbScaler::init(VkPhysicalDevice phys, VkDevice device, VkQueue queue,
	uint32_t family, uint64_t ring_bytes, string *error)
{
	if (!phys || !device || !queue || ring_bytes < kBytesPerPixel)
		return false;

	if (!impl_)
		impl_ = new Impl();
	Impl &e = *impl_;
	if (e.ready)
		return true;

	e.stop = false;
	e.phys = phys;
	e.device = device;
	e.queue = queue;
	e.queue_family = family;
	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(phys, &props);
	e.max_image_dim = props.limits.maxImageDimension2D;
	e.max_storage_range = props.limits.maxStorageBufferRange;
	e.alignment = max<uint64_t>({256, props.limits.nonCoherentAtomSize,
		props.limits.minStorageBufferOffsetAlignment});
	if (sizeof(ReducePush) > props.limits.maxPushConstantsSize) {
		if (error)
			*error = "thumbnail push constants exceed device limit";
		destroy_all(e);
		return false;
	}
	const uint64_t actual_ring =
		min<uint64_t>(ring_bytes, props.limits.maxStorageBufferRange);
	VkCommandPoolCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family};
	if (!CALL_VK(CreateCommandPool, " thumbs", device, &pci, nullptr,
			&e.command_pool)) {
		destroy_all(e);
		return false;
	}
	VkCommandBuffer commands[kBatchSlots]{};
	VkCommandBufferAllocateInfo cai{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = e.command_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = kBatchSlots};
	if (!CALL_VK(AllocateCommandBuffers, " thumbs", device, &cai, commands)) {
		destroy_all(e);
		return false;
	}
	for (uint32_t i = 0; i < kBatchSlots; i++) {
		e.batches[i].cmd = commands[i];
		VkFenceCreateInfo fi{.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		if (!CALL_VK(CreateFence, " thumbs", device, &fi, nullptr,
				&e.batches[i].fence) ||
			!make_descriptor_pool(e, e.batches[i], error)) {
			destroy_all(e);
			return false;
		}
	}
	VkDescriptorSetLayoutBinding bindings[2]{};
	for (uint32_t i = 0; i < 2; i++) {
		bindings[i].binding = i;
		bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
		bindings[i].descriptorCount = 1;
		bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
	}
	VkDescriptorSetLayoutCreateInfo dlci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings};
	if (!CALL_VK(CreateDescriptorSetLayout, " thumbs", device, &dlci, nullptr,
			&e.descriptor_layout)) {
		destroy_all(e);
		return false;
	}
	VkPushConstantRange pcr{
		.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(ReducePush)};
	VkPipelineLayoutCreateInfo plci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &e.descriptor_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &pcr};
	if (!CALL_VK(CreatePipelineLayout, " thumbs", device, &plci, nullptr,
			&e.pipeline_layout) ||
		!make_pipeline(
			e, thumb_scale_h, thumb_scale_h_words, &e.scale_h, error) ||
		!make_pipeline(
			e, thumb_scale_v, thumb_scale_v_words, &e.scale_v, error) ||
		!make_pipeline(e, thumb_reduce, thumb_reduce_words, &e.reduce, error) ||
		!create_buffer(e, e.ring, actual_ring, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
			VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, true, error)) {
		destroy_all(e);
		return false;
	}
	e.ring_bytes = actual_ring;
	e.free_ranges = {{0, actual_ring}};
	e.ready = true;
	return true;
}

bool
ThumbScaler::queue(const Job &job)
{
	if (!impl_ || !impl_->ready)
		return false;
	Impl &e = *impl_;
	auto fail = [&] {
		lock_guard lock(e.mu);
		e.priority_overrides.erase(job.user);
		return false;
	};
	uint64_t bytes = 0;
	if (!job_size(job, nullptr, &bytes))
		return fail();
	if (bytes <= e.ring_bytes && job.src_w <= e.max_image_dim &&
		job.src_h <= e.max_image_dim) {
		if (queue_full(e, job))
			return true;
		return fail();
	}

	uint32_t k = 0;
	vector<Tile> tiles;
	if (!choose_k(e, job.src_w, job.src_h, &k) ||
		!plan_tiles(e, job.src_w, job.src_h, k, &tiles) || tiles.empty())
		return fail();

	SessionInfo info;
	info.path = job.path;
	info.user = job.user;
	info.priority = job.priority;
	info.src_w = job.src_w;
	info.src_h = job.src_h;
	info.outputs = job.outputs;
	info.k = k;
	info.tile_count = uint32_t(tiles.size());
	info.orientation = job.orientation;
	info.transfer = job.transfer;
	// The transparent path is also exact for opaque pixels and avoids a second
	// full pass over gigantic sources.
	info.opaque = false;
	uint32_t session = 0;
	if (!begin_session(e, info, &session))
		return fail();

	const auto *base = reinterpret_cast<const uint8_t *>(job.pixels->data());
	for (const Tile &tile : tiles) {
		Slot slot;
		const size_t tile_row = size_t(tile.w) * kBytesPerPixel;
		if (!claim(e, tile_row * tile.h, job.user, job.priority, &slot))
			break;

		auto *dst = static_cast<uint8_t *>(slot.mapped);
		for (uint32_t y = 0; y < tile.h; y++) {
			const uint8_t *src = base + size_t(tile.oy + y) * job.stride +
				size_t(tile.ox) * kBytesPerPixel;
			memcpy(dst, src, tile_row);
			dst += tile_row;
		}

		Request req;
		req.slot = slot.id;
		req.src_w = tile.w;
		req.src_h = tile.h;
		req.orientation = job.orientation;
		req.transfer = job.transfer;
		req.opaque = false;
		req.user = job.user;
		req.priority = job.priority;
		req.path = job.path;
		req.session = session;
		req.tile_ox = tile.ox;
		req.tile_oy = tile.oy;
		req.tile_w = tile.w;
		req.tile_h = tile.h;
		if (!enqueue(e, req))
			break;
	}
	end_session(e, session);
	return true;
}

bool
ThumbScaler::reprioritize(uint64_t user, Priority priority)
{
	if (!impl_ || !user)
		return false;

	Impl &e = *impl_;
	lock_guard lock(e.mu);
	bool found = false;
	e.priority_overrides[user] = priority;
	for (Waiter *waiter : e.waiters) {
		if (waiter->user == user) {
			found = true;
			waiter->priority = priority;
		}
	}
	for (Pending &pending : e.pending) {
		if (pending.req.user == user) {
			found = true;
			pending.req.priority = priority;
		}
	}
	for (auto &[id, session] : e.sessions) {
		(void) id;
		if (session->info.user == user) {
			found = true;
			session->info.priority = priority;
		}
	}
	e.cv.notify_all();
	return found;
}

bool
ThumbScaler::cancel(uint64_t user)
{
	if (!impl_ || !user)
		return false;

	Impl &e = *impl_;
	lock_guard lock(e.mu);
	bool found = false;
	e.canceled.insert(user);
	for (auto it = e.pending.begin(); it != e.pending.end();) {
		if (it->req.user != user) {
			it++;
			continue;
		}
		found = true;
		release_range(e, it->req.slot);
		it = e.pending.erase(it);
	}
	for (auto &[id, session] : e.sessions) {
		(void) id;
		if (session->info.user != user)
			continue;
		found = true;
		session->failed = true;
		session->emitted = true;
	}
	for (Waiter *waiter : e.waiters)
		found |= waiter->user == user;
	e.priority_overrides.erase(user);
	e.cv.notify_all();
	return found;
}

void
ThumbScaler::flush()
{
	if (!impl_ || !impl_->ready)
		return;

	Impl &e = *impl_;
	Batch *batch = nullptr;
	vector<Pending> jobs;
	vector<uint32_t> fits;
	{
		lock_guard lock(e.mu);
		for (Batch &candidate : e.batches)
			if (!candidate.in_flight) {
				batch = &candidate;
				break;
			}
		if (!batch)
			return;
		optional<Priority> selected;
		for (const Pending &pending : e.pending)
			if (!selected || higher(pending.req.priority, *selected))
				selected = pending.req.priority;
		for (const auto &[id, session] : e.sessions) {
			(void) id;
			const Session &s = *session;
			if (s.ended && !s.failed && !s.fitted &&
				s.done == s.info.tile_count &&
				(!selected || higher(s.info.priority, *selected)))
				selected = s.info.priority;
		}
		if (!selected)
			return;
		size_t item_room = kMaxBatchReqs;
		for (auto it = e.pending.begin(); it != e.pending.end();) {
			const size_t cost = it->req.session ? 1 : it->req.outputs.size();
			if (it->req.priority == *selected && cost <= item_room) {
				jobs.push_back(std::move(*it));
				it = e.pending.erase(it);
				item_room -= cost;
			} else {
				it++;
			}
		}
		for (auto &entry : e.sessions) {
			Session &s = *entry.second;
			const size_t cost = s.info.outputs.size();
			if (cost <= item_room && s.ended && !s.failed && !s.fitted &&
				s.done == s.info.tile_count && s.info.priority == *selected) {
				s.fitted = true;
				fits.push_back(s.id);
				item_room -= cost;
			}
		}
	}
	if (jobs.empty() && fits.empty())
		return;

	string error;
	if (!build_batch(e, *batch, std::move(jobs), fits, &error))
		return;

	if (!e.ring.coherent) {
		VkMappedMemoryRange range{
			.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
			.memory = e.ring.memory,
			.offset = 0,
			.size = VK_WHOLE_SIZE};
		if (vkFlushMappedMemoryRanges(e.device, 1, &range) != VK_SUCCESS) {
			fail_items(e, *batch);
			return;
		}
	}
	if (vkResetFences(e.device, 1, &batch->fence) != VK_SUCCESS) {
		fail_items(e, *batch);
		return;
	}
	VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &batch->cmd};
	if (vkQueueSubmit(e.queue, 1, &submit, batch->fence) != VK_SUCCESS) {
		fail_items(e, *batch);
		return;
	}
	batch->in_flight = true;
}

void
ThumbScaler::poll(vector<Result> *done)
{
	if (!done)
		return;

	done->clear();
	if (!impl_ || !impl_->ready)
		return;

	Impl &e = *impl_;
	for (Batch &batch : e.batches) {
		if (!batch.in_flight)
			continue;
		const VkResult status = vkGetFenceStatus(e.device, batch.fence);
		if (status == VK_NOT_READY)
			continue;
		if (status != VK_SUCCESS) {
			fail_items(e, batch);
			batch.in_flight = false;
			continue;
		}
		if (batch.readback.mapped && !batch.readback.coherent) {
			VkMappedMemoryRange range{
				.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
				.memory = batch.readback.memory,
				.offset = 0,
				.size = VK_WHOLE_SIZE};
			vkInvalidateMappedMemoryRanges(e.device, 1, &range);
		}
		vector<uint32_t> finished;
		vector<Result> batch_results;
		unordered_map<uint64_t, size_t> result_index;
		for (const Item &item : batch.items) {
			if (item.kind == Item::Kind::Tile) {
				lock_guard lock(e.mu);
				if (Session *s = session(e, item.session))
					s->done++;
				continue;
			}
			size_t index = 0;
			if (auto found = result_index.find(item.req.user);
				found != result_index.end()) {
				index = found->second;
			} else {
				index = batch_results.size();
				result_index.emplace(item.req.user, index);
				Result result;
				result.user = item.req.user;
				result.path = item.req.path;
				batch_results.push_back(std::move(result));
			}
			Result::Output output;
			output.width = item.req.out_w;
			output.height = item.req.out_h;
			output.tag = item.req.output_tag;
			const size_t values = size_t(output.width) * output.height * 4;
			output.data.resize(values);
			memcpy(output.data.data(),
				static_cast<const uint8_t *>(batch.readback.mapped) +
					item.readback_off,
				values * sizeof(uint16_t));
			batch_results[index].outputs.push_back(std::move(output));
			if (item.kind == Item::Kind::Fit) {
				lock_guard lock(e.mu);
				if (Session *s = session(e, item.session)) {
					s->emitted = true;
					if (find(finished.begin(), finished.end(), s->id) ==
						finished.end())
						finished.push_back(s->id);
				}
				e.priority_overrides.erase(item.req.user);
				e.canceled.erase(item.req.user);
			} else {
				lock_guard lock(e.mu);
				e.priority_overrides.erase(item.req.user);
				e.canceled.erase(item.req.user);
			}
		}
		for (Result &result : batch_results)
			done->push_back(std::move(result));
		{
			lock_guard lock(e.mu);
			for (const Item &item : batch.items)
				if (item.slot)
					release_range(e, item.slot);
		}
		batch.items.clear();
		batch.in_flight = false;
		{
			lock_guard lock(e.mu);
			for (uint32_t id : finished) {
				auto it = e.sessions.find(id);
				if (it != e.sessions.end()) {
					destroy_buffer(e, it->second->reduced);
					e.sessions.erase(it);
				}
			}
		}
	}
	{
		lock_guard lock(e.mu);
		for (Result &result : e.failed_results) {
			e.priority_overrides.erase(result.user);
			e.canceled.erase(result.user);
			done->push_back(std::move(result));
		}
		e.failed_results.clear();
		vector<uint32_t> erase;
		for (auto &entry : e.sessions) {
			if (!entry.second->emitted)
				continue;
			bool referenced = false;
			for (const Batch &batch : e.batches)
				for (const Item &item : batch.items)
					referenced |= item.session == entry.first;
			if (!referenced)
				erase.push_back(entry.first);
		}
		for (uint32_t id : erase) {
			destroy_buffer(e, e.sessions[id]->reduced);
			e.sessions.erase(id);
		}
	}
}

void
ThumbScaler::shutdown()
{
	if (!impl_)
		return;

	lock_guard lock(impl_->mu);
	impl_->stop = true;
	impl_->cv.notify_all();
}

bool
ThumbScaler::busy() const
{
	if (!impl_)
		return false;

	lock_guard lock(impl_->mu);
	if (!impl_->pending.empty() || !impl_->live.empty() ||
		!impl_->failed_results.empty())
		return true;
	for (const Batch &batch : impl_->batches)
		if (batch.in_flight)
			return true;
	for (const auto &entry : impl_->sessions)
		if (!entry.second->emitted)
			return true;
	return false;
}

}  // namespace dawn
