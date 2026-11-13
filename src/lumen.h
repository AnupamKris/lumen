#ifndef LUMEN_H
#define LUMEN_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/backend.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_idle_notify_v1.h>
#include <wlr/types/wlr_session_lock_v1.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_server_decoration.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>
#include <scenefx/types/wlr_scene.h>
#include <scenefx/types/fx/corner_location.h>
#include <scenefx/types/fx/clipped_region.h>

#define MAX_WORKSPACES 9
#define MAX_BINDS 96
#define MAX_AUTOSTART 16

/* ---------- config ---------- */

struct lumen_bind {
	uint32_t mods;
	xkb_keysym_t sym;
	char action[256];
};

struct lumen_config {
	char terminal[256];
	char launcher[256];
	char autostart[MAX_AUTOSTART][512];
	int n_autostart;
	char font[128];
	char wallpaper[512];
	char kb_layout[64], kb_variant[64], kb_options[128];
	int repeat_rate, repeat_delay;

	int corner_radius;
	int border_width;
	int titlebar_height;
	int gaps;
	bool shadows;
	int shadow_size;
	bool blur;
	bool animations;
	bool default_tiling;
	float master_ratio;

	float accent[4];
	float bg[4];          /* wallpaper base when no image */
	float title_bg[4];    /* focused titlebar */
	float title_bg_inactive[4];
	float title_fg[4];
	float title_fg_inactive[4];
	float border[4];
	float border_inactive[4];
	float shadow_color[4];

	struct lumen_bind binds[MAX_BINDS];
	int n_binds;
};

void config_defaults(struct lumen_config *c);
void config_load(struct lumen_config *c, const char *path);
const char *config_path(void);

/* ---------- core types ---------- */

enum node_kind { NODE_VIEW, NODE_TITLEBAR, NODE_LAYER, NODE_POPUP };

/* Attached to scene_node->data so hit tests can find their owner. */
struct node_tag {
	enum node_kind kind;
	void *owner;
};

enum cursor_mode { CURSOR_PASSTHROUGH, CURSOR_MOVE, CURSOR_RESIZE, CURSOR_PRESS_BUTTON };

enum title_button { TB_NONE = 0, TB_MIN, TB_MAX, TB_CLOSE };

struct lumen_server {
	struct wl_display *display;
	struct wl_event_loop *loop;
	struct wlr_backend *backend;
	struct wlr_session *session;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;
	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;

	/* stacking layers, bottom → top */
	struct wlr_scene_tree *layer_bg, *layer_bottom, *layer_views, *layer_top,
		*layer_overlay, *layer_drag;
	struct wlr_scene_optimized_blur *blur_node;

	struct wlr_xdg_shell *xdg_shell;
	struct wlr_layer_shell_v1 *layer_shell;
	struct wlr_xdg_decoration_manager_v1 *deco_mgr;
	struct wlr_foreign_toplevel_manager_v1 *ftl_mgr;

	struct wl_list views;   /* lumen_view.link, front (focused) first */
	struct wl_list outputs; /* lumen_output.link */
	struct wl_list keyboards;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *xcursor_mgr;
	struct wlr_seat *seat;
	struct wlr_output_layout *output_layout;

	enum cursor_mode cursor_mode;
	struct lumen_view *grab_view;
	double grab_x, grab_y;
	struct wlr_box grab_box;
	uint32_t resize_edges;
	enum title_button pressed_button;

	struct wlr_layer_surface_v1 *focused_layer; /* exclusive keyboard grab */

	int ws;                                  /* active workspace, 0-based */
	uint32_t next_order;
	bool ws_tiling[MAX_WORKSPACES];
	float ws_master[MAX_WORKSPACES];

	struct lumen_config cfg;

	struct wl_event_source *anim_timer;

	/* session lock (lock.c) */
	struct wlr_scene_tree *layer_lock;
	struct wlr_scene_rect *lock_bg;
	struct wlr_session_lock_v1 *lock;
	bool locked;
	struct wl_listener lock_new, lock_new_surface, lock_unlock, lock_destroy;

	/* idle */
	struct wlr_idle_notifier_v1 *idle_notifier;
	struct wlr_idle_inhibit_manager_v1 *idle_inhibit;
	int n_inhibitors;
	struct wl_listener new_inhibitor;

	/* ipc */
	int ipc_fd;
	char ipc_path[256];
	struct wl_event_source *ipc_source;
	struct wl_list ipc_clients;

	struct wl_listener new_output, new_input, new_xdg_toplevel, new_xdg_popup,
		new_layer_surface, new_decoration, cursor_motion, cursor_motion_abs,
		cursor_button, cursor_axis, cursor_frame, request_cursor,
		request_set_selection, request_set_primary_selection, new_virtual_keyboard,
		new_virtual_pointer, cursor_shape_request, activation_request,
		layout_change, new_kde_decoration;
};

struct lumen_output {
	struct wl_list link;
	struct lumen_server *server;
	struct wlr_output *wlr_output;
	struct wlr_scene_output *scene_output;
	struct wlr_scene_buffer *wallpaper;
	struct wlr_box full, usable;
	struct wl_list layers[4]; /* lumen_layer.link per zwlr layer */
	struct wl_listener frame, request_state, destroy;
};

