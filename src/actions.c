#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <wlr/util/log.h>
#include "lumen.h"

void spawn(const char *cmd) {
	if (!cmd || !*cmd) return;
	pid_t pid = fork();
	if (pid < 0) return;
	if (pid == 0) {
		/* double fork so we never have zombies to reap */
		setsid();
		if (fork() == 0) {
			execl("/bin/sh", "/bin/sh", "-c", cmd, (char *)NULL);
			_exit(127);
		}
		_exit(0);
	}
	waitpid(pid, NULL, 0);
}

static void reload(struct lumen_server *s) {
	config_defaults(&s->cfg);
	config_load(&s->cfg, config_path());
	wlr_scene_node_set_enabled(&s->blur_node->node, s->cfg.blur);
	struct lumen_output *o;
	wl_list_for_each(o, &s->outputs, link) output_render_wallpaper(o);
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link) v->title_dirty = true;
	arrange_all(s);
	wlr_log(WLR_INFO, "config reloaded");
}

/* Turn every output on or off (swayidle: "lumen --msg 'dpms off'"). */
static void set_dpms(struct lumen_server *s, bool on) {
	struct lumen_output *o;
	wl_list_for_each(o, &s->outputs, link) {
		if (o->wlr_output->enabled == on) continue;
		struct wlr_output_state st;
		wlr_output_state_init(&st);
		wlr_output_state_set_enabled(&st, on);
		wlr_output_commit_state(o->wlr_output, &st);
		wlr_output_state_finish(&st);
	}
	if (on) schedule_frames(s);
}

void run_action(struct lumen_server *s, const char *action) {
	char cmd[64] = {0};
	const char *arg = "";
	const char *sp = strchr(action, ' ');
	size_t n = sp ? (size_t)(sp - action) : strlen(action);
	if (n >= sizeof(cmd)) n = sizeof(cmd) - 1;
	memcpy(cmd, action, n);
	if (sp) { arg = sp + 1; while (*arg == ' ') arg++; }

	/* while locked nothing but display power may be changed (e.g. via IPC) */
	if (s->locked && strcmp(cmd, "dpms")) return;

	struct lumen_view *v = view_focused(s);

	if (!strcmp(cmd, "dpms")) set_dpms(s, strcmp(arg, "off") != 0);
	else if (!strcmp(cmd, "exec")) spawn(arg);
	else if (!strcmp(cmd, "exec-terminal")) spawn(s->cfg.terminal);
	else if (!strcmp(cmd, "exec-launcher")) spawn(s->cfg.launcher);
	else if (!strcmp(cmd, "exit")) wl_display_terminate(s->display);
	else if (!strcmp(cmd, "reload")) reload(s);
	else if (!strcmp(cmd, "workspace")) workspace_switch(s, atoi(arg) - 1);
	else if (!strcmp(cmd, "move-to")) { if (v) view_move_to_workspace(v, atoi(arg) - 1); }
	else if (!strcmp(cmd, "focus")) focus_cycle(s, !strcmp(arg, "prev") ? -1 : 1);
	else if (!strcmp(cmd, "toggle-tiling")) {
		s->ws_tiling[s->ws] = !s->ws_tiling[s->ws];
		arrange_workspace(s, s->ws);
		ipc_broadcast_state(s);
	} else if (!strcmp(cmd, "master")) {
		float r = s->ws_master[s->ws] + strtof(arg, NULL);
		s->ws_master[s->ws] = r < 0.15f ? 0.15f : r > 0.85f ? 0.85f : r;
		arrange_workspace(s, s->ws);
	} else if (!strcmp(cmd, "swap-master")) tile_swap_master(s);
	else if (!v) return;
	else if (!strcmp(cmd, "close")) view_close(v);
	else if (!strcmp(cmd, "fullscreen")) view_set_fullscreen(v, !v->fullscreen);
	else if (!strcmp(cmd, "maximize")) view_set_maximized(v, !v->maximized);
	else if (!strcmp(cmd, "minimize")) view_set_minimized(v, true);
	else if (!strcmp(cmd, "toggle-floating")) {
		v->floating = !v->floating;
		arrange_workspace(s, v->ws);
	} else wlr_log(WLR_ERROR, "unknown action '%s'", action);
}
