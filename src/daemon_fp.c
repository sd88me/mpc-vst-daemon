/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 sd88me */
#define _GNU_SOURCE
#include "daemon_fp.h"
#include <dlfcn.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int fp_b64_decode(const char *b64, uint8_t *out, size_t cap) {
    static signed char tab[256]; static int init;
    if (!init) { memset(tab, -1, sizeof tab); const char *a = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"; for (int i = 0; i < 64; i++) tab[(unsigned char)a[i]] = (signed char)i; init = 1; }
    size_t n = 0; unsigned acc = 0; int bits = 0;
    for (const char *p = b64; *p && *p != '='; p++) {
        if (*p == '\n' || *p == '\r' || *p == ' ') continue;
        int v = tab[(unsigned char)*p]; if (v < 0) return -1;
        acc = acc << 6 | (unsigned)v; bits += 6;
        if (bits >= 8) { bits -= 8; if (n >= cap) return -1; out[n++] = (uint8_t)(acc >> bits & 255); }
    }
    return (int)n;
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

int fp_png_decode(const uint8_t *png, size_t len, uint8_t **rgb, int *w, int *h, char *err, size_t errcap) {
    static const uint8_t sig[8] = { 0x89, 'P', 'N', 'G', 13, 10, 26, 10 };
    *rgb = NULL;
    if (len < 33 || memcmp(png, sig, 8)) { snprintf(err, errcap, "not a PNG"); return -1; }
    int W = 0, H = 0, depth = 0, ctype = -1, interlace = 0; uint8_t *idat = malloc(len); size_t il = 0;
    for (size_t p = 8; p + 12 <= len;) {
        uint32_t cl = be32(png + p); const uint8_t *type = png + p + 4;
        if ((size_t)cl > len - p - 12) break;
        if (!memcmp(type, "IHDR", 4) && cl >= 13) { W = (int)be32(png + p + 8); H = (int)be32(png + p + 12); depth = png[p + 16]; ctype = png[p + 17]; interlace = png[p + 20]; }
        else if (!memcmp(type, "IDAT", 4)) { memcpy(idat + il, png + p + 8, cl); il += cl; }
        else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + cl;
    }
    int bpp = ctype == 2 ? 3 : ctype == 6 ? 4 : 0;
    if (!bpp || depth != 8 || interlace || W <= 0 || H <= 0 || W > 4096 || H > 4096) { snprintf(err, errcap, "unsupported PNG (need 8-bit RGB or RGBA, not interlaced)"); free(idat); return -1; }
    void *z = dlopen("libz.so.1", RTLD_NOW | RTLD_LOCAL);
    int (*uncompress_)(uint8_t *, unsigned long *, const uint8_t *, unsigned long) = z ? dlsym(z, "uncompress") : NULL;
    if (!uncompress_) { snprintf(err, errcap, "libz.so.1 not available"); free(idat); if (z) dlclose(z); return -1; }
    size_t stride = (size_t)W * bpp, rawlen = (size_t)H * (stride + 1); uint8_t *raw = malloc(rawlen); unsigned long dl = rawlen;
    int rc = uncompress_(raw, &dl, idat, (unsigned long)il); free(idat); dlclose(z);
    if (rc != 0 || dl != rawlen) { snprintf(err, errcap, "PNG data is damaged"); free(raw); return -1; }
    uint8_t *out = malloc((size_t)W * H * 3); uint8_t *prev = calloc(stride, 1), *cur = malloc(stride);
    for (int y = 0; y < H; y++) {
        const uint8_t *line = raw + (size_t)y * (stride + 1); int f = line[0]; line++;
        for (size_t i = 0; i < stride; i++) {
            int a = i >= (size_t)bpp ? cur[i - bpp] : 0, b = prev[i], c = i >= (size_t)bpp ? prev[i - bpp] : 0, x = line[i], v;
            switch (f) {
            case 0: v = x; break; case 1: v = x + a; break; case 2: v = x + b; break; case 3: v = x + (a + b) / 2; break;
            case 4: { int pp = a + b - c, pa = abs(pp - a), pb = abs(pp - b), pc = abs(pp - c); v = x + (pa <= pb && pa <= pc ? a : pb <= pc ? b : c); break; }
            default: snprintf(err, errcap, "PNG data is damaged"); free(raw); free(out); free(prev); free(cur); return -1;
            }
            cur[i] = (uint8_t)v;
        }
        for (int x = 0; x < W; x++) { out[((size_t)y * W + x) * 3] = cur[x * bpp]; out[((size_t)y * W + x) * 3 + 1] = cur[x * bpp + 1]; out[((size_t)y * W + x) * 3 + 2] = cur[x * bpp + 2]; }
        uint8_t *t = prev; prev = cur; cur = t;
    }
    free(raw); free(prev); free(cur);
    *rgb = out; *w = W; *h = H; return 0;
}

void fp_thumb(const uint8_t *rgb, int w, int h, uint8_t out[FP_BYTES]) {
    for (int cy = 0; cy < FP_H; cy++) for (int cx = 0; cx < FP_W; cx++) {
        int x0 = cx * w / FP_W, x1 = (cx + 1) * w / FP_W, y0 = cy * h / FP_H, y1 = (cy + 1) * h / FP_H;
        if (x1 <= x0) x1 = x0 + 1;
        if (y1 <= y0) y1 = y0 + 1;
        if (x1 > w) x1 = w;
        if (y1 > h) y1 = h;
        double sum = 0; long n = 0;
        for (int y = y0; y < y1; y++) for (int x = x0; x < x1; x++) { const uint8_t *p = rgb + ((size_t)y * w + x) * 3; sum += 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2]; n++; }
        out[cy * FP_W + cx] = (uint8_t)lround(n ? sum / (double)n : 0);
    }
}

