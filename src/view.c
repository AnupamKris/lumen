#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include "lumen.h"

#define FADE_MS 160

/* ---------- geometry helpers ---------- */

int view_deco_top(struct lumen_view *v) {
	if (!v->ssd || v->fullscreen) return 0;
	return v->server->cfg.border_width + v->server->cfg.titlebar_height;
}

int view_deco_side(struct lumen_view *v) {
	if (!v->ssd || v->fullscreen) return 0;
	return v->server->cfg.border_width;
}

struct lumen_view *view_focused(struct lumen_server *s) {
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped && v->ws == s->ws && !v->minimized) return v;
	return NULL;
}

static bool view_is_focused(struct lumen_view *v) {
	struct wlr_surface *f = v->server->seat->keyboard_state.focused_surface;
	return f && f == v->xdg->base->surface;
}

/* Actual content size: what the client committed, else what we asked for. */
static void content_size(struct lumen_view *v, int *w, int *h) {
	struct wlr_box g;
	wlr_xdg_surface_get_geometry(v->xdg->base, &g);
	*w = g.width > 0 ? g.width : v->geo.width;
	*h = g.height > 0 ? g.height : v->geo.height;
}

/* ---------- effects ---------- */

struct fx_ctx {
	struct lumen_view *v;
	int radius;
	enum corner_location corners;
	float alpha;
};

static void buffer_fx(struct wlr_scene_buffer *b, int sx, int sy, void *data) {
	struct fx_ctx *c = data;
	struct lumen_view *v = c->v;
	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(b);
	bool main = ss && ss->surface == v->xdg->base->surface;
	wlr_scene_buffer_set_opacity(b, c->alpha);
	if (main) {
		wlr_scene_buffer_set_corner_radius(b, c->radius, c->corners);
		bool blur = v->server->cfg.blur && !v->fullscreen;
		wlr_scene_buffer_set_backdrop_blur(b, blur);
		wlr_scene_buffer_set_backdrop_blur_optimized(b, blur);
		wlr_scene_buffer_set_backdrop_blur_ignore_transparent(b, blur);
	}
}

static void title_fx(struct lumen_view *v) {
	if (v->title) wlr_scene_buffer_set_opacity(v->title, v->alpha);
}

/* Applies per-buffer effects (rounded corners, blur, opacity). Must be re-run
 * when the client attaches new subsurfaces/buffers, so it's called on commit. */
void view_apply_effects(struct lumen_view *v) {
	struct lumen_config *cfg = &v->server->cfg;
	bool square = v->fullscreen || v->maximized;
	struct fx_ctx c = {
		.v = v,
		.radius = square ? 0 : cfg->corner_radius,
		.corners = v->ssd ? CORNER_LOCATION_BOTTOM : CORNER_LOCATION_ALL,
		.alpha = v->alpha,
	};
	wlr_scene_node_for_each_buffer(&v->content->node, buffer_fx, &c);
	title_fx(v);

	float col[4];
	const float *src = view_is_focused(v) ? cfg->border : cfg->border_inactive;
	for (int i = 0; i < 4; i++) col[i] = src[i] * v->alpha;
	wlr_scene_rect_set_color(v->border, col);
	src = view_is_focused(v) ? cfg->title_bg : cfg->title_bg_inactive;
	for (int i = 0; i < 4; i++) col[i] = src[i] * v->alpha;
	wlr_scene_rect_set_color(v->frame, col);
	float sh[4];
	float sa = view_is_focused(v) ? 1.0f : 0.6f;
	for (int i = 0; i < 4; i++) sh[i] = cfg->shadow_color[i] * v->alpha * sa;
	wlr_scene_shadow_set_color(v->shadow, sh);
}

/* ---------- decorations ---------- */

static void render_title(struct lumen_view *v, int w) {
	int th = v->server->cfg.titlebar_height;
	bool focused = view_is_focused(v);
	if (v->title_w == w && v->title_focused == focused && !v->title_dirty) return;
	struct wlr_buffer *buf = deco_render_title(v, w, th);
	if (!buf) return;
	wlr_scene_buffer_set_buffer(v->title, buf);
	wlr_buffer_drop(buf);
	v->title_w = w;
	v->title_focused = focused;
	v->title_dirty = false;
}

