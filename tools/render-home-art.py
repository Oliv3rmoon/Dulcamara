#!/usr/bin/env python3
# Nightshade Player - Draw the home-screen pictures: the icon, the background and the launch picture.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw Nightshade Player's home-screen pictures in Nightshade's style.

usage: python3 tools/render-home-art.py              (needs Pillow and NumPy)
       python3 tools/render-home-art.py --preview DIR  (small copies in DIR; nothing else changes)

Nightshade Station's tools/render-home-art.py (itself Nightshade's, Oliv3rmoon/Nightshade), with an RPG's
message window in place of Station's disc: the same nightshade flower (five red petals round a pale cone),
black berries and dark leaves, on near black with a red glow and faint stars, in Nightshade's palette (its
"nightly" theme). The name is set in Inter (assets/fonts, SIL Open Font License).

Writes, in sce_sys/:
  icon0.png                     512x512: an RPG's menu window, the cursor at its first entry, with the
                                nightshade blooming over it
  background-source.png         3840x2160 (pic0, on the home screen): the window of a title screen at the
                                right with the name in it, the nightshade climbing to flower over its corner
  launch-background-source.png  3840x2160 (pic1, while the app starts): the flower over a window holding
                                the name
  pic0.dds, pic1.dds            those two as BC7 (one 3840x2160 BC7_UNORM DX10 texture, no
                                mipmaps, as tools/validate-assets.sh asks)
