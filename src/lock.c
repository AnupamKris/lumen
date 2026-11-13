#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/util/log.h>
#include "lumen.h"

/*
 * ext-session-lock-v1 (what swaylock speaks).
 *
 * While locked, an opaque backdrop covers every output and only lock
 * surfaces receive input; keybindings are off (except VT switching). If the
 * locker crashes without unlocking we stay locked, keep the backdrop up and
 * accept a new locker, which is the behaviour the protocol requires.
 */

struct lock_surface {
	struct lumen_server *server;
	struct wlr_session_lock_surface_v1 *ls;
	struct wlr_scene_tree *tree;
	struct wl_listener map, destroy;
};

static void lock_focus(struct lumen_server *s, struct wlr_surface *surf) {
	struct wlr_keyboard *kb = wlr_seat_get_keyboard(s->seat);
	if (kb)
		wlr_seat_keyboard_notify_enter(s->seat, surf, kb->keycodes, kb->num_keycodes, &kb->modifiers);
	else
		wlr_seat_keyboard_notify_enter(s->seat, surf, NULL, 0, NULL);
}

void lock_update_backdrop(struct lumen_server *s) {
	if (!s->lock_bg) return;
	struct wlr_box b;
	wlr_output_layout_get_box(s->output_layout, NULL, &b);
	wlr_scene_node_set_position(&s->lock_bg->node, b.x, b.y);
	wlr_scene_rect_set_size(s->lock_bg, b.width, b.height);
	/* also reconfigure lock surfaces to their outputs' current size */
	if (!s->lock) return;
	struct wlr_session_lock_surface_v1 *ls;
	wl_list_for_each(ls, &s->lock->surfaces, link) {
		struct wlr_box ob;
		wlr_output_layout_get_box(s->output_layout, ls->output, &ob);
		if (ob.width > 0) wlr_session_lock_surface_v1_configure(ls, ob.width, ob.height);
		struct lock_surface *l = ls->data;
		if (l) wlr_scene_node_set_position(&l->tree->node, ob.x, ob.y);
	}
}

static void surface_map(struct wl_listener *l, void *data) {
	struct lock_surface *ls = wl_container_of(l, ls, map);
	struct lumen_server *s = ls->server;
	struct wlr_surface *cur = s->seat->keyboard_state.focused_surface;
	if (!cur || !wlr_session_lock_surface_v1_try_from_wlr_surface(cur))
		lock_focus(s, ls->ls->surface);
	cursor_rebase(s);
}

static void surface_destroy(struct wl_listener *l, void *data) {
	struct lock_surface *ls = wl_container_of(l, ls, destroy);
	struct lumen_server *s = ls->server;
	wl_list_remove(&ls->map.link);
	wl_list_remove(&ls->destroy.link);
	ls->ls->data = NULL;
	/* hand the keyboard to another lock surface if this one had it */
	if (s->seat->keyboard_state.focused_surface == ls->ls->surface) {
		wlr_seat_keyboard_clear_focus(s->seat);
		if (s->lock) {
			struct wlr_session_lock_surface_v1 *o;
			wl_list_for_each(o, &s->lock->surfaces, link)
				if (o != ls->ls && o->surface->mapped) { lock_focus(s, o->surface); break; }
		}
	}
	free(ls);
}

static void new_surface(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, lock_new_surface);
	struct wlr_session_lock_surface_v1 *wls = data;
	struct lock_surface *ls = calloc(1, sizeof(*ls));
	ls->server = s;
	ls->ls = wls;
	wls->data = ls;
	ls->tree = wlr_scene_subsurface_tree_create(s->layer_lock, wls->surface);
	wls->surface->data = ls->tree;
	ls->map.notify = surface_map;
	wl_signal_add(&wls->surface->events.map, &ls->map);
	ls->destroy.notify = surface_destroy;
	wl_signal_add(&wls->events.destroy, &ls->destroy);
	struct wlr_box ob;
	wlr_output_layout_get_box(s->output_layout, wls->output, &ob);
	wlr_scene_node_set_position(&ls->tree->node, ob.x, ob.y);
	wlr_session_lock_surface_v1_configure(wls, ob.width, ob.height);
}

static void lock_detach(struct lumen_server *s) {
	wl_list_remove(&s->lock_new_surface.link);
	wl_list_remove(&s->lock_unlock.link);
	wl_list_remove(&s->lock_destroy.link);
	s->lock = NULL;
}

static void lock_unlock(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, lock_unlock);
	lock_detach(s);
	s->locked = false;
	if (s->lock_bg) {
		wlr_scene_node_destroy(&s->lock_bg->node);
		s->lock_bg = NULL;
	}
	wlr_seat_keyboard_clear_focus(s->seat);
	struct lumen_view *v = view_focused(s);
	if (v) view_focus(v);
	struct lumen_output *o;
	wl_list_for_each(o, &s->outputs, link) arrange_layers(o);
	cursor_rebase(s);
	wlr_log(WLR_INFO, "session unlocked");
}

static void lock_destroy(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, lock_destroy);
	/* locker went away without unlocking: stay locked */
	lock_detach(s);
	wlr_seat_keyboard_clear_focus(s->seat);
	wlr_log(WLR_ERROR, "locker died while locked; session stays locked");
}

static void new_lock(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, lock_new);
	struct wlr_session_lock_v1 *lock = data;
	if (s->lock) {
		wlr_session_lock_v1_destroy(lock);
		return;
	}
	s->lock = lock;
	s->locked = true;
	if (!s->lock_bg) {
		const float c[4] = {0.11f, 0.114f, 0.13f, 1.0f};
		s->lock_bg = wlr_scene_rect_create(s->layer_lock, 0, 0, c);
		wlr_scene_node_lower_to_bottom(&s->lock_bg->node);
	}
	lock_update_backdrop(s);

	/* end any grab and take input away from everything else */
	s->cursor_mode = CURSOR_PASSTHROUGH;
	s->grab_view = NULL;
	s->focused_layer = NULL;
	wlr_seat_keyboard_clear_focus(s->seat);
	wlr_seat_pointer_clear_focus(s->seat);

	s->lock_new_surface.notify = new_surface;
	wl_signal_add(&lock->events.new_surface, &s->lock_new_surface);
	s->lock_unlock.notify = lock_unlock;
	wl_signal_add(&lock->events.unlock, &s->lock_unlock);
	s->lock_destroy.notify = lock_destroy;
	wl_signal_add(&lock->events.destroy, &s->lock_destroy);
	wlr_session_lock_v1_send_locked(lock);
	wlr_log(WLR_INFO, "session locked");
}

void lock_init(struct lumen_server *s) {
	s->layer_lock = wlr_scene_tree_create(&s->scene->tree);
	struct wlr_session_lock_manager_v1 *m = wlr_session_lock_manager_v1_create(s->display);
	s->lock_new.notify = new_lock;
	wl_signal_add(&m->events.new_lock, &s->lock_new);
}
