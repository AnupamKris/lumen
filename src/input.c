#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <libinput.h>
#include <linux/input-event-codes.h>
#include <wlr/backend/libinput.h>
#include <wlr/backend/session.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_primary_selection.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/edges.h>
#include <wlr/util/log.h>
#include <wlr/xcursor.h>
#include "lumen.h"

#define RESIZE_MARGIN 6  /* px inside the frame edge that start a resize */
#define DOUBLE_CLICK_MS 400
#define MOD_MASK (WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)

uint32_t now_ms(void) {
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint32_t)(t.tv_sec * 1000 + t.tv_nsec / 1000000);
}

/* ---------- keyboard ---------- */

static bool handle_bind(struct lumen_server *s, uint32_t mods, xkb_keysym_t sym) {
	sym = xkb_keysym_to_lower(sym);
	mods &= MOD_MASK;
	for (int i = 0; i < s->cfg.n_binds; i++) {
		struct lumen_bind *b = &s->cfg.binds[i];
		if (b->mods == mods && b->sym == sym) {
			run_action(s, b->action);
			return true;
		}
	}
	return false;
}

static void kb_key(struct wl_listener *l, void *data) {
	struct lumen_keyboard *k = wl_container_of(l, k, key);
	struct lumen_server *s = k->server;
	struct wlr_keyboard_key_event *ev = data;
	struct wlr_keyboard *kb = k->wlr_keyboard;
	uint32_t keycode = ev->keycode + 8;
	bool handled = false;
	wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);

	if (ev->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
		const xkb_keysym_t *syms;
		int n = xkb_state_key_get_syms(kb->xkb_state, keycode, &syms);
		uint32_t mods = wlr_keyboard_get_modifiers(kb);
		for (int i = 0; i < n && !handled; i++) {
			if (syms[i] >= XKB_KEY_XF86Switch_VT_1 && syms[i] <= XKB_KEY_XF86Switch_VT_12) {
				if (s->session)
					wlr_session_change_vt(s->session, syms[i] - XKB_KEY_XF86Switch_VT_1 + 1);
				handled = true;
			}
		}
		/* the lock screen gets every other key; no shortcuts while locked */
		if (s->locked) goto deliver;
		/* match on translated syms, then on the unshifted level so that
		 * Super+Shift+1 matches "SUPER+SHIFT+1" rather than "exclam" */
		for (int i = 0; i < n && !handled && mods; i++)
			handled = handle_bind(s, mods, syms[i]);
		if (!handled && mods) {
			xkb_layout_index_t layout = xkb_state_key_get_layout(kb->xkb_state, keycode);
			n = xkb_keymap_key_get_syms_by_level(kb->keymap, keycode, layout, 0, &syms);
			for (int i = 0; i < n && !handled; i++) handled = handle_bind(s, mods, syms[i]);
		}
		if (!handled && !mods) /* bare-key binds: Print, volume, brightness */
			for (int i = 0; i < n && !handled; i++) handled = handle_bind(s, 0, syms[i]);
	}
deliver:
	if (!handled) {
		wlr_seat_set_keyboard(s->seat, kb);
		wlr_seat_keyboard_notify_key(s->seat, ev->time_msec, ev->keycode, ev->state);
	}
}

static void kb_modifiers(struct wl_listener *l, void *data) {
	struct lumen_keyboard *k = wl_container_of(l, k, modifiers);
	wlr_seat_set_keyboard(k->server->seat, k->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(k->server->seat, &k->wlr_keyboard->modifiers);
}

static void kb_destroy(struct wl_listener *l, void *data) {
	struct lumen_keyboard *k = wl_container_of(l, k, destroy);
	wl_list_remove(&k->modifiers.link);
	wl_list_remove(&k->key.link);
	wl_list_remove(&k->destroy.link);
	wl_list_remove(&k->link);
	free(k);
}

static void update_caps(struct lumen_server *s) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&s->keyboards)) caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(s->seat, caps);
}

