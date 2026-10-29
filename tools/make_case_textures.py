#!/usr/bin/env python3
"""Makes assets/psx_case_{front.png,back.jpg,inner.jpg,hinge.png} for the 3D jewel case (src/ui/case3d.c) from the
textures of "Hypnagogia Boundless Dreams Jewel Case" (sodaraptor, CC-BY 4.0), the glTF download of
https://sketchfab.com/3d-models/hypnagogia-boundless-dreams-jewel-case-20e4780167b6441fb364060c79870378 unzipped next to
this repository (the generated assets are committed; it is only needed to make them again):

  python3 tools/make_case_textures.py [hypnagogia__boundless_dreams_jewel_case]

Its textures are the UV atlases of assets/psx_case_NTSC.obj (the same case), flipped upside down by the glTF's
convention and at 1024x512; they are put back as 1698x1000 pictures first.

 - psx_case_front.png: the lid. Its printed area (the art and the black banner) is transparent: the downloaded cover
   shows through it, from the plane behind (ntsc_case_promo), which is what gives the case its depth.
 - psx_case_back.jpg: the back of the case, and the case's edges: the sides' black cards without the model's texts or the
   PAL box, with the app's own ("SwanStationPS5" in the PlayStation font, "By AngelXex" labels) and SLUS-00000 kept.
 - psx_case_hinge.png: the lid's front over the ribbed strip (the hinge) at the left of the case, from the tray's atlas.
 - psx_case_inner.jpg: the tray, as it is.
 - psx_case_lidback.png: the lid's paper seen from behind when the case is open: black, with the app's name in the middle.
 - psx_disc.png: the CD's top, with its printed art cut out (transparent) so the game's cover shows through it.
SPDX-License-Identifier: GPL-3.0-or-later
"""
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFont

ROOT = Path(__file__).resolve().parent.parent
SOURCE = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "hypnagogia__boundless_dreams_jewel_case"
TEXTURES = {"front": "01", "back": "02", "inner": "07", "disc": "08"}  # the atlas of each part, in the glTF's textures folder
FONT = ROOT / "playstation_by_lyricwest_dby5il4.ttf"  # the PlayStation font: the spine card's text and the labels
OUT = ROOT / "assets"
W, H = 1698, 1000  # the atlases
OUT_W, OUT_H = 1280, 754
EDGES = (1448, 0, 1698, 1000)  # the strips right of the art: the case's edges
TOP_BOTTOM = (108, 0, 192, 1000)  # the strip the back mesh's top and bottom edges use (the lid's atlas)
HINGE = (190, 0, 293, 1000)  # the ribbed strip of the tray's atlas, at the left
DISC = {"size": 1024, "centre": (513.5, 512.6), "art": (135.0, 446.0)}  # the CD's atlas: its centre and the art's rings (px)
COVER = (308, 21, 1430, 981)  # where the cover goes: the banner and the art (the downloaded cover has its own banner) (px)

SPINE_TEXT = "SwanStationPS5"
LABEL_TEXT = "By AngelXex"
CARDS = ((1456, 1522), (1544, 1682))  # the black cards of the two strips right of the art (px, inside their frames)
CARD_ROWS = ((20, 868), (170, 980))  # what is cleared of each card (SLUS-00000 stays below the first, the logo box above the PAL label)
NTSC_LABEL = (1480, 40, 1503, 150)  # the white NTSC and U/C labels, one now (px)
PAL_LABEL = (1566, 144, 1666, 165)  # the white PAL label (px)


def atlas(part):
    """The part's atlas as the OBJ's UV expect it: upright, at 1698x1000."""
    src = SOURCE / "textures" / f"{TEXTURES[part]}_-_Default_baseColor.png"
    img = Image.open(src).convert("RGB").transpose(Image.FLIP_TOP_BOTTOM)
    return img if part == "disc" else img.resize((W, H), Image.LANCZOS)


