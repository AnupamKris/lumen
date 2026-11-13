#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/util/log.h>
#include "lumen.h"

/*
 * Theme: neutral, quiet, no glow. Colours are stored premultiplied (what the
 * scene graph expects). Shadows are soft, black and low-alpha; borders are a
 * 1px hairline that only separates windows, never decorates them.
 */

static void rgba(float out[4], uint32_t hex, float a) {
	float r = ((hex >> 16) & 0xff) / 255.0f;
	float g = ((hex >> 8) & 0xff) / 255.0f;
	float b = (hex & 0xff) / 255.0f;
	out[0] = r * a; out[1] = g * a; out[2] = b * a; out[3] = a;
}

static void add_bind(struct lumen_config *c, uint32_t mods, xkb_keysym_t sym, const char *action) {
	/* later binds for the same chord replace earlier ones */
	for (int i = 0; i < c->n_binds; i++)
		if (c->binds[i].mods == mods && c->binds[i].sym == sym) {
			snprintf(c->binds[i].action, sizeof(c->binds[i].action), "%s", action);
			return;
		}
	if (c->n_binds >= MAX_BINDS) return;
	struct lumen_bind *b = &c->binds[c->n_binds++];
	b->mods = mods;
	b->sym = sym;
	snprintf(b->action, sizeof(b->action), "%s", action);
}

void config_defaults(struct lumen_config *c) {
	memset(c, 0, sizeof(*c));
	snprintf(c->terminal, sizeof(c->terminal), "foot");
	snprintf(c->launcher, sizeof(c->launcher), "fuzzel");
	snprintf(c->font, sizeof(c->font), "Inter 10");
	c->repeat_rate = 30;
	c->repeat_delay = 300;

	c->corner_radius = 10;
	c->border_width = 1;
	c->titlebar_height = 32;
	c->gaps = 8;
	c->shadows = true;
	c->shadow_size = 30;
	c->blur = true;
	c->animations = true;
	c->default_tiling = false;
	c->master_ratio = 0.55f;

	rgba(c->accent, 0x8ab4f8, 1.0f);
	rgba(c->bg, 0x1c1d21, 1.0f);
	rgba(c->title_bg, 0x2a2c31, 1.0f);
	rgba(c->title_bg_inactive, 0x232529, 1.0f);
	rgba(c->title_fg, 0xe8e8ea, 1.0f);
	rgba(c->title_fg_inactive, 0x8a8d94, 1.0f);
	rgba(c->border, 0xffffff, 0.10f);
	rgba(c->border_inactive, 0xffffff, 0.05f);
	rgba(c->shadow_color, 0x000000, 0.50f);

	const uint32_t S = WLR_MODIFIER_LOGO, SH = WLR_MODIFIER_SHIFT, A = WLR_MODIFIER_ALT;
	add_bind(c, S, XKB_KEY_Return, "exec-terminal");
	add_bind(c, S, XKB_KEY_d, "exec-launcher");
	add_bind(c, S, XKB_KEY_space, "exec-launcher");
	add_bind(c, S, XKB_KEY_q, "close");
	add_bind(c, S, XKB_KEY_f, "fullscreen");
	add_bind(c, S, XKB_KEY_m, "maximize");
	add_bind(c, S, XKB_KEY_n, "minimize");
	add_bind(c, S, XKB_KEY_v, "toggle-floating");
	add_bind(c, S, XKB_KEY_t, "toggle-tiling");
	add_bind(c, S, XKB_KEY_j, "focus next");
	add_bind(c, S, XKB_KEY_k, "focus prev");
	add_bind(c, A, XKB_KEY_Tab, "focus next");
	add_bind(c, A | SH, XKB_KEY_ISO_Left_Tab, "focus prev");
	add_bind(c, S, XKB_KEY_bracketleft, "master -0.05");
	add_bind(c, S, XKB_KEY_bracketright, "master +0.05");
	add_bind(c, S, XKB_KEY_l, "exec lumen-lock");
	add_bind(c, S, XKB_KEY_Escape, "exec lumen-power");
	add_bind(c, S | SH, XKB_KEY_Return, "swap-master");
	add_bind(c, S | SH, XKB_KEY_r, "reload");
	add_bind(c, S | SH, XKB_KEY_e, "exit");
	add_bind(c, 0, XKB_KEY_Print, "exec lumen-screenshot");
	add_bind(c, SH, XKB_KEY_Print, "exec lumen-screenshot region");
	add_bind(c, 0, XKB_KEY_XF86AudioRaiseVolume, "exec pactl set-sink-volume @DEFAULT_SINK@ +5%");
	add_bind(c, 0, XKB_KEY_XF86AudioLowerVolume, "exec pactl set-sink-volume @DEFAULT_SINK@ -5%");
	add_bind(c, 0, XKB_KEY_XF86AudioMute, "exec pactl set-sink-mute @DEFAULT_SINK@ toggle");
	add_bind(c, 0, XKB_KEY_XF86AudioMicMute, "exec pactl set-source-mute @DEFAULT_SOURCE@ toggle");
	add_bind(c, 0, XKB_KEY_XF86MonBrightnessUp, "exec brightnessctl -q set 5%+");
	add_bind(c, 0, XKB_KEY_XF86MonBrightnessDown, "exec brightnessctl -q set 5%-");
	for (int i = 0; i < 9; i++) {
		char a[32];
		snprintf(a, sizeof(a), "workspace %d", i + 1);
		add_bind(c, S, XKB_KEY_1 + i, a);
		snprintf(a, sizeof(a), "move-to %d", i + 1);
		/* Shift+digit yields a symbol keysym; binds match the unshifted sym */
		add_bind(c, S | SH, XKB_KEY_1 + i, a);
	}
}