double fp_distance(const uint8_t a[FP_BYTES], const uint8_t b[FP_BYTES]) {
    long s = 0; for (int i = 0; i < FP_BYTES; i++) s += abs((int)a[i] - (int)b[i]);
    return (double)s / FP_BYTES;
}

void fp_hex(const uint8_t t[FP_BYTES], char out[FP_BYTES * 2 + 1]) { for (int i = 0; i < FP_BYTES; i++) sprintf(out + 2 * i, "%02x", t[i]); out[FP_BYTES * 2] = 0; }

int fp_unhex(const char *hex, uint8_t out[FP_BYTES]) {
    if (strlen(hex) != FP_BYTES * 2) return -1;
    for (int i = 0; i < FP_BYTES; i++) { unsigned v; char t[3] = { hex[2 * i], hex[2 * i + 1], 0 }; char *e; v = (unsigned)strtoul(t, &e, 16); if (*e) return -1; out[i] = (uint8_t)v; }
    return 0;
}

int fp_capture(mcp_t *mcp, int x, int y, int w, int h, uint8_t out[FP_BYTES], char *err, size_t errcap) {
    char args[160], text[300]; char *b64 = NULL;
    snprintf(args, sizeof args, "{\"x\":%d,\"y\":%d,\"width\":%d,\"height\":%d,\"zoom\":1}", x, y, w, h);
    if (mcp_call(mcp, "screenshot", args, text, sizeof text, &b64) < 0 || !b64) { snprintf(err, errcap, "could not read the screen: %.120s", text); free(b64); return -1; }
    size_t cap = strlen(b64) * 3 / 4 + 8; uint8_t *png = malloc(cap); int n = fp_b64_decode(b64, png, cap); free(b64);
    if (n < 0) { snprintf(err, errcap, "bad image data from the remote addin"); free(png); return -1; }
    uint8_t *rgb; int iw, ih; int rc = fp_png_decode(png, (size_t)n, &rgb, &iw, &ih, err, errcap); free(png);
    if (rc) return -1;
    fp_thumb(rgb, iw, ih, out); free(rgb); return 0;
}
