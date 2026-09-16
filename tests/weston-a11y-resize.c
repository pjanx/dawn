//
// weston-a11y-resize.c: compositor window resize for the AT-SPI harness
//
// Copyright The Dawn Authors
// SPDX-License-Identifier: MPL-2.0
//
// The accessibility test must shrink the window to force toolbar overflow.
// Qt's AT-SPI Component.SetSize is a stub, and a production Value interface
// on the frame is not a resize API.  This module is window-management: it
// sends xdg_toplevel configure through libweston-desktop.
//
// Commands on $XDG_RUNTIME_DIR/dn-a11y-resize (SOCK_DGRAM):
//   shrink   configure a width that packs the toolbar into overflow
//   wide     configure at least 1600 px, enough to unpack it
//   restore  configure the size snapshotted before the first change
//

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <libweston/desktop.h>
#include <libweston/libweston.h>
#include <wayland-server.h>
#include <weston.h>

enum {
	kNarrowWidth = 400,
	kWideWidth = 1600,
};

struct resize_mod {
	struct weston_compositor *ec;
	int fd;
	struct wl_event_source *source;
	struct sockaddr_un addr;
	int32_t saved_w;
	int32_t saved_h;
	bool saved;
};

static struct weston_desktop_surface *
first_toplevel(struct weston_compositor *ec)
{
	struct weston_layer *layer;

	wl_list_for_each (layer, &ec->layer_list, link) {
		struct weston_view *view;

		wl_list_for_each (view, &layer->view_list.link, layer_link.link) {
			struct weston_desktop_surface *ds;

			if (!view->surface ||
				!weston_surface_is_desktop_surface(view->surface))
				continue;
			ds = weston_surface_get_desktop_surface(view->surface);
			if (ds && !weston_desktop_surface_get_parent(ds))
				return ds;
		}
	}
	return NULL;
}

static void
snapshot(struct resize_mod *mod, struct weston_desktop_surface *ds)
{
	struct weston_surface *surface;
	struct weston_geometry geo;
	int32_t w;
	int32_t h;

	if (mod->saved)
		return;

	surface = weston_desktop_surface_get_surface(ds);
	geo = weston_desktop_surface_get_geometry(ds);
	w = surface->width_from_buffer;
	h = surface->height_from_buffer;
	if (w < geo.width) {
		w = geo.width;
		h = geo.height;
	}
	if (w < surface->width) {
		w = surface->width;
		h = surface->height;
	}
	if (w < kNarrowWidth || h < kNarrowWidth)
		return;
	mod->saved_w = w;
	mod->saved_h = h;
	mod->saved = true;
}

static void
configure(struct weston_desktop_surface *ds, int32_t width, int32_t height)
{
	weston_desktop_surface_set_size(ds, width, height);
}

static void
apply(struct resize_mod *mod, const char *cmd)
{
	struct weston_desktop_surface *ds = first_toplevel(mod->ec);
	int32_t width;
	int32_t height;

	if (!ds)
		return;

	snapshot(mod, ds);
	if (!mod->saved)
		return;

	if (strcmp(cmd, "restore") == 0) {
		configure(ds, mod->saved_w, mod->saved_h);
		return;
	}
	height = mod->saved_h;
	if (strcmp(cmd, "shrink") == 0)
		width = kNarrowWidth;
	else if (strcmp(cmd, "wide") == 0)
		width = mod->saved_w > kWideWidth ? mod->saved_w : kWideWidth;
	else
		return;
	configure(ds, width, height);
}

static int
on_socket(int fd, uint32_t mask, void *data)
{
	struct resize_mod *mod = data;
	char buf[64];
	ssize_t n;

	if (!(mask & WL_EVENT_READABLE))
		return 0;

	n = recv(fd, buf, sizeof buf - 1, 0);
	if (n <= 0)
		return 0;
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		n--;

	buf[n] = '\0';
	apply(mod, buf);
	return 0;
}

WL_EXPORT int
wet_module_init(struct weston_compositor *ec, int *argc, char *argv[])
{
	struct resize_mod *mod;
	struct wl_event_loop *loop;
	const char *runtime;
	int n;

	(void) argc;
	(void) argv;

	runtime = getenv("XDG_RUNTIME_DIR");
	if (!runtime || !ec || !ec->wl_display)
		return -1;

	mod = calloc(1, sizeof *mod);
	if (!mod)
		return -1;

	mod->ec = ec;
	mod->fd = -1;
	mod->addr.sun_family = AF_UNIX;
	n = snprintf(mod->addr.sun_path, sizeof mod->addr.sun_path,
		"%s/dn-a11y-resize", runtime);
	if (n < 0 || n >= (int) sizeof mod->addr.sun_path) {
		free(mod);
		return -1;
	}

	mod->fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (mod->fd < 0) {
		free(mod);
		return -1;
	}

	unlink(mod->addr.sun_path);
	if (bind(mod->fd, (struct sockaddr *) &mod->addr, sizeof mod->addr) < 0) {
		close(mod->fd);
		free(mod);
		return -1;
	}

	loop = wl_display_get_event_loop(ec->wl_display);
	mod->source =
		wl_event_loop_add_fd(loop, mod->fd, WL_EVENT_READABLE, on_socket, mod);
	if (!mod->source) {
		unlink(mod->addr.sun_path);
		close(mod->fd);
		free(mod);
		return -1;
	}
	return 0;
}
