//
// test-scale-scaler.cpp: headless Vulkan scaler tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "dn-present-frag-spv.h"
#include "dn/renderer.hpp"
#include "fullscreen-vert-spv.h"
#include "libdn/libdnvk.hpp"
#include "libdn/scale-scaler.hpp"
#include "libdn/vk-device.hpp"
#include "test.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using namespace std;
using namespace dawn;

/// One BGRA_PREMUL_4X16LE pixel, as the scaler takes them.
struct Pixel {
	uint16_t b = 0, g = 0, r = 0, a = 0;
};

static const Pixel kRed{.r = 65535, .a = 65535};
static const Pixel kGreen{.g = 65535, .a = 65535};
static const Pixel kBlue{.b = 65535, .a = 65535};
static const Pixel kWhite{65535, 65535, 65535, 65535};

static dawn::ScaleScaler scaler;

static bool
scale(const vector<Pixel> &src, uint32_t w, uint32_t h, uint32_t out_w,
	uint32_t out_h, dawn::Orientation orientation, dawn::ScaleOutput *out)
{
	string error;
	if (scaler.scale(w, h, (const uint8_t *) src.data(), w * sizeof(Pixel),
			out_w, out_h, orientation, out, &error))
		return true;

	test::fail("scale failed: %s", error.c_str());
	return false;
}

/// Output pixels are straight RGBA8, whatever the input order is.
static bool
pixel_is(const dawn::ScaleOutput &out, uint32_t x, uint32_t y, uint8_t r,
	uint8_t g, uint8_t b, uint8_t a)
{
	if (x >= out.width || y >= out.height)
		return false;

	const uint8_t *p = out.rgba8.data() + (size_t(y) * out.width + x) * 4;
	if (p[0] == r && p[1] == g && p[2] == b && p[3] == a)
		return true;

	test::fail("pixel %u,%u is %u %u %u %u, want %u %u %u %u", x, y, p[0], p[1],
		p[2], p[3], r, g, b, a);
	return false;
}

// Read the engine's premultiplied RGBA16 output directly, before the
// convenience scaler unassociates and quantizes it to RGBA8.
namespace
{
struct EngineReadback {
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice phys = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer cmd = VK_NULL_HANDLE;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkImageView image_view = VK_NULL_HANDLE;
	VkFramebuffer fb = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory staging = VK_NULL_HANDLE;
	dawn::ScaleEngine engine;
	dn::OverlayVulkan overlay;
	VkImage presented = VK_NULL_HANDLE;
	VkDeviceMemory presented_memory = VK_NULL_HANDLE;
	VkImageView presented_view = VK_NULL_HANDLE;
	VkFramebuffer presented_fb = VK_NULL_HANDLE;
	VkDescriptorSetLayout set_layout = VK_NULL_HANDLE;
	VkDescriptorPool descriptors = VK_NULL_HANDLE;
	VkDescriptorSet set = VK_NULL_HANDLE;
	VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;
	VkPipeline pipeline = VK_NULL_HANDLE;
	VkSampler sampler = VK_NULL_HANDLE;

	~EngineReadback();
	bool init(string *error);
	bool init_presentation(string *error);
	bool readback(VkImage source, array<uint16_t, 16> *pixels, string *error);
	bool compose(const dn::OverlayMesh &mesh, bool premultiplied, float levels,
		array<uint16_t, 16> *pixels, string *error);
	bool draw(const dawn::ScaleView &view, const float clear[4],
		array<uint16_t, 16> *pixels, string *error);
};
}  // namespace

EngineReadback::~EngineReadback()
{
	if (device) {
		vkDeviceWaitIdle(device);
		overlay.destroy();
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
		vkDestroyDescriptorPool(device, descriptors, nullptr);
		vkDestroyDescriptorSetLayout(device, set_layout, nullptr);
		vkDestroySampler(device, sampler, nullptr);
		engine.destroy_offscreen(
			&presented, &presented_memory, &presented_view, &presented_fb);
		engine.destroy_offscreen(&image, &memory, &image_view, &fb);
		engine.destroy();
		vkDestroyBuffer(device, buffer, nullptr);
		vkFreeMemory(device, staging, nullptr);
		vkDestroyCommandPool(device, pool, nullptr);
		vkDestroyDevice(device, nullptr);
	}
	vkDestroyInstance(instance, nullptr);
}

