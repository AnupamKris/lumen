/*
 * lumen-panel — the Lumen top bar.
 *
 *   [launcher] [workspaces] [window list ......]     [battery] [clock] [power]
 *
 * A single layer-shell surface drawn with cairo/pango into wl_shm buffers.
 * It only redraws when something changes (workspace state from the
 * compositor's IPC socket, window list from wlr-foreign-toplevel, the clock
 * minute, battery, pointer hover), so it sits idle at ~0% CPU.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <linux/input-event-codes.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#include <cairo.h>
#include <pango/pangocairo.h>
#include <wayland-client.h>
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include "cursor-shape-v1-client-protocol.h"

#define BAR_H 34
#define PAD 8
#define CHIP_H 24
#define TASK_MAX_W 220
#define FONT "Inter, sans-serif 10"

/* theme (matches the compositor defaults) */
#define C_BG        0x17181c, 1.00
#define C_LINE      0xffffff, 0.06
#define C_TEXT      0xc9cbd1, 1.00
#define C_TEXT_HI   0xf2f2f4, 1.00
#define C_TEXT_DIM  0x6b6e75, 1.00
#define C_HOVER     0xffffff, 0.07
#define C_ACTIVE    0x2e3036, 1.00
#define C_ACCENT    0x8ab4f8, 1.00
#define C_WARN      0xf28b82, 1.00
#define C_DOT_EMPTY 0xffffff, 0.22

/* ---------- state ---------- */

struct toplevel {
	struct wl_list link;
	struct zwlr_foreign_toplevel_handle_v1 *h;
	char *title, *app_id;
	bool activated, minimized;
};

enum hit_kind { HIT_NONE, HIT_LAUNCHER, HIT_WS, HIT_TASK, HIT_POWER };
struct hit {
	enum hit_kind kind;
	int x0, x1, idx;
	struct toplevel *tl;
};

struct buffer {
	struct wl_buffer *wl;
	void *data;
	int w, h, size;
	bool busy;
};

static struct {
	struct wl_display *display;
	struct wl_compositor *compositor;
	struct wl_shm *shm;
	struct wl_seat *seat;
	struct wl_pointer *pointer;
	struct zwlr_layer_shell_v1 *layer_shell;
	struct zwlr_foreign_toplevel_manager_v1 *ftl;
	struct wp_cursor_shape_manager_v1 *cursor_shape;
	struct wp_cursor_shape_device_v1 *cursor_dev;

	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_callback *frame_cb;
	struct buffer bufs[2];
	int width, scale;
	bool configured, dirty, running;

	struct wl_list toplevels;

	/* workspace state from IPC */
	int ws_active;      /* 1-based */
	unsigned ws_occupied;
	bool ws_tiling;
	int ipc_fd;
	char ipc_buf[256];
	size_t ipc_len;

	/* clock / battery */
	char clock[64];
	int bat_pct;        /* -1 = no battery */
	bool bat_charging;

	/* pointer */
	double px;
	bool pointer_in;
	int hover;          /* index into hits, -1 none */
	struct hit hits[64];
	int n_hits;
} P = { .scale = 1, .hover = -1, .bat_pct = -1, .ipc_fd = -1, .ws_active = 1 };

/* ---------- helpers ---------- */

static void set_rgba(cairo_t *cr, unsigned hex, double a) {
	cairo_set_source_rgba(cr, ((hex >> 16) & 0xff) / 255.0, ((hex >> 8) & 0xff) / 255.0,
		(hex & 0xff) / 255.0, a);
}

static void rounded(cairo_t *cr, double x, double y, double w, double h, double r) {
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
	cairo_close_path(cr);
}

static char *xstrdup(const char *s) { return strdup(s ? s : ""); }

/* ---------- IPC with the compositor ---------- */

