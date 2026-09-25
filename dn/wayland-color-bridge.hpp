//
// wayland-color-bridge.hpp: Wayland color-management-v1 bridge
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "color-management-v1-client-protocol.h"
#include "display-profile.hpp"
#include "renderer.hpp"

#include <wayland-client.h>

#include <array>
#include <cstdint>
#include <vector>

namespace dn
{

class Window;

/// What the compositor tells of the output, and of itself.
struct WaylandOutput {
	DisplayRange range;
	/// Parametric descriptions with luminances, usable primaries,
	/// and the extended linear transfer function.
	bool extended = false;
	/// ICC descriptions (icc_v2_v4), without which a surface can only be
	/// described parametrically, or not at all.
	bool icc = false;
	/// Parametric descriptions take our own primaries, not BT.2020's.
	bool own_primaries = false;
};

class WaylandColorBridge
{
	// A description the compositor has made ready, kept for switching to.
	struct Described {
		wp_image_description_v1 *object = nullptr;
		uint64_t id = 0;  ///< Zero for none, which is no description
		float white = 1;
	};
	// The preferred description's information, as its events bring it.
	struct Info {
		uint32_t min = 0, max = 0, reference = 0;
		uint32_t target_min = 0, target_max = 0;
		std::array<int32_t, 8> target_primaries{};
		bool parametric = false;
		bool have_target_primaries = false;
	};

	Window *window_ = nullptr;
	wl_display *display_ = nullptr;
	wl_surface *surface_ = nullptr;
	wl_registry *registry_ = nullptr;
	wp_color_manager_v1 *manager_ = nullptr;
	wp_color_management_surface_v1 *color_surface_ = nullptr;
	wp_color_management_surface_feedback_v1 *feedback_ = nullptr;
	wp_image_description_v1 *preferred_ = nullptr;
	wp_image_description_info_v1 *pending_info_ = nullptr;
	std::vector<uint8_t> icc_;
	bool icc_supported_ = false;
	bool parametric_ = false;
	bool set_primaries_ = false;
	bool set_luminances_ = false;
	bool ext_linear_ = false;
	bool bt2020_ = false;
	bool unmatched_ = false;  ///< The ICC description failed

	Info pending_, info_;
	WaylandOutput output_;

	// Encoded frames go out under the ICC description on an HDR output,
	// the preferred description otherwise, or none.  Extended frames take
	// a parametric variant: SDR declares nothing above SDR white.
	Described encoded_, sdr_, hdr_;
	wp_image_description_v1 *icc_pending_ = nullptr;
	wp_image_description_v1 *sdr_pending_ = nullptr;
	wp_image_description_v1 *hdr_pending_ = nullptr;
	float hdr_pending_white_ = 1;
	bool hdr_stale_ = false;  ///< Newer luminances await hdr_pending_
	bool encoded_icc_ = false;
	uint64_t next_id_ = 1;

	// The screen profile's colourants, when the window can present extended.
	bool describing_ = false;
	std::array<int32_t, 8> primaries_{};

	Described latched_;
	uint64_t applied_ = 0;

	void request_preferred();
	void describe_icc();
	wp_image_description_v1 *describe_parametric(uint32_t peak);
	void describe_hdr();
	void sync_variants(bool replace);
	void drop(Described &described);
	void set_unmatched(bool unmatched);
	void post_refresh(bool heavy);
	void ready(Described &slot, wp_image_description_v1 *object, float white);

	static void registry_global(void *data, wl_registry *registry,
		uint32_t name, const char *interface, uint32_t);
	static void registry_remove(void *, wl_registry *, uint32_t);
	static void ignore_u32(void *, wp_color_manager_v1 *, uint32_t);
	static void supported_feature(
		void *data, wp_color_manager_v1 *, uint32_t feature);
	static void supported_tf_named(
		void *data, wp_color_manager_v1 *, uint32_t tf);
	static void supported_primaries_named(
		void *data, wp_color_manager_v1 *, uint32_t primaries);
	static void manager_done(void *data, wp_color_manager_v1 *);
	static void preferred_changed(
		void *data, wp_color_management_surface_feedback_v1 *, uint32_t);
	static void description_failed(void *data,
		wp_image_description_v1 *description, uint32_t, const char *message);
	static void description_ready(
		void *data, wp_image_description_v1 *description, uint32_t);
	static void info_icc_file(
		void *, wp_image_description_info_v1 *, int32_t fd, uint32_t);
	static void info_ignore_primaries(void *, wp_image_description_info_v1 *,
		int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t, int32_t);
	static void info_target_primaries(void *data,
		wp_image_description_info_v1 *, int32_t r_x, int32_t r_y, int32_t g_x,
		int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y);
	static void info_ignore_u32(
		void *, wp_image_description_info_v1 *, uint32_t);
	static void info_luminances(void *data, wp_image_description_info_v1 *,
		uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum);
	static void info_target_luminance(void *data,
		wp_image_description_info_v1 *, uint32_t min_lum, uint32_t max_lum);
	static void info_done(void *data, wp_image_description_info_v1 *info);

	static const wl_registry_listener kRegistryListener;
	static const wp_color_manager_v1_listener kManagerListener;
	static const wp_color_management_surface_feedback_v1_listener
		kFeedbackListener;
	static const wp_image_description_v1_listener kDescriptionListener;
	static const wp_image_description_info_v1_listener kInfoListener;

public:
	WaylandColorBridge() = default;
	~WaylandColorBridge();

	WaylandColorBridge(const WaylandColorBridge &) = delete;
	WaylandColorBridge &operator=(const WaylandColorBridge &) = delete;

	void attach(Window *window);
	void detach();

	/// The profile our pixels are encoded in, and its colourants when the
	/// window can present extended range, in which case they get
	/// parametric descriptions too.
	void set_screen(
		std::vector<uint8_t> icc, const dawn::ProfileEncoding *extended);
	/// The compositor reads our pixels as sRGB, whatever set_screen() said.
	[[nodiscard]] bool unmatched() const { return this->unmatched_; }
	[[nodiscard]] const WaylandOutput &output() const { return this->output_; }

	/// Picks the description a frame goes out under, among those ready,
	/// and returns the presentation that allows, with its SDR white.
	Presentation latch(Presentation wanted, float *white);
	/// Applies the latched description, right before the frame's present.
	void apply_latched();
	/// Which description the last frame went out under, marked with N_().
	[[nodiscard]] const char *latched_name() const;
};

// True when the compositor does not advertise zxdg_decoration_manager_v1
// (current Mutter). Never binds the global; Qt negotiates decoration itself.
[[nodiscard]] bool wayland_needs_csd();

}  // namespace dn
