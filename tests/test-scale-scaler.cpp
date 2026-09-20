//
// test-scale-scaler.cpp: headless Vulkan scaler tests
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

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

	~EngineReadback();
	bool init(string *error);
	bool draw(const dawn::ScaleView &view, const float clear[4],
		array<uint16_t, 16> *pixels, string *error);
};
}  // namespace

EngineReadback::~EngineReadback()
{
	if (device) {
		vkDeviceWaitIdle(device);
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
	VkImageMemoryBarrier barrier{
		.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
		.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
		.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
		.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
		.image = image,
		.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
	vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &barrier);
	VkBufferImageCopy copy{
		.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
		.imageExtent = {2, 2, 1}};
	vkCmdCopyImageToBuffer(
		cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
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
	});
}
