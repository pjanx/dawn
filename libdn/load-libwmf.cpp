//
// load-libwmf.cpp: WMF rendering through libwmf's GD device
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include <dawn-gettext.h>

#include "libdn-loaders.h"
#include "libdn.h"

#include <libwmf/api.h>
#include <libwmf/gd.h>

#include <cmath>
#include <cstring>
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

	ImagePtr render(Cmm *cmm, Profile *target, double scale) override;
	ImagePtr render_internal(
		double scale, Cmm *cmm, Profile *target, Error *error);
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
	uint32_t *width, uint32_t *height, Error *error)
{
	if (data.size() > size_t(LONG_MAX)) {
		set_error(error, _("libwmf: input is too large"));
		return false;
	}

	wmfAPI_Options options{};
	options.function = wmf_gd_function;
	unsigned long flags = WMF_OPT_FUNCTION | WMF_OPT_IGNORE_NONFATAL |
		WMF_OPT_NO_ERROR | WMF_OPT_NO_DEBUG;
	wmf_error_t status = wmf_api_create(&wmf.api, flags, &options);
	if (status != wmf_E_None) {
		set_error(
			error, format_message(_("libwmf: %s"), wmf_error_string(status)));
		return false;
	}

	// The wmf_gd_image device hands out a GD image that libwmf offers no
	// way to destroy; only its encoders dispose of one themselves.
	wmf_gd_t *device = WMF_GD_GetData(wmf.api);
	if (!(device->flags & WMF_GD_SUPPORTS_PNG)) {
		set_error(error, _("libwmf: built without PNG support"));
		return false;
	}
	device->type = wmf_gd_png;
	device->flags |= WMF_GD_OUTPUT_MEMORY;

	status = wmf_mem_open(wmf.api, data.data(), long(data.size()));
	if (status != wmf_E_None) {
		set_error(
			error, format_message(_("libwmf: %s"), wmf_error_string(status)));
		return false;
	}

	wmf.opened = true;
	status = wmf_scan(wmf.api, 0, bbox);
	unsigned int w = 0, h = 0;
	if (status == wmf_E_None)
		status = wmf_display_size(wmf.api, &w, &h, 72, 72);
	if (status != wmf_E_None) {
		set_error(
			error, format_message(_("libwmf: %s"), wmf_error_string(status)));
		return false;
	}
	if (!w || !h || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, _("libwmf: invalid image dimensions"));
		return false;
	}

	*width = w;
	*height = h;
	return true;
}

/// libwmf reports no length for what it writes to memory, though PNG is
/// self-delimiting, so measure its own output by walking the chunks.
static size_t
png_length(const char *data)
{
	auto png = (const uint8_t *) data;
	if (memcmp(png, "\x89PNG\r\n\x1a\n", 8))
		return 0;

	const uint8_t *chunk = png + 8;
	for (bool last = false; !last;) {
		size_t length = size_t(chunk[0]) << 24 | size_t(chunk[1]) << 16 |
			size_t(chunk[2]) << 8 | chunk[3];
		last = !memcmp(chunk + 4, "IEND", 4);
		chunk += 12 + length;
	}
	return size_t(chunk - png);
}

/// Renders at the given dimensions, or at the metafile's own when they are
/// zero, in which case they are reported back.
static ImagePtr
render_wmf(vector<uint8_t> &data, uint32_t *width, uint32_t *height,
	const OpenContext &ctx, Error *error)
{
	WmfApi wmf;
	wmfD_Rect bbox{};
	uint32_t base_width = 0, base_height = 0;
	if (!wmf_open_and_scan(wmf, data, &bbox, &base_width, &base_height, error))
		return nullptr;
	if (!*width || !*height) {
		*width = base_width;
		*height = base_height;
	}

	wmf_gd_t *device = WMF_GD_GetData(wmf.api);
	device->bbox = bbox;
	device->width = *width;
	device->height = *height;

	wmf_error_t status = wmf_play(wmf.api, 0, &bbox);
	size_t length =
		status == wmf_E_None && device->memory ? png_length(device->memory) : 0;
	if (!length) {
		set_error(error,
			format_message(_("libwmf: %s"),
				status == wmf_E_None ? _("GD device produced no image")
									 : wmf_error_string(status)));
		return nullptr;
	}
	return detail::load_wuffs(
		{(const uint8_t *) device->memory, length}, ctx, error);
}

ImagePtr
WmfRenderClosure::render(Cmm *cmm, Profile *target, double scale)
{
	Error ignored;
	return render_internal(scale, cmm, target, &ignored);
}

ImagePtr
WmfRenderClosure::render_internal(
	double scale, Cmm *cmm, Profile *target, Error *error)
{
	double w = ceil(width_ * scale), h = ceil(height_ * scale);
	if (w < 1 || h < 1 || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, _("libwmf: image dimensions overflow"));
		return nullptr;
	}
	OpenContext ctx;
	if (cmm)
		ctx.cmm = cmm->shared_from_this();
	if (target)
		ctx.screen_profile = shared_ptr<Profile>(shared_ptr<Profile>(), target);

	auto width = uint32_t(w), height = uint32_t(h);
	return render_wmf(data_, &width, &height, ctx, error);
}

ImagePtr
detail::load_libwmf(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	vector<uint8_t> owned(data.begin(), data.end());
	uint32_t width = 0, height = 0;
	ImagePtr image = render_wmf(owned, &width, &height, ctx, error);
	if (image)
		image->render =
			make_unique<WmfRenderClosure>(std::move(owned), width, height);
	return image;
}

}  // namespace dawn