static void keyboard_add(struct lumen_server *s, struct wlr_keyboard *kb, bool set_keymap) {
	struct lumen_config *c = &s->cfg;
	if (set_keymap) {
		struct xkb_rule_names rules = {
			.layout = c->kb_layout[0] ? c->kb_layout : NULL,
			.variant = c->kb_variant[0] ? c->kb_variant : NULL,
			.options = c->kb_options[0] ? c->kb_options : NULL,
		};
		struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &rules, XKB_KEYMAP_COMPILE_NO_FLAGS);
		if (!km) {
			wlr_log(WLR_ERROR, "bad keyboard layout, falling back to us");
			km = xkb_keymap_new_from_names(ctx, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
		}
		wlr_keyboard_set_keymap(kb, km);
		xkb_keymap_unref(km);
		xkb_context_unref(ctx);
		wlr_keyboard_set_repeat_info(kb, c->repeat_rate, c->repeat_delay);
	}
	struct lumen_keyboard *k = calloc(1, sizeof(*k));
	k->server = s;
	k->wlr_keyboard = kb;
	k->modifiers.notify = kb_modifiers;
	wl_signal_add(&kb->events.modifiers, &k->modifiers);
	k->key.notify = kb_key;
	wl_signal_add(&kb->events.key, &k->key);
	k->destroy.notify = kb_destroy;
	wl_signal_add(&kb->base.events.destroy, &k->destroy);
	wlr_seat_set_keyboard(s->seat, kb);
	wl_list_insert(&s->keyboards, &k->link);
	update_caps(s);
}

static void pointer_add(struct lumen_server *s, struct wlr_input_device *dev) {
	if (wlr_input_device_is_libinput(dev)) {
		struct libinput_device *li = wlr_libinput_get_device_handle(dev);
		if (libinput_device_config_tap_get_finger_count(li) > 0) {
			libinput_device_config_tap_set_enabled(li, LIBINPUT_CONFIG_TAP_ENABLED);
			libinput_device_config_dwt_set_enabled(li, LIBINPUT_CONFIG_DWT_ENABLED);
			if (libinput_device_config_scroll_has_natural_scroll(li))
				libinput_device_config_scroll_set_natural_scroll_enabled(li, true);
		}
	}
	wlr_cursor_attach_input_device(s->cursor, dev);
}

void input_new(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_input);
	struct wlr_input_device *dev = data;
	switch (dev->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		keyboard_add(s, wlr_keyboard_from_input_device(dev), true);
		break;
	case WLR_INPUT_DEVICE_POINTER:
	case WLR_INPUT_DEVICE_TOUCH:
	case WLR_INPUT_DEVICE_TABLET:
		pointer_add(s, dev);
		break;
	default:
		break;
	}
	update_caps(s);
}

static void new_virtual_keyboard(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *vk = data;
	/* the client supplies its own keymap */
	keyboard_add(s, &vk->keyboard, false);
}

static void new_virtual_pointer(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *ev = data;
	wlr_cursor_attach_input_device(s->cursor, &ev->new_pointer->pointer.base);
}

void focus_layer(struct lumen_server *s, struct wlr_layer_surface_v1 *ls) {
	if (!ls || !ls->surface->mapped || s->locked) return;
	s->focused_layer = ls;
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, ls->surface, kb->keycodes, kb->num_keycodes,
			&kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, ls->surface, NULL, 0, NULL);
}

/* ---------- hit testing ---------- */

struct hit {
	struct wlr_surface *surface;
	double sx, sy;
	struct lumen_view *view;   /* owner view if any */
	struct lumen_layer *layer; /* owner layer if any */
	bool deco;                 /* hit our titlebar/border */
};

static void hit_test(struct lumen_server *s, double lx, double ly, struct hit *h) {
	memset(h, 0, sizeof(*h));
	/* while locked only the lock surfaces exist as far as input goes */
	struct wlr_scene_node *root = s->locked ? &s->layer_lock->node : &s->scene->tree.node;
	struct wlr_scene_node *n = wlr_scene_node_at(root, lx, ly, &h->sx, &h->sy);
	if (!n) return;
	if (n->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_surface *ss =
			wlr_scene_surface_try_from_buffer(wlr_scene_buffer_from_node(n));
		if (ss) h->surface = ss->surface;
	}
	for (struct wlr_scene_node *p = n; p; p = p->parent ? &p->parent->node : NULL) {
		struct node_tag *t = p->data;
		if (!t) continue;
		if (t->kind == NODE_TITLEBAR) { h->view = t->owner; h->deco = true; break; }
		if (t->kind == NODE_VIEW) { h->view = t->owner; break; }
		if (t->kind == NODE_LAYER) { h->layer = t->owner; break; }
	}
	/* popups are parented into the view tree, so a surface hit inside a view
	 * that isn't the view's own surface is still fine to deliver */
}