Everything is procedural; the same seeds give the same files every time.
"""
import io
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parent.parent
target = root / 'sce_sys'

# Nightshade's palette: base, accent_deep, accent, accent_pale; the leaves' near black.
BASE = (0x09, 0x09, 0x0b)
DEEP = (0x5a, 0x14, 0x19)
RED = (0xe5, 0x48, 0x4d)
PALE = (0xf3, 0xa9, 0xad)
LEAF = (0x17, 0x08, 0x0b)
# Nightshade's panel and text colours (its "nightly" theme): the windows' glass, the name, the dim lines.
PANEL = (0x14, 0x14, 0x17)
EDGE = (0x2b, 0x2b, 0x31)
TEXT = (0xed, 0xed, 0xf0)
SOFT = (0xb4, 0xb4, 0xbc)
DIM = (0x8f, 0x8f, 0x98)


def rgb(color):
    return np.array(color, np.float32) / 255.0


def canvas(width, height):
    image = np.empty((height, width, 3), np.float32)
    image[:] = rgb(BASE)
    return image


def glow(image, cx, cy, radius, color, strength, ring=None, width=None):
    """Adds a soft round light; with ring, a ring of light of that radius instead."""
    height, width_px = image.shape[:2]
    y, x = np.ogrid[:height, :width_px]
    d = np.sqrt((x - cx) ** 2 + (y - cy) ** 2, dtype=np.float32)
    if ring is None:
        falloff = np.exp(-(d / radius) ** 2)
    else:
        falloff = np.exp(-((d - ring) / width) ** 2)
    image += falloff[..., None] * rgb(color) * strength


def vignette(image, amount):
    height, width = image.shape[:2]
    y, x = np.ogrid[:height, :width]
    d = np.sqrt(((x - width / 2) / (width / 2)) ** 2 + ((y - height / 2) / (height / 2)) ** 2)
    image *= (1.0 - amount * np.clip(d - 0.55, 0, None) ** 1.5)[..., None]


def stars(image, count, seed, avoid=None):
    rng = np.random.default_rng(seed)
    height, width = image.shape[:2]
    for _ in range(count):
        x, y = rng.uniform(0, width), rng.uniform(0, height * 0.8)
        if avoid and math.hypot(x - avoid[0], y - avoid[1]) < avoid[2]:
            continue
        size = rng.uniform(0.6, 1.6) * width / 3840
        light = rng.uniform(0.15, 0.6) ** 2
        tint = PALE if rng.random() < 0.3 else (0xff, 0xf4, 0xf4)
        r = int(size * 4) + 2
        x0, y0 = int(x) - r, int(y) - r
        if x0 < 0 or y0 < 0 or x0 + 2 * r >= width or y0 + 2 * r >= height:
            continue
        yy, xx = np.mgrid[y0:y0 + 2 * r, x0:x0 + 2 * r]
        dot = np.exp(-(((xx - x) ** 2 + (yy - y) ** 2) / (size * size)))
        image[y0:y0 + 2 * r, x0:x0 + 2 * r] += dot[..., None].astype(np.float32) * rgb(tint) * light


def leaf_points(cx, cy, length, wide, angle, steps=64, bend=0.0):
    """A pointed leaf or petal from (cx, cy) along angle (radians): its outline."""
    left, right = [], []
    for i in range(steps + 1):
        t = i / steps
        w = wide * (math.sin(math.pi * t) ** 0.85) * (1 - t) ** 0.25
        off = bend * length * t * t
        left.append((t * length, w + off))
        right.append((t * length, -w + off))
    outline = left + right[::-1]
    c, s = math.cos(angle), math.sin(angle)
    return [(cx + px * c - py * s, cy + px * s + py * c) for px, py in outline]


def shape_mask(points, box, scale=4):
    """The anti-aliased coverage of a polygon inside box (x0, y0, x1, y1)."""
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    big = Image.new('L', (w * scale, h * scale), 0)
    ImageDraw.Draw(big).polygon([((px - x0) * scale, (py - y0) * scale) for px, py in points], fill=255)
    return np.asarray(big.resize((w, h), Image.LANCZOS), np.float32) / 255.0


def stroke_mask(points, thickness, box, scale=4):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    big = Image.new('L', (w * scale, h * scale), 0)
    ImageDraw.Draw(big).line([((px - x0) * scale, (py - y0) * scale) for px, py in points], fill=255,
                             width=max(1, int(thickness * scale)), joint='curve')
    return np.asarray(big.resize((w, h), Image.LANCZOS), np.float32) / 255.0


def bounds(image, points, pad=4):
    height, width = image.shape[:2]
    xs, ys = [p[0] for p in points], [p[1] for p in points]
    return (max(0, int(min(xs)) - pad), max(0, int(min(ys)) - pad),
            min(width, int(max(xs)) + pad), min(height, int(max(ys)) + pad))


def put(image, box, mask, color):
    """Covers box with color (an RGB triple or an array the size of box) by mask."""
    x0, y0, x1, y1 = box
    region = image[y0:y1, x0:x1]
    fill = color if isinstance(color, np.ndarray) and color.ndim == 3 else rgb(color)
    region[:] = region * (1 - mask[..., None]) + fill * mask[..., None]


def leaf(image, cx, cy, length, wide, angle, bend=0.0, rim=0.35):
    points = leaf_points(cx, cy, length, wide, angle, bend=bend)
    box = bounds(image, points, pad=int(length * 0.05) + 4)
    mask = shape_mask(points, box)
    # Red light along one edge: the leaf less a copy shifted away from the light.
    shift = max(2, int(length * 0.012))
    moved = np.zeros_like(mask)
    moved[shift:, shift:] = mask[:-shift, :-shift]
    edge = np.clip(mask - moved, 0, 1)
    put(image, box, mask, LEAF)
    image[box[1]:box[3], box[0]:box[2]] += edge[..., None] * rgb(RED) * rim
    rib = [(cx + math.cos(angle) * length * t, cy + math.sin(angle) * length * t + bend * length * t * t * math.cos(angle))
           for t in np.linspace(0.02, 0.85, 24)]
    put(image, box, stroke_mask(rib, max(1.0, length * 0.006), box) * 0.5, DEEP)


def flower(image, cx, cy, size, rotation):
    """Five red petals round a pale cone, with a red light behind."""
    glow(image, cx, cy, size * 1.2, RED, 0.22)
    for k in range(5):
        angle = rotation + k * 2 * math.pi / 5
        points = leaf_points(cx, cy, size, size * 0.30, angle)
        box = bounds(image, points)
        mask = shape_mask(points, box)
        yy, xx = np.mgrid[box[1]:box[3], box[0]:box[2]]
        t = np.clip(np.hypot(xx - cx, yy - cy) / size, 0, 1)[..., None].astype(np.float32)
        color = rgb(DEEP) * (1 - t) + rgb(RED) * t
        color = color + (rgb(PALE) - color) * np.clip((t - 0.78) * 2.5, 0, 1) * 0.35
        put(image, box, mask, color)
        rib = [(cx + math.cos(angle) * size * s, cy + math.sin(angle) * size * s) for s in np.linspace(0.18, 0.8, 16)]
        put(image, box, stroke_mask(rib, max(1.0, size * 0.018), box) * 0.55, DEEP)
    # The cone: a pale star of short points, and its tip.
    cone = []
    for k in range(10):
        a = rotation + math.pi / 5 + k * math.pi / 5
        r = size * (0.26 if k % 2 == 0 else 0.11)
        cone.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    box = bounds(image, cone)
    yy, xx = np.mgrid[box[1]:box[3], box[0]:box[2]]
    t = np.clip(np.hypot(xx - cx, yy - cy) / (size * 0.26), 0, 1)[..., None].astype(np.float32)
    put(image, box, shape_mask(cone, box), rgb((0xff, 0xf1, 0xe6)) * (1 - t) + rgb(PALE) * t)
    glow(image, cx, cy, size * 0.08, (0xff, 0xff, 0xff), 0.5)


def berry(image, cx, cy, radius):
    """A glossy black berry lit red from the upper left, with a white highlight."""
    x0, y0 = int(cx - radius) - 2, int(cy - radius) - 2
    x1, y1 = int(cx + radius) + 3, int(cy + radius) + 3
    yy, xx = np.mgrid[y0:y1, x0:x1].astype(np.float32)
    nx, ny = (xx - cx) / radius, (yy - cy) / radius
    d = np.sqrt(nx * nx + ny * ny)
    mask = np.clip((1 - d) * radius * 0.7, 0, 1)
    nz = np.sqrt(np.clip(1 - d * d, 0, 1))
    light = np.clip(-0.55 * nx - 0.55 * ny + 0.62 * nz, 0, 1)
    rim = np.clip(d - 0.72, 0, 1) / 0.28 * np.clip(nx + ny, 0, 1)
    color = (rgb((0x0c, 0x0b, 0x0e))[None, None] + light[..., None] ** 3 * rgb(DEEP) * 0.9
             + rim[..., None] * rgb(RED) * 0.55)
    spec = np.exp(-(((nx + 0.38) ** 2 + (ny + 0.4) ** 2) / 0.012))
    color = color + spec[..., None] * 0.85
    put(image, (x0, y0, x1, y1), mask, color)


def stem(image, points, thickness):
    box = bounds(image, points, pad=int(thickness) + 4)
    put(image, box, stroke_mask(points, thickness, box), (0x2a, 0x0b, 0x10))


def curve(a, b, c, steps=40):
    return [((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0],
             (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1]) for t in np.linspace(0, 1, steps)]


def berries(image, x, y, r):
    for dx, dy, s in ((0, 0, 1.0), (1.55, 0.55, 0.86), (0.55, 1.6, 0.8)):
        berry(image, x + dx * r, y + dy * r, r * s)


# ---- Nightshade Player's own: an RPG's window and the name -------------------------------------


FONT = root / 'assets' / 'fonts' / 'Inter-600.ttf'


def rounded_mask(width, height, radius, scale=4):
    """A rounded rectangle's coverage, width x height, drawn large and shrunk for soft edges."""
    big = Image.new('L', (width * scale, height * scale), 0)
    ImageDraw.Draw(big).rounded_rectangle((0, 0, width * scale - 1, height * scale - 1), radius * scale, fill=255)
    return np.asarray(big.resize((width, height), Image.LANCZOS), np.float32) / 255.0


