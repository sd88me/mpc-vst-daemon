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

def key(name, w, label, fill=FACE, ink=AMBER, size=24, stroke=LINE):
    svg(name, w, 48, '<rect x="0.5" y="0.5" width="%d" height="47" rx="7" fill="%s" stroke="%s"/>' % (w - 1, fill, stroke)
        + '<text x="%g" y="%g" font-family="Titillium Web" font-size="%d" font-weight="700" fill="%s" text-anchor="middle">%s</text>' % (w / 2, 24 + size * 0.36, size, ink, label))

for c in "QWERTYUIOPASDFGHJKLZXCVBNM1234567890":
    key("key_%s.svg" % c, 86, c)
key("key_space.svg", 520, "SPACE", size=16)
key("key_back.svg", 180, "BACK", size=18)
key("key_clear.svg", 180, "CLEAR", size=18)
key("key_hide.svg", 180, "HIDE", size=18, ink=DIM)
key("key_send.svg", 180, "SEND", fill=AMBER, ink=BG, size=20, stroke=AMBER)
key("btn_keys.svg", 100, "KEYS", size=18)
svg("kb_bg.svg", 1280, 320, '<rect width="1280" height="320" fill="%s"/><rect y="0.5" width="1280" height="1" fill="%s"/>' % (BG, LINE))