bool
EngineReadback::init(string *error)
{
	VkApplicationInfo app{.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
		.apiVersion = VK_API_VERSION_1_1};
	VkInstanceCreateInfo ici{.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
		.pApplicationInfo = &app};
	uint32_t count = 0;
	if (!CALL_VK(EnumerateInstanceExtensionProperties, " test", nullptr, &count,
			nullptr))
		return false;
	vector<VkExtensionProperties> extensions(count);
	if (!CALL_VK(EnumerateInstanceExtensionProperties, " test", nullptr, &count,
			extensions.data()))
		return false;
	const char *portability = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
	for (const auto &ext : extensions) {
		if (strcmp(ext.extensionName, portability) == 0) {
			ici.flags = VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
			ici.enabledExtensionCount = 1;
			ici.ppEnabledExtensionNames = &portability;
		}
	}
	if (!CALL_VK(CreateInstance, " test", &ici, nullptr, &instance))
		return false;
	uint32_t family = 0;
	if (!dawn::vk_create_graphics_device(instance, VK_NULL_HANDLE, nullptr, {},
			&phys, &device, &queue, &family, error))
		return false;
	VkCommandPoolCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
		.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
		.queueFamilyIndex = family};
	if (!CALL_VK(CreateCommandPool, " test", device, &pci, nullptr, &pool))
		return false;
	VkCommandBufferAllocateInfo cai{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
		.commandPool = pool,
		.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
		.commandBufferCount = 1};
	if (!CALL_VK(AllocateCommandBuffers, " test", device, &cai, &cmd))
		return false;
	if (!engine.init(phys, device, queue, family, VK_FORMAT_R16G16B16A16_UNORM,
			VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, error) ||
		!engine.create_offscreen(
			2, 2, &image, &memory, &image_view, &fb, error))
		return false;
	if (!overlay.init(phys, device, queue, family))
		return false;
	overlay.set_encoding_buffer(engine.encoding_buffer());
	overlay.set_target(image_view, {2, 2});
	const uint16_t atlas[] = {
		65535, 65535, 65535, 65535, 32768, 32768, 32768, 32768};
	if (!overlay.upload_font((const unsigned char *) atlas, 2, 1))
		return false;
	VkBufferCreateInfo bci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
		.size = 32,
		.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
		.sharingMode = VK_SHARING_MODE_EXCLUSIVE};
	if (!CALL_VK(CreateBuffer, " test", device, &bci, nullptr, &buffer))
		return false;
	VkMemoryRequirements mr{};
	vkGetBufferMemoryRequirements(device, buffer, &mr);
	const uint32_t type = dawn::vk_memory_type(phys, mr.memoryTypeBits,
		VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
			VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
		error, nullptr);
	if (type == UINT32_MAX)
		return false;
	VkMemoryAllocateInfo mai{.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
		.allocationSize = mr.size,
		.memoryTypeIndex = type};
	return CALL_VK(AllocateMemory, " test", device, &mai, nullptr, &staging) &&
		CALL_VK(BindBufferMemory, " test", device, buffer, staging, 0);
}

bool
EngineReadback::draw(const dawn::ScaleView &view, const float clear[4],
	array<uint16_t, 16> *pixels, string *error)
{
	if (!CALL_VK(ResetCommandBuffer, " test", cmd, 0))
		return false;
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (!CALL_VK(BeginCommandBuffer, " test", cmd, &begin) ||
		!engine.record(cmd, fb, 2, 2, view, clear, error))
		return false;
	return readback(image, pixels, error);
}

