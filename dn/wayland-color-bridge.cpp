//
// wayland-color-bridge.cpp: Wayland color-management-v1 bridge
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//

#include "wayland-color-bridge.hpp"
#include "window.hpp"

#include <libdn/ipc-shm.hpp>

#include <QByteArray>
#include <QCoreApplication>
#include <QEvent>
#include <QGuiApplication>
#include <QMetaObject>
#include <QtGui/qguiapplication_platform.h>
#include <QtLogging>
#include <qpa/qplatformnativeinterface.h>

#include <cstring>
#include <utility>

#include <unistd.h>

using namespace std;

namespace dn
{

static void
registry_note(
	void *data, wl_registry *, uint32_t, const char *interface, uint32_t)
{
	if (interface && strcmp(interface, "zxdg_decoration_manager_v1") == 0)
		*static_cast<bool *>(data) = true;
}

static void
registry_drop(void *, wl_registry *, uint32_t)
{
}

bool
wayland_needs_csd()
{
	if (!qGuiApp ||
		QGuiApplication::platformName() != QStringLiteral("wayland"))
		return false;

	auto *native =
		qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
	if (!native)
		return false;

	wl_display *display = native->display();
	if (!display)
		return false;

	wl_registry *registry = wl_display_get_registry(display);
	if (!registry)
		return false;

	bool has_ssd = false;
	const wl_registry_listener listener = {
		.global = registry_note,
		.global_remove = registry_drop,
	};
	wl_registry_add_listener(registry, &listener, &has_ssd);
	wl_display_roundtrip(display);
	wl_registry_destroy(registry);
	return !has_ssd;
}

void
WaylandColorBridge::request_preferred()
{
	if (!this->feedback_)
		return;
	if (this->pending_info_)
		wp_image_description_info_v1_destroy(this->pending_info_);
	this->pending_info_ = nullptr;
	if (this->preferred_)
		wp_image_description_v1_destroy(this->preferred_);
	this->preferred_ =
		wp_color_management_surface_feedback_v1_get_preferred(this->feedback_);
	wp_image_description_v1_add_listener(
		this->preferred_, &kDescriptionListener, this);
	wl_display_flush(this->display_);
}

// Tagging with the preferred description passes our pixels through untouched,
// which is right while that is an SDR encoding: we have already converted
// to the display profile, and the compositor's idea of the output may well be
// wrong (Mutter's sdr-native assumes EDID primaries).  An HDR output cannot be
// matched by SDR pixels, so there we describe what they really are.
void
WaylandColorBridge::describe_icc()
{
	if (this->icc_pending_)
		wp_image_description_v1_destroy(this->icc_pending_);
	this->icc_pending_ = nullptr;

	// Other sizes are a fatal protocol error, and "32 MB" is read both ways.
	// libwayland duplicates the descriptor while marshalling the request.
	dawn::ipc::SharedMemory file;
	if (!this->icc_.empty() && this->icc_.size() <= 32'000'000)
		file =
			dawn::ipc::SharedMemory::copy(this->icc_.data(), this->icc_.size());
	if (file.handle() < 0) {
		qWarning("Wayland CM identity unavailable: HDR output, and no "
				 "display profile to describe our pixels with");
		this->encoded_icc_ = false;
		drop(this->encoded_);
		set_unmatched(true);
		return;
	}

	auto *creator = wp_color_manager_v1_create_icc_creator(this->manager_);
	wp_image_description_creator_icc_v1_set_icc_file(
		creator, int32_t(file.handle()), 0, uint32_t(this->icc_.size()));
	this->icc_pending_ = wp_image_description_creator_icc_v1_create(creator);
	wp_image_description_v1_add_listener(
		this->icc_pending_, &kDescriptionListener, this);
	wl_display_flush(this->display_);
}

// Our pixels in extended linear light, within the display's own primaries,
// with SDR white at the preferred reference, and nothing above `peak`.
wp_image_description_v1 *
WaylandColorBridge::describe_parametric(uint32_t peak)
{
	auto *creator =
		wp_color_manager_v1_create_parametric_creator(this->manager_);
	wp_image_description_creator_params_v1_set_tf_named(
		creator, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR);
	const auto &p = this->primaries_;
	if (this->set_primaries_)
		wp_image_description_creator_params_v1_set_primaries(
			creator, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
	else
		wp_image_description_creator_params_v1_set_primaries_named(
			creator, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
	// Mutter rejects a reference above the maximum.
	wp_image_description_creator_params_v1_set_luminances(creator,
		this->info_.target_min, peak, min(this->info_.reference, peak));
	auto *description = wp_image_description_creator_params_v1_create(creator);
	wp_image_description_v1_add_listener(
		description, &kDescriptionListener, this);
	wl_display_flush(this->display_);
	return description;
}

// At most one in flight, so that a brightness drag does not queue up
// a description per step: the latest values wait for it.
void
WaylandColorBridge::describe_hdr()
{
	if (this->hdr_pending_) {
		this->hdr_stale_ = true;
		return;
	}

	this->hdr_stale_ = false;
	const uint32_t peak = this->info_.target_max;
	this->hdr_pending_white_ =
		float(min(this->info_.reference, peak)) / float(peak);
	this->hdr_pending_ = describe_parametric(peak);
}

void
WaylandColorBridge::sync_variants(bool replace)
{
	const bool wanted = this->describing_ && this->output_.extended &&
		this->output_.range.hdr && this->info_.target_max > 0;
	if (!wanted || replace) {
		drop(this->sdr_);
		drop(this->hdr_);
		if (this->sdr_pending_)
			wp_image_description_v1_destroy(this->sdr_pending_);
		this->sdr_pending_ = nullptr;
		if (this->hdr_pending_)
			wp_image_description_v1_destroy(this->hdr_pending_);
		this->hdr_pending_ = nullptr;
	}
	if (!wanted)
		return;
	if (!this->sdr_.object && !this->sdr_pending_)
		this->sdr_pending_ = describe_parametric(
			max(min(this->info_.reference, this->info_.target_max), 1u));
	if (!this->hdr_.object && !this->hdr_pending_)
		describe_hdr();
}

// A description the surface has may be destroyed: it keeps its own.
void
WaylandColorBridge::drop(Described &described)
{
	if (described.object)
		wp_image_description_v1_destroy(described.object);
	described = {};
}

// The window renders in sRGB for the frames that go out without our profile.
void
WaylandColorBridge::set_unmatched(bool unmatched)
{
	if (exchange(this->unmatched_, unmatched) != unmatched)
		post_refresh(true);
}

// Not from within a listener, which the profile reload might reenter.
void
WaylandColorBridge::post_refresh(bool heavy)
{
	if (!this->window_)
		return;
	QMetaObject::invokeMethod(
		this->window_,
		[window = this->window_, heavy] {
			if (heavy)
				window->handle_screen_change(window->screen());
			else
				window->refresh_headroom();
		},
		Qt::QueuedConnection);
}

void
WaylandColorBridge::ready(
	Described &slot, wp_image_description_v1 *object, float white)
{
	drop(slot);
	slot = {object, this->next_id_++, white};
	if (this->window_)
		QCoreApplication::postEvent(
			this->window_, new QEvent(QEvent::UpdateRequest));
}

Presentation
WaylandColorBridge::latch(Presentation wanted, float *white)
{
	if (wanted == Presentation::Hdr && this->hdr_.object) {
		this->latched_ = this->hdr_;
		*white = this->hdr_.white;
		return Presentation::Hdr;
	}
	if (wanted != Presentation::Encoded && this->sdr_.object) {
		this->latched_ = this->sdr_;
		*white = this->sdr_.white;
		return Presentation::Sdr;
	}
	this->latched_ = this->encoded_;
	return Presentation::Encoded;
}

// Mesa commits inside vkQueuePresentKHR, on this call stack, with no Qt
// handler in between, so the description goes out with the frame it is for.
void
WaylandColorBridge::apply_latched()
{
	if (!this->color_surface_ || this->latched_.id == this->applied_)
		return;

	this->applied_ = this->latched_.id;
	if (this->latched_.object)
		wp_color_management_surface_v1_set_image_description(
			this->color_surface_, this->latched_.object,
			WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
	else
		wp_color_management_surface_v1_unset_image_description(
			this->color_surface_);
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
WaylandColorBridge::supported_feature(
	void *data, wp_color_manager_v1 *, uint32_t feature)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	switch (feature) {
	case WP_COLOR_MANAGER_V1_FEATURE_ICC_V2_V4:
		self->icc_supported_ = true;
		break;
	case WP_COLOR_MANAGER_V1_FEATURE_PARAMETRIC:
		self->parametric_ = true;
		break;
	case WP_COLOR_MANAGER_V1_FEATURE_SET_PRIMARIES:
		self->set_primaries_ = true;
		break;
	case WP_COLOR_MANAGER_V1_FEATURE_SET_LUMINANCES:
		self->set_luminances_ = true;
		break;
	}
}

void
WaylandColorBridge::supported_tf_named(
	void *data, wp_color_manager_v1 *, uint32_t tf)
{
	if (tf == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_EXT_LINEAR)
		static_cast<WaylandColorBridge *>(data)->ext_linear_ = true;
}

void
WaylandColorBridge::supported_primaries_named(
	void *data, wp_color_manager_v1 *, uint32_t primaries)
{
	if (primaries == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020)
		static_cast<WaylandColorBridge *>(data)->bt2020_ = true;
}

// The features are all in, and they decide what the output can take.
void
WaylandColorBridge::manager_done(void *data, wp_color_manager_v1 *)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	WaylandOutput &output = self->output_;
	output.icc = self->icc_supported_;
	output.own_primaries = self->set_primaries_;
	output.extended = self->parametric_ && self->set_luminances_ &&
		self->ext_linear_ && (self->set_primaries_ || self->bt2020_);
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
	const char *what = "preferred description";
	if (description == self->icc_pending_) {
		what = "display profile";
		self->icc_pending_ = nullptr;
		self->encoded_icc_ = false;
		self->drop(self->encoded_);
		self->set_unmatched(true);
	} else if (description == self->sdr_pending_) {
		what = "SDR description";
		self->sdr_pending_ = nullptr;
	} else if (description == self->hdr_pending_) {
		what = "HDR description";
		self->hdr_pending_ = nullptr;
		if (self->hdr_stale_)
			self->describe_hdr();
	} else {
		self->preferred_ = nullptr;
	}
	qWarning("Wayland CM identity: %s failed: %s", what,
		message ? message : "unknown failure");
	wp_image_description_v1_destroy(description);
}

void
WaylandColorBridge::description_ready(
	void *data, wp_image_description_v1 *description, uint32_t)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	if (description == self->icc_pending_) {
		self->icc_pending_ = nullptr;
		self->encoded_icc_ = true;
		self->ready(self->encoded_, description, 1);
		self->set_unmatched(false);
		qInfo("Wayland CM identity: display profile ready");
	} else if (description == self->sdr_pending_) {
		self->sdr_pending_ = nullptr;
		self->ready(self->sdr_, description, 1);
	} else if (description == self->hdr_pending_) {
		self->hdr_pending_ = nullptr;
		self->ready(self->hdr_, description, self->hdr_pending_white_);
		if (self->hdr_stale_)
			self->describe_hdr();
	} else {
		self->pending_ = {};
		self->pending_info_ =
			wp_image_description_v1_get_information(description);
		wp_image_description_info_v1_add_listener(
			self->pending_info_, &kInfoListener, self);
		wl_display_flush(self->display_);
	}
}

void
WaylandColorBridge::info_icc_file(
	void *, wp_image_description_info_v1 *, int32_t fd, uint32_t)
{
	close(fd);
}

void
WaylandColorBridge::info_ignore_primaries(void *,
	wp_image_description_info_v1 *, int32_t, int32_t, int32_t, int32_t, int32_t,
	int32_t, int32_t, int32_t)
{
}

void
WaylandColorBridge::info_target_primaries(void *data,
	wp_image_description_info_v1 *, int32_t r_x, int32_t r_y, int32_t g_x,
	int32_t g_y, int32_t b_x, int32_t b_y, int32_t w_x, int32_t w_y)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	self->pending_.target_primaries = {r_x, r_y, g_x, g_y, b_x, b_y, w_x, w_y};
	self->pending_.have_target_primaries = true;
}

void
WaylandColorBridge::info_ignore_u32(
	void *, wp_image_description_info_v1 *, uint32_t)
{
}

void
WaylandColorBridge::info_luminances(void *data, wp_image_description_info_v1 *,
	uint32_t min_lum, uint32_t max_lum, uint32_t reference_lum)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	self->pending_.min = min_lum;
	self->pending_.max = max_lum;
	self->pending_.reference = reference_lum;
	self->pending_.parametric = true;
}