def window(image, x, y, w, h):
    """An RPG's message window in Nightshade's colours: a dark glassy panel, lighter at the top, inside a pale
    line and a red one, as RPG Maker 2000 draws its windows from a system graphic."""
    x, y, w, h = int(x), int(y), int(w), int(h)
    r = max(4, int(min(w, h) * 0.06))
    edge = max(2, int(min(w, h) * 0.018))
    outer = rounded_mask(w, h, r)
    inner = np.zeros_like(outer)
    inner[edge:h - edge, edge:w - edge] = rounded_mask(w - 2 * edge, h - 2 * edge, max(2, r - edge))
    core = np.zeros_like(outer)
    e2 = edge * 2
    core[e2:h - e2, e2:w - e2] = rounded_mask(w - 2 * e2, h - 2 * e2, max(2, r - e2))
    region = image[y:y + h, x:x + w]
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None, None]
    fill = rgb(PANEL) * (1.25 - 0.45 * t) + rgb(DEEP) * 0.10 * (1 - t)
    region[:] = region * (1 - outer[..., None]) + rgb(PALE) * 0.75 * outer[..., None]       # the pale line
    region[:] = region * (1 - inner[..., None]) + rgb(RED) * inner[..., None]               # the red line
    region[:] = region * (1 - core[..., None] * 0.94) + fill * core[..., None] * 0.94       # the panel
    # a sheen along the top of the panel
    sheen = np.exp(-((t[..., 0] - 0.06) / 0.05) ** 2) * core
    region += sheen[..., None] * rgb(PALE) * 0.05