void view_update_deco(struct lumen_view *v) {
	struct lumen_config *cfg = &v->server->cfg;
	int cw, ch;
	content_size(v, &cw, &ch);
	int top = view_deco_top(v), side = view_deco_side(v);
	int ow = cw + 2 * side, oh = ch + top + side;
	bool square = v->fullscreen || v->maximized;
	int r = square ? 0 : cfg->corner_radius;

	struct wlr_box g;
	wlr_xdg_surface_get_geometry(v->xdg->base, &g);
	/* clip CSD shadows/margins away; with a clip set, the tree origin is g.x/g.y */
	if (g.width > 0 && g.height > 0) {
		wlr_scene_subsurface_tree_set_clip(&v->content->node, &g);
		wlr_scene_node_set_position(&v->content->node, side, top);
	} else {
		wlr_scene_node_set_position(&v->content->node, side - g.x, top - g.y);
	}

	bool ssd = v->ssd && !v->fullscreen;
	int th = cfg->titlebar_height;
	wlr_scene_node_set_enabled(&v->border->node, ssd && cfg->border_width > 0);
	wlr_scene_node_set_enabled(&v->frame->node, ssd);
	wlr_scene_node_set_enabled(&v->title->node, ssd);
	if (ssd) {
		int bw = cfg->border_width;
		/* hairline: a rounded rect with the frame punched out, so it never overdraws */
		wlr_scene_rect_set_size(v->border, ow, oh);
		wlr_scene_rect_set_corner_radius(v->border, r ? r + bw : 0, CORNER_LOCATION_ALL);
		wlr_scene_rect_set_clipped_region(v->border, (struct clipped_region){
			.area = {bw, bw, cw, ch + th},
			.corner_radius = r,
			.corners = r ? CORNER_LOCATION_ALL : CORNER_LOCATION_NONE,
		});
		/* frame = titlebar + body backing; the body part is clipped away under
		 * the client so translucent windows show the (blurred) backdrop */
		wlr_scene_node_set_position(&v->frame->node, bw, bw);
		wlr_scene_rect_set_size(v->frame, cw, ch + th);
		wlr_scene_rect_set_corner_radius(v->frame, r, r ? CORNER_LOCATION_ALL : CORNER_LOCATION_NONE);
		wlr_scene_rect_set_clipped_region(v->frame, (struct clipped_region){
			.area = {0, th, cw, ch},
			.corner_radius = r,
			.corners = r ? CORNER_LOCATION_BOTTOM : CORNER_LOCATION_NONE,
		});
		wlr_scene_node_set_position(&v->title->node, bw, bw);
		render_title(v, cw);
	}

	bool shadow = cfg->shadows && !v->fullscreen && !v->maximized && cfg->shadow_size > 0;
	wlr_scene_node_set_enabled(&v->shadow->node, shadow);
	if (shadow) {
		/* SceneFX: the shadow box is the node shrunk by sigma; gaussian is sigma/2 */
		int sg = cfg->shadow_size, dy = sg / 5;
		wlr_scene_node_set_position(&v->shadow->node, -sg, -sg + dy);
		wlr_scene_shadow_set_blur_sigma(v->shadow, sg);
		wlr_scene_shadow_set_size(v->shadow, ow + 2 * sg, oh + 2 * sg);
		wlr_scene_shadow_set_corner_radius(v->shadow, r ? r + side : 0);
		wlr_scene_shadow_set_clipped_region(v->shadow, (struct clipped_region){
			.area = {sg, sg - dy, ow, oh},
			.corner_radius = r ? r + side : 0,
			.corners = r ? CORNER_LOCATION_ALL : CORNER_LOCATION_NONE,
		});
	}
	view_apply_effects(v);
}

static void view_refresh_ssd(struct lumen_view *v) {
	bool ssd = false;
	if (v->decoration)
		ssd = v->decoration->current.mode == WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE;
	else if (v->kde_deco)
		ssd = v->kde_deco->mode == WLR_SERVER_DECORATION_MANAGER_MODE_SERVER;
	if (ssd != v->ssd) {
		v->ssd = ssd;
		v->title_dirty = true;
		if (v->mapped) arrange_workspace(v->server, v->ws);
	}
}