bool
EngineReadback::readback(
	VkImage source, array<uint16_t, 16> *pixels, string *error)
{
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = source,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferImageCopy copy{
		.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.imageExtent = {2, 2, 1}};
	vkCmdCopyImageToBuffer(
		cmd, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
	if (!CALL_VK(EndCommandBuffer, " test", cmd))
		return false;
	VkSubmitInfo submit{.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
		.commandBufferCount = 1,
		.pCommandBuffers = &cmd};
	if (!CALL_VK(QueueSubmit, " test", queue, 1, &submit, VK_NULL_HANDLE) ||
		!CALL_VK(QueueWaitIdle, " test", queue))
		return false;
	void *mapped = nullptr;
	if (!CALL_VK(
			MapMemory, " test", device, staging, 0, VK_WHOLE_SIZE, 0, &mapped))
		return false;
	memcpy(pixels->data(), mapped, sizeof *pixels);
	vkUnmapMemory(device, staging);
	return true;
}

bool
EngineReadback::init_presentation(string *error)
{
	if (!engine.create_offscreen(2, 2, &presented, &presented_memory,
			&presented_view, &presented_fb, error))
		return false;
	const VkDescriptorSetLayoutBinding bindings[] = {
		{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
			VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
		{1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT,
			nullptr},
	};
	VkDescriptorSetLayoutCreateInfo dlci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
		.bindingCount = 2,
		.pBindings = bindings};
	if (!CALL_VK(CreateDescriptorSetLayout, " present test", device, &dlci,
			nullptr, &set_layout))
		return false;
	const VkDescriptorPoolSize sizes[] = {
		{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1},
		{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
	VkDescriptorPoolCreateInfo dpci{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
		.maxSets = 1,
		.poolSizeCount = 2,
		.pPoolSizes = sizes};
	if (!CALL_VK(CreateDescriptorPool, " present test", device, &dpci, nullptr,
			&descriptors))
		return false;
	VkDescriptorSetAllocateInfo ai{
		.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
		.descriptorPool = descriptors,
		.descriptorSetCount = 1,
		.pSetLayouts = &set_layout};
	if (!CALL_VK(AllocateDescriptorSets, " present test", device, &ai, &set))
		return false;
	VkSamplerCreateInfo sci{.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
		.magFilter = VK_FILTER_NEAREST,
		.minFilter = VK_FILTER_NEAREST,
		.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
		.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE};
	if (!CALL_VK(
			CreateSampler, " present test", device, &sci, nullptr, &sampler))
		return false;
	const VkDescriptorImageInfo image_info{
		sampler, image_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
	const auto buffer_info = engine.encoding_buffer();
	const VkWriteDescriptorSet writes[] = {
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.dstBinding = 0,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
			.pImageInfo = &image_info},
		{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
			.dstSet = set,
			.dstBinding = 1,
			.descriptorCount = 1,
			.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
			.pBufferInfo = &buffer_info},
	};
	vkUpdateDescriptorSets(device, 2, writes, 0, nullptr);
	VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 12};
	VkPipelineLayoutCreateInfo plci{
		.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
		.setLayoutCount = 1,
		.pSetLayouts = &set_layout,
		.pushConstantRangeCount = 1,
		.pPushConstantRanges = &push};
	if (!CALL_VK(CreatePipelineLayout, " present test", device, &plci, nullptr,
			&pipeline_layout))
		return false;
	VkShaderModule vert = dawn::make_shader(
		device, fullscreen_vert, fullscreen_vert_words, error);
	VkShaderModule frag = dawn::make_shader(
		device, dn_present_frag, dn_present_frag_words, error);
	VkPipelineShaderStageCreateInfo stages[] = {
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_VERTEX_BIT,
			.module = vert,
			.pName = "main"},
		{.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
			.stage = VK_SHADER_STAGE_FRAGMENT_BIT,
			.module = frag,
			.pName = "main"},
	};
	VkGraphicsPipelineCreateInfo pci{
		.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
		.stageCount = 2,
		.pStages = stages,
		.pVertexInputState = &dawn::kNoVertexInput,
		.pInputAssemblyState = &dawn::kTriangleList,
		.pViewportState = &dawn::kOneViewport,
		.pRasterizationState = &dawn::kRasterFill,
		.pMultisampleState = &dawn::kNoMultisample,
		.pColorBlendState = &dawn::kBlendReplace,
		.pDynamicState = &dawn::kDynamicViewportScissor,
		.layout = pipeline_layout,
		.renderPass = engine.dest_render_pass()};
	const bool ok = vert && frag &&
		CALL_VK(CreateGraphicsPipelines, " present test", device,
			VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline);
	vkDestroyShaderModule(device, vert, nullptr);
	vkDestroyShaderModule(device, frag, nullptr);
	return ok;
}

bool
EngineReadback::compose(const dn::OverlayMesh &mesh, bool premultiplied,
	float levels, array<uint16_t, 16> *pixels, string *error)
{
	if (!CALL_VK(ResetCommandBuffer, " compose test", cmd, 0))
		return false;
	VkCommandBufferBeginInfo begin{
		.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	if (!CALL_VK(BeginCommandBuffer, " compose test", cmd, &begin))
		return false;
	const float clear[4] = {};
	if (!engine.record_clear(cmd, fb, 2, 2, clear, error))
		return false;
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
			VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0,
		nullptr, 1, &barrier);
	overlay.record(cmd, mesh);
	barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
		&barrier);
	const VkClearValue zero{};
	VkRenderPassBeginInfo rp{.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
		.renderPass = engine.dest_render_pass(),
		.framebuffer = presented_fb,
		.renderArea = {.extent = {2, 2}},
		.clearValueCount = 1,
		.pClearValues = &zero};
	vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
	VkViewport vp{.width = 2, .height = 2, .maxDepth = 1};
	VkRect2D scissor{.extent = {2, 2}};
	vkCmdSetViewport(cmd, 0, 1, &vp);
	vkCmdSetScissor(cmd, 0, 1, &scissor);
	vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
	vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
		pipeline_layout, 0, 1, &set, 0, nullptr);
	const struct {
		float levels;
		uint32_t premultiplied;
		uint32_t srgb;
	} push{levels, premultiplied, 0};
	vkCmdPushConstants(cmd, pipeline_layout, VK_SHADER_STAGE_FRAGMENT_BIT, 0,
		sizeof push, &push);
	vkCmdDraw(cmd, 3, 1, 0, 0);
	vkCmdEndRenderPass(cmd);
	return readback(presented, pixels, error);
}