def cursor(image, x, y, size, color=None):
    """The menu cursor of an RPG: a small triangle pointing right, in square pixels."""
    color = rgb(color or RED)
    px = max(1, int(size / 7))
    for row in range(7):
        span = 4 - abs(3 - row)
        for col in range(span):
            x0, y0 = int(x + col * px), int(y + row * px)
            image[y0:y0 + px, x0:x0 + px] = color


def text(image, x, y, words, size, color, spacing=0.0, anchor='la', weight=1.0):
    """Words in Inter (assets/fonts, SIL OFL), as a mask over the picture."""
    from PIL import ImageFont
    font = ImageFont.truetype(str(FONT), int(size))
    height, width = image.shape[:2]
    mask = Image.new('L', (width, height), 0)
    draw = ImageDraw.Draw(mask)
    if spacing:
        # letter by letter, for the spaced-out word
        total = sum(draw.textlength(c, font=font) for c in words) + spacing * size * (len(words) - 1)
        cx = x - total / 2 if anchor[0] == 'm' else x - total if anchor[0] == 'r' else x
        for c in words:
            draw.text((cx, y), c, font=font, fill=255, anchor='l' + anchor[1])
            cx += draw.textlength(c, font=font) + spacing * size
    else:
        draw.text((x, y), words, font=font, fill=255, anchor=anchor)
    a = np.asarray(mask, np.float32)[..., None] / 255.0 * weight
    image[:] = image * (1 - a) + rgb(color) * a


def lines(image, x, y, w, count, gap, thickness, color, lengths):
    """Text lines in a window, as bars: a menu's entries seen from afar."""
    for i in range(count):
        y0 = int(y + i * gap)
        image[y0:y0 + int(thickness), int(x):int(x + w * lengths[i % len(lengths)])] = rgb(color)


def vine(image, x0, y0, x1, y1, sway, turns, thickness):
    """The nightshade's stem climbing from (x0, y0) to (x1, y1), swaying side to side."""
    points = []
    for t in np.linspace(0, 1, 160):
        px = x0 + (x1 - x0) * t + sway * math.sin(t * turns * 2 * math.pi)
        points.append((px, y0 + (y1 - y0) * t))
    stem(image, points, thickness)
    return points


