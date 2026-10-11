#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 sd88me
"""Writes PNG fixtures for test/fp_test.c: png_fixture.py <outdir>. Rows use PNG filter types 0-4 in turn, so the decoder's unfiltering is
exercised; each image also gets a .raw file with the RGB pixels the decoder must give back."""
import os, struct, sys, zlib

def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)

def paeth(a, b, c):
    p = a + b - c; pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
    return a if pa <= pb and pa <= pc else (b if pb <= pc else c)

def png(w, h, px, bpp=3, depth=8, interlace=0, ctype=None):
    ctype = ctype if ctype is not None else (2 if bpp == 3 else 6)
    raw = bytearray(); prev = bytearray(w * bpp)
    for y in range(h):
        row = bytearray(); f = y % 5
        cur = bytearray(px(x, y)[i] for x in range(w) for i in range(bpp))
        for i in range(w * bpp):
            a = cur[i - bpp] if i >= bpp else 0; b = prev[i]; c = prev[i - bpp] if i >= bpp else 0; x = cur[i]
            row.append((x - [0, a, b, (a + b) // 2, paeth(a, b, c)][f]) & 255)
        raw += bytes([f]) + row; prev = cur
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, ctype, 0, 0, interlace)) + chunk(b"IDAT", zlib.compress(bytes(raw), 6)) + chunk(b"IEND", b"")

out = sys.argv[1]; os.makedirs(out, exist_ok=True)
def save(name, w, h, px, bpp=3, **kw):
    open(os.path.join(out, name + ".png"), "wb").write(png(w, h, px, bpp, **kw))
    open(os.path.join(out, name + ".raw"), "wb").write(bytes(px(x, y)[i] for y in range(h) for x in range(w) for i in range(3)))
grad = lambda x, y: ((x * 4) & 255, (y * 5) & 255, (x * y) & 255, 255)
save("grad_rgb", 64, 48, grad)
save("grad_rgba", 64, 48, grad, bpp=4)
half = lambda x, y: (0, 0, 0, 255) if x < 80 else (255, 255, 255, 255)
clamp = lambda v: max(0, min(255, v))
noisy = lambda x, y: (clamp((0 if x < 80 else 255) + ((x * 7 + y * 13) % 9) - 4),) * 3 + (255,)
save("half_noisy", 160, 80, noisy)
inv = lambda x, y: (255, 255, 255, 255) if x < 80 else (0, 0, 0, 255)
save("half_inverted", 160, 80, inv)
save("half", 160, 80, half)
open(os.path.join(out, "interlaced.png"), "wb").write(png(8, 8, grad, interlace=1))
open(os.path.join(out, "depth16.png"), "wb").write(png(8, 8, grad, depth=16))