static void
test_composition()
{
	EngineReadback gpu;
	string error;
	if (!gpu.init(&error) || !gpu.init_presentation(&error)) {
		test::fail("composition setup: %s", error.c_str());
		return;
	}
	const dn::Box box{0, 0, 2, 2};
	const dn::Colour white{1, 1, 1, 1};
	dn::OverlayList list;
	array<uint16_t, 16> pixels{};
	auto begin = [&] { list.begin(2, 2, {.5f, .5f, .5f, .5f}); };
	auto render = [&](bool premultiplied, float levels) {
		list.end();
		CHECK(gpu.compose(list.mesh(), premultiplied, levels, &pixels, &error));
	};
	auto near = [&](int pixel, int c, float want) {
		const float actual = pixels[pixel * 4 + c] / 65535.f;
		if (abs(actual - want) > .0003f)
			test::fail("composition pixel %d channel %d: %.6f != %.6f", pixel,
				c, actual, want);
	};
	begin();
	list.add_rect_filled(box, white);
	list.add_rect_filled(box, {0, 0, 0, .5f});
	render(true, 0);
	near(0, 0, .735357f);
	near(0, 3, 1);

	// Coverage is scalar opacity, not an encoded RGB sample.
	begin();
	list.add_rect_filled(box, white);
	list.add_image(box, {1.5f, .5f, 1.5f, .5f}, {0, 0, 0, 1});
	render(true, 0);
	near(0, 0, .735357f);

	begin();
	list.add_rect_filled_vgradient(box, white, {0, 0, 0, 1});
	render(true, 0);
	near(0, 0, .880825f);
	near(2, 0, .537099f);

	for (bool premultiplied : {false, true}) {
		begin();
		list.add_rect_filled(box, {1, 0, 0, .25f});
		list.add_rect_filled(box, {0, 0, 1, .5f});
		render(premultiplied, 0);
		near(0, 0, .484529f * (premultiplied ? .625f : 1));
		near(0, 2, .906332f * (premultiplied ? .625f : 1));
		near(0, 3, .625f);
		begin();
		render(premultiplied, 0);
		near(0, 0, 0);
		near(0, 3, 0);
	}
	// A half-transparent black image resolves against encoded checkers.
	const uint16_t thumbnail[] = {0, 0, 0, 32768};
	bool recreated = false;
	CHECK(gpu.overlay.upload_thumb(thumbnail, 1, 1, 0, 0, 2, &recreated));
	const dn::ThumbBackground background{{.25f, .25f, .25f, 1}, white, 0, 0, 1};
	begin();
	list.add_thumb(box, {0, 0, .5f, .5f}, white, background);
	render(true, 0);
	near(0, 0, .5f);
	near(1, 0, .268549f);
	begin();
	list.add_thumb(box, {0, 0, .5f, .5f}, white, background);
	list.add_rect_filled(box, {1, 0, 0, .5f});
	render(true, 0);
	near(0, 0, .801881f);
	near(0, 1, .360780f);

	// Clipping must not reset the checker phase; atlas data survives resize.
	gpu.overlay.set_target(VK_NULL_HANDLE, {});
	gpu.overlay.set_target(gpu.image_view, {2, 2});
	begin();
	auto shifted = background;
	shifted.origin_x = -1;
	list.push_clip({0, 0, 1, 2});
	list.add_thumb(box, {0, 0, .5f, .5f}, white, shifted);
	list.pop_clip();
	render(true, 0);
	near(0, 0, .268549f);
	near(1, 3, 0);
	near(2, 0, .5f);

	// A profile replacement updates both presentation and thumbnail curves.
	auto custom = dawn::profile_encoding(nullptr);
	for (size_t i = 0; i < custom.kSamples; i++) {
		const float x = float(i) / float(custom.kSamples - 1);
		for (int c = 0; c < 3; c++) {
			custom.decode[i][c] = powf(x, float(c + 1));
			custom.encode[i][c] = powf(x, 1.f / float(c + 1));
		}
	}
	CHECK(gpu.engine.set_encoding(custom, &error));
	begin();
	list.add_rect_filled(box, {.25f, .25f, .25f, 1});
	render(true, 0);
	near(0, 0, .25f);
	near(0, 1, .5f);
	near(0, 2, .629961f);
	begin();
	list.add_thumb(box, {0, 0, .5f, .5f}, white, background);
	render(true, 0);
	near(0, 0, .5f);
	near(0, 1, .5f);
	near(0, 2, .5f);
	near(1, 0, .125f);
	near(1, 1, .25f);
	near(1, 2, .31498f);

	for (float levels : {255.f, 1023.f}) {
		begin();
		list.add_rect_filled(box, {.25f, .25f, .25f, 1});
		render(true, levels);
		near(0, 0, floorf(.25f * levels + .5f / 64) / levels);
	}
}

