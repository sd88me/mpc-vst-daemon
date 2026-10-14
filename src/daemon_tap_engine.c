/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me
 *
 * Daemon Listen: an effect for the audio track that carries the mic. The audio passes through unchanged; a copy goes into the ring Daemon
 * (the instrument) reads for TALK (daemon_tap.h). One tap per device: the last inserted writes. */
#include "engine.h"
#include "daemon_tap.h"
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { tap_ring_t *ring; int peak, shown, rev; long blocks; } tp_t;

static void *create(const char *dir) { (void)dir; tp_t *t = calloc(1, sizeof *t); t->ring = tap_create(); t->shown = -1; return t; }
static void destroy(void *inst) { tp_t *t = inst; if (t->ring) munmap(t->ring, sizeof *t->ring); free(t); }
static void midi_in(void *inst, const uint8_t *m, int n) { (void)inst; (void)m; (void)n; }
static void set_param(void *inst, const char *k, const char *v) { (void)inst; (void)k; (void)v; }
static void render(void *inst, int16_t *o, int f) { (void)inst; memset(o, 0, (size_t)f * 4); }

static void process(void *inst, const int16_t *in, int16_t *out, int frames) {
    tp_t *t = inst;
    memcpy(out, in, (size_t)frames * 4);
    if (t->ring) tap_write(t->ring, in, frames);
    int pk = 0; for (int i = 0; i < frames * 2; i++) { int a = in[i] < 0 ? -in[i] : in[i]; if (a > pk) pk = a; }
    if (pk > t->peak) t->peak = pk;
    if (++t->blocks % 64 == 0) { int pct = t->peak * 100 / 32768 / 5 * 5; if (pct != t->shown) { t->shown = pct; t->rev++; } t->peak = 0; }   /* ~every 0.19 s, in steps of 5 % */
}

static int get_param(void *inst, const char *k, char *b, int n) {
    tp_t *t = inst;
    if (!strcmp(k, "display_rev")) return snprintf(b, n, "%d", t->rev);
    if (!strcmp(k, "status")) return snprintf(b, n, t->ring ? "FEEDING DAEMON  level %d%%" : "NO SHARED MEMORY: Daemon cannot hear this tap", t->shown < 0 ? 0 : t->shown);
    return 0;
}

static const mpc_engine_t ENGINE = { create, destroy, midi_in, set_param, get_param, render, process };
const mpc_engine_t *mpc_engine(void) { return &ENGINE; }
