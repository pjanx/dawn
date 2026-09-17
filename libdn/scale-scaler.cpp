//
// scale-scaler.cpp: headless Vulkan scaler
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "scale-scaler.hpp"

#include "libdnvk.hpp"
#include "vk-device.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

namespace dawn
{

static uint8_t
unpremul_channel8(uint8_t a, uint8_t x)
{
	if (a == 0)
		return 0;
	if (a == 255)
		return x;
	return uint8_t(min(255, (int(x) * 255 + a / 2) / a));
}

static void
unpremul_rgba8(uint8_t *data, uint32_t width, uint32_t height)
{
	for (uint32_t y = 0; y < height; y++) {
		uint8_t *p = data + size_t(y) * width * 4;
		for (uint32_t x = 0; x < width; x++) {
			uint8_t r = p[0], g = p[1], b = p[2], a = p[3];
			p[0] = unpremul_channel8(a, r);
			p[1] = unpremul_channel8(a, g);
			p[2] = unpremul_channel8(a, b);
			p[3] = a;
			p += 4;
		}
	}
}

static bool
instance_has_extension(const char *name)
{
	uint32_t count = 0;
	if (vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr) !=
		VK_SUCCESS)
		return false;

	vector<VkExtensionProperties> exts(count);
	if (vkEnumerateInstanceExtensionProperties(nullptr, &count, exts.data()) !=
		VK_SUCCESS)
		return false;
	for (const auto &e : exts) {
		if (strcmp(e.extensionName, name) == 0)
			return true;
	}
	return false;
}

struct ScaleScaler::Impl {
	mutex mu;

	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	uint32_t queue_family = 0;

	VkCommandPool cmd_pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;

	ScaleEngine engine;
	bool device_ready = false;
};

static void
destroy_device(ScaleScaler::Impl &s)
{
	if (!s.device)
		return;

	vkDeviceWaitIdle(s.device);
	s.engine.destroy();
	if (s.fence) {
		vkDestroyFence(s.device, s.fence, nullptr);
		s.fence = VK_NULL_HANDLE;
	}
	if (s.cmd_pool) {
		vkDestroyCommandPool(s.device, s.cmd_pool, nullptr);
		s.cmd_pool = VK_NULL_HANDLE;
		s.cmd = VK_NULL_HANDLE;
	}
	vkDestroyDevice(s.device, nullptr);
	s.device = VK_NULL_HANDLE;
	s.queue = VK_NULL_HANDLE;
	s.phys = VK_NULL_HANDLE;
	if (s.instance) {
		vkDestroyInstance(s.instance, nullptr);
		s.instance = VK_NULL_HANDLE;
	}
	s.device_ready = false;
}

namespace
{

struct Staging {
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	void *mapped = nullptr;
};

}  // namespace

static bool
readback_staging(ScaleScaler::Impl &s, VkImage image, uint32_t out_w,
	uint32_t out_h, Staging *staging, ScaleOutput *result, string *error)
{
	const VkDeviceSize bytes = VkDeviceSize(out_w) * out_h * 4;

	VkBufferCreateInfo bci{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (!CALL_VK(CreateBuffer, " readback", s.device, &bci, nullptr,
			&staging->buffer))
		return false;

	VkMemoryRequirements mr{};
	vkGetBufferMemoryRequirements(s.device, staging->buffer, &mr);
	uint32_t mem_type = vk_memory_type(s.phys, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		error, nullptr);
	if (mem_type == UINT32_MAX)
		return false;

	VkMemoryAllocateInfo mai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!CALL_VK(AllocateMemory, " readback", s.device, &mai, nullptr,
			&staging->memory))
		return false;
	if (!CALL_VK(BindBufferMemory, " readback", s.device, staging->buffer,
			staging->memory, 0))
		return false;

	if (!CALL_VK(ResetCommandBuffer, " readback", s.cmd, 0))
		return false;

	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (!CALL_VK(BeginCommandBuffer, " readback", s.cmd, &begin))
		return false;

	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.levelCount = 1,
				.layerCount = 1,
			},
	};
	vkCmdPipelineBarrier(s.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);

	VkBufferImageCopy copy{
		.bufferOffset = 0,
		.imageSubresource =
			{
				.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
				.mipLevel = 0,
				.baseArrayLayer = 0,
				.layerCount = 1,
			},
		.imageExtent = {out_w, out_h, 1},
	};
	vkCmdCopyImageToBuffer(s.cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging->buffer, 1, &copy);
	if (!CALL_VK(EndCommandBuffer, " readback", s.cmd))
		return false;

	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s.cmd,
	};
	if (!CALL_VK(QueueSubmit, " readback", s.queue, 1, &submit, VK_NULL_HANDLE))
		return false;
	if (!CALL_VK(QueueWaitIdle, " readback", s.queue))
		return false;

	// Map the whole allocation, so that the VK_WHOLE_SIZE invalidation below
	// may end at the allocation, which need not be atom-aligned.
	if (!CALL_VK(MapMemory, " readback", s.device, staging->memory, 0,
			VK_WHOLE_SIZE, 0, &staging->mapped))
		return false;

	// HOST_CACHED memory need not be HOST_COHERENT, and then the GPU's writes
	// are not in the CPU's caches yet.
	VkMappedMemoryRange range{
		.sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
		.memory = staging->memory,
		.offset = 0,
		.size = VK_WHOLE_SIZE,
	};
	if (!CALL_VK(
			InvalidateMappedMemoryRanges, " readback", s.device, 1, &range))
		return false;

	try {
		result->width = out_w;
		result->height = out_h;
		result->rgba8.assign(size_t(bytes), 0);
		memcpy(result->rgba8.data(), staging->mapped, size_t(bytes));
	} catch (const bad_alloc &) {
		if (error)
			*error = "out of memory";
		return false;
	}

	unpremul_rgba8(result->rgba8.data(), out_w, out_h);
	return true;
}

