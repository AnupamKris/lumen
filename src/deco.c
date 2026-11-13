#define _POSIX_C_SOURCE 200809L
#include <math.h>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <drm_fourcc.h>
#include <pango/pangocairo.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/util/log.h>
#include "lumen.h"

/* ---------- cairo-backed wlr_buffer ---------- */

struct cairo_buffer {
	struct wlr_buffer base;
	cairo_surface_t *surface;
};

static void cbuf_destroy(struct wlr_buffer *b) {
	struct cairo_buffer *cb = wl_container_of(b, cb, base);
	cairo_surface_destroy(cb->surface);
	free(cb);
}

static bool cbuf_begin(struct wlr_buffer *b, uint32_t flags, void **data,
		uint32_t *format, size_t *stride) {
	struct cairo_buffer *cb = wl_container_of(b, cb, base);
	if (flags & WLR_BUFFER_DATA_PTR_ACCESS_WRITE) return false;
	*data = cairo_image_surface_get_data(cb->surface);
	*format = DRM_FORMAT_ARGB8888;
	*stride = cairo_image_surface_get_stride(cb->surface);
	return true;
}

static void cbuf_end(struct wlr_buffer *b) { (void)b; }

static const struct wlr_buffer_impl cbuf_impl = {
	.destroy = cbuf_destroy,
	.begin_data_ptr_access = cbuf_begin,
	.end_data_ptr_access = cbuf_end,
};

static struct wlr_buffer *cbuf_wrap(cairo_surface_t *surf) {
	cairo_surface_flush(surf);
	struct cairo_buffer *cb = calloc(1, sizeof(*cb));
	cb->surface = surf;
	wlr_buffer_init(&cb->base, &cbuf_impl,
		cairo_image_surface_get_width(surf), cairo_image_surface_get_height(surf));
	return &cb->base;
}

/* config colours are premultiplied; cairo wants straight alpha */
static void set_color(cairo_t *cr, const float c[4], float alpha_mul) {
	float a = c[3];
	if (a <= 0) { cairo_set_source_rgba(cr, 0, 0, 0, 0); return; }
	cairo_set_source_rgba(cr, c[0] / a, c[1] / a, c[2] / a, a * alpha_mul);
}

/* ---------- titlebar ---------- */

#define BTN_PAD_RIGHT 6
#define BTN_GAP 2

static int btn_size(int th) { return th - 10 > 12 ? th - 10 : 12; }

/* Button rects in title-local coords, right to left: close, max, min. */
static struct wlr_box btn_rect(int w, int th, enum title_button b) {
	int bs = btn_size(th);
	int idx = b == TB_CLOSE ? 0 : b == TB_MAX ? 1 : 2;
	int x = w - BTN_PAD_RIGHT - bs - idx * (bs + BTN_GAP);
	return (struct wlr_box){x, (th - bs) / 2, bs, bs};
}

enum title_button deco_button_at(struct lumen_view *v, double lx, double ly) {
	if (!v->ssd || v->fullscreen) return TB_NONE;
	int th = v->server->cfg.titlebar_height;
	double tx = lx - v->geo.x, ty = ly - (v->geo.y - th);
	if (ty < 0 || ty >= th) return TB_NONE;
	enum title_button order[] = {TB_CLOSE, TB_MAX, TB_MIN};
	for (int i = 0; i < 3; i++) {
		struct wlr_box r = btn_rect(v->title_w, th, order[i]);
		/* generous hit area: full titlebar height */
		if (tx >= r.x - BTN_GAP / 2 && tx < r.x + r.width + BTN_GAP / 2) return order[i];
	}
	return TB_NONE;
}