def icon(n=512):
    image = canvas(n, n)
    glow(image, n * 0.5, n * 0.48, n * 0.55, DEEP, 0.85)
    glow(image, n * 0.5, n * 0.38, n * 0.22, RED, 0.12)
    # an RPG's menu window in the lower half, the cursor at its first entry
    wx, wy, ww, wh = n * 0.12, n * 0.50, n * 0.76, n * 0.36
    window(image, wx, wy, ww, wh)
    cursor(image, wx + n * 0.07, wy + n * 0.075, n * 0.06, PALE)
    lines(image, wx + n * 0.17, wy + n * 0.092, n * 0.48, 1, n * 0.085, n * 0.028, TEXT, (0.82,))
    lines(image, wx + n * 0.17, wy + n * 0.177, n * 0.48, 2, n * 0.085, n * 0.028, SOFT, (0.62, 0.45))
    # the nightshade blooming over the window, berries hanging at its corner
    stem(image, curve((n * 0.50, n * 0.50), (n * 0.49, n * 0.43), (n * 0.50, n * 0.36)), n * 0.010)
    leaf(image, n * 0.30, n * 0.43, n * 0.15, n * 0.15 * 0.3, -2.75, bend=-0.12, rim=0.5)
    leaf(image, n * 0.70, n * 0.42, n * 0.14, n * 0.14 * 0.3, -0.35, bend=0.12, rim=0.5)
    flower(image, n * 0.50, n * 0.30, n * 0.15, -math.pi / 2)
    stem(image, curve((n * 0.80, n * 0.84), (n * 0.84, n * 0.88), (n * 0.83, n * 0.92)), n * 0.008)
    berries(image, n * 0.83, n * 0.91, n * 0.035)
    vignette(image, 0.55)
    return image


def background(w=3840, h=2160):
    image = canvas(w, h)
    cx, cy = w * 0.72, h * 0.47
    glow(image, cx, cy, w * 0.36, DEEP, 0.85)
    glow(image, cx, cy, w * 0.13, RED, 0.08)
    glow(image, w * 0.05, h * 1.05, w * 0.3, DEEP, 0.45)
    stars(image, 900, 1009, avoid=(cx, cy, h * 0.42))
    for x, y, length, angle, bend in ((w * 1.02, h * 0.08, w * 0.24, 2.55, 0.10), (w * 1.03, h * 0.62, w * 0.2, 3.35, -0.12),
                                      (w * 0.94, h * 1.06, w * 0.22, 3.9, 0.08), (w * 0.6, h * 1.08, w * 0.17, 4.5, -0.1),
                                      (-w * 0.02, h * 1.04, w * 0.2, 5.6, 0.1)):
        leaf(image, x, y, length, length * 0.24, angle, bend=bend)
    # The window of an RPG's title screen at the right, the name in it and its menu under the name.
    wx, wy, ww, wh = w * 0.53, h * 0.33, w * 0.38, h * 0.36
    glow(image, wx + ww / 2, wy + wh / 2, None, RED, 0.25, ring=wh * 0.62, width=h * 0.03)
    window(image, wx, wy, ww, wh)
    text(image, wx + ww / 2, wy + wh * 0.30, 'Nightshade', h * 0.105, TEXT, anchor='mm')
    text(image, wx + ww / 2, wy + wh * 0.53, 'PLAYER', h * 0.040, RED, spacing=0.55, anchor='mm')
    cursor(image, wx + ww * 0.33, wy + wh * 0.70, h * 0.030, PALE)
    lines(image, wx + ww * 0.40, wy + wh * 0.712, ww * 0.30, 2, wh * 0.12, h * 0.010, DIM, (0.9, 0.62))
    # The nightshade climbs from below at the window's left and flowers over its corner.
    points = vine(image, w * 0.50, h * 1.02, w * 0.515, h * 0.36, w * 0.010, 2.6, w * 0.0032)
    for t, angle, side in ((0.22, 3.6, -1), (0.46, 5.9, 1), (0.70, 3.3, -1)):
        px, py = points[int(t * (len(points) - 1))]
        leaf(image, px, py, w * 0.05, w * 0.05 * 0.32, angle, bend=0.15 * side, rim=0.45)
    stem(image, curve((w * 0.515, h * 0.36), (w * 0.52, h * 0.31), (w * 0.535, h * 0.29)), w * 0.003)
    flower(image, w * 0.54, h * 0.28, w * 0.06, -math.pi / 2 - 0.25)
    stem(image, curve((w * 0.905, h * 0.69), (w * 0.92, h * 0.72), (w * 0.918, h * 0.76)), w * 0.0025)
    berries(image, w * 0.918, h * 0.76, w * 0.016)
    flower(image, w * 0.88, h * 0.18, w * 0.03, -math.pi / 2 + 0.4)
    vignette(image, 0.55)
    return image


