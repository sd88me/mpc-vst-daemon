/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* A short-lived session with the commander addin (jacob-sabella/mpc-addin-commander, docs/PROTOCOL.md): one WebSocket to
 * 127.0.0.1:<port>/ws. On connect the addin pushes hello, the plugin list and the transport; we then read the project, set
 * parameters and press transport buttons over the same socket. Blocking with short timeouts: call from a worker thread only. */
#pragma once
#include <stddef.h>

typedef struct cmdr cmdr_t;
cmdr_t *cmdr_open(const char *host, int port, char *err, size_t errcap);     /* NULL + err on failure; reads hello and plugins */
void cmdr_close(cmdr_t *c);

/* Plugins as the addin sent them: a compact JSON array of {id, name, vendor, product, synth, params: n}. Written into buf. */
int cmdr_plugins_summary(cmdr_t *c, char *buf, size_t cap);
/* One plugin's parameters: [{i, name, value, text}] ; -1 if the id is unknown. */
int cmdr_plugin_params(cmdr_t *c, int id, char *buf, size_t cap);
/* The same, only the parameters whose name contains one of the |-separated words (case-insensitive); NULL or "" keeps all. */
int cmdr_plugin_params_match(cmdr_t *c, int id, const char *match, char *buf, size_t cap);
/* The project snapshot (the addin's JSON), asked for over the socket. */
int cmdr_project(cmdr_t *c, char *buf, size_t cap);
/* setParameter on an instance. Waits up to wait_ms for the addin to report the value text back; text gets it ("" if none came). */
int cmdr_set(cmdr_t *c, int id, int index, double value, char *text, size_t textcap, int wait_ms);   /* 0 ok, -1 error */
/* "play" | "stop" | "continue" | "record" */
int cmdr_transport(cmdr_t *c, const char *cmd);
/* Latest transport state as a JSON object string (playing, recording, tempo, bar, beat). */
int cmdr_transport_state(cmdr_t *c, char *buf, size_t cap);
