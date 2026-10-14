#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 sd88me
"""Draws the on-screen keyboard's key images into ../images/ (SVG). Amber on black like the rest of the skin.
Run: python3 art/make_keys.py"""
import os
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "images")
AMBER, DIM, LINE, FACE, BG, CYAN = "#ffb02a", "#8a5a10", "#7a4c0c", "#1a1208", "#0b0804", "#19e0ff"

def svg(name, w, h, body):
    open(os.path.join(OUT, name), "w").write('<svg xmlns="http://www.w3.org/2000/svg" width="%d" height="%d" viewBox="0 0 %d %d">%s</svg>\n' % (w, h, w, h, body))

def key(name, w, label, h=68, fill=FACE, ink=AMBER, size=34, stroke=LINE):
    svg(name, w, h, '<rect x="1" y="1" width="%d" height="%d" rx="9" fill="%s" stroke="%s" stroke-width="2"/>' % (w - 2, h - 2, fill, stroke)
        + '<text x="%g" y="%g" font-family="Titillium Web" font-size="%d" font-weight="700" fill="%s" text-anchor="middle">%s</text>' % (w / 2, h / 2 + size * 0.36, size, ink, label))

for c in "QWERTYUIOPASDFGHJKLZXCVBNM":
    key("key_%s.svg" % c, 112, c)
for c in "1234567890":
    key("key_%s.svg" % c, 112, c, h=40, size=26)
key("key_space.svg", 560, "SPACE", size=22)
key("key_back.svg", 150, "BACK", size=24)
key("key_clear.svg", 190, "CLEAR", size=24)
key("key_hide.svg", 180, "HIDE", size=24, ink=DIM)
key("key_send.svg", 220, "SEND", fill=AMBER, ink=BG, size=28, stroke=AMBER)
key("btn_keys.svg", 100, "KEYS", size=18, h=48)
svg("kb_bg.svg", 1280, 394, '<rect width="1280" height="394" fill="%s"/><rect y="0.5" width="1280" height="1" fill="%s"/>' % (BG, LINE))
