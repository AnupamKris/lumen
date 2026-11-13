#define _POSIX_C_SOURCE 200809L
#include <stdlib.h>
#include <string.h>
#include <wlr/util/log.h>
#include "lumen.h"

static struct wlr_scene_tree *tree_for_layer(struct lumen_server *s, enum zwlr_layer_shell_v1_layer l) {
	switch (l) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND: return s->layer_bg;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM: return s->layer_bottom;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP: return s->layer_top;
	default: return s->layer_overlay;
	}
}

static void arrange_pass(struct lumen_output *o, bool exclusive, struct wlr_box *usable) {
	for (int i = 3; i >= 0; i--) {
		struct lumen_layer *ly;
		wl_list_for_each(ly, &o->layers[i], link) {
			struct wlr_layer_surface_v1 *ls = ly->ls;
			if (!ls->initialized) continue;
			if ((ls->current.exclusive_zone > 0) != exclusive) continue;
			wlr_scene_layer_surface_v1_configure(ly->scene, &o->full, usable);
			wlr_scene_node_set_position(&ly->popups->node,
				ly->scene->tree->node.x, ly->scene->tree->node.y);
		}
	}
}

void arrange_layers(struct lumen_output *o) {
	struct lumen_server *s = o->server;
	wlr_output_layout_get_box(s->output_layout, o->wlr_output, &o->full);
	struct wlr_box usable = o->full;
	arrange_pass(o, true, &usable);
	arrange_pass(o, false, &usable);
	bool changed = memcmp(&usable, &o->usable, sizeof(usable)) != 0;
	o->usable = usable;
	if (changed) arrange_all(s);

	/* Keyboard: the topmost exclusive layer surface takes focus. */
	struct wlr_layer_surface_v1 *grab = NULL;
	for (int i = 3; i >= 2 && !grab; i--) {
		struct lumen_layer *ly;
		wl_list_for_each(ly, &o->layers[i], link) {
			if (ly->mapped && ly->ls->current.keyboard_interactive ==
					ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE) {
				grab = ly->ls;
				break;
			}
		}
	}
	if (grab) focus_layer(s, grab);
}

static void layer_map(struct wl_listener *l, void *data) {
	struct lumen_layer *ly = wl_container_of(l, ly, map);
	ly->mapped = true;
	if (ly->output) arrange_layers(ly->output);
	if (ly->ls->current.keyboard_interactive != ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE &&
			ly->ls->current.layer >= ZWLR_LAYER_SHELL_V1_LAYER_TOP)
		focus_layer(ly->server, ly->ls);
	cursor_rebase(ly->server);
}

static void layer_unmap(struct wl_listener *l, void *data) {
	struct lumen_layer *ly = wl_container_of(l, ly, unmap);
	struct lumen_server *s = ly->server;
	ly->mapped = false;
	if (s->focused_layer == ly->ls) {
		s->focused_layer = NULL;
		struct lumen_view *v = view_focused(s);
		if (v) view_focus(v);
		else wlr_seat_keyboard_clear_focus(s->seat);
	}
	if (ly->output) arrange_layers(ly->output);
	cursor_rebase(s);
}

static void layer_commit(struct wl_listener *l, void *data) {
	struct lumen_layer *ly = wl_container_of(l, ly, commit);
	struct wlr_layer_surface_v1 *ls = ly->ls;
	if (!ly->output) return;
	struct wlr_scene_tree *want = tree_for_layer(ly->server, ls->current.layer);
	if (ly->scene->tree->node.parent != want) {
		wlr_scene_node_reparent(&ly->scene->tree->node, want);
		wl_list_remove(&ly->link);
		wl_list_insert(&ly->output->layers[ls->current.layer], &ly->link);
	}
	if (ls->initial_commit || ls->current.committed) arrange_layers(ly->output);
}

static void layer_destroy(struct wl_listener *l, void *data) {
	struct lumen_layer *ly = wl_container_of(l, ly, destroy);
	struct lumen_server *s = ly->server;
	if (s->focused_layer == ly->ls) s->focused_layer = NULL;
	wl_list_remove(&ly->link);
	wl_list_remove(&ly->map.link);
	wl_list_remove(&ly->unmap.link);
	wl_list_remove(&ly->commit.link);
	wl_list_remove(&ly->destroy.link);
	wlr_scene_node_destroy(&ly->popups->node);
	struct lumen_output *o = ly->output;
	free(ly);
	if (o) arrange_layers(o);
}

void layer_surface_new(struct wl_listener *l, void *data) {
	struct lumen_server *s = wl_container_of(l, s, new_layer_surface);
	struct wlr_layer_surface_v1 *ls = data;
	if (!ls->output) {
		struct lumen_output *o = output_at(s, s->cursor->x, s->cursor->y);
		if (!o) o = output_primary(s);
		if (!o) { wlr_layer_surface_v1_destroy(ls); return; }
		ls->output = o->wlr_output;
	}
	struct lumen_layer *ly = calloc(1, sizeof(*ly));
	ly->server = s;
	ly->ls = ls;
	ly->output = ls->output->data;
	ly->scene = wlr_scene_layer_surface_v1_create(tree_for_layer(s, ls->pending.layer), ls);
	ly->popups = wlr_scene_tree_create(s->layer_overlay);
	ly->tag = (struct node_tag){NODE_LAYER, ly};
	ly->scene->tree->node.data = &ly->tag;
	ls->surface->data = ly->popups; /* popups of this surface parent here */
	ls->data = ly;

	ly->map.notify = layer_map;
	wl_signal_add(&ls->surface->events.map, &ly->map);
	ly->unmap.notify = layer_unmap;
	wl_signal_add(&ls->surface->events.unmap, &ly->unmap);
	ly->commit.notify = layer_commit;
	wl_signal_add(&ls->surface->events.commit, &ly->commit);
	ly->destroy.notify = layer_destroy;
	wl_signal_add(&ls->events.destroy, &ly->destroy);
	wl_list_insert(&ly->output->layers[ls->pending.layer], &ly->link);
}