void
WaylandColorBridge::info_target_luminance(void *data,
	wp_image_description_info_v1 *, uint32_t min_lum, uint32_t max_lum)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	self->pending_.target_min = min_lum;
	self->pending_.target_max = max_lum;
}

// Everything the information brought takes effect together.  Only the HDR
// display test and the target primaries need the screen profile refreshed;
// luminances just move the headroom, and the HDR variant with it.
void
WaylandColorBridge::info_done(void *data, wp_image_description_info_v1 *info)
{
	auto *self = static_cast<WaylandColorBridge *>(data);
	wp_image_description_info_v1_destroy(info);
	self->pending_info_ = nullptr;

	const Info old = self->info_;
	const Info &now = self->info_ = self->pending_;
	DisplayRange &range = self->output_.range;
	const bool was_hdr = range.hdr;
	range.hdr = now.parametric &&
		(now.max > now.reference || now.target_max > now.reference);
	range.headroom = 1;
	if (now.parametric && now.reference)
		range.headroom =
			float(min(now.max, now.target_max ? now.target_max : now.max)) /
			float(now.reference);
	range.primaries.reset();
	if (now.have_target_primaries) {
		array<double, 8> xy{};
		for (size_t i = 0; i < xy.size(); i++)
			xy[i] = now.target_primaries[i] / 1e6;
		range.primaries = xy;
	}

	if (!range.hdr) {
		// An SDR output takes the preferred description as it is.
		self->encoded_icc_ = false;
		self->ready(self->encoded_, self->preferred_, 1);
		self->preferred_ = nullptr;
		self->set_unmatched(false);
	} else {
		wp_image_description_v1_destroy(self->preferred_);
		self->preferred_ = nullptr;
		if (!self->icc_supported_) {
			// Nothing to tag with, and nothing that would call for sRGB.
			self->encoded_icc_ = false;
			self->drop(self->encoded_);
			self->set_unmatched(false);
		} else if (!self->encoded_icc_ && !self->icc_pending_) {
			self->describe_icc();
		}
	}

	const bool heavy = range.hdr != was_hdr ||
		now.have_target_primaries != old.have_target_primaries ||
		now.target_primaries != old.target_primaries;
	if (heavy)
		self->post_refresh(true);
	else if (now.max != old.max || now.reference != old.reference ||
		now.target_max != old.target_max)
		self->post_refresh(false);

	// Only a variant still wanted is described anew: an ICC description
	// brings no luminances, and zeros are a fatal protocol error.
	self->sync_variants(false);
	if ((self->hdr_.object || self->hdr_pending_) &&
		(now.reference != old.reference || now.target_max != old.target_max ||
			now.target_min != old.target_min))
		self->describe_hdr();
}

