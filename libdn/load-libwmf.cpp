//
// load-libwmf.cpp: WMF rendering through libwmf's GD device
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "gettext.hpp"
#include "libdn-loaders.hpp"
#include "libdn.hpp"

#include <libwmf/api.h>
#include <libwmf/gd.h>

#include <memory>

using namespace std;

namespace dawn
{
namespace
{

class WmfRenderClosure : public RenderClosure
{
	vector<uint8_t> data_;
	double width_;
	double height_;

public:
	WmfRenderClosure(vector<uint8_t> &&data, double width, double height)
		: data_(std::move(data)), width_(width), height_(height)
	{
	}

	ImagePtr render(
		const OpenContext &ctx, double scale, Error *error) override;
	bool dimensions(double scale, uint32_t *width, uint32_t *height,
		Error *error) const override;
};

/// Everything libwmf hands out has to go back, on every exit path.
struct WmfApi {
	wmfAPI *api = nullptr;
	bool opened = false;

	~WmfApi()
	{
		if (opened)
			wmf_mem_close(api);
		if (api)
			wmf_api_destroy(api);
	}
};

}  // namespace

static const char *
wmf_error_string(wmf_error_t error)
{
	switch (error) {
	case wmf_E_None:
		return "no error";
	case wmf_E_InsMem:
		return "out of memory";
	case wmf_E_BadFile:
		return "unreadable input";
	case wmf_E_BadFormat:
		return "invalid WMF data";
	case wmf_E_EOF:
		return "unexpected end of file";
	case wmf_E_DeviceError:
		return "GD device error";
	case wmf_E_Glitch:
		return "internal error";
	case wmf_E_Assert:
		return "internal assertion failure";
	case wmf_E_UserExit:
		return "rendering cancelled";
	}
	return "unknown error";
}

static bool
wmf_open_and_scan(WmfApi &wmf, vector<uint8_t> &data, wmfD_Rect *bbox,
	uint32_t *width, uint32_t *height, const OpenContext &ctx, Error *error)
{
	if (data.size() > size_t(LONG_MAX)) {
		set_error(error, _("input is too large"));
		return false;
	}

	wmfAPI_Options options{};
	options.function = wmf_gd_function;
	unsigned long flags = WMF_OPT_FUNCTION | WMF_OPT_IGNORE_NONFATAL |
		WMF_OPT_NO_ERROR | WMF_OPT_NO_DEBUG;
	wmf_error_t status = wmf_api_create(&wmf.api, flags, &options);
	if (status != wmf_E_None) {
		set_error(error, wmf_error_string(status));
		return false;
	}

	// The wmf_gd_image device hands out a GD image that libwmf offers no
	// way to destroy; only its encoders dispose of one themselves.
	wmf_gd_t *device = WMF_GD_GetData(wmf.api);
	if (!(device->flags & WMF_GD_SUPPORTS_PNG)) {
		set_error(error, _("built without PNG support"));
		return false;
	}
	device->type = wmf_gd_png;
	device->flags |= WMF_GD_OUTPUT_MEMORY | WMF_GD_OWN_BUFFER;

	status = wmf_mem_open(wmf.api, data.data(), long(data.size()));
	if (status != wmf_E_None) {
		set_error(error, wmf_error_string(status));
		return false;
	}

	wmf.opened = true;
	status = wmf_scan(wmf.api, 0, bbox);

	// The metafile measures itself in inches, so scale 1 is as many pixels
	// as the screen puts in one, like every other vector loader here.
	const double dpi = ctx.screen_dpi > 0 ? ctx.screen_dpi : 96;
	unsigned int w = 0, h = 0;
	if (status == wmf_E_None)
		status = wmf_display_size(wmf.api, &w, &h, dpi, dpi);
	if (status != wmf_E_None) {
		set_error(error, wmf_error_string(status));
		return false;
	}
	if (!w || !h || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, _("invalid image dimensions"));
		return false;
	}

	*width = w;
	*height = h;
	return true;
}

/// libwmf reports no length for what it writes to memory, but its sink gets
/// the length of each part, so collect the PNG there.
static int
wmf_png_sink(void *context, char *buffer, int length)
{
	auto png = (vector<uint8_t> *) context;
	if (length <= 0)
		return length;
	try {
		png->insert(png->end(), buffer, buffer + length);
	} catch (const bad_alloc &) {
		return 0;
	}
	return length;
}

bool
WmfRenderClosure::dimensions(
	double scale, uint32_t *width, uint32_t *height, Error *error) const
{
	return render_dimensions(
		width_ * scale, height_ * scale, width, height, error);
}

ImagePtr
WmfRenderClosure::render(const OpenContext &ctx, double scale, Error *error)
{
	uint32_t width = 0, height = 0;
	if (!dimensions(scale, &width, &height, error))
		return nullptr;

	WmfApi wmf;
	wmfD_Rect bbox{};
	uint32_t base_width = 0, base_height = 0;
	if (!wmf_open_and_scan(
			wmf, data_, &bbox, &base_width, &base_height, ctx, error))
		return nullptr;

	vector<uint8_t> png;
	wmf_gd_t *device = WMF_GD_GetData(wmf.api);
	device->bbox = bbox;
	device->width = width;
	device->height = height;
	device->sink.context = &png;
	device->sink.function = wmf_png_sink;

	wmf_error_t status = wmf_play(wmf.api, 0, &bbox);
	if (status != wmf_E_None || png.empty()) {
		set_error(error,
			status == wmf_E_None ? _("GD device produced no image")
								 : wmf_error_string(status));
		return nullptr;
	}
	return load_wuffs(png, ctx, error);
}

ImagePtr
load_libwmf(span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	// What wmf_header_read() does, matching slightly wide.
	if (data.size() < 4 ||
		(le32(data.data()) != 0x9ac6cdd7 && le16(data.data() + 2) != 9))
		return nullptr;

	vector<uint8_t> owned(data.begin(), data.end());
	uint32_t width = 0, height = 0;
	{
		WmfApi wmf;
		wmfD_Rect bbox{};
		if (!wmf_open_and_scan(
				wmf, owned, &bbox, &width, &height, ctx, error))
			return nullptr;
	}

	auto closure =
		make_unique<WmfRenderClosure>(std::move(owned), width, height);
	ImagePtr image = render_now(*closure, ctx, error);
	if (image)
		image->render = std::move(closure);
	return image;
}

}  // namespace dawn