static const char *sock_path(void) {
	static char p[256];
	const char *env = getenv("LUMEN_SOCK");
	if (env) return env;
	const char *rt = getenv("XDG_RUNTIME_DIR");
	const char *wd = getenv("WAYLAND_DISPLAY");
	snprintf(p, sizeof(p), "%s/lumen-%s.sock", rt ? rt : "/tmp", wd ? wd : "wayland-0");
	return p;
}

static int ipc_connect(void) {
	int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (fd < 0) return -1;
	struct sockaddr_un a = {.sun_family = AF_UNIX};
	snprintf(a.sun_path, sizeof(a.sun_path), "%s", sock_path());
	if (connect(fd, (struct sockaddr *)&a, sizeof(a)) < 0) { close(fd); return -1; }
	return fd;
}

static void ipc_send(const char *msg) {
	int fd = ipc_connect();
	if (fd < 0) return;
	dprintf(fd, "%s\n", msg);
	char tmp[16];
	(void)!read(fd, tmp, sizeof(tmp)); /* wait for "ok" so the action has run */
	close(fd);
}

static void ipc_subscribe(void) {
	P.ipc_fd = ipc_connect();
	if (P.ipc_fd >= 0) dprintf(P.ipc_fd, "subscribe\n");
	else fprintf(stderr, "lumen-panel: no compositor IPC at %s (workspaces hidden)\n", sock_path());
}

static void ipc_read(void) {
	ssize_t r = read(P.ipc_fd, P.ipc_buf + P.ipc_len, sizeof(P.ipc_buf) - 1 - P.ipc_len);
	if (r <= 0) { close(P.ipc_fd); P.ipc_fd = -1; return; }
	P.ipc_len += r;
	P.ipc_buf[P.ipc_len] = 0;
	char *nl;
	while ((nl = strchr(P.ipc_buf, '\n'))) {
		*nl = 0;
		int a, t;
		unsigned occ;
		if (sscanf(P.ipc_buf, "ws %d %u %d", &a, &occ, &t) == 3) {
			if (a != P.ws_active || occ != P.ws_occupied || (t != 0) != P.ws_tiling) P.dirty = true;
			P.ws_active = a;
			P.ws_occupied = occ;
			P.ws_tiling = t != 0;
		}
		size_t used = nl + 1 - P.ipc_buf;
		memmove(P.ipc_buf, nl + 1, P.ipc_len - used + 1);
		P.ipc_len -= used;
	}
	if (P.ipc_len >= sizeof(P.ipc_buf) - 1) P.ipc_len = 0;
}

/* ---------- clock + battery ---------- */

static void update_clock(void) {
	char buf[64];
	time_t t = time(NULL);
	struct tm tm;
	localtime_r(&t, &tm);
	strftime(buf, sizeof(buf), "%a %d %b   %H:%M", &tm);
	if (strcmp(buf, P.clock)) { snprintf(P.clock, sizeof(P.clock), "%s", buf); P.dirty = true; }
}

static int read_int(const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int v = -1;
	if (fscanf(f, "%d", &v) != 1) v = -1;
	fclose(f);
	return v;
}

static void update_battery(void) {
	glob_t g;
	int pct = -1;
	bool chg = false;
	if (glob("/sys/class/power_supply/BAT*/capacity", 0, NULL, &g) == 0 && g.gl_pathc > 0) {
		pct = read_int(g.gl_pathv[0]);
		char st[256];
		snprintf(st, sizeof(st), "%s", g.gl_pathv[0]);
		char *slash = strrchr(st, '/');
		if (slash) {
			strcpy(slash + 1, "status");
			FILE *f = fopen(st, "r");
			if (f) {
				char s[32] = {0};
				if (fgets(s, sizeof(s), f)) chg = !strncmp(s, "Charging", 8) || !strncmp(s, "Full", 4);
				fclose(f);
			}
		}
	}
	globfree(&g);
	/* AC adapter online also counts as charging (some firmwares say "Not charging") */
	if (pct >= 0 && !chg && glob("/sys/class/power_supply/A*/online", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) if (read_int(g.gl_pathv[i]) == 1) chg = true;
		globfree(&g);
	}
	if (pct != P.bat_pct || chg != P.bat_charging) P.dirty = true;
	P.bat_pct = pct;
	P.bat_charging = chg;
}