def launch(w=3840, h=2160):
    image = canvas(w, h)
    cx, cy = w * 0.5, h * 0.5
    glow(image, cx, cy, h * 0.6, DEEP, 0.75)
    stars(image, 1100, 1010, avoid=(cx, cy, h * 0.5))
    for x, y, length, angle, bend in ((-w * 0.02, h * 0.02, w * 0.26, 0.55, 0.08), (-w * 0.03, h * 0.7, w * 0.23, -0.2, -0.1),
                                      (w * 0.1, h * 1.08, w * 0.2, -1.0, 0.1), (w * 1.02, h * 0.04, w * 0.26, 2.6, -0.08),
                                      (w * 1.03, h * 0.72, w * 0.23, 3.35, 0.1), (w * 0.9, h * 1.08, w * 0.2, 4.15, -0.1)):
        leaf(image, x, y, length, length * 0.24, angle, bend=bend)
    # The flower over a window holding the name, in the middle of the dark.
    wx, wy, ww, wh = w * 0.29, h * 0.47, w * 0.42, h * 0.27
    glow(image, cx, wy + wh / 2, None, RED, 0.30, ring=wh * 0.75, width=h * 0.035)
    window(image, wx, wy, ww, wh)
    text(image, cx, wy + wh * 0.40, 'Nightshade', h * 0.10, TEXT, anchor='mm')
    text(image, cx, wy + wh * 0.74, 'PLAYER', h * 0.040, RED, spacing=0.55, anchor='mm')
    stem(image, curve((cx, wy), (cx - w * 0.01, cy - h * 0.12), (cx, cy - h * 0.20)), w * 0.004)
    flower(image, cx, cy - h * 0.22, h * 0.15, -math.pi / 2)
    leaf(image, cx - w * 0.07, wy - h * 0.02, w * 0.08, w * 0.08 * 0.3, -2.8, bend=-0.12, rim=0.5)
    leaf(image, cx + w * 0.07, wy - h * 0.03, w * 0.07, w * 0.07 * 0.3, -0.3, bend=0.12, rim=0.5)
    berries(image, wx + ww - w * 0.01, wy + wh + h * 0.02, h * 0.03)
    vignette(image, 0.6)
    return image


def to_image(image, mode='RGB'):
    # A little noise keeps the dark glows from showing steps (the same noise every time).
    noise = np.random.default_rng(7).uniform(-0.75, 0.75, image.shape[:2])[..., None]
    data = np.clip(np.clip(image, 0, 1) * 255 + 0.5 + noise, 0, 255).astype(np.uint8)
    picture = Image.fromarray(data)
    return picture.convert(mode)


