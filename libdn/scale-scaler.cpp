//
// scale-scaler.cpp: headless Vulkan scaler
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdnvk.h"
#include "vk-device.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

using namespace std;

namespace dn
{
namespace
{

bool
check_vk(VkResult r, const char *what, string *error)
{
	if (r != VK_SUCCESS) {
		if (error)
			*error = string(what) + " failed: VkResult " +
				to_string(static_cast<int>(r));
		return false;
	}
	return true;
}

static uint8_t
unpremul_channel8(uint8_t a, uint8_t x)
{
	if (a == 0)
		return 0;
	if (a == 255)
		return x;
	return uint8_t(min(255, (int(x) * 255 + a / 2) / a));
}

void
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

bool
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

uint32_t
ceil_div(uint32_t a, uint32_t b)
{
	return b == 0 ? 0 : (a + b - 1) / b;
}

}  // namespace

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

	void destroy_device();
	bool readback_dest(VkImage image, uint32_t out_w, uint32_t out_h,
		ScaleOutput *result, string *error);
};

void
ScaleScaler::Impl::destroy_device()
{
	if (!device)
		return;
	vkDeviceWaitIdle(device);
	engine.destroy();
	if (fence) {
		vkDestroyFence(device, fence, nullptr);
		fence = VK_NULL_HANDLE;
	}
	if (cmd_pool) {
		vkDestroyCommandPool(device, cmd_pool, nullptr);
		cmd_pool = VK_NULL_HANDLE;
		cmd = VK_NULL_HANDLE;
	}
	vkDestroyDevice(device, nullptr);
	device = VK_NULL_HANDLE;
	queue = VK_NULL_HANDLE;
	phys = VK_NULL_HANDLE;
	if (instance) {
		vkDestroyInstance(instance, nullptr);
		instance = VK_NULL_HANDLE;
	}
	device_ready = false;
}