const wl_registry_listener WaylandColorBridge::kRegistryListener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

const wp_color_manager_v1_listener WaylandColorBridge::kManagerListener = {
	.supported_intent = ignore_u32,
	.supported_feature = supported_feature,
	.supported_tf_named = supported_tf_named,
	.supported_primaries_named = supported_primaries_named,
	.done = manager_done,
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

const wp_image_description_info_v1_listener WaylandColorBridge::kInfoListener =
	{
		.done = info_done,
		.icc_file = info_icc_file,
		.primaries = info_ignore_primaries,
		.primaries_named = info_ignore_u32,
		.tf_power = info_ignore_u32,
		.tf_named = info_ignore_u32,
		.luminances = info_luminances,
		.target_primaries = info_target_primaries,
		.target_luminance = info_target_luminance,
		.target_max_cll = info_ignore_u32,
		.target_max_fall = info_ignore_u32,
};

WaylandColorBridge::~WaylandColorBridge()
{
	detach();
}

void
WaylandColorBridge::attach(Window *window)
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
	// There is no better way of getting this than the private API.
	// (winId changes meaning across Qt versions.)
	auto *iface = QGuiApplication::platformNativeInterface();
	if (iface) {
		this->surface_ =
			static_cast<wl_surface *>(iface->nativeResourceForWindow(
				QByteArrayLiteral("surface"), window));
	}
	if (!this->display_ || !this->surface_)
		return;
	this->registry_ = wl_display_get_registry(this->display_);
	wl_registry_add_listener(this->registry_, &kRegistryListener, this);
	wl_display_flush(this->display_);
}

