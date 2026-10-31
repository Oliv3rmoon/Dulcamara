#!/usr/bin/env python3
# Dulcamara - nightly, the community on Discord: Settings > About shows it, and its QR code opens its invitation.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds tests/community_check.c with src/community.h, src/ui/qr.h and third_party/qrcodegen under the address and
undefined-behaviour sanitizers, warnings as errors (CC names the compiler, default clang), and reads the QR code it
prints, the one Settings > About > Join the community shows, with a reader of its own written from ISO/IEC 18004: the
format information, the modules unmasked in their order, the blocks and their Reed-Solomon codewords checked, the
segments decoded. It must open https://discord.gg/pSqgRU3YP9 and nothing else. The same reader also reads
tests/qr-segno.png, a QR code segno made (Mandrake's assets/qr-github.png, from its tools/make_qr.py), so it is
checked against a second encoder.

The About row and its card, the empty library's one-time line and its flag in the settings file are checked in the
sources, and the new texts: through tr(), in Latin-1, every character in the interface fonts. Dulcamara is in English
only (src/i18n.c), so there is no other language to check yet."""

import os
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
         "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-UNDEBUG"]

INVITE = "https://discord.gg/pSqgRU3YP9"
SERVER = "nightly"

# The texts the invitation added, as the sources give them to tr().
TEXTS = ["Join the community",
         "nightly, the Discord of Dulcamara and the other Nightshade apps: help, bug reports, new versions. "
         "Cross shows its QR code for your phone.",
         "Scan to join %s on Discord", "Join %s, Dulcamara's community on Discord: %s",
         "Its QR code is in Settings > About."]

# ---------------------------------------------------------------- a QR code reader (ISO/IEC 18004)

# Error correction codewords per block and number of blocks, by level and version 1..40 (table 9).
ECC_PER_BLOCK = {
    "L": (7, 10, 15, 20, 26, 18, 20, 24, 30, 18, 20, 24, 26, 30, 22, 24, 28, 30, 28, 28, 28, 28, 30, 30, 26, 28, 30,
          30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
    "M": (10, 16, 26, 18, 24, 16, 18, 22, 22, 26, 30, 22, 22, 24, 24, 28, 28, 26, 26, 26, 26, 28, 28, 28, 28, 28, 28,
          28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28),
    "Q": (13, 22, 18, 26, 18, 24, 18, 22, 20, 24, 28, 26, 24, 20, 30, 24, 28, 28, 26, 30, 28, 30, 30, 30, 30, 28, 30,
          30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
    "H": (17, 28, 22, 16, 22, 28, 26, 26, 24, 28, 24, 28, 22, 24, 24, 30, 28, 28, 26, 28, 30, 24, 30, 30, 30, 30, 30,
          30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30),
}
BLOCKS = {
    "L": (1, 1, 1, 1, 1, 2, 2, 2, 2, 4, 4, 4, 4, 4, 6, 6, 6, 6, 7, 8, 8, 9, 9, 10, 12, 12, 12, 13, 14, 15, 16, 17, 18,
          19, 19, 20, 21, 22, 24, 25),
    "M": (1, 1, 1, 2, 2, 4, 4, 4, 5, 5, 5, 8, 9, 9, 10, 10, 11, 13, 14, 16, 17, 17, 18, 20, 21, 23, 25, 26, 28, 29, 31,
          33, 35, 37, 38, 40, 43, 45, 47, 49),
    "Q": (1, 1, 2, 2, 4, 4, 6, 6, 8, 8, 8, 10, 12, 16, 12, 17, 16, 18, 21, 20, 23, 23, 25, 27, 29, 34, 34, 35, 38, 40,
          43, 45, 48, 51, 53, 56, 59, 62, 65, 68),
    "H": (1, 1, 2, 4, 4, 4, 5, 6, 8, 8, 11, 11, 16, 16, 18, 16, 19, 21, 25, 25, 25, 34, 30, 32, 35, 37, 40, 42, 45, 48,
          51, 54, 57, 60, 63, 66, 70, 74, 77, 81),
}
ALPHANUMERIC = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:"
MASKS = (lambda x, y: (x + y) % 2 == 0, lambda x, y: y % 2 == 0, lambda x, y: x % 3 == 0,
         lambda x, y: (x + y) % 3 == 0, lambda x, y: (x // 3 + y // 2) % 2 == 0,
         lambda x, y: x * y % 2 + x * y % 3 == 0, lambda x, y: (x * y % 2 + x * y % 3) % 2 == 0,
         lambda x, y: ((x + y) % 2 + x * y % 3) % 2 == 0)


def gf_mul(a, b):
    """Product in GF(256) modulo x^8 + x^4 + x^3 + x^2 + 1."""
    out = 0
    while b:
        if b & 1:
            out ^= a
        a = (a << 1) ^ (0x11D if a & 0x80 else 0)
        b >>= 1
    return out


def reed_solomon(data, degree):
    """The error correction codewords of data: its remainder by the generator with the roots 1, 2, 4, ..."""
    generator, root = [1], 1
    for _ in range(degree):
        generator = [high ^ gf_mul(low, root) for high, low in zip(generator + [0], [0] + generator)]
        root = gf_mul(root, 2)
    rest = list(data) + [0] * degree
    for i in range(len(data)):
        if rest[i]:
            factor = rest[i]
            for j, g in enumerate(generator):
                rest[i + j] ^= gf_mul(g, factor)
    return rest[len(data):]


def read_qr(rows):
    """The text of a QR code: rows of modules (True for dark), the symbol only. Fails on any part that isn't right."""
    size = len(rows)
    version = (size - 17) // 4
    assert 1 <= version <= 40 and size == 17 + 4 * version and all(len(r) == size for r in rows), "not a QR symbol"

    def bits(cells):
        return sum(int(rows[y][x]) << i for i, (x, y) in enumerate(cells))

    # the format information, in both of its places: level and mask, BCH-coded, XOR 101010000010010
    first = [(8, i) for i in range(6)] + [(8, 7), (8, 8), (7, 8)] + [(14 - i, 8) for i in range(9, 15)]
    second = [(size - 1 - i, 8) for i in range(8)] + [(8, size - 15 + i) for i in range(8, 15)]
    formats = {}
    for level_bits, level in ((1, "L"), (0, "M"), (3, "Q"), (2, "H")):
        for mask in range(8):
            data = level_bits << 3 | mask
            rest = data << 10
            for shift in range(14, 9, -1):
                if rest >> shift & 1:
                    rest ^= 0x537 << (shift - 10)
            formats[(data << 10 | rest) ^ 0x5412] = (level, mask)
    assert bits(first) in formats, "unreadable format information"
    assert bits(first) == bits(second), "the two copies of the format information differ"
    level, mask = formats[bits(first)]

    # the function patterns, which hold no data
    function = [[False] * size for _ in range(size)]

    def fill(left, top, width, height):
        for y in range(top, top + height):
            for x in range(left, left + width):
                function[y][x] = True

    fill(0, 0, 9, 9)                    # finder, separator and format information, top left
    fill(size - 8, 0, 8, 9)             # top right
    fill(0, size - 8, 9, 8)             # bottom left, with the dark module
    fill(6, 0, 1, size)                 # timing
    fill(0, 6, size, 1)
    if version > 1:                     # alignment patterns: not on the three finders
        count = version // 7 + 2
        step = (version * 8 + count * 3 + 5) // (count * 4 - 4) * 2
        centres = [6] + sorted(size - 7 - i * step for i in range(count - 1))
        for i, cx in enumerate(centres):
            for j, cy in enumerate(centres):
                if (i, j) not in ((0, 0), (0, count - 1), (count - 1, 0)):
                    fill(cx - 2, cy - 2, 5, 5)
    if version >= 7:                    # version information
        fill(size - 11, 0, 3, 6)
        fill(0, size - 11, 6, 3)

    # the data modules, two columns at a time from the bottom right, up then down, unmasked
    stream, upward, right = [], True, size - 1
    while right >= 1:
        if right == 6:
            right = 5
        for step in range(size):
            y = size - 1 - step if upward else step
            for x in (right, right - 1):
                if not function[y][x]:
                    stream.append(rows[y][x] != MASKS[mask](x, y))
        upward, right = not upward, right - 2
    codewords = [int("".join("1" if b else "0" for b in stream[i:i + 8]), 2) for i in range(0, len(stream) - 7, 8)]

    # the blocks, interleaved: their data codewords, then their error correction, which must be right
    ecc, count = ECC_PER_BLOCK[level][version - 1], BLOCKS[level][version - 1]
    short = count - len(codewords) % count
    lengths = [len(codewords) // count - ecc + (b >= short) for b in range(count)]
    blocks, at = [[] for _ in range(count)], 0
    for i in range(max(lengths)):
        for b in range(count):
            if i < lengths[b]:
                blocks[b].append(codewords[at])
                at += 1
    for i in range(ecc):
        for b in range(count):
            blocks[b].append(codewords[at])
            at += 1
    assert at == len(codewords)
    for block in blocks:
        assert reed_solomon(block[:-ecc], ecc) == block[-ecc:], "a block's error correction is wrong"

    # the segments: numeric, alphanumeric, bytes, up to the terminator
    data = "".join(format(c, "08b") for block in blocks for c in block[:-ecc])
    pos, text = 0, bytearray()

    def take(n):
        nonlocal pos
        assert pos + n <= len(data), "a segment runs past the data"
        pos += n
        return int(data[pos - n:pos], 2)

    group = 0 if version <= 9 else 1 if version <= 26 else 2
    while len(data) - pos >= 4:
        mode = take(4)
        if mode == 0:
            break
        if mode == 1:
            left = take((10, 12, 14)[group])
            while left > 0:
                digits = min(left, 3)
                text += str(take((4, 7, 10)[digits - 1])).zfill(digits).encode()
                left -= digits
        elif mode == 2:
            left = take((9, 11, 13)[group])
            while left >= 2:
                pair = take(11)
                text += (ALPHANUMERIC[pair // 45] + ALPHANUMERIC[pair % 45]).encode()
                left -= 2
            if left:
                text += ALPHANUMERIC[take(6)].encode()
        elif mode == 4:
            text += bytes(take(8) for _ in range(take((8, 16, 16)[group])))
        else:
            raise AssertionError(f"segment mode {mode:04b}")
    return text.decode("utf-8"), version, level


def read_png(path):
    """An 8-bit grey, RGB or RGBA PNG (not interlaced) as rows of booleans, True for dark."""
    raw = Path(path).read_bytes()
    assert raw[:8] == b"\x89PNG\r\n\x1a\n"
    at, idat = 8, b""
    while at < len(raw):
        length, kind = struct.unpack(">I4s", raw[at:at + 8])
        body = raw[at + 8:at + 8 + length]
        if kind == b"IHDR":
            width, height, depth, colour, _, _, interlace = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat += body
        at += 12 + length
    channels = {0: 1, 2: 3, 4: 2, 6: 4}[colour]
    assert depth == 8 and interlace == 0
    pixels, stride, rows, prev = zlib.decompress(idat), width * channels, [], bytearray(width * channels)
    for y in range(height):
        kind, line = pixels[y * (stride + 1)], bytearray(pixels[y * (stride + 1) + 1:(y + 1) * (stride + 1)])
        for i in range(stride):
            a, b = line[i - channels] if i >= channels else 0, prev[i]
            c = prev[i - channels] if i >= channels else 0
            p = a + b - c
            paeth = a if abs(p - a) <= abs(p - b) and abs(p - a) <= abs(p - c) else b if abs(p - b) <= abs(p - c) else c
            line[i] = (line[i] + (0, a, b, (a + b) // 2, paeth)[kind]) & 0xFF
        rows.append([line[x * channels] < 128 for x in range(width)])
        prev = line
    return rows


def without_quiet_zone(rows):
    """The symbol alone: the finder patterns put dark modules on its first and last rows and columns."""
    ys = [y for y, row in enumerate(rows) if any(row)]
    xs = [x for x in range(len(rows[0])) if any(row[x] for row in rows)]
    return [row[xs[0]:xs[-1] + 1] for row in rows[ys[0]:ys[-1] + 1]]


def font_codepoints(path):
    """The characters a TrueType font maps (its cmap's format 4 and 12 subtables)."""
    font = Path(path).read_bytes()
    tables = {font[12 + 16 * i:16 + 16 * i]: struct.unpack(">I", font[20 + 16 * i:24 + 16 * i])[0]
              for i in range(struct.unpack(">H", font[4:6])[0])}
    cmap, found = tables[b"cmap"], set()
    for i in range(struct.unpack(">H", font[cmap + 2:cmap + 4])[0]):
        sub = cmap + struct.unpack(">I", font[cmap + 8 + 8 * i:cmap + 12 + 8 * i])[0]
        kind = struct.unpack(">H", font[sub:sub + 2])[0]
        if kind == 4:
            segs = struct.unpack(">H", font[sub + 6:sub + 8])[0] // 2
            ends = struct.unpack(f">{segs}H", font[sub + 14:sub + 14 + 2 * segs])
            starts = struct.unpack(f">{segs}H", font[sub + 16 + 2 * segs:sub + 16 + 4 * segs])
            deltas = struct.unpack(f">{segs}h", font[sub + 16 + 4 * segs:sub + 16 + 6 * segs])
            offsets_at = sub + 16 + 6 * segs
            offsets = struct.unpack(f">{segs}H", font[offsets_at:offsets_at + 2 * segs])
            for k in range(segs):
                for c in range(starts[k], min(ends[k], 0xFFFE) + 1):
                    glyph = (c + deltas[k]) & 0xFFFF
                    if offsets[k]:
                        at = offsets_at + 2 * k + offsets[k] + 2 * (c - starts[k])
                        glyph = struct.unpack(">H", font[at:at + 2])[0]
                        glyph = (glyph + deltas[k]) & 0xFFFF if glyph else 0
                    if glyph:
                        found.add(c)
        elif kind == 12:
            for k in range(struct.unpack(">I", font[sub + 12:sub + 16])[0]):
                first, last, _ = struct.unpack(">III", font[sub + 16 + 12 * k:sub + 28 + 12 * k])
                found.update(range(first, last + 1))
    return found


def c_strings(row):
    return [re.sub(r"\\(.)", r"\1", s) for s in re.findall(r'"((?:[^"\\]|\\.)*)"', row)]


class CommunityTest(unittest.TestCase):
    def test_the_qr_code_opens_the_invitation(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "community-check"
            subprocess.run([cc, *FLAGS, "-I" + str(ROOT / "src"), str(ROOT / "tests/community_check.c"),
                            str(ROOT / "third_party/qrcodegen/qrcodegen.c"), "-o", str(executable)], check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=60, check=False)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        lines = result.stdout.splitlines()
        self.assertEqual(lines[:3], [f"name {SERVER}", "link discord.gg/pSqgRU3YP9", f"url {INVITE}"])
        size = int(lines[3].split()[1])
        symbol = [[c == "1" for c in line] for line in lines[4:4 + size]]
        text, version, level = read_qr(symbol)
        print(f"Settings > About > Join the community: version {version}-{level} QR code, opens {text}")
        self.assertEqual(text, "https://discord.gg/pSqgRU3YP9")

    def test_the_reader_reads_segnos_qr_code_too(self):
        # tests/qr-segno.png is Mandrake's assets/qr-github.png, made by segno (its tools/make_qr.py), not qrcodegen
        text, version, level = read_qr(without_quiet_zone(read_png(ROOT / "tests/qr-segno.png")))
        self.assertEqual(text, "https://github.com/Oliv3rmoon/Mandrake")
        self.assertEqual((version, level), (3, "M"))

    def test_about_shows_nightly_with_its_link_and_qr_code(self):
        header = (ROOT / "src/community.h").read_text(encoding="utf-8")
        self.assertRegex(header, r'#define COMMUNITY_NAME "nightly"\s')
        self.assertRegex(header, r'#define COMMUNITY_URL "https://discord\.gg/pSqgRU3YP9"\s')
        self.assertRegex(header, r'#define COMMUNITY_LINK "discord\.gg/pSqgRU3YP9"\s')
        screen = (ROOT / "src/ui/settings.c").read_text(encoding="utf-8")
        # About's first row, its value the name and the short link, and Cross its card
        about = re.search(r"static const Row ABOUT\[\] = \{(.*?)\n\};", screen, re.S).group(1)
        self.assertRegex(about, r'^\s*\{"Join the community", "[^"]+"\s*"[^"]+",\s*K_ACTION, 0, NULL, 0, NULL, '
                                r'A_COMMUNITY, -1\},')
        self.assertRegex(screen, r'snprintf\(community, sizeof\(community\), "%s  \\xC2\\xB7  %s", COMMUNITY_NAME, '
                                 r'COMMUNITY_LINK\);')
        self.assertIn(": it->action == A_COMMUNITY  ? community", screen)
        self.assertIn("case A_COMMUNITY: community_open = true; break;", screen)
        card = re.search(r"static void draw_community\(void\).*?\n\}", screen, re.S).group(0)
        self.assertIn("draw_url_qr(COMMUNITY_URL, ", card)
        self.assertIn('tr("Scan to join %s on Discord"), COMMUNITY_NAME', card)
        self.assertIn("ALIGN_CENTER, COMMUNITY_LINK);", card)
        self.assertRegex(screen, r"if \(community_open\)\n\s+draw_community\(\);\n\}")
        # all of About on one page: nothing on it can be scrolled to
        self.assertIn("const int visible = t->rows == ABOUT ? t->count : 8;", screen)
        # the QR code is made by qr_encode, as community_check.c makes it, and compiled in
        draw = re.search(r"static void draw_url_qr\(.*?\n\}", screen, re.S).group(0)
        self.assertIn("qr_encode(text, qr, temp)", draw)
        self.assertIn('#include "../third_party/qrcodegen/qrcodegen.c"', (ROOT / "src/vendor.c").read_text())
        for source in sorted((ROOT / "src").rglob("*.[ch]")):
            text = source.read_text(encoding="utf-8", errors="replace")
            if source.name != "qr.h":
                self.assertNotIn("qrcodegen_encode", text, source)
            # every invitation written anywhere in the app is this one
            for code in re.findall(r"discord\.gg/(\w+)", text):
                self.assertEqual(code, "pSqgRU3YP9", source)

    def test_the_empty_library_mentions_it_once(self):
        shelf = (ROOT / "src/ui/shelf.c").read_text(encoding="utf-8")
        once = re.search(r"static void invite_once\(void\).*?\n\}", shelf, re.S).group(0)
        self.assertIn("invite = !app.settings.community_seen;", once)
        self.assertRegex(once, r"if \(invite\)\n\s+\{\n\s+app\.settings\.community_seen = true;\n\s+"
                               r"app_save_settings\(\);")
        self.assertRegex(shelf, r"if \(n == 0\)\n\s+\{\n\s+invite_once\(\);\n\s+draw_empty\(\);")
        empty = re.search(r"static void draw_empty\(void\).*?\n\}", shelf, re.S).group(0)
        self.assertRegex(empty, r'tr\("Join %s, Dulcamara\'s community on Discord: %s"\), COMMUNITY_NAME,'
                                r'\s+COMMUNITY_LINK\)')
        self.assertIn('tr("Its QR code is in Settings > About.")', empty)
        # saved in the settings file and read back
        config = (ROOT / "src/config.c").read_text(encoding="utf-8")
        self.assertIn('fprintf(f, "community_seen = %d\\n", s->community_seen);', config)
        self.assertRegex(config, r'!strcmp\(key, "community_seen"\)\)\n\s+s->community_seen = v != 0;')

    def test_every_new_text_goes_through_tr_and_the_fonts_have_it(self):
        sources = "".join((ROOT / f).read_text(encoding="utf-8") for f in ("src/ui/settings.c", "src/ui/shelf.c"))
        # each C string as the compiler joins it: adjacent literals together
        literals = ["".join(c_strings(m)) for m in re.findall(r'((?:"(?:[^"\\]|\\.)*"\s*)+)', sources)]
        fonts = [font_codepoints(ROOT / "assets/fonts" / name) for name in ("Inter-400.ttf", "Inter-600.ttf")]
        for english in TEXTS:
            self.assertIn(english, literals, f"{english!r} is not in the sources as one string")
            self.assertTrue(all(ord(c) <= 0xFF for c in english), f"{english!r} is not Latin-1")
            for font in fonts:  # and the middle dot between the name and the link on the row
                missing = sorted({c for c in english + "\u00b7" if ord(c) >= 0x20 and ord(c) not in font})
                self.assertEqual(missing, [], f"not in the interface font: {''.join(missing)}")
        for call in ("Scan to join %s on Discord", "Join %s, Dulcamara's community on Discord: %s",
                     "Its QR code is in Settings > About."):
            self.assertIn(f'tr("{call}")', sources)
        # the row's label and help are drawn through tr() as every row's are
        self.assertIn("tr(it->label)", sources)
        self.assertIn("tr(r->help)", sources)


if __name__ == "__main__":
    unittest.main()
