//
// kit-viewer.hpp: image view (document, overlay chrome, view input)
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit-chrome.hpp"
#include "types.hpp"

#include <libdn.h>

#include <QString>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace dn
{

struct CieDiagram;
struct Kit;
struct Page;

constexpr float kInfoSidebarPts = 240.f;

struct Viewer : Widget {
	struct CachedOpen {
		std::string path;
		dawn::ImagePtr image;
		std::string message;
	};
	struct Worker;
	struct RestoreView {
		bool valid = false;
		float scale = 1.f;
		float pan_x = 0.f;
		float pan_y = 0.f;
		dawn::Orientation orientation = dawn::Orientation::Rotate0;
		float angle = 0.f;
		bool view_locked = true;
	};

	Kit &kit_;
	Page *page_ = nullptr;
	Panel *error_ = nullptr;
	Label *error_label_ = nullptr;
	Label *scale_label_ = nullptr;
	ScrollColumn *info_ = nullptr;
	Label *name_label_ = nullptr;
	Label *width_label_ = nullptr;
	Label *height_label_ = nullptr;
	Button *exiftool_button_ = nullptr;
	CieDiagram *cie_ = nullptr;
	const dawn::Image *info_text_src_ = nullptr;
	QString scale_text_;
	std::string message_;
	bool message_dismissed_ = false;
	bool opening_ = false;
	bool open_done_ = false;
	bool detached_ = false;

	// The URL identifies what is on screen; the loader below works on the
	// local paths derived from it.
	QUrl url_;
	std::string previous_path_;
	std::string next_path_;
	std::string basename_;
	std::shared_ptr<dawn::Cmm> cmm_;
	std::shared_ptr<dawn::Profile> screen_profile_;
	bool screen_profile_fallback_ = true;
	dawn::ImagePtr image_;
	dawn::ImagePtr current_;
	dawn::ImagePtr frame_;
	dawn::ImagePtr page_scaled_;
	bool playing_ = false;
	std::chrono::steady_clock::time_point frame_at_{};
	uint64_t remaining_loops_ = 0;
	float vector_scale_ = 0;
	uint32_t image_width_ = 0;
	uint32_t image_height_ = 0;
	float scale_ = 1.f;
	float pan_x_ = 0.f;
	float pan_y_ = 0.f;
	float angle_ = 0.f;
	bool scale_to_fit_ = true;
	bool view_locked_ = true;
	bool fixate_ = false;
	bool enable_cms_ = true;
	bool filter_ = true;
	bool checkerboard_ = true;
	dawn::Orientation orientation_ = dawn::Orientation::Rotate0;
	enum class Drag : uint8_t { None, Pan, Zoom, Rotate };
	Drag drag_ = Drag::None;
	double drag_x_ = 0;
	double drag_y_ = 0;
	float drag_pivot_x_ = 0;
	float drag_pivot_y_ = 0;
	float drag_angle_ = 0;
	std::unique_ptr<Worker> worker_;
	std::vector<CachedOpen> open_cache_;
	uint64_t load_epoch_ = 1;
	uint64_t open_gen_ = 0;
	uint64_t scale_gen_ = 0;
	bool scale_job_pending_ = false;
	float scale_job_target_ = 0;
	bool scale_failed_ = false;
	float scale_failed_target_ = 0;
	RestoreView restore_view_{};

	explicit Viewer(Kit &kit);
	~Viewer() override;

	void measure(Kit &kit, int max_w, int max_h) override;
	void arrange(Kit &kit, Rect alloc) override;
	void paint(Kit &kit) const override;
	void prepare(Kit &kit) override;
	[[nodiscard]] bool focusable() const override;

	void init();
	void destroy();
	void set_host(float width_pts, float height_pts, float dpr);
	void open(const QUrl &url);
	void set_preload_urls(const QUrl &previous, const QUrl &next);
	void cancel_loads();
	[[nodiscard]] bool has_view() const;
	[[nodiscard]] bool consume_open_done();
	void set_screen_profile(std::shared_ptr<dawn::Cmm> cmm,
		std::shared_ptr<dawn::Profile> profile, bool fallback = true);
	void present(Page &ui);
	[[nodiscard]] int wake_ms() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool scroll(Kit &kit, float x, float y, int delta) override;
	bool pan(Kit &kit, float x, float y, float dx, float dy) override;
	bool gesture(Kit &kit, float x, float y, float scale_factor,
		float angle_delta) override;
	bool key(Kit &kit, const Key &ev) override;
	bool double_click(Kit &kit, float x, float y, Qt::MouseButton button,
		unsigned mods) override;
};

std::unique_ptr<Page> make_viewer_page(
	Kit &kit, const HostActions &host, Viewer **out);

}  // namespace dn