static void arm_timer(int tfd) {
	/* wake at the next minute boundary, or in 30 s for the battery */
	struct timespec now;
	clock_gettime(CLOCK_REALTIME, &now);
	time_t next = (now.tv_sec / 60 + 1) * 60;
	if (next - now.tv_sec > 30) next = now.tv_sec + 30;
	struct itimerspec its = {.it_value = {.tv_sec = next, .tv_nsec = 0}};
	timerfd_settime(tfd, TFD_TIMER_ABSTIME, &its, NULL);
}

/* ---------- shm buffers ---------- */

static void buffer_release(void *data, struct wl_buffer *b) { ((struct buffer *)data)->busy = false; }
static const struct wl_buffer_listener buffer_listener = { .release = buffer_release };

static void buffer_destroy(struct buffer *b) {
	if (b->wl) wl_buffer_destroy(b->wl);
	if (b->data) munmap(b->data, b->size);
	memset(b, 0, sizeof(*b));
}

static struct buffer *get_buffer(int w, int h) {
	struct buffer *b = NULL;
	for (int i = 0; i < 2; i++) if (!P.bufs[i].busy) { b = &P.bufs[i]; break; }
	if (!b) return NULL;
	if (b->wl && (b->w != w || b->h != h)) buffer_destroy(b);
	if (b->wl) return b;
	int stride = w * 4, size = stride * h;
	int fd = memfd_create("lumen-panel", MFD_CLOEXEC);
	if (fd < 0 || ftruncate(fd, size) < 0) { if (fd >= 0) close(fd); return NULL; }
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) { close(fd); return NULL; }
	struct wl_shm_pool *pool = wl_shm_create_pool(P.shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	b->data = data;
	b->w = w;
	b->h = h;
	b->size = size;
	wl_buffer_add_listener(b->wl, &buffer_listener, b);
	return b;
}

/* ---------- drawing ---------- */

static void add_hit(enum hit_kind k, double x0, double x1, int idx, struct toplevel *tl) {
	if (P.n_hits >= (int)(sizeof(P.hits) / sizeof(P.hits[0]))) return;
	P.hits[P.n_hits++] = (struct hit){k, (int)x0, (int)x1, idx, tl};
}

static bool hovered(enum hit_kind k, int idx, struct toplevel *tl) {
	if (P.hover < 0 || P.hover >= P.n_hits) return false;
	struct hit *h = &P.hits[P.hover];
	return h->kind == k && h->idx == idx && h->tl == tl;
}

static double text_width(PangoLayout *l, const char *s) {
	pango_layout_set_width(l, -1);
	pango_layout_set_text(l, s, -1);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	return w;
}

static void draw_text(cairo_t *cr, PangoLayout *l, const char *s, double x, double maxw) {
	pango_layout_set_text(l, s, -1);
	pango_layout_set_width(l, maxw > 0 ? (int)(maxw * PANGO_SCALE) : -1);
	pango_layout_set_ellipsize(l, PANGO_ELLIPSIZE_END);
	int w, h;
	pango_layout_get_pixel_size(l, &w, &h);
	cairo_move_to(cr, x, (BAR_H - h) / 2.0);
	pango_cairo_show_layout(cr, l);
}

/* hover/pressed background for a button-like region; returns x after it */
static void chip_bg(cairo_t *cr, double x, double w, bool hover) {
	if (!hover) return;
	set_rgba(cr, C_HOVER);
	rounded(cr, x, (BAR_H - CHIP_H) / 2.0, w, CHIP_H, 7);
	cairo_fill(cr);
}

