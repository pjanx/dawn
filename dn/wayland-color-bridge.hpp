//
// wayland-color-bridge.hpp: Wayland color-management-v1 bridge
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#pragma once

#include "color-management-v1-client-protocol.h"

#include <wayland-client.h>

class QWindow;

namespace dn
{

class WaylandColorBridge
{
	QWindow *window_ = nullptr;
	wl_display *display_ = nullptr;
	wl_surface *surface_ = nullptr;
	wl_registry *registry_ = nullptr;
	wp_color_manager_v1 *manager_ = nullptr;
	wp_color_management_surface_v1 *color_surface_ = nullptr;
	wp_color_management_surface_feedback_v1 *feedback_ = nullptr;
	wp_image_description_v1 *pending_description_ = nullptr;

	void request_preferred();
	void description_ready(wp_image_description_v1 *description);

	static void registry_global(void *data, wl_registry *registry,
		uint32_t name, const char *interface, uint32_t);
	static void registry_remove(void *, wl_registry *, uint32_t);
	static void ignore_u32(void *, wp_color_manager_v1 *, uint32_t);
	static void ignore_done(void *, wp_color_manager_v1 *);
	static void preferred_changed(
		void *data, wp_color_management_surface_feedback_v1 *, uint32_t);
	static void description_failed(void *data,
		wp_image_description_v1 *description, uint32_t, const char *message);
	static void description_ready(
		void *data, wp_image_description_v1 *description, uint32_t);

	static const wl_registry_listener kRegistryListener;
	static const wp_color_manager_v1_listener kManagerListener;
	static const wp_color_management_surface_feedback_v1_listener
		kFeedbackListener;
	static const wp_image_description_v1_listener kDescriptionListener;

public:
	WaylandColorBridge() = default;
	~WaylandColorBridge();

	WaylandColorBridge(const WaylandColorBridge &) = delete;
	WaylandColorBridge &operator=(const WaylandColorBridge &) = delete;

	void attach(QWindow *window);
	void detach();
};

}  // namespace dn