/* Positions the view so its OUTER frame (incl. decorations) fills the box. */
void view_set_geometry(struct lumen_view *v, int x, int y, int w, int h) {
	int top = view_deco_top(v), side = view_deco_side(v);
	int cw = w - 2 * side, ch = h - top - side;
	if (cw < 1) cw = 1;
	if (ch < 1) ch = 1;
	v->geo = (struct wlr_box){x + side, y + top, cw, ch};
	wlr_scene_node_set_position(&v->tree->node, x, y);
	if (v->xdg->base->initialized) wlr_xdg_toplevel_set_size(v->xdg, cw, ch);
	view_update_deco(v);
}

/* ---------- focus / state ---------- */

/* Drop keyboard focus (e.g. switched to an empty workspace) and tell the
 * previously focused window, and the panel, that it is no longer active. */
void focus_clear(struct lumen_server *s) {
	if (s->locked || s->focused_layer) return;
	struct wlr_surface *prev = s->seat->keyboard_state.focused_surface;
	struct wlr_xdg_toplevel *pt = prev ? wlr_xdg_toplevel_try_from_wlr_surface(prev) : NULL;
	struct lumen_view *pv = pt ? pt->base->data : NULL;
	if (pt) wlr_xdg_toplevel_set_activated(pt, false);
	if (pv && pv->ftl) wlr_foreign_toplevel_handle_v1_set_activated(pv->ftl, false);
	wlr_seat_keyboard_clear_focus(s->seat);
	if (pv && pv->mapped) view_update_deco(pv);
}

void view_focus(struct lumen_view *v) {
	struct lumen_server *s = v->server;
	struct wlr_seat *seat = s->seat;
	struct wlr_surface *surf = v->xdg->base->surface;
	struct wlr_surface *prev = seat->keyboard_state.focused_surface;

	wlr_scene_node_raise_to_top(&v->tree->node);
	wl_list_remove(&v->link);
	wl_list_insert(&s->views, &v->link);
	restack(s);

	if (s->locked) return; /* the lock screen owns the keyboard */
	/* an exclusive layer surface (launcher, lock) keeps the keyboard */
	if (s->focused_layer && s->focused_layer->current.keyboard_interactive ==
			ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE)
		return;
	s->focused_layer = NULL;

	if (prev == surf) return;
	if (prev) {
		struct wlr_xdg_toplevel *pt = wlr_xdg_toplevel_try_from_wlr_surface(prev);
		if (pt) {
			wlr_xdg_toplevel_set_activated(pt, false);
			struct lumen_view *pv = pt->base->data;
			if (pv && pv->ftl) wlr_foreign_toplevel_handle_v1_set_activated(pv->ftl, false);
		}
	}
	wlr_xdg_toplevel_set_activated(v->xdg, true);
	if (v->ftl) wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, true);
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(seat, surf, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(seat, surf, NULL, 0, NULL);

	/* repaint both titlebars / borders for the new focus state */
	struct lumen_view *o;
	wl_list_for_each(o, &s->views, link)
		if (o->mapped) view_update_deco(o);
	ipc_broadcast_state(s);
}

void view_close(struct lumen_view *v) {
	wlr_xdg_toplevel_send_close(v->xdg);
}

void view_set_maximized(struct lumen_view *v, bool on) {
	if (v->maximized == on) return;
	v->maximized = on;
	if (v->xdg->base->initialized) wlr_xdg_toplevel_set_maximized(v->xdg, on);
	if (v->ftl) wlr_foreign_toplevel_handle_v1_set_maximized(v->ftl, on);
	v->title_dirty = true;
	arrange_workspace(v->server, v->ws);
}

void view_set_fullscreen(struct lumen_view *v, bool on) {
	if (v->fullscreen == on) return;
	v->fullscreen = on;
	if (v->xdg->base->initialized) wlr_xdg_toplevel_set_fullscreen(v->xdg, on);
	if (v->ftl) wlr_foreign_toplevel_handle_v1_set_fullscreen(v->ftl, on);
	/* fullscreen windows sit above the panel */
	wlr_scene_node_reparent(&v->tree->node, on ? v->server->layer_top : v->server->layer_views);
	if (on) wlr_scene_node_raise_to_top(&v->tree->node);
	arrange_workspace(v->server, v->ws);
}

