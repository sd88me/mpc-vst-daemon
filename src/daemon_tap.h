/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me
 *
 * The audio the Daemon Tap effect hears, handed to Daemon (the instrument): a ring of 44.1 kHz int16 stereo frames in a file in
 * /dev/shm that both plugins map (they live in the same MPC process, but separate shared objects). The tap writes in its audio
 * callback, Daemon reads in its own: no locks, only the write position is shared (a 64-bit counter of frames ever written).
 * DAEMON_TAP_PATH overrides the file (tests). */
#ifndef DAEMON_TAP_H
#define DAEMON_TAP_H
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define TAP_FRAMES 32768u                       /* ring size in frames (0.74 s): the reader polls every block */
#define TAP_MAGIC 0x44615470u                   /* "DaTp" */
typedef struct { uint32_t magic, frames; uint64_t wpos; uint8_t pad[48]; int16_t pcm[TAP_FRAMES * 2]; } tap_ring_t;

static inline const char *tap_path(void) { const char *p = getenv("DAEMON_TAP_PATH"); return p && *p ? p : "/dev/shm/daemon_tap"; }

/* The tap's side: create or open the file read-write. NULL on failure. */
static inline tap_ring_t *tap_create(void) {
    int f = open(tap_path(), O_RDWR | O_CREAT, 0666); if (f < 0) return NULL;
    if (ftruncate(f, (off_t)sizeof(tap_ring_t)) < 0) { close(f); return NULL; }
    tap_ring_t *r = mmap(NULL, sizeof *r, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0); close(f);
    if (r == MAP_FAILED) return NULL;
    if (r->magic != TAP_MAGIC) { r->frames = TAP_FRAMES; r->wpos = 0; __atomic_store_n(&r->magic, TAP_MAGIC, __ATOMIC_RELEASE); }
    return r;
}
static inline void tap_write(tap_ring_t *r, const int16_t *in_lr, int frames) {
    uint64_t w = __atomic_load_n(&r->wpos, __ATOMIC_RELAXED);
    for (int i = 0; i < frames; i++) { uint32_t k = (uint32_t)((w + (uint64_t)i) % TAP_FRAMES); r->pcm[2 * k] = in_lr[2 * i]; r->pcm[2 * k + 1] = in_lr[2 * i + 1]; }
    __atomic_store_n(&r->wpos, w + (uint64_t)frames, __ATOMIC_RELEASE);
}

/* Daemon's side: map the file read-only if it exists (NULL until a tap has been inserted). */
static inline const tap_ring_t *tap_attach(void) {
    int f = open(tap_path(), O_RDONLY); if (f < 0) return NULL;
    tap_ring_t *r = mmap(NULL, sizeof *r, PROT_READ, MAP_SHARED, f, 0); close(f);
    if (r == MAP_FAILED) return NULL;
    if (r->magic != TAP_MAGIC) { munmap(r, sizeof *r); return NULL; }
    return r;
}
static inline uint64_t tap_pos(const tap_ring_t *r) { return __atomic_load_n(&r->wpos, __ATOMIC_ACQUIRE); }
/* Copies the frames from *rpos up to the write position into out (at most max); a reader that fell more than the ring behind skips ahead. Returns frames. */
static inline int tap_read(const tap_ring_t *r, uint64_t *rpos, int16_t *out_lr, int max) {
    uint64_t w = tap_pos(r); if (w - *rpos > TAP_FRAMES) *rpos = w - TAP_FRAMES;
    int n = (int)(w - *rpos); if (n > max) n = max;
    for (int i = 0; i < n; i++) { uint32_t k = (uint32_t)((*rpos + (uint64_t)i) % TAP_FRAMES); out_lr[2 * i] = r->pcm[2 * k]; out_lr[2 * i + 1] = r->pcm[2 * k + 1]; }
    *rpos += (uint64_t)n; return n;
}
#endif