static double draw_launcher(cairo_t *cr, double x) {
	const double w = 30;
	chip_bg(cr, x, w, hovered(HIT_LAUNCHER, 0, NULL));
	/* 3x3 dot grid */
	set_rgba(cr, C_TEXT);
	double cx = x + w / 2, cy = BAR_H / 2.0;
	for (int i = -1; i <= 1; i++)
		for (int j = -1; j <= 1; j++) {
			cairo_arc(cr, cx + i * 5, cy + j * 5, 1.4, 0, 2 * M_PI);
			cairo_fill(cr);
		}
	add_hit(HIT_LAUNCHER, x, x + w, 0, NULL);
	return x + w;
}

static double draw_workspaces(cairo_t *cr, double x) {
	int last = 3; /* always show at least four */
	for (int i = 0; i < 9; i++) if (P.ws_occupied & (1u << i)) last = i > last ? i : last;
	if (P.ws_active - 1 > last) last = P.ws_active - 1;
	x += 6;
	for (int i = 0; i <= last; i++) {
		bool active = i == P.ws_active - 1;
		bool occ = P.ws_occupied & (1u << i);
		double w = active ? 20 : 7, slot = w + 10;
		bool hv = hovered(HIT_WS, i, NULL);
		double cy = BAR_H / 2.0;
		if (active) set_rgba(cr, C_ACCENT);
		else if (occ) set_rgba(cr, hv ? 0xf2f2f4 : 0xc9cbd1, hv ? 1.0 : 0.85);
		else set_rgba(cr, 0xffffff, hv ? 0.45 : 0.22);
		rounded(cr, x + 5, cy - 3.5, w, 7, 3.5);
		cairo_fill(cr);
		add_hit(HIT_WS, x, x + slot, i, NULL);
		x += slot;
	}
	return x;
}

static void draw_tasks(cairo_t *cr, PangoLayout *l, double x, double xmax) {
	int n = wl_list_length(&P.toplevels);
	if (n == 0 || xmax - x < 40) return;
	const double gap = 4;
	double w = (xmax - x - gap * (n - 1)) / n;
	if (w > TASK_MAX_W) w = TASK_MAX_W;
	if (w < 36) w = 36;
	struct toplevel *t;
	wl_list_for_each(t, &P.toplevels, link) {
		if (x + w > xmax) break;
		bool active = t->activated && !t->minimized;
		bool hv = hovered(HIT_TASK, 0, t);
		double y = (BAR_H - CHIP_H) / 2.0;
		if (active || hv) {
			if (active) set_rgba(cr, C_ACTIVE); else set_rgba(cr, C_HOVER);
			rounded(cr, x, y, w, CHIP_H, 7);
			cairo_fill(cr);
		}
		if (active) {
			set_rgba(cr, C_ACCENT);
			rounded(cr, x + w / 2 - 8, y + CHIP_H - 3, 16, 2, 1);
			cairo_fill(cr);
		}
		const char *label = t->title && *t->title ? t->title : t->app_id;
		if (t->minimized) set_rgba(cr, C_TEXT_DIM);
		else if (active) set_rgba(cr, C_TEXT_HI);
		else set_rgba(cr, C_TEXT);
		draw_text(cr, l, label ? label : "", x + 10, w - 20);
		add_hit(HIT_TASK, x, x + w, 0, t);
		x += w + gap;
	}
}

static void draw_power_icon(cairo_t *cr, double cx, double cy) {
	cairo_set_line_width(cr, 1.6);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	cairo_new_sub_path(cr);
	cairo_arc(cr, cx, cy + 0.5, 6, -M_PI / 2 + 0.75, 3 * M_PI / 2 - 0.75);
	cairo_stroke(cr);
	cairo_move_to(cr, cx, cy - 6.5);
	cairo_line_to(cr, cx, cy - 0.5);
	cairo_stroke(cr);
}

