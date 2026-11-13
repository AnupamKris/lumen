#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/util/edges.h>
#include "lumen.h"

#define MAX_TILES 64

struct lumen_output *output_primary(struct lumen_server *s) {
	if (wl_list_empty(&s->outputs)) return NULL;
	struct lumen_output *o = wl_container_of(s->outputs.next, o, link);
	return o;
}

struct lumen_output *output_at(struct lumen_server *s, double x, double y) {
	struct wlr_output *wo = wlr_output_layout_output_at(s->output_layout, x, y);
	return wo ? wo->data : NULL;
}

bool view_is_tiled(struct lumen_view *v) {
	return v->mapped && v->server->ws_tiling[v->ws] && !v->floating &&
		!v->maximized && !v->fullscreen && !v->minimized;
}

static int by_order(const void *a, const void *b) {
	const struct lumen_view *x = *(struct lumen_view *const *)a;
	const struct lumen_view *y = *(struct lumen_view *const *)b;
	return x->order < y->order ? -1 : x->order > y->order;
}

static int collect_tiled(struct lumen_server *s, int ws, struct lumen_view **out) {
	int n = 0;
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->ws == ws && view_is_tiled(v) && n < MAX_TILES) out[n++] = v;
	qsort(out, n, sizeof(*out), by_order);
	return n;
}

/* Floating windows always stay above tiled ones; fullscreen lives in layer_top. */
void restack(struct lumen_server *s) {
	struct lumen_view *v;
	/* iterate back-to-front so the focused floating window ends up on top */
	wl_list_for_each_reverse(v, &s->views, link)
		if (v->mapped && !view_is_tiled(v) && !v->fullscreen)
			wlr_scene_node_raise_to_top(&v->tree->node);
}

void arrange_workspace(struct lumen_server *s, int ws) {
	struct lumen_output *o = output_primary(s);
	if (!o) return;
	struct lumen_config *c = &s->cfg;
	struct wlr_box u = o->usable;
	int g = c->gaps;

	struct lumen_view *tiles[MAX_TILES];
	int n = collect_tiled(s, ws, tiles);
	float ratio = s->ws_master[ws];
	for (int i = 0; i < n; i++) {
		struct lumen_view *v = tiles[i];
		int x, y, w, h;
		if (n == 1) {
			x = u.x + g; y = u.y + g; w = u.width - 2 * g; h = u.height - 2 * g;
		} else {
			int mw = (int)((u.width - 3 * g) * ratio);
			if (i == 0) {
				x = u.x + g; y = u.y + g; w = mw; h = u.height - 2 * g;
			} else {
				int k = n - 1, j = i - 1;
				int avail = u.height - 2 * g - (k - 1) * g;
				int sh = avail / k;
				x = u.x + 2 * g + mw;
				w = u.width - 3 * g - mw;
				y = u.y + g + j * (sh + g);
				h = (j == k - 1) ? (u.y + u.height - g) - y : sh;
			}
		}
		if (v->xdg->base->initialized)
			wlr_xdg_toplevel_set_tiled(v->xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
				WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		view_set_geometry(v, x, y, w, h);
	}

	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->ws != ws || !v->mapped) continue;
		wlr_scene_node_set_enabled(&v->tree->node, ws == s->ws && !v->minimized);
		if (v->minimized || view_is_tiled(v)) continue;
		if (v->xdg->base->initialized)
			wlr_xdg_toplevel_set_tiled(v->xdg, v->maximized ?
				(WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT) : WLR_EDGE_NONE);
		if (v->fullscreen) {
			view_set_geometry(v, o->full.x, o->full.y, o->full.width, o->full.height);
		} else if (v->maximized) {
			view_set_geometry(v, u.x, u.y, u.width, u.height);
		} else {
			struct wlr_box *f = &v->float_geo;
			view_set_geometry(v, f->x, f->y, f->width, f->height);
		}
	}
	restack(s);
	schedule_frames(s);
}

void arrange_all(struct lumen_server *s) {
	for (int i = 0; i < MAX_WORKSPACES; i++) arrange_workspace(s, i);
}

void workspace_switch(struct lumen_server *s, int ws) {
	if (ws < 0 || ws >= MAX_WORKSPACES || ws == s->ws) return;
	int old = s->ws;
	s->ws = ws;
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped)
			wlr_scene_node_set_enabled(&v->tree->node, v->ws == ws && !v->minimized);
	(void)old;
	struct lumen_view *f = view_focused(s);
	if (f) view_focus(f);
	else focus_clear(s);
	arrange_workspace(s, ws);
	ipc_broadcast_state(s);
	cursor_rebase(s);
}

void view_move_to_workspace(struct lumen_view *v, int ws) {
	struct lumen_server *s = v->server;
	if (ws < 0 || ws >= MAX_WORKSPACES || ws == v->ws) return;
	int old = v->ws;
	v->ws = ws;
	v->order = ++s->next_order;
	wlr_scene_node_set_enabled(&v->tree->node, ws == s->ws && !v->minimized);
	if (old == s->ws) {
		struct lumen_view *f = view_focused(s);
		if (f) view_focus(f);
		else focus_clear(s);
	}
	arrange_workspace(s, old);
	arrange_workspace(s, ws);
	ipc_broadcast_state(s);
	cursor_rebase(s);
}

void focus_cycle(struct lumen_server *s, int dir) {
	struct lumen_view *list[MAX_TILES];
	int n = 0;
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && v->ws == s->ws && !v->minimized && n < MAX_TILES) list[n++] = v;
	if (n < 2) return;
	qsort(list, n, sizeof(*list), by_order);
	struct lumen_view *cur = view_focused(s);
	int i = 0;
	for (; i < n; i++) if (list[i] == cur) break;
	i = ((i + dir) % n + n) % n;
	view_focus(list[i]);
}

void tile_swap_master(struct lumen_server *s) {
	struct lumen_view *tiles[MAX_TILES];
	int n = collect_tiled(s, s->ws, tiles);
	struct lumen_view *f = view_focused(s);
	if (n < 2 || !f || !view_is_tiled(f)) return;
	struct lumen_view *other = (f == tiles[0]) ? tiles[1] : tiles[0];
	uint32_t t = f->order;
	f->order = other->order;
	other->order = t;
	arrange_workspace(s, s->ws);
}