static void draw_button(cairo_t *cr, struct lumen_view *v, enum title_button b,
		struct wlr_box r, const float fg[4]) {
	double cx = r.x + r.width / 2.0, cy = r.y + r.height / 2.0;
	bool hover = v->hover_button == b;
	if (hover) {
		cairo_arc(cr, cx, cy, r.width / 2.0, 0, 2 * M_PI);
		if (b == TB_CLOSE) cairo_set_source_rgba(cr, 0.90, 0.33, 0.30, 0.95);
		else cairo_set_source_rgba(cr, 1, 1, 1, 0.10);
		cairo_fill(cr);
	}
	double g = r.width * 0.19; /* glyph half-size */
	cairo_set_line_width(cr, 1.3);
	cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
	if (hover && b == TB_CLOSE) cairo_set_source_rgba(cr, 1, 1, 1, 1);
	else set_color(cr, fg, 0.85f);
	switch (b) {
	case TB_CLOSE:
		cairo_move_to(cr, cx - g, cy - g); cairo_line_to(cr, cx + g, cy + g);
		cairo_move_to(cr, cx + g, cy - g); cairo_line_to(cr, cx - g, cy + g);
		break;
	case TB_MAX:
		if (v->maximized) {
			double s = g * 0.8;
			cairo_rectangle(cr, cx - s - 1, cy - s + 1, 2 * s, 2 * s);
			cairo_move_to(cr, cx - s + 1, cy - s - 1);
			cairo_line_to(cr, cx + s + 1, cy - s - 1);
			cairo_line_to(cr, cx + s + 1, cy + s - 1);
		} else {
			cairo_rectangle(cr, cx - g, cy - g, 2 * g, 2 * g);
		}
		break;
	case TB_MIN:
		cairo_move_to(cr, cx - g, cy + 0.5); cairo_line_to(cr, cx + g, cy + 0.5);
		break;
	default: break;
	}
	cairo_stroke(cr);
}

struct wlr_buffer *deco_render_title(struct lumen_view *v, int w, int h) {
	if (w <= 0 || h <= 0) return NULL;
	struct lumen_config *cfg = &v->server->cfg;
	struct wlr_surface *f = v->server->seat->keyboard_state.focused_surface;
	bool focused = f && f == v->xdg->base->surface;
	const float *fg = focused ? cfg->title_fg : cfg->title_fg_inactive;

	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surf);
	cairo_set_antialias(cr, CAIRO_ANTIALIAS_BEST);

	/* title: centred, ellipsised, clear of the buttons on the right */
	int reserve = BTN_PAD_RIGHT + 3 * btn_size(h) + 2 * BTN_GAP + 8;
	int tw = w - 2 * reserve;
	const char *title = v->xdg->title ? v->xdg->title : (v->xdg->app_id ? v->xdg->app_id : "");
	if (tw > 20 && *title) {
		PangoLayout *pl = pango_cairo_create_layout(cr);
		PangoFontDescription *fd = pango_font_description_from_string(cfg->font);
		pango_font_description_set_weight(fd, PANGO_WEIGHT_MEDIUM);
		pango_layout_set_font_description(pl, fd);
		pango_layout_set_text(pl, title, -1);
		pango_layout_set_single_paragraph_mode(pl, true);
		pango_layout_set_ellipsize(pl, PANGO_ELLIPSIZE_END);
		pango_layout_set_width(pl, tw * PANGO_SCALE);
		pango_layout_set_alignment(pl, PANGO_ALIGN_CENTER);
		int lw, lh;
		pango_layout_get_pixel_size(pl, &lw, &lh);
		set_color(cr, fg, 1.0f);
		cairo_move_to(cr, reserve, (h - lh) / 2.0);
		pango_cairo_show_layout(cr, pl);
		pango_font_description_free(fd);
		g_object_unref(pl);
	}

	draw_button(cr, v, TB_MIN, btn_rect(w, h, TB_MIN), fg);
	draw_button(cr, v, TB_MAX, btn_rect(w, h, TB_MAX), fg);
	draw_button(cr, v, TB_CLOSE, btn_rect(w, h, TB_CLOSE), fg);

	cairo_destroy(cr);
	return cbuf_wrap(surf);
}

