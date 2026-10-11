#define _GNU_SOURCE
#include "daemon_cmdr.h"
#include "daemon_json.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define MSG_MAX (1 << 20)
struct cmdr { int fd; jv *plugins; jv *transport; char *msg; };

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

static int read_exact(int fd, unsigned char *b, size_t n, long deadline) {
    size_t got = 0;
    while (got < n) {
        long left = deadline - now_ms(); if (left < 0) return 0;
        struct pollfd p = { fd, POLLIN, 0 };
        int r = poll(&p, 1, (int)left); if (r <= 0) return 0;
        ssize_t k = read(fd, b + got, n - got); if (k <= 0) return -1;
        got += (size_t)k;
    }
    return 1;
}

static int write_all(int fd, const unsigned char *b, size_t n) {
    while (n) { ssize_t k = send(fd, b, n, MSG_NOSIGNAL); if (k <= 0) return -1; b += k; n -= (size_t)k; }
    return 0;
}

/* a client frame must be masked */
static int ws_send_text(int fd, const char *s) {
    size_t n = strlen(s); unsigned char hdr[14]; size_t h = 0;
    hdr[h++] = 0x81;
    if (n < 126) hdr[h++] = 0x80 | (unsigned char)n;
    else if (n < 65536) { hdr[h++] = 0x80 | 126; hdr[h++] = (unsigned char)(n >> 8); hdr[h++] = (unsigned char)n; }
    else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[h++] = (unsigned char)((unsigned long long)n >> (8 * i)); }
    unsigned char mask[4]; FILE *r = fopen("/dev/urandom", "rb"); if (!r || fread(mask, 1, 4, r) != 4) { memcpy(mask, "\x5a\xa5\x3c\xc3", 4); } if (r) fclose(r);
    memcpy(hdr + h, mask, 4); h += 4;
    unsigned char *body = malloc(n + 1); for (size_t i = 0; i < n; i++) body[i] = (unsigned char)s[i] ^ mask[i & 3];
    int rc = write_all(fd, hdr, h) || write_all(fd, body, n);
    free(body); return rc;
}

/* One text message, or 0 on timeout, -1 on error/close. Answers pings; joins fragments. */
static int ws_recv_text(cmdr_t *c, long deadline) {
    size_t len = 0;
    for (;;) {
        unsigned char h[2];
        int r = read_exact(c->fd, h, 2, deadline); if (r <= 0) return r;
        int fin = h[0] & 0x80, op = h[0] & 15; unsigned long long n = h[1] & 0x7f;
        if (n == 126) { unsigned char e[2]; if (read_exact(c->fd, e, 2, deadline) <= 0) return -1; n = (unsigned)(e[0] << 8 | e[1]); }
        else if (n == 127) { unsigned char e[8]; if (read_exact(c->fd, e, 8, deadline) <= 0) return -1; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | e[i]; }
        if (n > MSG_MAX || len + n > MSG_MAX) return -1;
        unsigned char *buf = malloc(n + 1);
        if (n && read_exact(c->fd, buf, (size_t)n, deadline + 2000) <= 0) { free(buf); return -1; }
        if (op == 8) { free(buf); return -1; }
        if (op == 9) { unsigned char pong[2] = { 0x8a, 0 }; write_all(c->fd, pong, 2); free(buf); continue; }   /* ping: an empty pong is fine for the addin */
        if (op == 1 || op == 0) { memcpy(c->msg + len, buf, (size_t)n); len += (size_t)n; }
        free(buf);
        if ((op == 1 || op == 0) && fin) { c->msg[len] = 0; return 1; }
    }
}

/* keep the plugin list and transport current as messages arrive */
static void track(cmdr_t *c, jv *m) {
    const char *t = djson_str(m, "t", "");
    if (!strcmp(t, "plugins")) {
        jv *ps = djson_get(m, "plugins");
        if (ps) { djson_free(c->plugins); /* detach the array from m so it survives */ c->plugins = ps; for (jv **k = &m->kid; *k; k = &(*k)->next) if (*k == ps) { *k = ps->next; ps->next = NULL; break; } }
    } else if (!strcmp(t, "transport")) {
        djson_free(c->transport); c->transport = m; return;
    } else if (!strcmp(t, "plugin_removed")) {
        /* the list is refreshed by the next 'list' request; ids are never reused so a stale entry is harmless */
    }
    djson_free(m);
}

static jv *next_msg(cmdr_t *c, long deadline) {
    int r = ws_recv_text(c, deadline); if (r <= 0) return NULL;
    return djson_parse(c->msg, strlen(c->msg));
}

cmdr_t *cmdr_open(const char *host, int port, char *err, size_t errcap) {
    cmdr_t *c = calloc(1, sizeof *c); c->fd = -1; c->msg = malloc(MSG_MAX + 1);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { snprintf(err, errcap, "bad address %s", host); goto fail; }
    c->fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval tv = { 2, 0 }; setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv); setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    if (connect(c->fd, (struct sockaddr *)&a, sizeof a) < 0) { snprintf(err, errcap, "commander not reachable on %s:%d", host, port); goto fail; }
    char req[256]; snprintf(req, sizeof req, "GET /ws HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", host, port);
    if (write_all(c->fd, (unsigned char *)req, strlen(req))) { snprintf(err, errcap, "commander closed the connection"); goto fail; }
    char resp[1024]; size_t n = 0; long dl = now_ms() + 3000;
    while (n < sizeof resp - 1) {                                  /* the HTTP reply, up to the blank line */
        unsigned char ch; if (read_exact(c->fd, &ch, 1, dl) <= 0) { snprintf(err, errcap, "no answer to the websocket request"); goto fail; }
        resp[n++] = (char)ch; if (n >= 4 && !memcmp(resp + n - 4, "\r\n\r\n", 4)) break;
    }
    resp[n] = 0;
    if (!strstr(resp, " 101 ")) { snprintf(err, errcap, "commander refused the websocket (%.40s)", resp); goto fail; }
    dl = now_ms() + 3000;                                          /* hello, then plugins (and transport) are pushed */
    for (int i = 0; i < 6 && !c->plugins; i++) { jv *m = next_msg(c, dl); if (!m) break; track(c, m); }
    if (!c->plugins) { snprintf(err, errcap, "commander sent no plugin list"); goto fail; }
    return c;
