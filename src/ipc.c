#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <wlr/util/log.h>
#include "lumen.h"

/*
 * Tiny line protocol on $LUMEN_SOCK:
 *   "<action>\n"   run a bind action (e.g. "workspace 2"), reply "ok\n"
 *   "state\n"      reply one state line
 *   "subscribe\n"  keep the connection; a state line is pushed on every change
 * state line: "ws <active 1-9> <occupied bitmask> <tiling 0|1>\n"
 */

struct ipc_client {
	struct wl_list link;
	struct lumen_server *server;
	int fd;
	struct wl_event_source *src;
	bool subscribed;
	char buf[512];
	size_t len;
};

static void state_line(struct lumen_server *s, char *out, size_t n) {
	unsigned occ = 0;
	struct lumen_view *v;
	wl_list_for_each(v, &s->views, link)
		if (v->mapped) occ |= 1u << v->ws;
	snprintf(out, n, "ws %d %u %d\n", s->ws + 1, occ, s->ws_tiling[s->ws] ? 1 : 0);
}

static void client_free(struct ipc_client *c) {
	wl_event_source_remove(c->src);
	close(c->fd);
	wl_list_remove(&c->link);
	free(c);
}

static bool client_send(struct ipc_client *c, const char *msg) {
	size_t n = strlen(msg);
	ssize_t w = send(c->fd, msg, n, MSG_NOSIGNAL | MSG_DONTWAIT);
	return w == (ssize_t)n;
}

static int client_readable(int fd, uint32_t mask, void *data) {
	struct ipc_client *c = data;
	struct lumen_server *s = c->server;
	if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) { client_free(c); return 0; }
	ssize_t r = read(fd, c->buf + c->len, sizeof(c->buf) - 1 - c->len);
	if (r <= 0) { client_free(c); return 0; }
	c->len += r;
	c->buf[c->len] = 0;
	char *nl;
	while ((nl = strchr(c->buf, '\n'))) {
		*nl = 0;
		char line[512];
		snprintf(line, sizeof(line), "%s", c->buf);
		memmove(c->buf, nl + 1, c->len - (nl + 1 - c->buf) + 1);
		c->len = strlen(c->buf);
		char st[64];
		if (!strcmp(line, "subscribe")) {
			c->subscribed = true;
			state_line(s, st, sizeof(st));
			client_send(c, st);
		} else if (!strcmp(line, "state")) {
			state_line(s, st, sizeof(st));
			client_send(c, st);
		} else if (*line) {
			run_action(s, line);
			client_send(c, "ok\n");
		}
	}
	if (c->len >= sizeof(c->buf) - 1) client_free(c); /* line too long */
	return 0;
}

static int ipc_accept(int fd, uint32_t mask, void *data) {
	struct lumen_server *s = data;
	int cfd = accept(fd, NULL, NULL);
	if (cfd < 0) return 0;
	fcntl(cfd, F_SETFD, FD_CLOEXEC);
	struct ipc_client *c = calloc(1, sizeof(*c));
	c->server = s;
	c->fd = cfd;
	c->src = wl_event_loop_add_fd(s->loop, cfd, WL_EVENT_READABLE, client_readable, c);
	wl_list_insert(&s->ipc_clients, &c->link);
	return 0;
}

void ipc_broadcast_state(struct lumen_server *s) {
	if (!s->ipc_clients.next) return;
	char st[64];
	state_line(s, st, sizeof(st));
	struct ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &s->ipc_clients, link)
		if (c->subscribed && !client_send(c, st)) client_free(c);
}

static void sock_path(char *out, size_t n, const char *display) {
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(out, n, "%s/lumen-%s.sock", rt ? rt : "/tmp", display ? display : "wayland-0");
}

bool ipc_init(struct lumen_server *s) {
	wl_list_init(&s->ipc_clients);
	sock_path(s->ipc_path, sizeof(s->ipc_path), getenv("WAYLAND_DISPLAY"));
	s->ipc_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (s->ipc_fd < 0) return false;
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", s->ipc_path);
	unlink(s->ipc_path);
	if (bind(s->ipc_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(s->ipc_fd, 8) < 0) {
		wlr_log(WLR_ERROR, "ipc: cannot bind %s: %s", s->ipc_path, strerror(errno));
		close(s->ipc_fd);
		s->ipc_fd = -1;
		return false;
	}
	s->ipc_source = wl_event_loop_add_fd(s->loop, s->ipc_fd, WL_EVENT_READABLE, ipc_accept, s);
	setenv("LUMEN_SOCK", s->ipc_path, true);
	return true;
}

void ipc_finish(struct lumen_server *s) {
	if (s->ipc_fd < 0) return;
	struct ipc_client *c, *tmp;
	wl_list_for_each_safe(c, tmp, &s->ipc_clients, link) client_free(c);
	wl_event_source_remove(s->ipc_source);
	close(s->ipc_fd);
	unlink(s->ipc_path);
}

int ipc_client_main(const char *msg) {
	char path[256];
	const char *env = getenv("LUMEN_SOCK");
	if (env) snprintf(path, sizeof(path), "%s", env);
	else sock_path(path, sizeof(path), getenv("WAYLAND_DISPLAY"));
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	struct sockaddr_un addr = {.sun_family = AF_UNIX};
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		fprintf(stderr, "lumen: cannot connect to %s\n", path);
		return 1;
	}
	dprintf(fd, "%s\n", msg);
	bool follow = !strcmp(msg, "subscribe");
	char buf[512];
	ssize_t r;
	while ((r = read(fd, buf, sizeof(buf))) > 0) {
		fwrite(buf, 1, r, stdout);
		fflush(stdout);
		if (!follow && memchr(buf, '\n', r)) break;
	}
	close(fd);
	return 0;
}
