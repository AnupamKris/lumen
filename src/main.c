#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_control_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_export_dmabuf_v1.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_single_pixel_buffer_v1.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_output_v1.h>
#include <wlr/util/log.h>
#include <scenefx/render/fx_renderer/fx_renderer.h>
#include <scenefx/types/fx/blur_data.h>
#include "lumen.h"

static void new_xdg_toplevel(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_xdg_toplevel);
	view_init_toplevel(s, data);
}

static void activation_request(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, activation_request);
	struct wlr_xdg_activation_v1_request_activate_event *ev = data;
	struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(ev->surface);
	if (!t || !t->base->data) return;
	struct lumen_view *v = t->base->data;
	if (!v->mapped) return;
	if (v->ws != s->ws) workspace_switch(s, v->ws);
	if (v->minimized) view_set_minimized(v, false);
	view_focus(v);
}

static void layout_change(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, layout_change);
	struct lumen_output *o;
	wl_list_for_each(o, &s->outputs, link) arrange_layers(o);
	arrange_all(s);
	lock_update_backdrop(s);
}

/* idle-inhibit (video players, presentations) pauses swayidle timers */
struct inhibitor { struct lumen_server *s; struct wl_listener destroy; };

static void inhibitor_destroy(struct wl_listener *l, void *data) {
	struct inhibitor *i = wl_container_of(l, i, destroy);
	struct lumen_server *s = i->s;
	wl_list_remove(&i->destroy.link);
	free(i);
	if (--s->n_inhibitors == 0) wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, false);
}

static void new_inhibitor(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_inhibitor);
	struct wlr_idle_inhibitor_v1 *wi = data;
	struct inhibitor *i = calloc(1, sizeof(*i));
	i->s = s;
	i->destroy.notify = inhibitor_destroy;
	wl_signal_add(&wi->events.destroy, &i->destroy);
	if (s->n_inhibitors++ == 0) wlr_idle_notifier_v1_set_inhibited(s->idle_notifier, true);
}

static void sigchld(int sig) { (void)sig; }