bool
ScaleScaler::Impl::readback_dest(VkImage image, uint32_t out_w, uint32_t out_h,
	ScaleOutput *result, string *error)
{
	const VkDeviceSize bytes = VkDeviceSize(out_w) * out_h * 4;
	VkBuffer staging = VK_NULL_HANDLE;
	VkDeviceMemory staging_mem = VK_NULL_HANDLE;

	VkBufferCreateInfo bci{
		.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = bytes,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE,
	};
	if (!check_vk(vkCreateBuffer(device, &bci, nullptr, &staging),
			"vkCreateBuffer readback", error))
		return false;

	VkMemoryRequirements mr{};
	vkGetBufferMemoryRequirements(device, staging, &mr);
	uint32_t mem_type = vk_memory_type(phys, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
		error);
	if (mem_type == UINT32_MAX) {
		vkDestroyBuffer(device, staging, nullptr);
		return false;
	}
	VkMemoryAllocateInfo mai{
		.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = mem_type,
	};
	if (!check_vk(vkAllocateMemory(device, &mai, nullptr, &staging_mem),
			"vkAllocateMemory readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		return false;
	}
	if (!check_vk(vkBindBufferMemory(device, staging, staging_mem, 0),
			"vkBindBufferMemory readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	if (!check_vk(vkResetCommandBuffer(cmd, 0),
			"vkResetCommandBuffer readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
		.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
	};
	if (!check_vk(vkBeginCommandBuffer(cmd, &begin),
			"vkBeginCommandBuffer readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

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
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&barrier);

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
	vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		staging, 1, &copy);
	if (!check_vk(vkEndCommandBuffer(cmd), "vkEndCommandBuffer readback",
			error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd,
	};
	if (!check_vk(vkQueueSubmit(queue, 1, &submit, VK_NULL_HANDLE),
			"vkQueueSubmit readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}
	if (!check_vk(
			vkQueueWaitIdle(queue), "vkQueueWaitIdle readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	void *mapped = nullptr;
	if (!check_vk(vkMapMemory(device, staging_mem, 0, bytes, 0, &mapped),
			"vkMapMemory readback", error)) {
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		return false;
	}

	try {
		result->width = out_w;
		result->height = out_h;
		result->rgba8.assign(size_t(bytes), 0);
		memcpy(result->rgba8.data(), mapped, size_t(bytes));
	} catch (const bad_alloc &) {
		vkUnmapMemory(device, staging_mem);
		vkDestroyBuffer(device, staging, nullptr);
		vkFreeMemory(device, staging_mem, nullptr);
		if (error)
			*error = "out of memory";
		return false;
	}

	vkUnmapMemory(device, staging_mem);
	vkDestroyBuffer(device, staging, nullptr);
	vkFreeMemory(device, staging_mem, nullptr);

	unpremul_rgba8(result->rgba8.data(), out_w, out_h);
	return true;
}


ScaleScaler::ScaleScaler() = default;

ScaleScaler::~ScaleScaler()
{
	destroy();
}

void
ScaleScaler::destroy()
{
	if (impl_) {
		impl_->destroy_device();
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

	s.destroy_device();

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
	if (!check_vk(vkCreateInstance(&ici, nullptr, &s.instance),
			"vkCreateInstance", error))
		return false;

	if (!vk_create_graphics_device(s.instance, VK_NULL_HANDLE, nullptr, {},
			&s.phys, &s.device, &s.queue, &s.queue_family, error)) {
		s.destroy_device();
		return false;
	}

	VkCommandPoolCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = s.queue_family,
	};
	if (!check_vk(vkCreateCommandPool(s.device, &pci, nullptr, &s.cmd_pool),
			"vkCreateCommandPool", error)) {
		s.destroy_device();
		return false;
	}
	VkCommandBufferAllocateInfo cai{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = s.cmd_pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1,
	};
	if (!check_vk(vkAllocateCommandBuffers(s.device, &cai, &s.cmd),
			"vkAllocateCommandBuffers", error)) {
		s.destroy_device();
		return false;
	}

	VkFenceCreateInfo fci{
		.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
		.flags = VK_FENCE_CREATE_SIGNALED_BIT,
	};
	if (!check_vk(vkCreateFence(s.device, &fci, nullptr, &s.fence),
			"vkCreateFence", error)) {
		s.destroy_device();
		return false;
	}

	if (!s.engine.init(s.phys, s.device, s.queue, s.queue_family,
			VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
			error)) {
		s.destroy_device();
		return false;
	}

	s.device_ready = true;
	return true;
}

bool
ScaleScaler::scale(uint32_t src_w, uint32_t src_h, const uint8_t *pixels,
	size_t stride, uint32_t want_out_w, uint32_t want_out_h, ScaleOutput *out,
	string *error)
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

	const uint32_t edge = min(4096u, max_dim);
	const uint32_t grid_cols = max(1u, ceil_div(src_w, edge));
	const uint32_t grid_rows = max(1u, ceil_div(src_h, edge));
	const uint32_t mid_cols_est = max(1u, ceil_div(want_out_w, max_dim));
	const uint32_t mid_rows_est = max(1u, ceil_div(src_h, max_dim));
	const uint32_t mid_pad_w_est = ceil_div(want_out_w, mid_cols_est);
	const uint32_t mid_pad_h_est = ceil_div(src_h, mid_rows_est);
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

	if (!check_vk(vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, UINT64_MAX),
			"vkWaitForFences", error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!check_vk(
			vkResetFences(s.device, 1, &s.fence), "vkResetFences", error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!check_vk(
			vkResetCommandBuffer(s.cmd, 0), "vkResetCommandBuffer", error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (!check_vk(vkBeginCommandBuffer(s.cmd, &begin), "vkBeginCommandBuffer",
			error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	ScaleView view{};
	view.scale = float(want_out_w) / float(src_w);
	view.filter = preferred_filter(s.phys);
	view.transfer = Transfer::Srgb;
	const float clear[4] = {0, 0, 0, 0};
	if (!s.engine.record(
			s.cmd, dest_fb, want_out_w, want_out_h, view, clear, error)) {
		vkEndCommandBuffer(s.cmd);
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	if (!check_vk(vkEndCommandBuffer(s.cmd), "vkEndCommandBuffer", error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	VkSubmitInfo submit{
		.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &s.cmd,
	};
	if (!check_vk(vkQueueSubmit(s.queue, 1, &submit, s.fence), "vkQueueSubmit",
			error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}
	if (!check_vk(vkWaitForFences(s.device, 1, &s.fence, VK_TRUE, UINT64_MAX),
			"vkWaitForFences render", error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	if (!s.readback_dest(dest_image, want_out_w, want_out_h, out, error)) {
		s.engine.destroy_offscreen(
			&dest_image, &dest_mem, &dest_view, &dest_fb);
		return false;
	}

	s.engine.destroy_offscreen(&dest_image, &dest_mem, &dest_view, &dest_fb);
	s.engine.clear_image();
	return true;
}

}  // namespace dn