static double draw_battery(cairo_t *cr, PangoLayout *l, double xr) {
	/* draws right-aligned ending at xr, returns the new left edge */
	char txt[16];
	snprintf(txt, sizeof(txt), "%d%%", P.bat_pct);
	double tw = text_width(l, txt);
	double x = xr - tw;
	set_rgba(cr, C_TEXT);
	draw_text(cr, l, txt, x, -1);
	double bw = 20, bh = 10, bx = x - 6 - bw - 2, by = (BAR_H - bh) / 2.0;
	set_rgba(cr, 0xc9cbd1, 0.9);
	cairo_set_line_width(cr, 1.2);
	rounded(cr, bx + 0.6, by + 0.6, bw - 1.2, bh - 1.2, 2.5);
	cairo_stroke(cr);
	cairo_rectangle(cr, bx + bw, by + 3, 2, bh - 6);
	cairo_fill(cr);
	double fill = (bw - 4) * (P.bat_pct < 0 ? 0 : P.bat_pct > 100 ? 100 : P.bat_pct) / 100.0;
	if (P.bat_charging) set_rgba(cr, C_ACCENT);
	else if (P.bat_pct <= 15) set_rgba(cr, C_WARN);
	else set_rgba(cr, C_TEXT);
	rounded(cr, bx + 2, by + 2, fill > 1.5 ? fill : 1.5, bh - 4, 1.2);
	cairo_fill(cr);
	return bx;
}

static void render(void) {
	if (!P.configured || P.width <= 0 || P.frame_cb) return;
	int s = P.scale;
	struct buffer *b = get_buffer(P.width * s, BAR_H * s);
	if (!b) return; /* both busy; release will come */
	P.dirty = false;
	P.n_hits = 0;

	cairo_surface_t *cs = cairo_image_surface_create_for_data(b->data, CAIRO_FORMAT_ARGB32,
		b->w, b->h, b->w * 4);
	cairo_t *cr = cairo_create(cs);
	cairo_scale(cr, s, s);
	cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
	set_rgba(cr, C_BG);
	cairo_paint(cr);
	cairo_set_operator(cr, CAIRO_OPERATOR_OVER);
	set_rgba(cr, C_LINE);
	cairo_rectangle(cr, 0, BAR_H - 1, P.width, 1);
	cairo_fill(cr);

	PangoLayout *l = pango_cairo_create_layout(cr);
	PangoFontDescription *fd = pango_font_description_from_string(FONT);
	pango_layout_set_font_description(l, fd);
	pango_font_description_free(fd);
	pango_layout_set_single_paragraph_mode(l, TRUE);

	/* right side first, so the window list knows how much room it has */
	double xr = P.width - PAD;
	const double pw = 30;
	double px0 = xr - pw;
	chip_bg(cr, px0, pw, hovered(HIT_POWER, 0, NULL));
	set_rgba(cr, C_TEXT);
	draw_power_icon(cr, px0 + pw / 2, BAR_H / 2.0);
	add_hit(HIT_POWER, px0, xr, 0, NULL);
	xr = px0 - 10;

	double cw = text_width(l, P.clock);
	set_rgba(cr, C_TEXT_HI);
	draw_text(cr, l, P.clock, xr - cw, -1);
	xr -= cw + 18;

	if (P.bat_pct >= 0) xr = draw_battery(cr, l, xr) - 18;

	double x = PAD;
	x = draw_launcher(cr, x);
	x = draw_workspaces(cr, x);
	x += 12;
	draw_tasks(cr, l, x, xr);

	g_object_unref(l);
	cairo_destroy(cr);
	cairo_surface_flush(cs);
	cairo_surface_destroy(cs);

	extern const struct wl_callback_listener frame_listener;
	P.frame_cb = wl_surface_frame(P.surface);
	wl_callback_add_listener(P.frame_cb, &frame_listener, NULL);
	wl_surface_set_buffer_scale(P.surface, s);
	wl_surface_attach(P.surface, b->wl, 0, 0);
	wl_surface_damage_buffer(P.surface, 0, 0, b->w, b->h);
	wl_surface_commit(P.surface);
	b->busy = true;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t t) {
	wl_callback_destroy(cb);
	P.frame_cb = NULL;
}
const struct wl_callback_listener frame_listener = { .done = frame_done };