void view_set_minimized(struct lumen_view *v, bool on) {
	struct lumen_server *s = v->server;
	if (v->minimized == on) return;
	v->minimized = on;
	if (v->ftl) wlr_foreign_toplevel_handle_v1_set_minimized(v->ftl, on);
	wlr_scene_node_set_enabled(&v->tree->node, !on && v->ws == s->ws);
	if (on) {
		if (view_is_focused(v)) {
			wlr_xdg_toplevel_set_activated(v->xdg, false);
			if (v->ftl) wlr_foreign_toplevel_handle_v1_set_activated(v->ftl, false);
			wlr_seat_keyboard_clear_focus(s->seat);
		}
		/* push to the back so focus goes to the next window */
		wl_list_remove(&v->link);
		wl_list_insert(s->views.prev, &v->link);
		struct lumen_view *n = view_focused(s);
		if (n) view_focus(n);
	} else {
		view_focus(v);
	}
	arrange_workspace(s, v->ws);
	cursor_rebase(s);
}

void view_begin_interactive(struct lumen_view *v, enum cursor_mode mode, uint32_t edges) {
	struct lumen_server *s = v->server;
	if (v->fullscreen) return;
	if (v->maximized) view_set_maximized(v, false);
	if (view_is_tiled(v)) {
		/* dragging a tiled window pops it out to float under the cursor */
		v->floating = true;
		int cw, ch;
		content_size(v, &cw, &ch);
		v->float_geo.width = cw * 3 / 4 > 300 ? cw * 3 / 4 : cw;
		v->float_geo.height = ch * 3 / 4 > 200 ? ch * 3 / 4 : ch;
		v->float_geo.x = (int)s->cursor->x - v->float_geo.width / 2;
		v->float_geo.y = (int)s->cursor->y - view_deco_top(v) / 2;
		arrange_workspace(s, v->ws);
	}
	view_focus(v);
	s->grab_view = v;
	s->cursor_mode = mode;
	int top = view_deco_top(v), side = view_deco_side(v);
	int cw, ch;
	content_size(v, &cw, &ch);
	struct wlr_box outer = {v->geo.x - side, v->geo.y - top, cw + 2 * side, ch + top + side};
	if (mode == CURSOR_MOVE) {
		s->grab_x = s->cursor->x - outer.x;
		s->grab_y = s->cursor->y - outer.y;
	} else {
		s->grab_x = s->cursor->x;
		s->grab_y = s->cursor->y;
		s->grab_box = outer;
		s->resize_edges = edges;
	}
}

/* ---------- animation ---------- */

bool views_animate(struct lumen_server *s, uint32_t now) {
	bool more = false;
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link) {
		if (v->alpha >= 1.0f) continue;
		float t = (float)(now - v->anim_start) / FADE_MS;
		if (t >= 1.0f) t = 1.0f;
		else more = true;
		/* ease-out cubic */
		float e = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
		v->alpha = t >= 1.0f ? 1.0f : e;
		view_apply_effects(v);
	}
	return more;
}

/* ---------- foreign toplevel (taskbar) ---------- */

static void ftl_activate(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, ftl_activate);
	if (v->ws != v->server->ws) workspace_switch(v->server, v->ws);
	if (v->minimized) view_set_minimized(v, false);
	else view_focus(v);
}
static void ftl_close(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, ftl_close);
	view_close(v);
}
static void ftl_minimize(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, ftl_minimize);
	struct wlr_foreign_toplevel_handle_v1_minimized_event *ev = data;
	view_set_minimized(v, ev->minimized);
}
static void ftl_maximize(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, ftl_maximize);
	struct wlr_foreign_toplevel_handle_v1_maximized_event *ev = data;
	view_set_maximized(v, ev->maximized);
}
static void ftl_fullscreen(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, ftl_fullscreen);
	struct wlr_foreign_toplevel_handle_v1_fullscreen_event *ev = data;
	view_set_fullscreen(v, ev->fullscreen);
}