static void
test_viewer_curves()
{
	EngineReadback gpu;
	string error;
	if (!gpu.init(&error)) {
		test::fail("viewer curves setup: %s", error.c_str());
		return;
	}
	auto curves = dawn::profile_encoding(nullptr);
	for (size_t i = 0; i < curves.kSamples; i++) {
		const float x = float(i) / float(curves.kSamples - 1);
		for (int c = 0; c < 3; c++) {
			curves.decode[i][c] = powf(x, float(c + 1));
			curves.encode[i][c] = powf(x, 1.f / float(c + 1));
		}
	}
	CHECK(gpu.engine.set_encoding(curves, &error));
	const array<Pixel, 4> src{
		{{16384, 16384, 16384, 32768}, {16384, 16384, 16384, 32768},
			{16384, 16384, 16384, 32768}, {16384, 16384, 16384, 32768}}};
	CHECK(gpu.engine.set_image(
		2, 2, (const uint8_t *) src.data(), 2 * sizeof(Pixel), &error));
	CHECK(gpu.engine.ensure_viewport(2, 2, &error));
	for (auto filter : {dawn::Filter::Nearest, dawn::Filter::Bilinear,
			 dawn::Filter::Expensive}) {
		for (int flags = 0; flags < 8; flags++) {
			dawn::ScaleView view;
			view.profile_curves = true;
			view.filter = filter;
			view.linear_blend = flags & 1;
			const bool linear = flags & 2;
			view.output_encoding = linear ? dawn::ScaleEncoding::Linear
										  : dawn::ScaleEncoding::Encoded;
			view.composite = flags & 4;
			const float bg = view.composite ? 1 : 0;
			const float clear[] = {bg, bg, bg, bg};
			array<uint16_t, 16> pixels{};
			CHECK(gpu.draw(view, clear, &pixels, &error));
			for (int c = 0; c < 3; c++) {
				const float gamma = float(c + 1);
				float expected = .5f * (linear ? powf(.5f, gamma) : .5f);
				if (view.composite) {
					expected =
						view.linear_blend ? .5f + .5f * powf(.5f, gamma) : .75f;
					if (linear != view.linear_blend)
						expected = powf(expected, linear ? gamma : 1.f / gamma);
				}
				CHECK(abs(pixels[c] / 65535.f - expected) < .0003f);
			}
			CHECK(abs(pixels[3] / 65535.f - (view.composite ? 1.f : .5f)) <
				.0001f);
		}
	}
	// CSD margins remain transparent even when the well and image are drawn.
	gpu.engine.set_dest_inset(1, 0, 0, 0);
	dawn::ScaleView view;
	view.profile_curves = true;
	view.output_encoding = dawn::ScaleEncoding::Linear;
	view.composite = true;
	const float clear[] = {1, 1, 1, 1};
	array<uint16_t, 16> pixels{};
	CHECK(gpu.draw(view, clear, &pixels, &error));
	CHECK(pixels[0] == 0 && pixels[3] == 0 && pixels[11] == 0);
	CHECK(pixels[7] == 65535 && pixels[15] == 65535);
}