/* ---------- pointer ---------- */

static void update_hover(void) {
	int h = -1;
	if (P.pointer_in)
		for (int i = 0; i < P.n_hits; i++)
			if (P.px >= P.hits[i].x0 && P.px < P.hits[i].x1) { h = i; break; }
	if (h != P.hover) { P.hover = h; P.dirty = true; }
}

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s,
		wl_fixed_t x, wl_fixed_t y) {
	if (P.cursor_dev)
		wp_cursor_shape_device_v1_set_shape(P.cursor_dev, serial,
			WP_CURSOR_SHAPE_DEVICE_V1_SHAPE_DEFAULT);
	P.pointer_in = true;
	P.px = wl_fixed_to_double(x);
	update_hover();
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
	P.pointer_in = false;
	update_hover();
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t x, wl_fixed_t y) {
	P.px = wl_fixed_to_double(x);
	update_hover();
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t, uint32_t button,
		uint32_t state) {
	if (state != WL_POINTER_BUTTON_STATE_RELEASED || P.hover < 0) return;
	struct hit *h = &P.hits[P.hover];
	char msg[64];
	switch (h->kind) {
	case HIT_LAUNCHER: ipc_send("exec-launcher"); break;
	case HIT_POWER: ipc_send("exec lumen-power"); break;
	case HIT_WS:
		snprintf(msg, sizeof(msg), "workspace %d", h->idx + 1);
		ipc_send(msg);
		break;
	case HIT_TASK:
		if (button == BTN_MIDDLE) zwlr_foreign_toplevel_handle_v1_close(h->tl->h);
		else if (button == BTN_LEFT) {
			if (h->tl->activated && !h->tl->minimized)
				zwlr_foreign_toplevel_handle_v1_set_minimized(h->tl->h);
			else {
				if (h->tl->minimized) zwlr_foreign_toplevel_handle_v1_unset_minimized(h->tl->h);
				zwlr_foreign_toplevel_handle_v1_activate(h->tl->h, P.seat);
			}
		}
		break;
	default: break;
	}
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
	/* scroll over the workspace dots to switch workspace */
	if (P.hover < 0 || P.hits[P.hover].kind != HIT_WS || axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	static uint32_t last;
	if (t - last < 150) return;
	last = t;
	int ws = P.ws_active + (wl_fixed_to_double(v) > 0 ? 1 : -1);
	if (ws < 1 || ws > 9) return;
	char msg[32];
	snprintf(msg, sizeof(msg), "workspace %d", ws);
	ipc_send(msg);
}
static void ptr_frame(void *d, struct wl_pointer *p) {}
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) {}
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void ptr_axis_rel(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) {}
static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
	.axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
	.axis_value120 = ptr_axis_value120, .axis_relative_direction = ptr_axis_rel,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps) {
	bool has = caps & WL_SEAT_CAPABILITY_POINTER;
	if (has && !P.pointer) {
		P.pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(P.pointer, &pointer_listener, NULL);
		if (P.cursor_shape)
			P.cursor_dev = wp_cursor_shape_manager_v1_get_pointer(P.cursor_shape, P.pointer);
	} else if (!has && P.pointer) {
		if (P.cursor_dev) { wp_cursor_shape_device_v1_destroy(P.cursor_dev); P.cursor_dev = NULL; }
		wl_pointer_release(P.pointer);
		P.pointer = NULL;
	}
}
static void seat_name(void *d, struct wl_seat *s, const char *n) {}
static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps, .name = seat_name };

/* ---------- foreign toplevels (window list) ---------- */

