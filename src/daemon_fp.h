/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
/* Screen fingerprints: tell whether MPC's screen shows what a macro expects, without any model. A fingerprint is a 16 x 8 grid of average
 * brightness of a screen region (from the remote addin's PNG), compared by mean absolute difference, so small live changes (a meter, a
 * cursor, a clock) stay under the tolerance. */
#pragma once
#include "daemon_mcp.h"
#include <stddef.h>
#include <stdint.h>

#define FP_W 16
#define FP_H 8
#define FP_BYTES (FP_W * FP_H)

/* Base64 (standard alphabet) -> bytes; returns the length or -1. */
int fp_b64_decode(const char *b64, uint8_t *out, size_t cap);
/* PNG (8-bit RGB or RGBA, not interlaced) -> RGB. rgb is malloc'd (3*w*h); returns 0 or -1 (err says why). zlib is loaded at run time. */
int fp_png_decode(const uint8_t *png, size_t len, uint8_t **rgb, int *w, int *h, char *err, size_t errcap);
void fp_thumb(const uint8_t *rgb, int w, int h, uint8_t out[FP_BYTES]);
double fp_distance(const uint8_t a[FP_BYTES], const uint8_t b[FP_BYTES]);   /* mean absolute difference, 0..255 */
void fp_hex(const uint8_t t[FP_BYTES], char out[FP_BYTES * 2 + 1]);
int fp_unhex(const char *hex, uint8_t out[FP_BYTES]);                         /* 0 ok, -1 not exactly FP_BYTES*2 hex digits */

/* Take the fingerprint of the screen region now (screenshot of just that region through the remote addin). 0 ok, -1 (err says why). */
int fp_capture(mcp_t *mcp, int x, int y, int w, int h, uint8_t out[FP_BYTES], char *err, size_t errcap);
