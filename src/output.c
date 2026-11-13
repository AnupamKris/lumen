#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <time.h>
#include <wlr/util/log.h>
#include "lumen.h"

void schedule_frames(struct lumen_server *s) {
	struct lumen_output *o;
	wl_list_for_each(o, &s->outputs, link) wlr_output_schedule_frame(o->wlr_output);
}

void output_render_wallpaper(struct lumen_output *o) {
	struct lumen_server *s = o->server;
	int w = o->full.width, h = o->full.height;
	if (w <= 0 || h <= 0) return;
	struct wlr_buffer *buf = deco_render_wallpaper(s, w, h);
	if (!buf) return;
	if (!o->wallpaper) o->wallpaper = wlr_scene_buffer_create(s->layer_bg, buf);
	else wlr_scene_buffer_set_buffer(o->wallpaper, buf);
	wlr_buffer_drop(buf);
	wlr_scene_node_set_position(&o->wallpaper->node, o->full.x, o->full.y);
	/* optimized blur caches a blurred copy of everything below it (wallpaper) */
	wlr_scene_optimized_blur_set_size(s->blur_node, w, h);
}

static void output_frame(struct wl_listener *l, void *data) {
	struct lumen_output *o = wl_container_of(l, o, frame);
	struct lumen_server *s = o->server;
	struct wlr_scene_output *so = o->scene_output;

	bool animating = views_animate(s, now_ms());

	wlr_scene_output_commit(so, NULL);
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(so, &now);
	if (animating) wlr_output_schedule_frame(o->wlr_output);
}

static void output_request_state(struct wl_listener *l, void *data) {
	struct lumen_output *o = wl_container_of(l, o, request_state);
	const struct wlr_output_event_request_state *ev = data;
	wlr_output_commit_state(o->wlr_output, ev->state);
	arrange_layers(o);
	output_render_wallpaper(o);
	arrange_all(o->server);
}

static void output_destroy(struct wl_listener *l, void *data) {
	struct lumen_output *o = wl_container_of(l, o, destroy);
	wl_list_remove(&o->frame.link);
	wl_list_remove(&o->request_state.link);
	wl_list_remove(&o->destroy.link);
	wl_list_remove(&o->link);
	if (o->wallpaper) wlr_scene_node_destroy(&o->wallpaper->node);
	struct lumen_layer *ly, *tmp;
	for (int i = 0; i < 4; i++)
		wl_list_for_each_safe(ly, tmp, &o->layers[i], link) {
			ly->output = NULL;
			wl_list_remove(&ly->link);
			wl_list_init(&ly->link);
			wlr_layer_surface_v1_destroy(ly->ls);
		}
	free(o);
}

void output_new(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_output);
	struct wlr_output *wo = data;
	wlr_output_init_render(wo, s->allocator, s->renderer);

	struct wlr_output_state st;
	wlr_output_state_init(&st);
	wlr_output_state_set_enabled(&st, true);
	struct wlr_output_mode *mode = wlr_output_preferred_mode(wo);
	if (mode) wlr_output_state_set_mode(&st, mode);
	wlr_output_commit_state(wo, &st);
	wlr_output_state_finish(&st);

	struct lumen_output *o = calloc(1, sizeof(*o));
	o->server = s;
	o->wlr_output = wo;
	wo->data = o;
	for (int i = 0; i < 4; i++) wl_list_init(&o->layers[i]);
	o->frame.notify = output_frame;
	wl_signal_add(&wo->events.frame, &o->frame);
	o->request_state.notify = output_request_state;
	wl_signal_add(&wo->events.request_state, &o->request_state);
	o->destroy.notify = output_destroy;
	wl_signal_add(&wo->events.destroy, &o->destroy);
	wl_list_insert(s->outputs.prev, &o->link);

	struct wlr_output_layout_output *lo = wlr_output_layout_add_auto(s->output_layout, wo);
	o->scene_output = wlr_scene_output_create(s->scene, wo);
	wlr_scene_output_layout_add_output(s->scene_layout, lo, o->scene_output);

	arrange_layers(o);
	output_render_wallpaper(o);
	wlr_log(WLR_INFO, "output %s %dx%d", wo->name, wo->width, wo->height);
}
