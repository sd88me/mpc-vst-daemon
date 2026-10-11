#!/usr/bin/env python3
"""Draws the Daemon face, one PNG per state, into ../images/ (200x180, transparent). Amber lines, cyan for the live parts.
Run: python3 art/make_face.py   (needs Pillow)."""
import math
import os
from PIL import Image, ImageDraw, ImageFilter

AMBER, DIM, CYAN, RED = (255, 176, 42), (138, 90, 16), (25, 224, 255), (255, 80, 64)
W, H = 200, 180
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "images")


def eye(d, cx, cy, kind, col):
    if kind == "open":
        d.ellipse((cx - 22, cy - 22, cx + 22, cy + 22), outline=AMBER, width=4); d.ellipse((cx - 7, cy - 7, cx + 7, cy + 7), fill=col)
    elif kind == "wide":
        d.ellipse((cx - 26, cy - 26, cx + 26, cy + 26), outline=AMBER, width=4); d.ellipse((cx - 11, cy - 11, cx + 11, cy + 11), fill=col)
    elif kind == "aside":
        d.ellipse((cx - 22, cy - 22, cx + 22, cy + 22), outline=AMBER, width=4); d.ellipse((cx + 6, cy - 6, cx + 20, cy + 8), fill=col)
    elif kind == "happy":
        d.arc((cx - 22, cy - 14, cx + 22, cy + 30), 200, 340, fill=col, width=5)
    elif kind == "x":
        d.line((cx - 16, cy - 16, cx + 16, cy + 16), fill=col, width=5); d.line((cx - 16, cy + 16, cx + 16, cy - 16), fill=col, width=5)


def mouth(d, kind, col):
    if kind == "flat":
        d.line((52, 142, 148, 142), fill=AMBER, width=4)
    elif kind == "wave":
        d.line([(40 + i * 2.2, 142 + math.sin(i * .6) * math.sin(i * .13) * 16) for i in range(0, 50)], fill=col, width=4)
    elif kind == "dots":
        for k in range(3): d.ellipse((72 + k * 24, 136, 84 + k * 24, 148), fill=col)
    elif kind == "smile":
        d.arc((50, 100, 150, 160), 20, 160, fill=col, width=5)
    elif kind == "frown":
        d.arc((50, 130, 150, 190), 200, 340, fill=col, width=5)


STATES = {  # name: (eye, eye colour, mouth, mouth colour, frame)
    "idle": ("open", CYAN, "flat", AMBER, AMBER),
    "listening": ("wide", CYAN, "wave", CYAN, AMBER),
    "thinking": ("aside", CYAN, "dots", CYAN, AMBER),
    "done": ("happy", CYAN, "smile", CYAN, AMBER),
    "error": ("x", RED, "frown", RED, RED),
}
os.makedirs(OUT, exist_ok=True)
for name, (e, ec, m, mc, fr) in STATES.items():
    layer = Image.new("RGBA", (W, H), (0, 0, 0, 0)); d = ImageDraw.Draw(layer)
    d.rectangle((2, 2, W - 3, H - 3), outline=fr, width=4)
    for x in (62, 138): eye(d, x, 66, e, ec)
    mouth(d, m, mc)
    glow = layer.filter(ImageFilter.GaussianBlur(4))
    out = Image.alpha_composite(glow, layer)
    out.save(os.path.join(OUT, "face_%s.png" % name))
print("wrote", len(STATES), "faces")