const char *config_path(void) {
	static char p[512];
	const char *x = getenv("XDG_CONFIG_HOME");
	const char *h = getenv("HOME");
	if (x && *x) snprintf(p, sizeof(p), "%s/lumen/lumen.conf", x);
	else snprintf(p, sizeof(p), "%s/.config/lumen/lumen.conf", h ? h : "");
	return p;
}

static char *trim(char *s) {
	while (isspace((unsigned char)*s)) s++;
	char *e = s + strlen(s);
	while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
	return s;
}

static bool parse_bool(const char *v) {
	return !strcasecmp(v, "true") || !strcasecmp(v, "yes") || !strcasecmp(v, "on") || !strcmp(v, "1");
}

static bool parse_color(const char *v, float out[4]) {
	if (*v == '#') v++;
	size_t n = strlen(v);
	if (n != 6 && n != 8) return false;
	char *end;
	unsigned long x = strtoul(v, &end, 16);
	if (*end) return false;
	if (n == 6) rgba(out, (uint32_t)x, 1.0f);
	else rgba(out, (uint32_t)(x >> 8), (x & 0xff) / 255.0f);
	return true;
}

static void parse_bind(struct lumen_config *c, char *v) {
	/* "SUPER+SHIFT+Return exec foot" */
	char *sp = v;
	while (*sp && !isspace((unsigned char)*sp)) sp++;
	if (!*sp) return;
	*sp++ = 0;
	char *action = trim(sp);
	uint32_t mods = 0;
	xkb_keysym_t sym = XKB_KEY_NoSymbol;
	char *save = NULL;
	for (char *tok = strtok_r(v, "+", &save); tok; tok = strtok_r(NULL, "+", &save)) {
		if (!strcasecmp(tok, "super") || !strcasecmp(tok, "logo") || !strcasecmp(tok, "mod4"))
			mods |= WLR_MODIFIER_LOGO;
		else if (!strcasecmp(tok, "shift")) mods |= WLR_MODIFIER_SHIFT;
		else if (!strcasecmp(tok, "ctrl") || !strcasecmp(tok, "control")) mods |= WLR_MODIFIER_CTRL;
		else if (!strcasecmp(tok, "alt") || !strcasecmp(tok, "mod1")) mods |= WLR_MODIFIER_ALT;
		else sym = xkb_keysym_from_name(tok, XKB_KEYSYM_CASE_INSENSITIVE);
	}
	if (sym == XKB_KEY_NoSymbol) {
		wlr_log(WLR_ERROR, "config: unknown key in bind '%s'", v);
		return;
	}
	sym = xkb_keysym_to_lower(sym);
	if (!strcmp(action, "none")) {
		for (int i = 0; i < c->n_binds; i++)
			if (c->binds[i].mods == mods && c->binds[i].sym == sym) {
				c->binds[i] = c->binds[--c->n_binds];
				return;
			}
		return;
	}
	add_bind(c, mods, sym, action);
}