static void ftl_create(struct lumen_view *v) {
	struct lumen_server *s = v->server;
	v->ftl = wlr_foreign_toplevel_handle_v1_create(s->ftl_mgr);
	if (!v->ftl) return;
	if (v->xdg->title) wlr_foreign_toplevel_handle_v1_set_title(v->ftl, v->xdg->title);
	if (v->xdg->app_id) wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, v->xdg->app_id);
	struct lumen_output *o = output_primary(s);
	if (o) wlr_foreign_toplevel_handle_v1_output_enter(v->ftl, o->wlr_output);
	v->ftl_activate.notify = ftl_activate;
	wl_signal_add(&v->ftl->events.request_activate, &v->ftl_activate);
	v->ftl_close.notify = ftl_close;
	wl_signal_add(&v->ftl->events.request_close, &v->ftl_close);
	v->ftl_minimize.notify = ftl_minimize;
	wl_signal_add(&v->ftl->events.request_minimize, &v->ftl_minimize);
	v->ftl_maximize.notify = ftl_maximize;
	wl_signal_add(&v->ftl->events.request_maximize, &v->ftl_maximize);
	v->ftl_fullscreen.notify = ftl_fullscreen;
	wl_signal_add(&v->ftl->events.request_fullscreen, &v->ftl_fullscreen);
}

static void ftl_destroy(struct lumen_view *v) {
	if (!v->ftl) return;
	wl_list_remove(&v->ftl_activate.link);
	wl_list_remove(&v->ftl_close.link);
	wl_list_remove(&v->ftl_minimize.link);
	wl_list_remove(&v->ftl_maximize.link);
	wl_list_remove(&v->ftl_fullscreen.link);
	wlr_foreign_toplevel_handle_v1_destroy(v->ftl);
	v->ftl = NULL;
}

/* ---------- xdg toplevel lifecycle ---------- */

static bool wants_float(struct lumen_view *v) {
	struct wlr_xdg_toplevel *t = v->xdg;
	if (t->parent) return true; /* dialogs */
	struct wlr_xdg_toplevel_state *st = &t->current;
	if (st->min_width && st->min_width == st->max_width &&
			st->min_height && st->min_height == st->max_height)
		return true; /* fixed size */
	return false;
}

static void place_floating(struct lumen_view *v) {
	struct lumen_server *s = v->server;
	struct lumen_output *o = output_at(s, s->cursor->x, s->cursor->y);
	if (!o) o = output_primary(s);
	if (!o) return;
	struct wlr_box u = o->usable;
	int cw, ch;
	content_size(v, &cw, &ch);
	if (cw <= 1 || ch <= 1) { cw = u.width * 3 / 5; ch = u.height * 3 / 5; }
	int top = view_deco_top(v), side = view_deco_side(v);
	int ow = cw + 2 * side, oh = ch + top + side;
	if (ow > u.width) ow = u.width;
	if (oh > u.height) oh = u.height;
	if (v->xdg->parent && v->xdg->parent->base->data) {
		/* centre dialogs on their parent */
		struct lumen_view *p = v->xdg->parent->base->data;
		v->float_geo.x = p->geo.x + p->geo.width / 2 - ow / 2;
		v->float_geo.y = p->geo.y + p->geo.height / 2 - oh / 2;
	} else {
		/* centre, cascading a little per open window so they don't stack exactly */
		int n = 0;
		struct lumen_view *it;
		wl_list_for_each(it, &s->views, link)
			if (it != v && it->mapped && it->ws == v->ws && !view_is_tiled(it)) n++;
		int off = (n % 6) * 28;
		v->float_geo.x = u.x + (u.width - ow) / 2 + off;
		v->float_geo.y = u.y + (u.height - oh) / 2 + off;
	}
	if (v->float_geo.x < u.x) v->float_geo.x = u.x;
	if (v->float_geo.y < u.y) v->float_geo.y = u.y;
	v->float_geo.width = ow;
	v->float_geo.height = oh;
	v->placed = true;
}

static void view_map(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, map);
	struct lumen_server *s = v->server;
	v->mapped = true;
	v->ws = s->ws;
	v->order = ++s->next_order;
	if (wants_float(v)) v->floating = true;
	view_refresh_ssd(v);
	if (!v->placed) place_floating(v);
	wl_list_insert(&s->views, &v->link);
	wlr_scene_node_set_enabled(&v->tree->node, true);

	v->alpha = s->cfg.animations ? 0.0f : 1.0f;
	v->anim_start = now_ms();
	ftl_create(v);
	if (v->xdg->requested.maximized) v->maximized = true;
	if (v->xdg->requested.fullscreen) view_set_fullscreen(v, true);

	arrange_workspace(s, v->ws);
	view_focus(v);
	schedule_frames(s);
	cursor_rebase(s);
}