int main(int argc, char **argv) {
	const char *startup = NULL;
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--msg") && i + 1 < argc) return ipc_client_main(argv[i + 1]);
		if (!strcmp(argv[i], "-s") && i + 1 < argc) startup = argv[++i];
		else if (!strcmp(argv[i], "-v")) wlr_log_init(WLR_DEBUG, NULL);
		else if (!strcmp(argv[i], "-h") || !strcmp(argv[i], "--help")) {
			printf("usage: lumen [-v] [-s startup-cmd] | lumen --msg \"<action>\"\n");
			return 0;
		}
	}
	if (wlr_log_get_verbosity() < WLR_INFO) wlr_log_init(WLR_INFO, NULL);
	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, sigchld); /* children are double-forked; nothing to reap */

	static struct lumen_server s = {0};
	config_defaults(&s.cfg);
	config_load(&s.cfg, config_path());
	for (int i = 0; i < MAX_WORKSPACES; i++) {
		s.ws_tiling[i] = s.cfg.default_tiling;
		s.ws_master[i] = s.cfg.master_ratio;
	}

	s.display = wl_display_create();
	s.loop = wl_display_get_event_loop(s.display);
	s.backend = wlr_backend_autocreate(s.loop, &s.session);
	if (!s.backend) { wlr_log(WLR_ERROR, "failed to create backend"); return 1; }

	/* SceneFX renderer (GLES2 with rounded corners, shadows, blur). */
	s.renderer = fx_renderer_create(s.backend);
	if (!s.renderer) { wlr_log(WLR_ERROR, "failed to create fx renderer"); return 1; }
	wlr_renderer_init_wl_shm(s.renderer, s.display);
	if (wlr_renderer_get_texture_formats(s.renderer, WLR_BUFFER_CAP_DMABUF)) {
		struct wlr_linux_dmabuf_v1 *dmabuf =
			wlr_linux_dmabuf_v1_create_with_renderer(s.display, 4, s.renderer);
		s.scene = wlr_scene_create();
		wlr_scene_set_linux_dmabuf_v1(s.scene, dmabuf);
	} else {
		s.scene = wlr_scene_create();
	}
	s.allocator = wlr_allocator_autocreate(s.backend, s.renderer);

	struct wlr_compositor *compositor = wlr_compositor_create(s.display, 6, s.renderer);
	wlr_subcompositor_create(s.display);
	wlr_data_device_manager_create(s.display);
	wlr_primary_selection_v1_device_manager_create(s.display);
	wlr_data_control_manager_v1_create(s.display);
	wlr_viewporter_create(s.display);
	wlr_single_pixel_buffer_manager_v1_create(s.display);
	wlr_fractional_scale_manager_v1_create(s.display, 1);
	wlr_presentation_create(s.display, s.backend);
	wlr_export_dmabuf_manager_v1_create(s.display);
	wlr_screencopy_manager_v1_create(s.display);
	wlr_gamma_control_manager_v1_create(s.display);
	s.idle_notifier = wlr_idle_notifier_v1_create(s.display);
	s.idle_inhibit = wlr_idle_inhibit_v1_create(s.display);
	s.new_inhibitor.notify = new_inhibitor;
	wl_signal_add(&s.idle_inhibit->events.new_inhibitor, &s.new_inhibitor);
	(void)compositor;

	s.output_layout = wlr_output_layout_create(s.display);
	wlr_xdg_output_manager_v1_create(s.display, s.output_layout);
	s.layout_change.notify = layout_change;
	wl_signal_add(&s.output_layout->events.change, &s.layout_change);

	wl_list_init(&s.outputs);
	wl_list_init(&s.views);
	wl_list_init(&s.keyboards);

	/* Scene layers */
	s.layer_bg = wlr_scene_tree_create(&s.scene->tree);
	s.blur_node = wlr_scene_optimized_blur_create(&s.scene->tree, 0, 0);
	s.layer_bottom = wlr_scene_tree_create(&s.scene->tree);
	s.layer_views = wlr_scene_tree_create(&s.scene->tree);
	s.layer_top = wlr_scene_tree_create(&s.scene->tree);
	s.layer_overlay = wlr_scene_tree_create(&s.scene->tree);
	s.layer_drag = wlr_scene_tree_create(&s.scene->tree);
	lock_init(&s); /* lock tree sits above everything */
	struct blur_data blur = blur_data_get_default();
	blur.num_passes = 2;
	blur.radius = 6;
	wlr_scene_set_blur_data(s.scene, blur);
	wlr_scene_node_set_enabled(&s.blur_node->node, s.cfg.blur);

	s.scene_layout = wlr_scene_attach_output_layout(s.scene, s.output_layout);
	s.new_output.notify = output_new;
	wl_signal_add(&s.backend->events.new_output, &s.new_output);

	s.xdg_shell = wlr_xdg_shell_create(s.display, 6);
	s.new_xdg_toplevel.notify = new_xdg_toplevel;
	wl_signal_add(&s.xdg_shell->events.new_toplevel, &s.new_xdg_toplevel);
	s.new_xdg_popup.notify = xdg_popup_new;
	wl_signal_add(&s.xdg_shell->events.new_popup, &s.new_xdg_popup);

	s.layer_shell = wlr_layer_shell_v1_create(s.display, 4);
	s.new_layer_surface.notify = layer_surface_new;
	wl_signal_add(&s.layer_shell->events.new_surface, &s.new_layer_surface);

	/* Ask clients for server-side decorations: GTK3 (XFCE apps) uses the KDE
	 * protocol, Qt/foot/Firefox the xdg one. GTK4/libadwaita stays CSD. */
	struct wlr_server_decoration_manager *kde =
		wlr_server_decoration_manager_create(s.display);
	wlr_server_decoration_manager_set_default_mode(kde, WLR_SERVER_DECORATION_MANAGER_MODE_SERVER);
	s.new_kde_decoration.notify = kde_decoration_new;
	wl_signal_add(&kde->events.new_decoration, &s.new_kde_decoration);
	s.deco_mgr = wlr_xdg_decoration_manager_v1_create(s.display);
	s.new_decoration.notify = decoration_new;
	wl_signal_add(&s.deco_mgr->events.new_toplevel_decoration, &s.new_decoration);

	s.ftl_mgr = wlr_foreign_toplevel_manager_v1_create(s.display);

	struct wlr_xdg_activation_v1 *act = wlr_xdg_activation_v1_create(s.display);
	s.activation_request.notify = activation_request;
	wl_signal_add(&act->events.request_activate, &s.activation_request);

	input_init(&s);

	const char *socket = wl_display_add_socket_auto(s.display);
	if (!socket) { wlr_backend_destroy(s.backend); return 1; }
	if (!wlr_backend_start(s.backend)) {
		wlr_backend_destroy(s.backend);
		wl_display_destroy(s.display);
		return 1;
	}
	setenv("WAYLAND_DISPLAY", socket, true);
	setenv("XDG_CURRENT_DESKTOP", "Lumen", true);
	setenv("XDG_SESSION_TYPE", "wayland", true);
	setenv("MOZ_ENABLE_WAYLAND", "1", true);
	setenv("QT_QPA_PLATFORM", "wayland;xcb", true);
	setenv("SDL_VIDEODRIVER", "wayland", true);
	setenv("_JAVA_AWT_WM_NONREPARENTING", "1", true);
	unsetenv("DISPLAY");

	ipc_init(&s);

	for (int i = 0; i < s.cfg.n_autostart; i++) spawn(s.cfg.autostart[i]);
	if (startup) spawn(startup);

	wlr_log(WLR_INFO, "Lumen running on WAYLAND_DISPLAY=%s", socket);
	wl_display_run(s.display);

	ipc_finish(&s);
	wl_display_destroy_clients(s.display);
	wlr_scene_node_destroy(&s.scene->tree.node);
	wlr_xcursor_manager_destroy(s.xcursor_mgr);
	wlr_cursor_destroy(s.cursor);
	wlr_allocator_destroy(s.allocator);
	wlr_renderer_destroy(s.renderer);
	wlr_backend_destroy(s.backend);
	wl_display_destroy(s.display);
	return 0;
}