#define STR(k, f) else if (!strcmp(key, k)) snprintf(c->f, sizeof(c->f), "%s", val);
#define INT(k, f) else if (!strcmp(key, k)) c->f = atoi(val);
#define BOOL(k, f) else if (!strcmp(key, k)) c->f = parse_bool(val);
#define COL(k, f) else if (!strcmp(key, k)) { if (!parse_color(val, c->f)) \
	wlr_log(WLR_ERROR, "config:%d bad colour '%s'", line, val); }

void config_load(struct lumen_config *c, const char *path) {
	FILE *f = fopen(path, "r");
	if (!f) {
		wlr_log(WLR_INFO, "no config at %s, using defaults", path);
		return;
	}
	char buf[1024];
	int line = 0;
	while (fgets(buf, sizeof(buf), f)) {
		line++;
		char *s = trim(buf);
		if (!*s || *s == '#') continue;
		char *eq = strchr(s, '=');
		if (!eq) {
			wlr_log(WLR_ERROR, "config:%d expected key = value", line);
			continue;
		}
		*eq = 0;
		char *key = trim(s), *val = trim(eq + 1);
		if (0) {}
		STR("terminal", terminal)
		STR("launcher", launcher)
		STR("font", font)
		STR("wallpaper", wallpaper)
		STR("kb_layout", kb_layout)
		STR("kb_variant", kb_variant)
		STR("kb_options", kb_options)
		INT("repeat_rate", repeat_rate)
		INT("repeat_delay", repeat_delay)
		INT("corner_radius", corner_radius)
		INT("border_width", border_width)
		INT("titlebar_height", titlebar_height)
		INT("gaps", gaps)
		INT("shadow_size", shadow_size)
		BOOL("shadows", shadows)
		BOOL("blur", blur)
		BOOL("animations", animations)
		BOOL("tiling", default_tiling)
		COL("accent", accent)
		COL("background", bg)
		COL("title_bg", title_bg)
		COL("title_bg_inactive", title_bg_inactive)
		COL("title_fg", title_fg)
		COL("title_fg_inactive", title_fg_inactive)
		COL("border", border)
		COL("border_inactive", border_inactive)
		COL("shadow_color", shadow_color)
		else if (!strcmp(key, "master_ratio")) c->master_ratio = strtof(val, NULL);
		else if (!strcmp(key, "autostart")) {
			if (c->n_autostart < MAX_AUTOSTART)
				snprintf(c->autostart[c->n_autostart++], sizeof(c->autostart[0]), "%s", val);
		} else if (!strcmp(key, "bind")) parse_bind(c, val);
		else wlr_log(WLR_ERROR, "config:%d unknown key '%s'", line, key);
	}
	fclose(f);
	if (c->master_ratio < 0.1f) c->master_ratio = 0.1f;
	if (c->master_ratio > 0.9f) c->master_ratio = 0.9f;
	if (c->titlebar_height < 16) c->titlebar_height = 16;
	if (c->border_width < 0) c->border_width = 0;
	if (c->shadow_size < 0) c->shadow_size = 0;
}