static void view_unmap(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, unmap);
	struct lumen_server *s = v->server;
	if (s->grab_view == v) {
		s->grab_view = NULL;
		s->cursor_mode = CURSOR_PASSTHROUGH;
	}
	bool had_focus = view_is_focused(v);
	v->mapped = false;
	wlr_scene_node_set_enabled(&v->tree->node, false);
	wl_list_remove(&v->link);
	wl_list_init(&v->link);
	ftl_destroy(v);
	if (had_focus) {
		struct lumen_view *n = view_focused(s);
		if (n) view_focus(n);
		else focus_clear(s);
	}
	arrange_workspace(s, v->ws);
	ipc_broadcast_state(s);
	cursor_rebase(s);
}

static void view_commit(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, commit);
	struct lumen_server *s = v->server;
	if (v->xdg->base->initial_commit) {
		wlr_xdg_toplevel_set_wm_capabilities(v->xdg,
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MAXIMIZE |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_FULLSCREEN |
			WLR_XDG_TOPLEVEL_WM_CAPABILITIES_MINIMIZE);
		if (v->decoration)
			wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration,
				WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
		/* tiled workspaces: tell the client so GTK drops its CSD shadows */
		if (s->ws_tiling[s->ws] && !v->xdg->parent)
			wlr_xdg_toplevel_set_tiled(v->xdg, WLR_EDGE_TOP | WLR_EDGE_BOTTOM |
				WLR_EDGE_LEFT | WLR_EDGE_RIGHT);
		wlr_xdg_toplevel_set_size(v->xdg, 0, 0);
		return;
	}
	if (!v->mapped) return;
	view_refresh_ssd(v);

	/* interactive resize from the top/left: keep the opposite edge anchored */
	if (s->cursor_mode == CURSOR_RESIZE && s->grab_view == v) {
		int cw, ch;
		content_size(v, &cw, &ch);
		int top = view_deco_top(v), side = view_deco_side(v);
		struct wlr_box *gb = &s->grab_box;
		int x = v->geo.x - side, y = v->geo.y - top;
		if (s->resize_edges & WLR_EDGE_LEFT) x = gb->x + gb->width - (cw + 2 * side);
		if (s->resize_edges & WLR_EDGE_TOP) y = gb->y + gb->height - (ch + top + side);
		wlr_scene_node_set_position(&v->tree->node, x, y);
		v->geo.x = x + side;
		v->geo.y = y + top;
		v->float_geo = (struct wlr_box){x, y, cw + 2 * side, ch + top + side};
	}
	view_update_deco(v);
}

static void view_destroy(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, destroy);
	wl_list_remove(&v->map.link);
	wl_list_remove(&v->unmap.link);
	wl_list_remove(&v->commit.link);
	wl_list_remove(&v->destroy.link);
	wl_list_remove(&v->request_move.link);
	wl_list_remove(&v->request_resize.link);
	wl_list_remove(&v->request_maximize.link);
	wl_list_remove(&v->request_fullscreen.link);
	wl_list_remove(&v->request_minimize.link);
	wl_list_remove(&v->set_title.link);
	wl_list_remove(&v->set_app_id.link);
	if (v->decoration) {
		wl_list_remove(&v->deco_request_mode.link);
		wl_list_remove(&v->deco_destroy.link);
		v->decoration->data = NULL;
	}
	if (v->kde_deco) {
		wl_list_remove(&v->kde_mode.link);
		wl_list_remove(&v->kde_destroy.link);
		v->kde_deco->data = NULL;
	}
	ftl_destroy(v);
	wl_list_remove(&v->link);
	wlr_scene_node_destroy(&v->tree->node);
	free(v);
}

static void view_request_move(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, request_move);
	view_begin_interactive(v, CURSOR_MOVE, 0);
}

static void view_request_resize(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, request_resize);
	struct wlr_xdg_toplevel_resize_event *ev = data;
	if (view_is_tiled(v)) return;
	view_begin_interactive(v, CURSOR_RESIZE, ev->edges);
}