static struct wlr_box view_outer(struct lumen_view *v) {
	int top = view_deco_top(v), side = view_deco_side(v);
	struct wlr_box g;
	wlr_xdg_surface_get_geometry(v->xdg->base, &g);
	int cw = g.width > 0 ? g.width : v->geo.width;
	int ch = g.height > 0 ? g.height : v->geo.height;
	return (struct wlr_box){v->geo.x - side, v->geo.y - top, cw + 2 * side, ch + top + side};
}

static uint32_t edges_at(struct lumen_view *v, double x, double y) {
	if (view_is_tiled(v) || v->maximized || v->fullscreen) return 0;
	struct wlr_box o = view_outer(v);
	uint32_t e = 0;
	if (x < o.x + RESIZE_MARGIN) e |= WLR_EDGE_LEFT;
	else if (x >= o.x + o.width - RESIZE_MARGIN) e |= WLR_EDGE_RIGHT;
	if (y < o.y + RESIZE_MARGIN) e |= WLR_EDGE_TOP;
	else if (y >= o.y + o.height - RESIZE_MARGIN) e |= WLR_EDGE_BOTTOM;
	return e;
}

static void set_hover(struct lumen_server *s, struct lumen_view *hv, enum title_button b) {
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link) {
		enum title_button want = (v == hv) ? b : TB_NONE;
		if (v->hover_button != want) {
			v->hover_button = want;
			v->title_dirty = true;
			view_update_deco(v);
		}
	}
}

/* ---------- cursor ---------- */

static void process_motion(struct lumen_server *s, uint32_t time) {
	struct wlr_cursor *c = s->cursor;
	wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
	struct lumen_view *v = s->grab_view;

	if (s->cursor_mode == CURSOR_MOVE && v) {
		int x = (int)(c->x - s->grab_x), y = (int)(c->y - s->grab_y);
		/* keep the titlebar reachable below the panel */
		struct lumen_output *o = output_at(s, c->x, c->y);
		if (o && y < o->usable.y) y = o->usable.y;
		v->float_geo.x = x;
		v->float_geo.y = y;
		wlr_scene_node_set_position(&v->tree->node, x, y);
		v->geo.x = x + view_deco_side(v);
		v->geo.y = y + view_deco_top(v);
		return;
	}
	if (s->cursor_mode == CURSOR_RESIZE && v) {
		struct wlr_box b = s->grab_box;
		double dx = c->x - s->grab_x, dy = c->y - s->grab_y;
		int left = b.x, right = b.x + b.width, top = b.y, bottom = b.y + b.height;
		uint32_t e = s->resize_edges;
		if (e & WLR_EDGE_LEFT) left += (int)dx;
		if (e & WLR_EDGE_RIGHT) right += (int)dx;
		if (e & WLR_EDGE_TOP) top += (int)dy;
		if (e & WLR_EDGE_BOTTOM) bottom += (int)dy;
		const int minw = 160, minh = 80;
		if (right - left < minw) { if (e & WLR_EDGE_LEFT) left = right - minw; else right = left + minw; }
		if (bottom - top < minh) { if (e & WLR_EDGE_TOP) top = bottom - minh; else bottom = top + minh; }
		v->float_geo = (struct wlr_box){left, top, right - left, bottom - top};
		view_set_geometry(v, left, top, right - left, bottom - top);
		return;
	}
	if (s->cursor_mode == CURSOR_PRESS_BUTTON) return;

	struct hit h;
	hit_test(s, c->x, c->y, &h);

	enum title_button hb = TB_NONE;
	const char *shape = "default";
	if (h.view && !h.surface) {
		uint32_t e = edges_at(h.view, c->x, c->y);
		if (e) shape = wlr_xcursor_get_resize_name(e);
		else if (h.deco) hb = deco_button_at(h.view, c->x, c->y);
	}
	set_hover(s, h.view, hb);

	if (h.surface) {
		wlr_seat_pointer_notify_enter(s->seat, h.surface, h.sx, h.sy);
		wlr_seat_pointer_notify_motion(s->seat, time, h.sx, h.sy);
	} else {
		wlr_cursor_set_xcursor(c, s->xcursor_mgr, shape);
		wlr_seat_pointer_clear_focus(s->seat);
	}
}