fail:
    cmdr_close(c); return NULL;
}

void cmdr_close(cmdr_t *c) {
    if (!c) return;
    if (c->fd >= 0) { unsigned char close_frame[6] = { 0x88, 0x80, 1, 2, 3, 4 }; write_all(c->fd, close_frame, 6); close(c->fd); }
    djson_free(c->plugins); djson_free(c->transport); free(c->msg); free(c);
}

/* wait for a message whose "t" is t (tracking everything else); returns it or NULL */
static jv *wait_for(cmdr_t *c, const char *t, int ms) {
    long dl = now_ms() + ms;
    for (;;) {
        jv *m = next_msg(c, dl); if (!m) return NULL;
        if (!strcmp(djson_str(m, "t", ""), t)) return m;
        track(c, m);
    }
}

int cmdr_plugins_summary(cmdr_t *c, char *buf, size_t cap) {
    size_t n = 0; int count = 0;
    if (cap < 3) return -1;
    buf[n++] = '['; buf[n] = 0;
    for (jv *p = c->plugins ? c->plugins->kid : NULL; p; p = p->next) {
        jv nodes[5]; const char *keys[] = { "id", "name", "vendor", "synth", "params" }; jv *prev = NULL, *first = NULL;
        for (int q = 0; q < 5; q++) {
            jv *src = q < 4 ? djson_get(p, keys[q]) : NULL;
            memset(&nodes[q], 0, sizeof nodes[q]);
            if (q < 4) { if (!src) continue; nodes[q] = *src; nodes[q].kid = NULL; }
            else { nodes[q].t = 'n'; nodes[q].num = djson_len(djson_get(p, "params")); }
            nodes[q].key = (char *)keys[q]; nodes[q].next = NULL;
            if (prev) prev->next = &nodes[q]; else first = &nodes[q];
            prev = &nodes[q];
        }
        jv obj = { 'o', 0, NULL, first, NULL, NULL };
        if (count++) { if (n + 2 > cap) return -1; buf[n++] = ','; }
        int w = djson_write(&obj, buf + n, cap - n); if (w < 0) return -1; n += (size_t)w;
    }
    if (n + 2 > cap) return -1;
    buf[n++] = ']'; buf[n] = 0; return (int)n;
}

int cmdr_plugin_params(cmdr_t *c, int id, char *buf, size_t cap) {
    for (jv *p = c->plugins ? c->plugins->kid : NULL; p; p = p->next)
        if ((int)djson_num(p, "id", -1) == id) {
            jv *ps = djson_get(p, "params"); if (!ps) return djson_write(&(jv){ 'a', 0, NULL, NULL, NULL, NULL }, buf, cap);
            return djson_write(ps, buf, cap);
        }
    snprintf(buf, cap, "[]"); return -1;
}

int cmdr_project(cmdr_t *c, char *buf, size_t cap) {
    if (ws_send_text(c->fd, "{\"t\":\"project\"}")) return -1;
    jv *m = wait_for(c, "project", 3000);
    if (!m) { snprintf(buf, cap, "{\"error\":\"no project reply\"}"); return -1; }
    int r = djson_write(m, buf, cap); djson_free(m); return r;
}

int cmdr_set(cmdr_t *c, int id, int index, double value, char *text, size_t textcap, int wait_ms) {
    if (value < 0) value = 0;
    if (value > 1) value = 1;
    char req[160]; snprintf(req, sizeof req, "{\"t\":\"set\",\"id\":%d,\"i\":%d,\"value\":%.6f}", id, index, value);
    if (text && textcap) text[0] = 0;
    if (ws_send_text(c->fd, req)) return -1;
    long dl = now_ms() + wait_ms;
    for (;;) {
        jv *m = next_msg(c, dl); if (!m) return 0;                 /* no report in time: set anyway */
        if (!strcmp(djson_str(m, "t", ""), "error")) { if (text) snprintf(text, textcap, "%s", djson_str(m, "msg", "error")); djson_free(m); return -1; }
        if (!strcmp(djson_str(m, "t", ""), "values") && (int)djson_num(m, "id", -1) == id)
            for (jv *e = djson_get(m, "v") ? djson_get(m, "v")->kid : NULL; e; e = e->next)
                if ((int)(djson_at(e, 0) ? djson_at(e, 0)->num : -1) == index) {
                    jv *tx = djson_at(e, 2); if (text && tx && tx->t == 's') snprintf(text, textcap, "%s", tx->str);
                    djson_free(m); return 0;
                }
        track(c, m);
    }
}

int cmdr_transport(cmdr_t *c, const char *cmd) {
    if (strcmp(cmd, "play") && strcmp(cmd, "stop") && strcmp(cmd, "continue") && strcmp(cmd, "record")) return -1;
    char req[80]; snprintf(req, sizeof req, "{\"t\":\"transport\",\"cmd\":\"%s\"}", cmd);
    return ws_send_text(c->fd, req);
}

int cmdr_transport_state(cmdr_t *c, char *buf, size_t cap) {
    if (!c->transport) { snprintf(buf, cap, "{}"); return 2; }
    return djson_write(c->transport, buf, cap);
}