struct lumen_view {
	struct wl_list link;
	struct lumen_server *server;
	struct wlr_xdg_toplevel *xdg;
	struct wlr_scene_tree *tree;     /* positioned at outer frame origin */
	struct wlr_scene_tree *content;  /* xdg surface tree */
	struct wlr_scene_shadow *shadow;
	struct wlr_scene_rect *border;   /* 1px border, behind frame */
	struct wlr_scene_rect *frame;    /* titlebar bg + body */
	struct wlr_scene_buffer *title;  /* title text + buttons */
	struct node_tag tag, title_tag;

	struct wlr_xdg_toplevel_decoration_v1 *decoration;
	struct wl_listener deco_request_mode, deco_destroy;
	struct wlr_server_decoration *kde_deco;
	struct wl_listener kde_mode, kde_destroy;
	bool ssd;          /* we draw titlebar + border */
	bool mapped;

	int ws;
	bool floating;     /* floats even on a tiling workspace */
	bool maximized, fullscreen, minimized;
	struct wlr_box geo;       /* content box in layout coords (no deco) */
	struct wlr_box float_geo; /* remembered floating geometry */
	bool placed;
	uint32_t order;           /* stable tiling order */
	int title_w;              /* width the title buffer was rendered at */
	bool title_focused;
	bool title_dirty;         /* title text / hover changed */
	enum title_button hover_button;

	float alpha;              /* fade-in animation */
	uint32_t anim_start;

	struct wlr_foreign_toplevel_handle_v1 *ftl;

	struct wl_listener map, unmap, commit, destroy, request_move, request_resize,
		request_maximize, request_fullscreen, request_minimize, set_title, set_app_id,
		ftl_activate, ftl_close, ftl_minimize, ftl_maximize, ftl_fullscreen;
};

struct lumen_layer {
	struct wl_list link;
	struct lumen_server *server;
	struct lumen_output *output;
	struct wlr_layer_surface_v1 *ls;
	struct wlr_scene_layer_surface_v1 *scene;
	struct wlr_scene_tree *popups;
	struct node_tag tag;
	bool mapped;
	struct wl_listener map, unmap, commit, destroy, new_popup;
};

struct lumen_keyboard {
	struct wl_list link;
	struct lumen_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers, key, destroy;
};

/* view.c */
void view_init_toplevel(struct lumen_server *s, struct wlr_xdg_toplevel *xdg);
void view_focus(struct lumen_view *v);
void focus_clear(struct lumen_server *s);
void view_close(struct lumen_view *v);
void view_set_geometry(struct lumen_view *v, int x, int y, int w, int h);
void view_update_deco(struct lumen_view *v);
void view_set_maximized(struct lumen_view *v, bool on);
void view_set_fullscreen(struct lumen_view *v, bool on);
void view_set_minimized(struct lumen_view *v, bool on);
void view_begin_interactive(struct lumen_view *v, enum cursor_mode mode, uint32_t edges);
int view_deco_top(struct lumen_view *v);
int view_deco_side(struct lumen_view *v);
struct lumen_view *view_focused(struct lumen_server *s);
void xdg_popup_new(struct wl_listener *l, void *data);
void decoration_new(struct wl_listener *l, void *data);
void kde_decoration_new(struct wl_listener *l, void *data);
void view_apply_effects(struct lumen_view *v);
bool views_animate(struct lumen_server *s, uint32_t now_ms);

/* layout.c */
void arrange_workspace(struct lumen_server *s, int ws);
void arrange_all(struct lumen_server *s);
void workspace_switch(struct lumen_server *s, int ws);
void view_move_to_workspace(struct lumen_view *v, int ws);
struct lumen_output *output_primary(struct lumen_server *s);
struct lumen_output *output_at(struct lumen_server *s, double x, double y);
bool view_is_tiled(struct lumen_view *v);
void focus_cycle(struct lumen_server *s, int dir);
void tile_swap_master(struct lumen_server *s);
void restack(struct lumen_server *s);

/* layer.c */
void layer_surface_new(struct wl_listener *l, void *data);
void arrange_layers(struct lumen_output *o);

/* output.c */
void output_new(struct wl_listener *l, void *data);
void output_render_wallpaper(struct lumen_output *o);
void schedule_frames(struct lumen_server *s);

/* input.c */
void input_init(struct lumen_server *s);
void input_new(struct wl_listener *l, void *data);
void cursor_rebase(struct lumen_server *s);
void focus_layer(struct lumen_server *s, struct wlr_layer_surface_v1 *ls);
uint32_t now_ms(void);

/* deco.c */
struct wlr_buffer *deco_render_title(struct lumen_view *v, int w, int h);
enum title_button deco_button_at(struct lumen_view *v, double lx, double ly);
struct wlr_buffer *deco_render_wallpaper(struct lumen_server *s, int w, int h);

/* actions.c */
void run_action(struct lumen_server *s, const char *action);
void spawn(const char *cmd);

/* lock.c */
void lock_init(struct lumen_server *s);
void lock_update_backdrop(struct lumen_server *s);

/* ipc.c */
bool ipc_init(struct lumen_server *s);
void ipc_broadcast_state(struct lumen_server *s);
void ipc_finish(struct lumen_server *s);
int ipc_client_main(const char *msg);

#endif