static void view_request_maximize(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, request_maximize);
	if (!v->xdg->base->initialized) return;
	if (v->mapped) view_set_maximized(v, v->xdg->requested.maximized);
	else wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_fullscreen(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, request_fullscreen);
	if (!v->xdg->base->initialized) return;
	if (v->mapped) view_set_fullscreen(v, v->xdg->requested.fullscreen);
	else wlr_xdg_surface_schedule_configure(v->xdg->base);
}

static void view_request_minimize(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, request_minimize);
	if (v->mapped) view_set_minimized(v, true);
}

static void view_set_title(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, set_title);
	if (v->ftl && v->xdg->title) wlr_foreign_toplevel_handle_v1_set_title(v->ftl, v->xdg->title);
	v->title_dirty = true;
	if (v->mapped) view_update_deco(v);
}

static void view_set_app_id(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, set_app_id);
	if (v->ftl && v->xdg->app_id) wlr_foreign_toplevel_handle_v1_set_app_id(v->ftl, v->xdg->app_id);
}

void view_init_toplevel(struct lumen_server *s, struct wlr_xdg_toplevel *xdg) {
	struct lumen_config *cfg = &s->cfg;
	struct lumen_view *v = calloc(1, sizeof(*v));
	v->server = s;
	v->xdg = xdg;
	v->alpha = 1.0f;
	wl_list_init(&v->link);

	v->tree = wlr_scene_tree_create(s->layer_views);
	wlr_scene_node_set_enabled(&v->tree->node, false);
	v->shadow = wlr_scene_shadow_create(v->tree, 1, 1, 0, 1.0f, cfg->shadow_color);
	v->border = wlr_scene_rect_create(v->tree, 1, 1, cfg->border);
	v->frame = wlr_scene_rect_create(v->tree, 1, 1, cfg->title_bg);
	v->title = wlr_scene_buffer_create(v->tree, NULL);
	v->content = wlr_scene_xdg_surface_create(v->tree, xdg->base);

	v->tag = (struct node_tag){NODE_VIEW, v};
	v->title_tag = (struct node_tag){NODE_TITLEBAR, v};
	v->tree->node.data = &v->tag;
	v->title->node.data = &v->title_tag;
	v->frame->node.data = &v->title_tag;
	v->border->node.data = &v->title_tag;
	xdg->base->data = v;
	xdg->base->surface->data = v->content; /* popups parent here */

	struct wlr_surface *surf = xdg->base->surface;
	v->map.notify = view_map;
	wl_signal_add(&surf->events.map, &v->map);
	v->unmap.notify = view_unmap;
	wl_signal_add(&surf->events.unmap, &v->unmap);
	v->commit.notify = view_commit;
	wl_signal_add(&surf->events.commit, &v->commit);
	v->destroy.notify = view_destroy;
	wl_signal_add(&xdg->events.destroy, &v->destroy);
	v->request_move.notify = view_request_move;
	wl_signal_add(&xdg->events.request_move, &v->request_move);
	v->request_resize.notify = view_request_resize;
	wl_signal_add(&xdg->events.request_resize, &v->request_resize);
	v->request_maximize.notify = view_request_maximize;
	wl_signal_add(&xdg->events.request_maximize, &v->request_maximize);
	v->request_fullscreen.notify = view_request_fullscreen;
	wl_signal_add(&xdg->events.request_fullscreen, &v->request_fullscreen);
	v->request_minimize.notify = view_request_minimize;
	wl_signal_add(&xdg->events.request_minimize, &v->request_minimize);
	v->set_title.notify = view_set_title;
	wl_signal_add(&xdg->events.set_title, &v->set_title);
	v->set_app_id.notify = view_set_app_id;
	wl_signal_add(&xdg->events.set_app_id, &v->set_app_id);
}

/* ---------- decoration protocols ---------- */

static void deco_request_mode(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, deco_request_mode);
	if (v->xdg->base->initialized)
		wlr_xdg_toplevel_decoration_v1_set_mode(v->decoration,
			WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
}

static void deco_destroy(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, deco_destroy);
	wl_list_remove(&v->deco_request_mode.link);
	wl_list_remove(&v->deco_destroy.link);
	v->decoration = NULL;
	view_refresh_ssd(v);
}

