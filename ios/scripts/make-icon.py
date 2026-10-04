#!/usr/bin/env python3
"""Draws FastPlay's app icon: a play triangle with speed lines, on a blue ground.

    python3 ios/scripts/make-icon.py   (needs Pillow)

Writes ios/FastPlay/Assets.xcassets/AppIcon.appiconset/AppIcon.png, 1024 pixels
square with no transparency, as the App Store wants it. A placeholder: replace the
PNG with real artwork of the same size whenever there is some.
"""
import os
from PIL import Image, ImageDraw

SIZE = 1024
SCALE = 4  # drawn large and shrunk, for smooth edges
big = SIZE * SCALE
image = Image.new("RGB", (big, big))
draw = ImageDraw.Draw(image)

# The ground: a deep blue, lighter towards the top
top, bottom = (40, 120, 255), (16, 40, 140)
for y in range(big):
    t = y / (big - 1)
    draw.line([(0, y), (big, y)], fill=tuple(round(a + (b - a) * t) for a, b in zip(top, bottom)))

def s(v):
    return round(v * SCALE)

# The play triangle, a little right of centre to leave room for the lines
draw.polygon([(s(430), s(292)), (s(430), s(732)), (s(820), s(512))], fill=(255, 255, 255))

# Speed lines trailing it, shorter towards top and bottom
for y, x0 in ((372, 250), (512, 170), (652, 250)):
    draw.rounded_rectangle([s(x0), s(y - 24), s(372), s(y + 24)], radius=s(24), fill=(255, 255, 255))

out = os.path.join(os.path.dirname(__file__), "..", "FastPlay", "Assets.xcassets", "AppIcon.appiconset", "AppIcon.png")
image.resize((SIZE, SIZE), Image.LANCZOS).save(os.path.normpath(out), "PNG")
print("wrote", os.path.normpath(out))