void cursor_rebase(struct lumen_server *s) {
	if (s->cursor_mode == CURSOR_PASSTHROUGH) process_motion(s, now_ms());
}

static void cursor_motion(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_motion);
	struct wlr_pointer_motion_event *ev = data;
	wlr_cursor_move(s->cursor, &ev->pointer->base, ev->delta_x, ev->delta_y);
	process_motion(s, ev->time_msec);
}

static void cursor_motion_abs(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_motion_abs);
	struct wlr_pointer_motion_absolute_event *ev = data;
	wlr_cursor_warp_absolute(s->cursor, &ev->pointer->base, ev->x, ev->y);
	process_motion(s, ev->time_msec);
}

static void end_grab(struct lumen_server *s) {
	s->cursor_mode = CURSOR_PASSTHROUGH;
	s->grab_view = NULL;
	cursor_rebase(s);
}

static void cursor_button(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_button);
	struct wlr_pointer_button_event *ev = data;
	struct wlr_cursor *c = s->cursor;
	wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
	if (s->locked) {
		wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
		return;
	}
	static uint32_t last_title_click;
	static struct lumen_view *last_title_view;

	if (ev->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (s->cursor_mode == CURSOR_PRESS_BUTTON) {
			struct lumen_view *v = s->grab_view;
			enum title_button b = s->pressed_button;
			s->pressed_button = TB_NONE;
			if (v && deco_button_at(v, c->x, c->y) == b) {
				if (b == TB_CLOSE) view_close(v);
				else if (b == TB_MAX) {
					if (view_is_tiled(v)) { v->floating = true; view_set_maximized(v, true); }
					else view_set_maximized(v, !v->maximized);
				} else if (b == TB_MIN) view_set_minimized(v, true);
			}
			end_grab(s);
			return;
		}
		if (s->cursor_mode != CURSOR_PASSTHROUGH) {
			end_grab(s);
			return;
		}
		wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
		return;
	}

	struct hit h;
	hit_test(s, c->x, c->y, &h);

	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	uint32_t mods = kb ? wlr_keyboard_get_modifiers(kb) : 0;

	if (h.view) {
		struct lumen_view *v = h.view;
		if (!h.surface || v->xdg->base->surface == h.surface ||
				wlr_surface_get_root_surface(h.surface) == v->xdg->base->surface)
			view_focus(v);
		/* Super + drag: left moves, right resizes (from the nearest corner) */
		if (mods & WLR_MODIFIER_LOGO) {
			if (ev->button == BTN_LEFT) { view_begin_interactive(v, CURSOR_MOVE, 0); return; }
			if (ev->button == BTN_RIGHT && !view_is_tiled(v)) {
				struct wlr_box o = view_outer(v);
				uint32_t e = (c->x < o.x + o.width / 2 ? WLR_EDGE_LEFT : WLR_EDGE_RIGHT) |
					(c->y < o.y + o.height / 2 ? WLR_EDGE_TOP : WLR_EDGE_BOTTOM);
				view_begin_interactive(v, CURSOR_RESIZE, e);
				wlr_cursor_set_xcursor(c, s->xcursor_mgr, wlr_xcursor_get_resize_name(e));
				return;
			}
		}
		if (!h.surface && ev->button == BTN_LEFT) {
			uint32_t e = edges_at(v, c->x, c->y);
			if (e) { view_begin_interactive(v, CURSOR_RESIZE, e); return; }
			if (h.deco) {
				enum title_button b = deco_button_at(v, c->x, c->y);
				if (b != TB_NONE) {
					s->cursor_mode = CURSOR_PRESS_BUTTON;
					s->pressed_button = b;
					s->grab_view = v;
					return;
				}
				uint32_t t = now_ms();
				if (last_title_view == v && t - last_title_click < DOUBLE_CLICK_MS) {
					last_title_view = NULL;
					if (view_is_tiled(v)) v->floating = true;
					view_set_maximized(v, !v->maximized);
					return;
				}
				last_title_click = t;
				last_title_view = v;
				view_begin_interactive(v, CURSOR_MOVE, 0);
				return;
			}
		}
		if (!h.surface && ev->button == BTN_MIDDLE && h.deco) {
			view_close(v);
			return;
		}
	} else if (h.layer) {
		struct wlr_layer_surface_v1 *ls = h.layer->ls;
		if (ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE)
			focus_layer(s, ls);
	}
	wlr_seat_pointer_notify_button(s->seat, ev->time_msec, ev->button, ev->state);
}

