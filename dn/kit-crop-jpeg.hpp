//
// kit-crop-jpeg.hpp: lossless JPEG cropper subapplication
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "kit-chrome.hpp"

namespace dn
{

struct Cropper : Widget {
	Kit &kit_;
	QUrl jpeg_url_;
	std::vector<uint8_t> file_;
	dawn::ImagePtr image_;
	dawn::JpegGrid grid_{};
	dawn::Orientation exif_ = dawn::Orientation::Rotate0;
	// Half-open rectangle in the working JPEG, with its origin on the MCU grid.
	uint32_t left_ = 0, top_ = 0, right_ = 0, bottom_ = 0;
	int zoom_ = 1;
	float pan_x_ = 0, pan_y_ = 0;
	enum class Drag : uint8_t { None, Origin, Corner, Pan };
	Drag drag_ = Drag::None;
	double drag_x_ = 0, drag_y_ = 0;
	std::shared_ptr<dawn::Cmm> cmm_;
	std::shared_ptr<dawn::Profile> screen_profile_;
	Label *region_label_ = nullptr;
	Label *scale_label_ = nullptr;
	Panel *error_ = nullptr;
	Label *error_label_ = nullptr;
	std::string message_;
	bool message_dismissed_ = false;

	explicit Cropper(Kit &kit);
	void open(const QUrl &url);
	bool load_working(const std::vector<uint8_t> &data);
	void reset_region();
	void zoom_at(int zoom, float x, float y);
	// The displayed image's top-left, snapped exactly as the renderer is.
	void origin(double *x, double *y) const;
	void turn(Action action);
	QString save(const QString &input);
	bool apply(Action action);
	bool enabled(Action action) const;

	Size measure_content(Kit &, int max_w, int max_h) override;
	void arrange_content(Kit &, Rect alloc) override;
	void paint(Kit &kit) const override;
	void update(Kit &kit) override;
	void screen_changed(
		const ScreenState &state, bool changed, bool force_reload) override;
	bool focusable() const override { return shown(); }
	Qt::CursorShape cursor() const override;
	bool press(Kit &kit, float x, float y, Qt::MouseButton button) override;
	bool release(Kit &, float, float, Qt::MouseButton button) override;
	bool motion(Kit &kit, float x, float y) override;
	bool scroll(Kit &, float x, float y, int delta) override;
	bool pan(Kit &, float, float, float dx, float dy) override;
	bool key(Kit &, const Key &ev) override;
};

std::unique_ptr<Page> make_crop_jpeg_page(
	Kit &kit, const HostActions &host, Cropper **out);

}  // namespace dn