static void
check_output(EngineReadback &gpu, const dawn::ScaleView &view,
	const array<Pixel, 4> &src)
{
	const bool composite = view.composite || view.checkerboard;
	const bool linear = view.output_encoding == dawn::ScaleEncoding::Linear;
	const auto transfer = view.transfer;
	// The destination uses source-over, so readback needs transparent black.
	const array<float, 4> clear =
		composite ? array<float, 4>{.8f, .6f, .4f, 1.f} : array<float, 4>{};
	array<uint16_t, 16> actual{};
	string error;
	if (!gpu.draw(view, clear.data(), &actual, &error)) {
		test::fail("engine draw: %s", error.c_str());
		return;
	}
	for (int i = 0; i < 4; i++) {
		const float a = src[i].a / 65535.f;
		const float rgb[] = {
			src[i].r / 65535.f, src[i].g / 65535.f, src[i].b / 65535.f};
		for (int c = 0; c < 3; c++) {
			const float straight = a > 0 ? rgb[c] / a : 0;
			// Alpha-preserving output decodes straight colour, then
			// re-associates; decoding premultiplied RGB would be wrong.
			float expected = (linear ? dawn::transfer_decode(straight, transfer)
									 : straight) *
				a;
			if (composite) {
				float bg = view.checkerboard && (i == 0 || i == 3)
					? view.checker_r
					: clear[c];
				expected = rgb[c];
				if (view.linear_blend) {
					bg = dawn::transfer_decode(bg, transfer);
					expected = dawn::transfer_decode(straight, transfer) * a;
				}
				expected += (1 - a) * bg;
				if (view.linear_blend != linear)
					expected = linear
						? dawn::transfer_decode(expected, transfer)
						: dawn::transfer_encode(expected, transfer);
			}
			if (abs(actual[i * 4 + c] / 65535.f - expected) > .001f)
				test::fail("filter %d transfer %d blend %d linear %d bg %d "
						   "checker %d pixel %d channel %d: %.6f != %.6f",
					int(view.filter), int(transfer), view.linear_blend, linear,
					composite, view.checkerboard, i, c,
					actual[i * 4 + c] / 65535.f, expected);
		}
		CHECK(
			abs(actual[i * 4 + 3] / 65535.f - (composite ? 1.f : a)) < .0001f);
	}
}

static void
test_output_encoding()
{
	EngineReadback gpu;
	string error;
	if (!gpu.init(&error)) {
		test::fail("engine readback init: %s", error.c_str());
		return;
	}
	// Include coloured partial alpha, alpha zero, opaque grey and black.
	const array<Pixel, 4> src{{{8192, 16384, 24576, 32768}, {},
		{32768, 32768, 32768, 65535}, {0, 0, 0, 32768}}};
	if (!gpu.engine.set_image(
			2, 2, (const uint8_t *) src.data(), 2 * sizeof(Pixel), &error) ||
		!gpu.engine.ensure_viewport(2, 2, &error)) {
		test::fail("engine image: %s", error.c_str());
		return;
	}
	CHECK(dawn::ScaleView{}.output_encoding == dawn::ScaleEncoding::Encoded);
	// Bilinear uses H/V; nearest and expensive at 1:1 use the 2D shader.
	for (auto filter : {dawn::Filter::Nearest, dawn::Filter::Bilinear,
			 dawn::Filter::Expensive}) {
		for (auto transfer : {dawn::Transfer::Linear, dawn::Transfer::Srgb,
				 dawn::Transfer::AdobeRgb}) {
			// Two independent policy bits, with each background choice.
			for (int combination = 0; combination < 12; combination++) {
				dawn::ScaleView view;
				view.filter = filter;
				view.transfer = transfer;
				view.linear_blend = combination & 1;
				view.output_encoding = (combination & 2)
					? dawn::ScaleEncoding::Linear
					: dawn::ScaleEncoding::Encoded;
				view.composite = combination / 4 == 1;
				view.checkerboard = combination / 4 == 2;
				view.checker_size = 1;
				check_output(gpu, view, src);
			}
		}
	}
}