def label(img, box, text, vertical):
    """A white label (box = x0, y0, x1, y1) with `text` in black, like the model's NTSC / U/C and PAL ones; vertical ones
    read downwards."""
    ImageDraw.Draw(img).rectangle(box, fill=(240, 240, 240))
    x0, y0, x1, y1 = box
    across = (x1 - x0) if vertical else (y1 - y0)  # the letters' height runs across the label
    font = ImageFont.truetype(str(FONT), 100)
    bb = font.getbbox(text)
    glyphs = Image.new("L", (bb[2] - bb[0] + 4, bb[3] - bb[1] + 4), 0)
    ImageDraw.Draw(glyphs).text((2 - bb[0], 2 - bb[1]), text, font=font, fill=255)
    along = (y1 - y0) if vertical else (x1 - x0)  # the room along the text
    height = max(6, int(across * 0.62))
    width = int(glyphs.width * height / glyphs.height)
    if width > along - 8:  # a long text: smaller letters, to fit
        height = max(6, int(height * (along - 8) / width))
        width = int(glyphs.width * height / glyphs.height)
    glyphs = glyphs.resize((width, height), Image.LANCZOS)
    if vertical:
        glyphs = glyphs.rotate(-90, expand=True)
    layer = Image.new("L", img.size, 0)
    layer.paste(glyphs, ((x0 + x1 - glyphs.width) // 2, (y0 + y1 - glyphs.height) // 2))
    img.paste(Image.new("RGB", img.size, (12, 12, 12)), (0, 0), layer)


def edges_of(front):
    """The model's own edge strips (the lid atlas' right side) with their texts, logos and the PAL box taken out: inside
    each black card the picture is replaced by the card's own plain look (its horizontal profile, from the dark rows),
    so its frame and gloss stay. Then the cards get the app's texts: the spine card says SwanStationPS5, its NTSC / U/C
    labels one "By AngelXex", and the PAL label of the second one too. SLUS-00000 is left."""
    a = np.asarray(front).astype(float)
    for (x0, x1), (y0, y1) in zip(CARDS, CARD_ROWS):
        profile = np.percentile(a[200:860, x0:x1], 12, axis=0)  # the card's own dark, under the marks
        a[y0:y1, x0:x1] = profile[None, :, :]
    out = Image.fromarray(a.astype(np.uint8))
    # the spine card (the first strip): the app's name, reading downwards like the model's title
    x0, x1 = CARDS[0]
    font = ImageFont.truetype(str(FONT), 64)
    box = font.getbbox(SPINE_TEXT)
    text = Image.new("L", (box[2] - box[0] + 8, box[3] - box[1] + 8), 0)
    ImageDraw.Draw(text).text((4 - box[0], 4 - box[1]), SPINE_TEXT, font=font, fill=255)
    height = int((x1 - x0) * 0.58)  # the letters' height across the card
    text = text.resize((int(text.width * height / text.height), height), Image.LANCZOS).rotate(-90, expand=True)
    layer = Image.new("L", out.size, 0)
    layer.paste(text, ((x0 + x1 - text.width) // 2, 200))
    out.paste(Image.new("RGB", out.size, (236, 236, 240)), (0, 0), layer)
    label(out, NTSC_LABEL, LABEL_TEXT, vertical=True)
    label(out, PAL_LABEL, LABEL_TEXT, vertical=False)
    return out


def registered_mark(size):
    """The (R) of a registered trademark, which the PlayStation font does not have: a ring with an R, `size` px wide."""
    scale = 4  # drawn big and shrunk, for smooth lines
    big = size * scale
    mark = Image.new("L", (big, big), 0)
    d = ImageDraw.Draw(mark)
    ring = max(2, big // 9)
    d.ellipse((0, 0, big - 1, big - 1), outline=255, width=ring)
    font = ImageFont.truetype(str(FONT), int(big * 0.58))
    bb = font.getbbox("R")
    d.text(((big - (bb[2] - bb[0])) // 2 - bb[0], (big - (bb[3] - bb[1])) // 2 - bb[1]), "R", font=font, fill=255)
    return mark.resize((size, size), Image.LANCZOS)


def lid_back():
    """The back of the lid's paper, seen with the lid open: dark blue (#161a26), with "SwanStationPS5(R)" in the middle (PlayStation
    font), the mark small, on the baseline like in the PlayStation(R) logo, and "By AngelXex" at the bottom right."""
    w, h = 1024, 984  # the cover window's proportions
    img = Image.new("RGB", (w, h), (0x16, 0x1a, 0x26))  # #161a26, the dark blue of the app icon
    font = ImageFont.truetype(str(FONT), 100)
    bb = font.getbbox(SPINE_TEXT)
    text = Image.new("L", (bb[2] - bb[0] + 8, bb[3] - bb[1] + 8), 0)
    ImageDraw.Draw(text).text((4 - bb[0], 4 - bb[1]), SPINE_TEXT, font=font, fill=255)
    width = int(w * 0.68)
    text = text.resize((width, int(text.height * width / text.width)), Image.LANCZOS)
    mark = registered_mark(max(20, int(text.height * 0.44)))
    gap = max(4, mark.width // 4)
    total = text.width + gap + mark.width
    x0, y0 = (w - total) // 2, (h - text.height) // 2
    layer = Image.new("L", (w, h), 0)
    layer.paste(text, (x0, y0))
    layer.paste(mark, (x0 + text.width + gap, y0 + text.height - mark.height), mark)  # on the letters' baseline, as in the logo
    # "By AngelXex", small, at the bottom right
    sig_font = ImageFont.truetype(str(FONT), 100)
    sb = sig_font.getbbox(LABEL_TEXT)
    sig = Image.new("L", (sb[2] - sb[0] + 8, sb[3] - sb[1] + 8), 0)
    ImageDraw.Draw(sig).text((4 - sb[0], 4 - sb[1]), LABEL_TEXT, font=sig_font, fill=255)
    sig_w = int(w * 0.17)
    sig = sig.resize((sig_w, int(sig.height * sig_w / sig.width)), Image.LANCZOS)
    margin = int(w * 0.05)
    layer.paste(sig, (w - margin - sig.width, h - margin - sig.height), sig)
    img.paste(Image.new("RGB", (w, h), (236, 236, 240)), (0, 0), layer)
    img.save(OUT / "psx_case_lidback.png", optimize=True)


def disc_top():
    """The CD's top as its atlas has it (hub, clear ring, rim, gloss) but with the printed ring cut out: the cover shows
    through it from the plane behind (case3d.c)."""
    img = atlas("disc").convert("RGBA")
    size = DISC["size"]
    ys, xs = np.mgrid[0:size, 0:size]
    r = np.hypot(xs - DISC["centre"][0], ys - DISC["centre"][1])
    r0, r1 = DISC["art"]
    feather = 3.0
    art = np.clip(np.minimum(r - r0, r1 - r) / feather + 0.5, 0.0, 1.0)  # 1 inside the art ring, soft edges
    a = 1.0 - art  # so it is transparent there
    rgba = np.asarray(img).copy()
    rgba[..., 3] = (a * 255).astype(np.uint8)
    Image.fromarray(rgba, "RGBA").save(OUT / "psx_disc.png", optimize=True)


def main():
    lid_back()
    disc_top()
    lid, tray, back = atlas("front"), atlas("inner"), atlas("back")
    x0, y0, x1, y1 = COVER
    # the model's UV: u = 0.4834 + x * 0.011715 over the front plane, v = 0.5 + y / 56.48 (see psx_case_NTSC.obj)
    print("cover area: x %.2f .. %.2f, y %.2f .. %.2f (model units)" % (
        (x0 / W - 0.4834) / 0.011715, (x1 / W - 0.4834) / 0.011715, (0.5 - y1 / H) * 56.48, (0.5 - y0 / H) * 56.48))

    # the lid: solid, but for the cover's area
    rgba = np.asarray(lid.convert("RGBA")).copy()
    ys, xs = np.mgrid[0:H, 0:W]
    inside = (xs >= x0) & (xs < x1) & (ys >= y0) & (ys < y1)
    rgba[inside, :3] = 0
    rgba[inside, 3] = 0
    rgba[~inside, 3] = 255
    Image.fromarray(rgba, "RGBA").resize((OUT_W, OUT_H), Image.LANCZOS).save(OUT / "psx_case_front.png", optimize=True)

    # the lid's front over the ribbed strip at the left: the tray's ribs, clean (the lid atlas' copy has cracks)
    tray.crop(HINGE).save(OUT / "psx_case_hinge.png", optimize=True)

    # the back, and the case's edges: the model's back_01 mesh (sides, top, bottom) uses the lid's atlas
    back.paste(edges_of(lid).crop(EDGES), EDGES[:2])
    back.paste(lid.crop(TOP_BOTTOM), TOP_BOTTOM[:2])
    back.resize((OUT_W, OUT_H), Image.LANCZOS).save(OUT / "psx_case_back.jpg", quality=88, optimize=True)

    tray.resize((OUT_W, OUT_H), Image.LANCZOS).save(OUT / "psx_case_inner.jpg", quality=88, optimize=True)
    for name in ("psx_case_front.png", "psx_case_back.jpg", "psx_case_inner.jpg", "psx_case_hinge.png", "psx_case_lidback.png", "psx_disc.png"):
        print(name, (OUT / name).stat().st_size // 1024, "KB")


main()