def bc7(picture):
    """BC7 blocks of an RGBA picture, every block in mode 6 (one pair of endpoints, 16 steps)."""
    pixels = np.asarray(picture.convert('RGBA'), np.float32)
    h, w = pixels.shape[:2]
    blocks = pixels.reshape(h // 4, 4, w // 4, 4, 4).transpose(0, 2, 1, 3, 4).reshape(-1, 16, 4)
    weights = np.array([0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64], np.int32)
    out = np.empty((blocks.shape[0], 16), np.uint8)
    for start in range(0, blocks.shape[0], 32768):
        b = blocks[start:start + 32768]
        mean = b.mean(1, keepdims=True)
        centred = b - mean
        cov = np.einsum('npi,npj->nij', centred, centred)
        axis = np.ones((b.shape[0], 4), np.float32)
        for _ in range(8):
            axis = np.einsum('nij,nj->ni', cov, axis)
            axis /= np.maximum(np.linalg.norm(axis, axis=1, keepdims=True), 1e-9)
        proj = np.einsum('npi,ni->np', centred, axis)
        e0 = np.clip(mean[:, 0] + proj.min(1, keepdims=True) * axis, 0, 255)
        e1 = np.clip(mean[:, 0] + proj.max(1, keepdims=True) * axis, 0, 255)
        # 7 bits and a p-bit of 1 per endpoint: an endpoint is 2q + 1, so opaque stays 255.
        q0 = np.clip(np.round((e0 - 1) / 2), 0, 127).astype(np.int32)
        q1 = np.clip(np.round((e1 - 1) / 2), 0, 127).astype(np.int32)
        v0, v1 = q0 * 2 + 1, q1 * 2 + 1
        palette = ((64 - weights)[None, :, None] * v0[:, None, :] + weights[None, :, None] * v1[:, None, :] + 32) >> 6
        dist = ((b[:, :, None, :] - palette[:, None, :, :].astype(np.float32)) ** 2).sum(-1)
        index = dist.argmin(-1).astype(np.int32)
        # The first pixel's index has 3 bits: when it would need 4, the endpoints change places.
        flip = index[:, 0] >= 8
        q0[flip], q1[flip] = q1[flip].copy(), q0[flip].copy()
        index[flip] = 15 - index[flip]
        lo = np.zeros(b.shape[0], np.uint64)
        hi = np.zeros(b.shape[0], np.uint64)
        position = 0

        def field(value, bits):
            nonlocal position
            value = value.astype(np.uint64) & np.uint64((1 << bits) - 1)
            if position >= 64:
                hi[:] |= value << np.uint64(position - 64)
            elif position + bits <= 64:
                lo[:] |= value << np.uint64(position)
            else:
                low = 64 - position
                lo[:] |= (value & np.uint64((1 << low) - 1)) << np.uint64(position)
                hi[:] |= value >> np.uint64(low)
            position += bits

        field(np.full(b.shape[0], 1 << 6), 7)
        for channel in range(4):
            field(q0[:, channel], 7)
            field(q1[:, channel], 7)
        field(np.ones(b.shape[0]), 1)
        field(np.ones(b.shape[0]), 1)
        field(index[:, 0], 3)
        for pixel in range(1, 16):
            field(index[:, pixel], 4)
        assert position == 128
        out[start:start + 32768, :8] = lo.view(np.uint8).reshape(-1, 8)
        out[start:start + 32768, 8:] = hi.view(np.uint8).reshape(-1, 8)
    return out.tobytes()


def dds(picture, alpha_mode):
    """A DDS file with a DX10 header: BC7_UNORM, one level, as SwanStationPS5's pictures were."""
    w, h = picture.size
    flags = 0x1 | 0x2 | 0x4 | 0x1000 | 0x20000 | 0x80000  # caps, size, format, levels, linear size
    header = struct.pack('<4s7I44x', b'DDS ', 124, flags, h, w, w * h, 1, 1)
    header += struct.pack('<2I4s5I', 32, 0x4, b'DX10', 0, 0, 0, 0, 0)  # pixel format: FourCC
    header += struct.pack('<5I', 0x1000, 0, 0, 0, 0)  # caps: texture
    header += struct.pack('<5I', 98, 3, 0, 1, alpha_mode)  # BC7_UNORM, 2D, one, alpha mode
    assert len(header) == 148
    return header + bc7(picture)


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '--preview':
        # Small copies to look at, in the folder given (nothing in the repository changes).
        out = Path(sys.argv[2])
        out.mkdir(parents=True, exist_ok=True)
        to_image(icon()).save(out / 'icon0.png')
        to_image(background(960, 540)).save(out / 'background.png')
        to_image(launch(960, 540)).save(out / 'launch.png')
        return
    pictures = {'icon0.png': to_image(icon())}
    for source, name, draw in (('background-source.png', 'pic0.dds', background),
                               ('launch-background-source.png', 'pic1.dds', launch)):
        picture = to_image(draw(), 'RGB')
        pictures[source] = picture
        # Opaque, as SwanStationPS5's pictures were (DX10 alpha mode 3).
        pictures[name] = dds(picture, 3)
        decoded = np.asarray(Image.open(io.BytesIO(pictures[name])).convert('RGB'), np.float32)
        error = ((decoded - np.asarray(picture, np.float32)) ** 2).mean()
        psnr = 10 * math.log10(255 * 255 / max(error, 1e-9))
        print(f'{name}: {psnr:.1f} dB against {source}')
        if psnr < 38:
            sys.exit(f'{name}: the BC7 blocks are too far from the picture')
    for name, picture in pictures.items():
        if isinstance(picture, bytes):
            (target / name).write_bytes(picture)
        else:
            picture.save(target / name, optimize=True)
    print(target.relative_to(root), ', '.join(pictures))


if __name__ == '__main__':
    main()