static void tl_title(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
	struct toplevel *t = d; free(t->title); t->title = xstrdup(s);
}
static void tl_app_id(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, const char *s) {
	struct toplevel *t = d; free(t->app_id); t->app_id = xstrdup(s);
}
static void tl_out_enter(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void tl_out_leave(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_output *o) {}
static void tl_state(void *d, struct zwlr_foreign_toplevel_handle_v1 *h, struct wl_array *st) {
	struct toplevel *t = d;
	t->activated = t->minimized = false;
	uint32_t *e;
	wl_array_for_each(e, st) {
		if (*e == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED) t->activated = true;
		if (*e == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_MINIMIZED) t->minimized = true;
	}
}
static void tl_done(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) { P.dirty = true; }
static void tl_closed(void *d, struct zwlr_foreign_toplevel_handle_v1 *h) {
	struct toplevel *t = d;
	wl_list_remove(&t->link);
	zwlr_foreign_toplevel_handle_v1_destroy(h);
	free(t->title);
	free(t->app_id);
	free(t);
	P.hover = -1; /* hit table points at freed entries until the next render */
	P.n_hits = 0;
	P.dirty = true;
}
static void tl_parent(void *d, struct zwlr_foreign_toplevel_handle_v1 *h,
		struct zwlr_foreign_toplevel_handle_v1 *p) {}
static const struct zwlr_foreign_toplevel_handle_v1_listener tl_listener = {
	.title = tl_title, .app_id = tl_app_id, .output_enter = tl_out_enter,
	.output_leave = tl_out_leave, .state = tl_state, .done = tl_done, .closed = tl_closed,
	.parent = tl_parent,
};

static void ftl_toplevel(void *d, struct zwlr_foreign_toplevel_manager_v1 *m,
		struct zwlr_foreign_toplevel_handle_v1 *h) {
	struct toplevel *t = calloc(1, sizeof(*t));
	t->h = h;
	wl_list_insert(P.toplevels.prev, &t->link);
	zwlr_foreign_toplevel_handle_v1_add_listener(h, &tl_listener, t);
}
static void ftl_finished(void *d, struct zwlr_foreign_toplevel_manager_v1 *m) {}
static const struct zwlr_foreign_toplevel_manager_v1_listener ftl_listener = {
	.toplevel = ftl_toplevel, .finished = ftl_finished,
};

/* ---------- surface ---------- */

static void layer_configure(void *d, struct zwlr_layer_surface_v1 *l, uint32_t serial,
		uint32_t w, uint32_t h) {
	zwlr_layer_surface_v1_ack_configure(l, serial);
	if ((int)w != P.width) P.dirty = true;
	P.width = w;
	P.configured = true;
	P.dirty = true;
}
static void layer_closed(void *d, struct zwlr_layer_surface_v1 *l) { P.running = false; }
static const struct zwlr_layer_surface_v1_listener layer_listener = {
	.configure = layer_configure, .closed = layer_closed,
};

static void surf_enter(void *d, struct wl_surface *s, struct wl_output *o) {}
static void surf_leave(void *d, struct wl_surface *s, struct wl_output *o) {}
static void surf_scale(void *d, struct wl_surface *s, int32_t scale) {
	if (scale < 1) scale = 1;
	if (scale != P.scale) { P.scale = scale; P.dirty = true; }
}
static void surf_transform(void *d, struct wl_surface *s, uint32_t t) {}
static const struct wl_surface_listener surface_listener = {
	.enter = surf_enter, .leave = surf_leave,
	.preferred_buffer_scale = surf_scale, .preferred_buffer_transform = surf_transform,
};

/* ---------- registry ---------- */

static uint32_t minu(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
	if (!strcmp(iface, wl_compositor_interface.name))
		P.compositor = wl_registry_bind(r, name, &wl_compositor_interface, minu(ver, 6));
	else if (!strcmp(iface, wl_shm_interface.name))
		P.shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	else if (!strcmp(iface, wl_seat_interface.name) && !P.seat) {
		P.seat = wl_registry_bind(r, name, &wl_seat_interface, minu(ver, 7));
		wl_seat_add_listener(P.seat, &seat_listener, NULL);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name))
		P.layer_shell = wl_registry_bind(r, name, &zwlr_layer_shell_v1_interface, minu(ver, 4));
	else if (!strcmp(iface, zwlr_foreign_toplevel_manager_v1_interface.name)) {
		P.ftl = wl_registry_bind(r, name, &zwlr_foreign_toplevel_manager_v1_interface, minu(ver, 3));
		zwlr_foreign_toplevel_manager_v1_add_listener(P.ftl, &ftl_listener, NULL);
	} else if (!strcmp(iface, wp_cursor_shape_manager_v1_interface.name))
		P.cursor_shape = wl_registry_bind(r, name, &wp_cursor_shape_manager_v1_interface, 1);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) {}
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