static void cursor_axis(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_axis);
	struct wlr_pointer_axis_event *ev = data;
	wlr_idle_notifier_v1_notify_activity(s->idle_notifier, s->seat);
	wlr_seat_pointer_notify_axis(s->seat, ev->time_msec, ev->orientation, ev->delta,
		ev->delta_discrete, ev->source, ev->relative_direction);
}

static void cursor_frame(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_frame);
	wlr_seat_pointer_notify_frame(s->seat);
}

static void request_cursor(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client &&
			s->cursor_mode == CURSOR_PASSTHROUGH)
		wlr_cursor_set_surface(s->cursor, ev->surface, ev->hotspot_x, ev->hotspot_y);
}

static void cursor_shape_request(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, cursor_shape_request);
	struct wlr_cursor_shape_manager_v1_request_set_shape_event *ev = data;
	if (s->seat->pointer_state.focused_client == ev->seat_client &&
			s->cursor_mode == CURSOR_PASSTHROUGH)
		wlr_cursor_set_xcursor(s->cursor, s->xcursor_mgr, wlr_cursor_shape_v1_name(ev->shape));
}

static void request_set_selection(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, request_set_selection);
	struct wlr_seat_request_set_selection_event *ev = data;
	wlr_seat_set_selection(s->seat, ev->source, ev->serial);
}

static void request_set_primary_selection(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, request_set_primary_selection);
	struct wlr_seat_request_set_primary_selection_event *ev = data;
	wlr_seat_set_primary_selection(s->seat, ev->source, ev->serial);
}

void input_init(struct lumen_server *s) {
	s->cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(s->cursor, s->output_layout);
	const char *theme = getenv("XCURSOR_THEME");
	const char *size_s = getenv("XCURSOR_SIZE");
	int size = size_s ? atoi(size_s) : 24;
	if (size <= 0) size = 24;
	s->xcursor_mgr = wlr_xcursor_manager_create(theme, size);

	s->cursor_motion.notify = cursor_motion;
	wl_signal_add(&s->cursor->events.motion, &s->cursor_motion);
	s->cursor_motion_abs.notify = cursor_motion_abs;
	wl_signal_add(&s->cursor->events.motion_absolute, &s->cursor_motion_abs);
	s->cursor_button.notify = cursor_button;
	wl_signal_add(&s->cursor->events.button, &s->cursor_button);
	s->cursor_axis.notify = cursor_axis;
	wl_signal_add(&s->cursor->events.axis, &s->cursor_axis);
	s->cursor_frame.notify = cursor_frame;
	wl_signal_add(&s->cursor->events.frame, &s->cursor_frame);

	s->seat = wlr_seat_create(s->display, "seat0");
	s->new_input.notify = input_new;
	wl_signal_add(&s->backend->events.new_input, &s->new_input);
	s->request_cursor.notify = request_cursor;
	wl_signal_add(&s->seat->events.request_set_cursor, &s->request_cursor);
	s->request_set_selection.notify = request_set_selection;
	wl_signal_add(&s->seat->events.request_set_selection, &s->request_set_selection);
	s->request_set_primary_selection.notify = request_set_primary_selection;
	wl_signal_add(&s->seat->events.request_set_primary_selection,
		&s->request_set_primary_selection);

	struct wlr_virtual_keyboard_manager_v1 *vkm = wlr_virtual_keyboard_manager_v1_create(s->display);
	s->new_virtual_keyboard.notify = new_virtual_keyboard;
	wl_signal_add(&vkm->events.new_virtual_keyboard, &s->new_virtual_keyboard);
	struct wlr_virtual_pointer_manager_v1 *vpm = wlr_virtual_pointer_manager_v1_create(s->display);
	s->new_virtual_pointer.notify = new_virtual_pointer;
	wl_signal_add(&vpm->events.new_virtual_pointer, &s->new_virtual_pointer);

	struct wlr_cursor_shape_manager_v1 *csm = wlr_cursor_shape_manager_v1_create(s->display, 1);
	s->cursor_shape_request.notify = cursor_shape_request;
	wl_signal_add(&csm->events.request_set_shape, &s->cursor_shape_request);
}
