#!/usr/bin/env python3
"""Drumbox: draws the big touch buttons (images/btn_*.png) the layout uses. Needs Pillow; run by hand after changing
them (the images are committed, so the release build doesn't need Pillow):  python3 mpc/art.py"""
import os
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "images")


def button(w, h, fill, edge, name):
    s = 3   # drawn large, scaled down: smooth corners
    im = Image.new("RGBA", (w * s, h * s), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle((s, s, w * s - s - 1, h * s - s - 1), radius=10 * s, fill=fill, outline=edge, width=2 * s)
    im.resize((w, h), Image.LANCZOS).save(os.path.join(OUT, name))


os.makedirs(OUT, exist_ok=True)
for w, h in ((220, 70), (180, 64)):
    button(w, h, "#2c2f38", "#4a4f5b", "btn_%dx%d.png" % (w, h))
    button(w, h, "#ff8a3d", "#ffc08f", "btn_%dx%d_on.png" % (w, h))
print("images:", sorted(os.listdir(OUT)))