/* ---------- main ---------- */

int main(int argc, char **argv) {
	signal(SIGPIPE, SIG_IGN);
	wl_list_init(&P.toplevels);
	P.display = wl_display_connect(NULL);
	if (!P.display) { fprintf(stderr, "lumen-panel: no Wayland display\n"); return 1; }
	struct wl_registry *reg = wl_display_get_registry(P.display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(P.display);
	if (!P.compositor || !P.shm || !P.layer_shell) {
		fprintf(stderr, "lumen-panel: compositor lacks wl_shm or wlr-layer-shell\n");
		return 1;
	}
	/* the pointer may have been created before cursor_shape was bound */
	if (P.pointer && P.cursor_shape && !P.cursor_dev)
		P.cursor_dev = wp_cursor_shape_manager_v1_get_pointer(P.cursor_shape, P.pointer);

	P.surface = wl_compositor_create_surface(P.compositor);
	wl_surface_add_listener(P.surface, &surface_listener, NULL);
	P.layer = zwlr_layer_shell_v1_get_layer_surface(P.layer_shell, P.surface, NULL,
		ZWLR_LAYER_SHELL_V1_LAYER_TOP, "lumen-panel");
	zwlr_layer_surface_v1_add_listener(P.layer, &layer_listener, NULL);
	zwlr_layer_surface_v1_set_size(P.layer, 0, BAR_H);
	zwlr_layer_surface_v1_set_anchor(P.layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
		ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
	zwlr_layer_surface_v1_set_exclusive_zone(P.layer, BAR_H);
	wl_surface_commit(P.surface);

	ipc_subscribe();
	update_clock();
	update_battery();
	int tfd = timerfd_create(CLOCK_REALTIME, TFD_CLOEXEC | TFD_NONBLOCK);
	arm_timer(tfd);

	P.running = true;
	while (P.running) {
		if (P.dirty) render();
		while (wl_display_prepare_read(P.display) != 0)
			if (wl_display_dispatch_pending(P.display) < 0) return 1;
		if (wl_display_flush(P.display) < 0 && errno != EAGAIN) { wl_display_cancel_read(P.display); break; }
		struct pollfd fds[3] = {
			{wl_display_get_fd(P.display), POLLIN, 0},
			{tfd, POLLIN, 0},
			{P.ipc_fd, POLLIN, 0},
		};
		int n = poll(fds, P.ipc_fd >= 0 ? 3 : 2, -1);
		if (n < 0 && errno != EINTR) { wl_display_cancel_read(P.display); break; }
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(P.display) < 0) break;
		} else {
			wl_display_cancel_read(P.display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP)) break;
		if (wl_display_dispatch_pending(P.display) < 0) break;
		if (fds[1].revents & POLLIN) {
			uint64_t x;
			(void)!read(tfd, &x, sizeof(x));
			update_clock();
			update_battery();
			arm_timer(tfd);
		}
		if (P.ipc_fd >= 0 && (fds[2].revents & (POLLIN | POLLHUP))) ipc_read();
	}
	return 0;
}