// --- Cases -------------------------------------------------------------------

static void
test_init_destroy()
{
	string error;
	CHECK(scaler.init(&error));

	scaler.destroy();
	scaler.destroy();

	// Nothing has been submitted, so there is no fence to wait on either.
	const vector<Pixel> src{kRed};
	dawn::ScaleOutput out;
	CHECK(!scaler.scale(1, 1, (const uint8_t *) src.data(), sizeof(Pixel), 1, 1,
		dawn::Orientation::Rotate0, &out, &error));
	CHECK(error == "ScaleScaler not initialized");

	CHECK(scaler.init(&error));
	CHECK(scaler.init(&error));
}

static void
test_rejected_then_valid()
{
	const vector<Pixel> src{kRed, kGreen};
	const auto *pixels = (const uint8_t *) src.data();
	const size_t stride = 2 * sizeof(Pixel);
	const auto none = dawn::Orientation::Rotate0;

	string error;
	dawn::ScaleOutput out;
	CHECK(!scaler.scale(2, 1, pixels, stride, 0, 1, none, &out, &error));
	CHECK(!scaler.scale(0, 1, pixels, stride, 2, 1, none, &out, &error));
	CHECK(!scaler.scale(2, 1, nullptr, stride, 2, 1, none, &out, &error));

	// Rejected by the device-byte limit, before any Vulkan object is made.
	CHECK(
		!scaler.scale(2, 1, pixels, stride, 65535, 65535, none, &out, &error));
	CHECK(out.width == 0 && out.height == 0 && out.rgba8.empty());

	if (scale(src, 2, 1, 4, 2, none, &out)) {
		CHECK(out.width == 4 && out.height == 2);
		CHECK(out.rgba8.size() == 4 * 2 * 4);
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 3, 1, 0, 255, 0, 255);
	}
}

static void
test_orientation()
{
	// Stored top-left to bottom-right; all four differ, so that every
	// rotation and mirror lands somewhere else.
	const vector<Pixel> src{kRed, kGreen, kBlue, kWhite};

	dawn::ScaleOutput out;
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Rotate0, &out)) {
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 1, 0, 0, 255, 0, 255);
		pixel_is(out, 0, 1, 0, 0, 255, 255);
		pixel_is(out, 1, 1, 255, 255, 255, 255);
	}
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Rotate90, &out)) {
		pixel_is(out, 0, 0, 0, 0, 255, 255);
		pixel_is(out, 1, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 255, 255, 255, 255);
		pixel_is(out, 1, 1, 0, 255, 0, 255);
	}
	if (scale(src, 2, 2, 2, 2, dawn::Orientation::Mirror0, &out)) {
		pixel_is(out, 0, 0, 0, 255, 0, 255);
		pixel_is(out, 1, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 255, 255, 255, 255);
		pixel_is(out, 1, 1, 0, 0, 255, 255);
	}

	// A quarter turn of a wide image is a tall one.
	const vector<Pixel> wide{kRed, kGreen};
	if (scale(wide, 2, 1, 1, 2, dawn::Orientation::Rotate90, &out)) {
		CHECK(out.width == 1 && out.height == 2);
		pixel_is(out, 0, 0, 255, 0, 0, 255);
		pixel_is(out, 0, 1, 0, 255, 0, 255);
	}
}

static void
test_partial_transparency()
{
	// Half-transparent red, premultiplied, next to nothing at all.
	const vector<Pixel> src{{.r = 32768, .a = 32768}, {}};

	dawn::ScaleOutput out;
	if (scale(src, 2, 1, 2, 1, dawn::Orientation::Rotate0, &out)) {
		pixel_is(out, 0, 0, 255, 0, 0, 127);
		pixel_is(out, 1, 0, 0, 0, 0, 0);
	}
}

int
main()
{
	string error;
	if (!scaler.init(&error)) {
		fprintf(stderr, "no usable Vulkan device: %s\n", error.c_str());
		return 77;
	}

	return test::run({
		{"init and destroy", test_init_destroy},
		{"valid after rejected", test_rejected_then_valid},
		{"orientation", test_orientation},
		{"partial transparency", test_partial_transparency},
		{"output encoding and composition", test_output_encoding},
		{"linear GUI composition", test_composition},
		{"viewer display curves", test_viewer_curves},
	});
}