static bool
readback_dest(ScaleScaler::Impl &s, VkImage image, uint32_t out_w,
	uint32_t out_h, ScaleOutput *result, string *error)
{
	Staging staging{};
	bool ok = readback_staging(s, image, out_w, out_h, &staging, result, error);
	if (staging.mapped)
		vkUnmapMemory(s.device, staging.memory);
	vkDestroyBuffer(s.device, staging.buffer, nullptr);
	vkFreeMemory(s.device, staging.memory, nullptr);
	return ok;
}

void
ScaleScaler::destroy()
{
	if (impl_) {
		destroy_device(*impl_);
		delete impl_;
		impl_ = nullptr;
	}
}

bool
ScaleScaler::init(string *error)
{
	if (impl_ && impl_->device_ready)
		return true;

	if (!impl_)
		impl_ = new Impl();

	Impl &s = *impl_;
	lock_guard lock(s.mu);

	if (s.device_ready)
		return true;

	destroy_device(s);

	// Before the first call that makes the loader scan for drivers.
	vk_add_bundled_driver_files();

	vector<const char *> inst_exts;
	VkInstanceCreateFlags flags = 0;
	if (instance_has_extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
		inst_exts.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	VkApplicationInfo app_info{
		.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.pApplicationName = "dn_vk",
		.applicationVersion = VK_MAKE_VERSION(1, 0, 0),
		.apiVersion = VK_API_VERSION_1_1,
	};
	VkInstanceCreateInfo ici{
		.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.flags = flags,
		.pApplicationInfo = &app_info,
		.enabledExtensionCount = uint32_t(inst_exts.size()),
		.ppEnabledExtensionNames = inst_exts.data(),
	};
	if (!CALL_VK(CreateInstance, "", &ici, nullptr, &s.instance))
		return false;

	if (!vk_create_graphics_device(s.instance, VK_NULL_HANDLE, nullptr, {},
			&s.phys, &s.device, &s.queue, &s.queue_family, error)) {
		destroy_device(s);
		return false;
	}

	VkCommandPoolCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = s.queue_family,
	};
	if (!CALL_VK(CreateCommandPool, "", s.device, &pci, nullptr, &s.cmd_pool)) {
		destroy_device(s);
		return false;
	}
	VkCommandBufferAllocateInfo cai{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = s.cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!CALL_VK(AllocateCommandBuffers, "", s.device, &cai, &s.cmd)) {
		destroy_device(s);
		return false;
	}

	VkFenceCreateInfo fci{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	if (!CALL_VK(CreateFence, "", s.device, &fci, nullptr, &s.fence)) {
		destroy_device(s);
		return false;
	}

	if (!s.engine.init(s.phys, s.device, s.queue, s.queue_family,
			VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			error)) {
		destroy_device(s);
		return false;
	}

	s.device_ready = true;
	return true;
}

bool
ScaleScaler::scale(uint32_t src_w, uint32_t src_h, const uint8_t *pixels,
	size_t stride, uint32_t want_out_w, uint32_t want_out_h,
	Orientation orientation, ScaleOutput *out, string *error)
{
	if (!out) {
		if (error)
			*error = "null ScaleOutput";
		return false;
	}
	out->width = 0;
	out->height = 0;
	out->rgba8.clear();

	if (!impl_ || !impl_->device_ready) {
		if (error)
			*error = "ScaleScaler not initialized";
		return false;
	}
	if (src_w == 0 || src_h == 0 || want_out_w == 0 || want_out_h == 0 ||
		!pixels) {
		if (error)
			*error = "invalid scale parameters";
		return false;
	}

	Impl &s = *impl_;
	lock_guard lock(s.mu);

	VkPhysicalDeviceProperties props{};
	vkGetPhysicalDeviceProperties(s.phys, &props);
	const uint32_t max_dim = props.limits.maxImageDimension2D;

	// Tiles hold the stored image; the mid buffer is oriented already.
	uint32_t disp_w = 0, disp_h = 0;
	orientation_display_size(src_w, src_h, orientation, &disp_w, &disp_h);

	const uint32_t edge = min(4096u, max_dim);
	const uint32_t grid_cols = max(1u, ceil_div(src_w, edge));
	const uint32_t grid_rows = max(1u, ceil_div(src_h, edge));
	const uint32_t mid_cols_est = max(1u, ceil_div(want_out_w, max_dim));
	const uint32_t mid_rows_est = max(1u, ceil_div(disp_h, max_dim));
	const uint32_t mid_pad_w_est = ceil_div(want_out_w, mid_cols_est);
	const uint32_t mid_pad_h_est = ceil_div(disp_h, mid_rows_est);
	const uint32_t tile_pad_w_est = ceil_div(src_w, grid_cols);
	const uint32_t tile_pad_h_est = ceil_div(src_h, grid_rows);
	const uint64_t tile_bytes =
		uint64_t(grid_cols * grid_rows) * tile_pad_w_est * tile_pad_h_est * 8;
	const uint64_t mid_bytes = uint64_t(mid_cols_est * mid_rows_est) *
		mid_pad_w_est * mid_pad_h_est * 8;
	const uint64_t dest_bytes = uint64_t(want_out_w) * want_out_h * 4;
	const uint64_t total = tile_bytes + mid_bytes + dest_bytes;
	if (total > kMaxDeviceBytes) {
		if (error)
			*error = "scale exceeds kMaxDeviceBytes (" + to_string(total) +
				" > " + to_string(kMaxDeviceBytes) + ")";
		return false;
	}

	if (!s.engine.set_image(src_w, src_h, pixels, stride, error))
		return false;
	if (!s.engine.ensure_viewport(want_out_w, want_out_h, error))
		return false;

	VkImage dest_image = VK_NULL_HANDLE;
	VkDeviceMemory dest_mem = VK_NULL_HANDLE;
	VkImageView dest_view = VK_NULL_HANDLE;
	VkFramebuffer dest_fb = VK_NULL_HANDLE;
	if (!s.engine.create_offscreen(want_out_w, want_out_h, &dest_image,
			&dest_mem, &dest_view, &dest_fb, error))
		return false;

	if (!CALL_VK(
			WaitForFences, "", s.device, 1, &s.fence, VK_TRUE, UINT64_MAX)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!CALL_VK(ResetFences, "", s.device, 1, &s.fence)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!CALL_VK(ResetCommandBuffer, "", s.cmd, 0)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (!CALL_VK(BeginCommandBuffer, "", s.cmd, &begin)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	ScaleView view{};
	view.scale = float(want_out_w) / float(disp_w);
	view.filter = preferred_filter(s.phys);
	view.transfer = Transfer::Srgb;
	view.orientation = orientation;
	const float clear[4] = {0, 0, 0, 0};
	if (!s.engine.record(
			s.cmd, dest_fb, want_out_w, want_out_h, view, clear, error)) {
		vkEndCommandBuffer(s.cmd);
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	if (!CALL_VK(EndCommandBuffer, "", s.cmd)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s.cmd,
	};
	if (!CALL_VK(QueueSubmit, "", s.queue, 1, &submit, s.fence)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!CALL_VK(WaitForFences, " render", s.device, 1, &s.fence, VK_TRUE,
			UINT64_MAX)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	if (!readback_dest(s, dest_image, want_out_w, want_out_h, out, error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	s.engine.destroy_offscreen(&dest_image, &dest_mem, &dest_view, &dest_fb);
	s.engine.clear_image();
	return true;
}

}  // namespace dawn