void
WaylandColorBridge::detach()
{
	if (this->pending_info_)
		wp_image_description_info_v1_destroy(this->pending_info_);
	for (auto *description : {this->preferred_, this->icc_pending_,
			 this->sdr_pending_, this->hdr_pending_})
		if (description)
			wp_image_description_v1_destroy(description);
	drop(this->encoded_);
	drop(this->sdr_);
	drop(this->hdr_);
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
	this->preferred_ = nullptr;
	this->pending_info_ = nullptr;
	this->icc_pending_ = nullptr;
	this->sdr_pending_ = nullptr;
	this->hdr_pending_ = nullptr;
	this->icc_supported_ = this->parametric_ = this->set_primaries_ =
		this->set_luminances_ = this->ext_linear_ = this->bt2020_ = false;
	this->icc_.clear();
	this->unmatched_ = this->encoded_icc_ = this->hdr_stale_ = false;
	this->describing_ = false;
	this->primaries_ = {};
	this->pending_ = this->info_ = {};
	this->output_ = {};
	this->latched_ = {};
	this->applied_ = 0;
}

void
WaylandColorBridge::set_screen(
	vector<uint8_t> icc, const dawn::ProfileEncoding *extended)
{
	if (icc != this->icc_) {
		this->icc_ = std::move(icc);
		if (this->manager_ && this->icc_supported_ && this->output_.range.hdr)
			describe_icc();
	}

	// Adapted, the colourants sum to D65, so the compositor's RGB-to-XYZ
	// is ours without any adaptation of its own.
	array<int32_t, 8> primaries{0, 0, 0, 0, 0, 0, 312700, 329000};
	if (extended) {
		const dawn::RgbMatrix m = dawn::display_colourants_d65(*extended);
		for (int c = 0; c < 3; c++) {
			const double sum = m[c][0] + m[c][1] + m[c][2];
			primaries[c * 2] = int32_t(lround(m[c][0] / sum * 1e6));
			primaries[c * 2 + 1] = int32_t(lround(m[c][1] / sum * 1e6));
		}
	}
	const bool replace = primaries != this->primaries_;
	this->describing_ = extended;
	this->primaries_ = primaries;
	if (this->manager_)
		sync_variants(replace);
}

}  // namespace dn