/* ---------- wallpaper ---------- */

static void add_blob(cairo_t *cr, double x, double y, double r, double cr_, double cg, double cb,
		double a) {
	cairo_pattern_t *p = cairo_pattern_create_radial(x, y, 0, x, y, r);
	cairo_pattern_add_color_stop_rgba(p, 0, cr_, cg, cb, a);
	cairo_pattern_add_color_stop_rgba(p, 1, cr_, cg, cb, 0);
	cairo_set_source(cr, p);
	cairo_paint(cr);
	cairo_pattern_destroy(p);
}

/* light ordered noise to stop gradient banding on 8-bit panels */
static void dither(cairo_surface_t *s) {
	cairo_surface_flush(s);
	unsigned char *d = cairo_image_surface_get_data(s);
	int w = cairo_image_surface_get_width(s), h = cairo_image_surface_get_height(s);
	int st = cairo_image_surface_get_stride(s);
	uint32_t seed = 0x9e3779b9;
	for (int y = 0; y < h; y++) {
		uint32_t *row = (uint32_t *)(d + y * st);
		for (int x = 0; x < w; x++) {
			seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5;
			int n = (int)(seed & 3) - 1; /* -1..2 */
			uint32_t px = row[x];
			int r = ((px >> 16) & 0xff) + n, g = ((px >> 8) & 0xff) + n, b = (px & 0xff) + n;
			r = r < 0 ? 0 : r > 255 ? 255 : r;
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			b = b < 0 ? 0 : b > 255 ? 255 : b;
			row[x] = 0xff000000u | (uint32_t)r << 16 | (uint32_t)g << 8 | (uint32_t)b;
		}
	}
	cairo_surface_mark_dirty(s);
}

struct wlr_buffer *deco_render_wallpaper(struct lumen_server *s, int w, int h) {
	struct lumen_config *cfg = &s->cfg;
	cairo_surface_t *surf = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
	cairo_t *cr = cairo_create(surf);

	cairo_surface_t *img = NULL;
	if (cfg->wallpaper[0]) {
		img = cairo_image_surface_create_from_png(cfg->wallpaper);
		if (cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) {
			wlr_log(WLR_ERROR, "wallpaper %s: only PNG is supported", cfg->wallpaper);
			cairo_surface_destroy(img);
			img = NULL;
		}
	}
	if (img) {
		/* scale to cover, centred */
		double iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
		double sc = fmax(w / iw, h / ih);
		cairo_translate(cr, (w - iw * sc) / 2, (h - ih * sc) / 2);
		cairo_scale(cr, sc, sc);
		cairo_set_source_surface(cr, img, 0, 0);
		cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
		cairo_paint(cr);
		cairo_surface_destroy(img);
	} else {
		/* quiet default: near-black slate with two very faint, desaturated tints */
		cairo_pattern_t *p = cairo_pattern_create_linear(0, 0, 0, h);
		float a = cfg->bg[3] > 0 ? cfg->bg[3] : 1;
		double r = cfg->bg[0] / a, g = cfg->bg[1] / a, b = cfg->bg[2] / a;
		cairo_pattern_add_color_stop_rgb(p, 0, r * 1.08, g * 1.08, b * 1.12);
		cairo_pattern_add_color_stop_rgb(p, 1, r * 0.80, g * 0.80, b * 0.84);
		cairo_set_source(cr, p);
		cairo_paint(cr);
		cairo_pattern_destroy(p);
		double d = hypot(w, h);
		add_blob(cr, w * 0.18, h * 0.10, d * 0.55, 0.36, 0.42, 0.58, 0.16);
		add_blob(cr, w * 0.88, h * 0.95, d * 0.50, 0.45, 0.38, 0.55, 0.12);
		dither(surf);
	}
	cairo_destroy(cr);
	return cbuf_wrap(surf);
}
