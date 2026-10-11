/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_mcp.h"
#include "daemon_json.h"
#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

struct mcp { char host[48]; int port; int next_id; };

static long now_ms(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000L + t.tv_nsec / 1000000L; }

/* POST body to /mcp; the reply body (NUL-terminated, malloc'd) or NULL. status gets the HTTP status. */
static char *post(mcp_t *m, const char *body, int *status, int timeout_ms, char *err, size_t errcap) {
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)m->port);
    if (inet_pton(AF_INET, m->host, &a.sin_addr) != 1) { snprintf(err, errcap, "bad address"); return NULL; }
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    struct timeval tv = { 3, 0 }; setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) { snprintf(err, errcap, "remote addin not reachable on %s:%d", m->host, m->port); close(fd); return NULL; }
    size_t bl = strlen(body); char *req = malloc(bl + 400);
    int hl = snprintf(req, bl + 400, "POST /mcp HTTP/1.1\r\nHost: %s:%d\r\nContent-Type: application/json\r\nAccept: application/json, text/event-stream\r\nMCP-Protocol-Version: 2025-06-18\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n%s", m->host, m->port, bl, body);
    size_t sent = 0; while (sent < (size_t)hl) { ssize_t k = send(fd, req + sent, (size_t)hl - sent, MSG_NOSIGNAL); if (k <= 0) { snprintf(err, errcap, "remote addin closed the connection"); free(req); close(fd); return NULL; } sent += (size_t)k; }
    free(req);
    size_t cap = 1 << 16, n = 0; char *buf = malloc(cap + 1); long dl = now_ms() + timeout_ms;
    for (;;) {
        long left = dl - now_ms(); if (left < 0) { snprintf(err, errcap, "remote addin did not answer in time"); free(buf); close(fd); return NULL; }
        struct pollfd p = { fd, POLLIN, 0 }; int r = poll(&p, 1, (int)left); if (r <= 0) continue;
        if (n == cap) { cap *= 2; buf = realloc(buf, cap + 1); }
        ssize_t k = read(fd, buf + n, cap - n); if (k <= 0) break;
        n += (size_t)k;
    }
    close(fd); buf[n] = 0;
    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (!hdr_end || sscanf(buf, "HTTP/1.%*d %d", status) != 1) { snprintf(err, errcap, "bad reply from the remote addin"); free(buf); return NULL; }
    char *body_p = hdr_end + 4; char *out;
    if (strcasestr(buf, "transfer-encoding: chunked") && strcasestr(buf, "transfer-encoding: chunked") < hdr_end) {
        out = malloc(n + 1); size_t o = 0; char *p = body_p;
        for (;;) { char *e; unsigned long cl = strtoul(p, &e, 16); if (e == p || cl == 0) break; p = strstr(e, "\r\n"); if (!p) break; p += 2; if (p + cl > buf + n) break; memcpy(out + o, p, cl); o += cl; p += cl + 2; }
        out[o] = 0;
    } else out = strdup(body_p);
    free(buf); return out;
}

static char *rpc(mcp_t *m, const char *method, const char *params_json, int timeout_ms, char *err, size_t errcap) {
    char *body = malloc(strlen(params_json) + 120);
    snprintf(body, strlen(params_json) + 120, "{\"jsonrpc\":\"2.0\",\"id\":%d,\"method\":\"%s\",\"params\":%s}", m->next_id++, method, params_json);
    int st = 0; char *r = post(m, body, &st, timeout_ms, err, errcap); free(body);
    if (r && st != 200) { snprintf(err, errcap, "remote addin answered HTTP %d", st); free(r); return NULL; }
    return r;
}

mcp_t *mcp_open(const char *host, int port, char *err, size_t errcap) {
    mcp_t *m = calloc(1, sizeof *m); snprintf(m->host, sizeof m->host, "%s", host); m->port = port; m->next_id = 1;
    char *r = rpc(m, "initialize", "{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"daemon\",\"version\":\"0.1\"}}", 5000, err, errcap);
    if (!r) { free(m); return NULL; }
    jv *v = djson_parse(r, strlen(r)); free(r);
    if (!v || !djson_get(v, "result")) { snprintf(err, errcap, "remote addin did not initialize"); djson_free(v); free(m); return NULL; }
    djson_free(v);
    int st = 0; char e2[64]; char *n = post(m, "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}", &st, 3000, e2, sizeof e2); free(n);
    return m;
}

void mcp_close(mcp_t *m) { free(m); }

int mcp_call(mcp_t *m, const char *tool, const char *args_json, char *text, size_t cap, char **image_b64) {
    if (image_b64) *image_b64 = NULL;
    if (cap) text[0] = 0;
    char *params = malloc(strlen(args_json) + strlen(tool) + 60); snprintf(params, strlen(args_json) + strlen(tool) + 60, "{\"name\":\"%s\",\"arguments\":%s}", tool, args_json);
    char err[120] = ""; char *r = rpc(m, "tools/call", params, 30000, err, sizeof err); free(params);
    if (!r) { snprintf(text, cap, "%s", err); return -1; }
    jv *v = djson_parse(r, strlen(r)); free(r);
    if (!v) { snprintf(text, cap, "reply is not JSON"); return -1; }
    jv *res = djson_get(v, "result");
    if (!res) { snprintf(text, cap, "%s", djson_str(djson_get(v, "error"), "message", "the remote addin returned an error")); djson_free(v); return -1; }
    size_t n = 0;
    for (jv *p = djson_get(res, "content") ? djson_get(res, "content")->kid : NULL; p; p = p->next) {
        const char *t = djson_str(p, "type", "");
        if (!strcmp(t, "text")) { const char *s = djson_str(p, "text", ""); size_t l = strlen(s); if (n + l + 2 < cap) { if (n) text[n++] = '\n'; memcpy(text + n, s, l); n += l; text[n] = 0; } }
        else if (!strcmp(t, "image") && image_b64 && !*image_b64) *image_b64 = strdup(djson_str(p, "data", ""));
    }
    int bad = djson_get(res, "isError") && djson_get(res, "isError")->num != 0;
    djson_free(v);
    return bad ? -1 : 0;
}
