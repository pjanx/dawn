//
// load-resvg.cpp: SVG image loading via resvg (no GLib dependency)
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "libdn-loaders.h"
#include "libdn.h"

#include <resvg.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace std;

namespace dawn
{

namespace
{

namespace fs = filesystem;

// resvg documents share the project pixmap dimension limit.
constexpr double kMaxDimension = double(dawn::kMaxDimension);

const char *
resvg_error_string(int32_t err)
{
	switch (err) {
	case RESVG_ERROR_NOT_AN_UTF8_STR:
		return "not a UTF-8 string";
	case RESVG_ERROR_FILE_OPEN_FAILED:
		return "I/O failure";
	case RESVG_ERROR_MALFORMED_GZIP:
		return "malformed gzip";
	case RESVG_ERROR_ELEMENTS_LIMIT_REACHED:
		return "element limit reached";
	case RESVG_ERROR_INVALID_SIZE:
		return "invalid or unspecified image size";
	case RESVG_ERROR_PARSING_FAILED:
		return "parsing failed";
	default:
		return "general failure";
	}
}

class ResvgRenderClosure : public RenderClosure
{
	resvg_render_tree *tree_;
	double width_;   ///< Normal width at scale == 1
	double height_;  ///< Normal height at scale == 1

public:
	ResvgRenderClosure(resvg_render_tree *tree, double width, double height)
		: tree_(tree), width_(width), height_(height)
	{
	}

	~ResvgRenderClosure() override { resvg_tree_destroy(tree_); }

	ResvgRenderClosure(const ResvgRenderClosure &) = delete;
	ResvgRenderClosure &operator=(const ResvgRenderClosure &) = delete;

	ImagePtr render(Cmm *cmm, Profile *target, double scale) override;
	ImagePtr render_internal(
		double scale, Cmm *cmm, Profile *target, Error *error);
};

ImagePtr
ResvgRenderClosure::render(Cmm *cmm, Profile *target, double scale)
{
	Error ignored;
	return render_internal(scale, cmm, target, &ignored);
}

ImagePtr
ResvgRenderClosure::render_internal(
	double scale, Cmm *cmm, Profile *target, Error *error)
{
	double w = ceil(width_ * scale), h = ceil(height_ * scale);
	if (w < 1 || h < 1 || w > kMaxDimension || h > kMaxDimension) {
		set_error(error, "image dimensions overflow");
		return nullptr;
	}

	auto uw = uint32_t(w), uh = uint32_t(h);
	vector<uint8_t> pixmap(size_t(uw) * uh * 4, 0);

	resvg_transform transform = resvg_transform_identity();
	transform.a = float(scale);
	transform.d = float(scale);
	resvg_render(tree_, transform, uw, uh, (char *) pixmap.data());

	ImagePtr image = image_new(uw, uh);
	if (!image) {
		set_error(error, "image allocation failure");
		return nullptr;
	}

	// resvg_render() always produces premultiplied RGBA8888; pack it
	// into working-format BGRA16, leaving the association untouched.
	pack_rgba8_to_bgra16(*image, pixmap.data(), size_t(uw) * 4);

	OpenContext finish_ctx;
	if (cmm)
		finish_ctx.cmm = cmm->shared_from_this();
	if (target)
		finish_ctx.screen_profile =
			shared_ptr<Profile>(shared_ptr<Profile>(), target);
	ensure_working_premul(*image, finish_ctx, nullptr, /*input_premul=*/true);
	return image;
}

}  // namespace

ImagePtr
detail::load_resvg(
	span<const uint8_t> data, const OpenContext &ctx, Error *error)
{
	resvg_options *opt = resvg_options_create();
	resvg_options_load_system_fonts(opt);

	string path = uri_to_path(ctx.uri);
	if (!path.empty()) {
		fs::path parent = fs::path(path).parent_path();
		if (!parent.empty())
			resvg_options_set_resources_dir(opt, parent.string().c_str());
	}
	if (ctx.screen_dpi)
		resvg_options_set_dpi(opt, float(ctx.screen_dpi));

	resvg_render_tree *tree = nullptr;
	int32_t err = resvg_parse_tree_from_data(
		(const char *) data.data(), data.size(), opt, &tree);
	resvg_options_destroy(opt);
	if (err != RESVG_OK) {
		set_error(error, resvg_error_string(err));
		return nullptr;
	}

	resvg_size size = resvg_get_image_size(tree);
	auto closure =
		make_unique<ResvgRenderClosure>(tree, size.width, size.height);

	ImagePtr image = closure->render_internal(
		1., ctx.cmm.get(), ctx.screen_profile.get(), error);
	if (!image)
		return nullptr;

	image->render = std::move(closure);
	return image;
}

}  // namespace dawn