void decoration_new(struct wl_listener *l, void *data) {
	struct wlr_xdg_toplevel_decoration_v1 *d = data;
	struct lumen_view *v = d->toplevel->base->data;
	if (!v) return;
	v->decoration = d;
	d->data = v;
	v->deco_request_mode.notify = deco_request_mode;
	wl_signal_add(&d->events.request_mode, &v->deco_request_mode);
	v->deco_destroy.notify = deco_destroy;
	wl_signal_add(&d->events.destroy, &v->deco_destroy);
	deco_request_mode(&v->deco_request_mode, d);
}

static void kde_mode(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, kde_mode);
	view_refresh_ssd(v);
}

static void kde_destroy(struct wl_listener *l, void *data) {
	struct lumen_view *v = wl_container_of(l, v, kde_destroy);
	wl_list_remove(&v->kde_mode.link);
	wl_list_remove(&v->kde_destroy.link);
	v->kde_deco = NULL;
	view_refresh_ssd(v);
}

void kde_decoration_new(struct wl_listener *l, void *data) {
	struct wlr_server_decoration *d = data;
	struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(d->surface);
	if (!t || !t->base->data) return;
	struct lumen_view *v = t->base->data;
	if (v->kde_deco) return;
	v->kde_deco = d;
	d->data = v;
	v->kde_mode.notify = kde_mode;
	wl_signal_add(&d->events.mode, &v->kde_mode);
	v->kde_destroy.notify = kde_destroy;
	wl_signal_add(&d->events.destroy, &v->kde_destroy);
	view_refresh_ssd(v);
}

/* ---------- popups ---------- */

struct lumen_popup {
	struct wlr_xdg_popup *popup;
	struct wl_listener commit, destroy;
};

static void popup_unconstrain(struct lumen_popup *p) {
	struct wlr_xdg_popup *popup = p->popup;
	/* walk up to the root surface to find what the popup is relative to */
	struct wlr_surface *root = popup->parent;
	int dx = 0, dy = 0;
	struct wlr_xdg_popup *pp;
	while (root && (pp = wlr_xdg_popup_try_from_wlr_surface(root))) {
		dx += pp->current.geometry.x;
		dy += pp->current.geometry.y;
		root = pp->parent;
	}
	if (!root) return;
	struct lumen_server *s = NULL;
	double ox = 0, oy = 0;
	struct wlr_xdg_toplevel *t = wlr_xdg_toplevel_try_from_wlr_surface(root);
	struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (t && t->base->data) {
		struct lumen_view *v = t->base->data;
		s = v->server;
		struct wlr_box g;
		wlr_xdg_surface_get_geometry(t->base, &g);
		ox = v->geo.x - g.x;
		oy = v->geo.y - g.y;
	} else if (ls && ls->data) {
		struct lumen_layer *ly = ls->data;
		s = ly->server;
		ox = ly->scene->tree->node.x;
		oy = ly->scene->tree->node.y;
	}
	if (!s) return;
	struct lumen_output *o = output_at(s, ox + dx, oy + dy);
	if (!o) o = output_primary(s);
	if (!o) return;
	struct wlr_box box = o->full;
	box.x -= ox + dx;
	box.y -= oy + dy;
	wlr_xdg_popup_unconstrain_from_box(popup, &box);
}

static void popup_commit(struct wl_listener *l, void *data) {
	struct lumen_popup *p = wl_container_of(l, p, commit);
	if (p->popup->base->initial_commit) {
		popup_unconstrain(p);
		wlr_xdg_surface_schedule_configure(p->popup->base);
	}
}

static void popup_destroy(struct wl_listener *l, void *data) {
	struct lumen_popup *p = wl_container_of(l, p, destroy);
	wl_list_remove(&p->commit.link);
	wl_list_remove(&p->destroy.link);
	free(p);
}

void xdg_popup_new(struct wl_listener *l, void *data) {
	struct wlr_xdg_popup *popup = data;
	if (!popup->parent || !popup->parent->data) return;
	struct wlr_scene_tree *parent = popup->parent->data;
	struct wlr_scene_tree *tree = wlr_scene_xdg_surface_create(parent, popup->base);
	popup->base->surface->data = tree;
	struct lumen_popup *p = calloc(1, sizeof(*p));
	p->popup = popup;
	p->commit.notify = popup_commit;
	wl_signal_add(&popup->base->surface->events.commit, &p->commit);
	p->destroy.notify = popup_destroy;
	wl_signal_add(&popup->events.destroy, &p->destroy);
}
