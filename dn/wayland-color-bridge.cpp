//
// wayland-color-bridge.cpp: Wayland color-management-v1 bridge
//
// Copyright The dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "wayland-color-bridge.hpp"

#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QWindow>
#include <QtGui/qguiapplication_platform.h>

#include <cstdio>
#include <cstring>

using namespace std;

namespace dn
{

void
WaylandColorBridge::request_preferred()
{
	if (!this->feedback_)
		return;
	if (this->pending_description_)
		wp_image_description_v1_destroy(this->pending_description_);
	this->pending_description_ =
		wp_color_management_surface_feedback_v1_get_preferred(this->feedback_);
	wp_image_description_v1_add_listener(
		this->pending_description_, &kDescriptionListener, this);
	wl_display_flush(this->display_);
}

void
WaylandColorBridge::description_ready(wp_image_description_v1 *description)
{
	if (description != this->pending_description_)
		return;
	wp_color_management_surface_v1_set_image_description(this->color_surface_,
		description, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	wp_image_description_v1_destroy(description);
	this->pending_description_ = nullptr;
	printf("Wayland CM identity: preferred description applied\n");
	if (this->window_)
		QCoreApplication::postEvent(
			this->window_, new QEvent(QEvent::UpdateRequest));
}

void
WaylandColorBridge::registry_global(void *data, wl_registry *registry,
	uint32_t name, const char *interface, uint32_t)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	if (self->manager_ ||
		strcmp(interface, wp_color_manager_v1_interface.name) != 0)
		return;
	self->manager_ = static_cast<wp_color_manager_v1 *>(
		wl_registry_bind(registry, name, &wp_color_manager_v1_interface, 1));
	wp_color_manager_v1_add_listener(self->manager_, &kManagerListener, self);
	self->color_surface_ =
		wp_color_manager_v1_get_surface(self->manager_, self->surface_);
	self->feedback_ = wp_color_manager_v1_get_surface_feedback(
		self->manager_, self->surface_);
	wp_color_management_surface_feedback_v1_add_listener(
		self->feedback_, &kFeedbackListener, self);
	self->request_preferred();
}

void
WaylandColorBridge::registry_remove(void *, wl_registry *, uint32_t)
{
}

void
WaylandColorBridge::ignore_u32(void *, wp_color_manager_v1 *, uint32_t)
{
}

void
WaylandColorBridge::ignore_done(void *, wp_color_manager_v1 *)
{
}

void
WaylandColorBridge::preferred_changed(
	void *data, wp_color_management_surface_feedback_v1 *, uint32_t)
{
	static_cast<WaylandColorBridge *>(data)->request_preferred();
}

void
WaylandColorBridge::description_failed(void *data,
	wp_image_description_v1 *description, uint32_t, const char *message)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	fprintf(stderr, "Wayland CM identity: preferred description failed: %s\n",
		message ? message : "unknown failure");
	if (self->pending_description_ == description)
		self->pending_description_ = nullptr;
	wp_image_description_v1_destroy(description);
}

void
WaylandColorBridge::description_ready(
	void *data, wp_image_description_v1 *description, uint32_t)
{
	static_cast<WaylandColorBridge *>(data)->description_ready(description);
}

const wl_registry_listener WaylandColorBridge::kRegistryListener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

const wp_color_manager_v1_listener WaylandColorBridge::kManagerListener = {
	.supported_intent = ignore_u32,
	.supported_feature = ignore_u32,
	.supported_tf_named = ignore_u32,
	.supported_primaries_named = ignore_u32,
	.done = ignore_done,
};

const wp_color_management_surface_feedback_v1_listener
	WaylandColorBridge::kFeedbackListener = {
		.preferred_changed = preferred_changed,
};

const wp_image_description_v1_listener
	WaylandColorBridge::kDescriptionListener = {
		.failed = description_failed,
		.ready = description_ready,
};

WaylandColorBridge::~WaylandColorBridge()
{
	detach();
}

void
WaylandColorBridge::attach(QWindow *window)
{
	detach();
	if (!window || QGuiApplication::platformName() != QStringLiteral("wayland"))
		return;
	auto *native =
		qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
	if (!native)
		return;
	this->window_ = window;
	this->display_ = native->display();
	this->surface_ = reinterpret_cast<wl_surface *>(window->winId());
	if (!this->display_ || !this->surface_)
		return;
	this->registry_ = wl_display_get_registry(this->display_);
	wl_registry_add_listener(this->registry_, &kRegistryListener, this);
	wl_display_flush(this->display_);
}

void
WaylandColorBridge::detach()
{
	if (this->pending_description_)
		wp_image_description_v1_destroy(this->pending_description_);
	if (this->feedback_)
		wp_color_management_surface_feedback_v1_destroy(this->feedback_);
	if (this->color_surface_)
		wp_color_management_surface_v1_destroy(this->color_surface_);
	if (this->manager_)
		wp_color_manager_v1_destroy(this->manager_);
	if (this->registry_)
		wl_registry_destroy(this->registry_);
	this->window_ = nullptr;
	this->display_ = nullptr;
	this->surface_ = nullptr;
	this->registry_ = nullptr;
	this->manager_ = nullptr;
	this->color_surface_ = nullptr;
	this->feedback_ = nullptr;
	this->pending_description_ = nullptr;
}

}  // namespace dn
