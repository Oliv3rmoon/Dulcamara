#!/usr/bin/env python3
# Dulcamara - Ren'Py 8's core, built for this computer and run headless on the test game of tests/renpy-game.
# SPDX-FileCopyrightText: Copyright 2026 Oliv3rmoon
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the libretro core the PS5 app links (tools/build-renpy.sh host: the same Ren'Py, CPython, FFmpeg and
Assimp, and the same renamed SDL calls) with the headless frontend of tests/core/renpy_headless.c, and plays
tests/renpy-game: it must load, give a picture, change it when Cross is pressed, play its sound, take a menu choice
and save, and a second run must load that save; it must list its sounds, and play none that is switched off. Also runs
the core's own checks (tests/core/renpy_unit.c): the handoff and its clock, the pictures, the sound and the OpenGL
read-back."""

import contextlib
import math
import os
import pickle
import re
import shutil
import struct
import subprocess
import tempfile
import unittest
import wave
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
GAME = ROOT / "tests/renpy-game"


def write_png(path, width, height):
    """The test game's picture: a gradient, so that no two rows are alike."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(((x + y) & 255, x & 255, y & 255))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows)))
                     + chunk(b"IEND", b""))


def write_png_rgba(path, width, height):
    """A picture partly clear: its colours vary across it and its alpha down it, from clear (its colours kept, as
    a game's images often keep them) to opaque, so that premultiplying them wrongly shows."""
    rows = bytearray()
    for y in range(height):
        rows.append(0)
        for x in range(width):
            rows += bytes(((x * 7) & 255, (y * 11 + x) & 255, (200 - x) & 255, min(255, y * 255 // (height - 1))))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(rows)))
                     + chunk(b"IEND", b""))


def write_tone(path, seconds=1.0, rate=48000, hz=440):
    """The test game's sound: a second of a 440 Hz tone, which the game loops."""
    samples = b"".join(struct.pack("<h", int(12000 * math.sin(2 * math.pi * hz * i / rate)))
                       for i in range(int(seconds * rate)))
    with wave.open(str(path), "wb") as out:
        out.setnchannels(1)
        out.setsampwidth(2)
        out.setframerate(rate)
        out.writeframes(samples)


def write_rpa(path, files):
    """A Ren'Py archive (RPA-3.0) of `files` (name: bytes): its header, the files, then its index (a pickle, zlib'd),
    whose offsets and lengths are XORed with the header's key."""
    key, data, index = 0x42424242, b"", {}
    for name, blob in files.items():
        index[name] = [((34 + len(data)) ^ key, len(blob) ^ key, b"")]
        data += blob
    path.write_bytes(b"RPA-3.0 %016x %08x\n" % (34 + len(data), key) + data
                     + zlib.compress(pickle.dumps(index, protocol=2)))


def make_game(directory, width=1280, height=720, sound_rate=48000, broken=False, archived=False, renpy_version=None,
              script_version=None, stray_version=None):
    """A copy of the test game in `directory`, with its picture, its sound and this run's screen size; `archived`, its
    sounds in an archive (game/audio.rpa) instead of their own files. Its OGG sound (game/audio/tone.ogg, a second of
    440 and 660 Hz, oggenc -q 0) is kept in the repository: no Vorbis encoder is at hand. `renpy_version`: the Ren'Py
    the game says it was made with, in renpy/vc_version.py beside game/ as a released game has it; `script_version`, a
    (major, minor, patch) in game/script_version.txt, as Ren'Py's launcher adds to a build it makes; `stray_version`, a
    vc_version.py and __init__.py of that version loose beside game/ (not in a renpy/ folder, which the game lacks)."""
    shutil.copytree(GAME, directory)
    if stray_version:
        (directory / "vc_version.py").write_text(f"version = '{stray_version}'\n", encoding="utf-8")
        numbers = ", ".join(stray_version.split(".")[:3])
        (directory / "__init__.py").write_text(f"version_tuple = ({numbers}, 0)\n", encoding="utf-8")
    if renpy_version:
        (directory / "renpy").mkdir()
        (directory / "renpy/vc_version.py").write_text(f"branch = 'fix'\nversion = '{renpy_version}'\n"
                                                       "official = True\n", encoding="utf-8")
    game = directory / "game"
    if script_version:
        (game / "script_version.txt").write_text(repr(tuple(script_version)), encoding="utf-8")
    (game / "images").mkdir(exist_ok=True)
    (game / "audio").mkdir(exist_ok=True)
    write_png(game / "images/dulcamara.png", 256, 256)
    write_tone(game / "audio/tone.wav")
    if archived:
        sounds = sorted((game / "audio").iterdir())
        write_rpa(game / "audio.rpa", {f"audio/{sound.name}": sound.read_bytes() for sound in sounds})
        for sound in sounds:
            sound.unlink()
    (game / "size.rpy").write_text(f"define config.screen_width = {width}\n"
                                   f"define config.screen_height = {height}\n"
                                   f"define config.sound_sample_rate = {sound_rate}\n", encoding="utf-8")
    if broken:
        # a line that is no Ren'Py statement: the script cannot be parsed
        (game / "broken.rpy").write_text("this is not a statement\n", encoding="utf-8")
    return directory


def build_preloaded(directory, name):
    """tests/core/<name>.c as a library the headless frontend loads first, and the environment that loads it; None
    without a compiler that makes one"""
    cc = os.environ.get("CC", "cc")
    if not shutil.which(cc):
        return None
    darwin = os.uname().sysname == "Darwin"
    library = directory / (f"{name}.dylib" if darwin else f"{name}.so")
    flags = ["-dynamiclib"] if darwin else ["-shared", "-fPIC"]
    built = subprocess.run([cc, *flags, "-O2", "-o", str(library), str(ROOT / f"tests/core/{name}.c"),
                            *([] if darwin else ["-ldl", "-pthread"])], capture_output=True, text=True)
    if built.returncode != 0:
        return None
    return {"DYLD_INSERT_LIBRARIES" if darwin else "LD_PRELOAD": str(library)}


def build_thread_refuser(directory):
    """tests/core/refuse_threads.c as a library the headless frontend loads first, and the environment that loads it
    (DULCAMARA_REFUSE_THREADS then says which threads are refused); None without a compiler that makes one"""
    return build_preloaded(directory, "refuse_threads")


def write_ttc(path, fonts):
    """A font collection (.ttc) of `fonts` (each an OpenType/TrueType font's bytes): its header, then each font's
    table directory and tables, their offsets made the collection's."""
    start = 12 + 4 * len(fonts)
    body, offsets = bytearray(), []
    for data in fonts:
        count = struct.unpack(">H", data[4:6])[0]
        directory, tables = bytearray(data[:12 + 16 * count]), bytearray()
        offsets.append(start + len(body))
        first_table = start + len(body) + len(directory)
        for i in range(count):
            at = 12 + 16 * i
            _tag, _sum, offset, length = struct.unpack(">4sIII", data[at:at + 16])
            struct.pack_into(">I", directory, at + 8, first_table + len(tables))
            tables += data[offset:offset + length] + b"\0" * (-length % 4)
        body += directory + tables
    path.write_bytes(b"ttcf" + struct.pack(">HHI", 1, 0, len(fonts)) + b"".join(struct.pack(">I", o) for o in offsets)
                     + bytes(body))


# a system's font in CFF outlines (.otf) with Latin letters, where one is at hand
OTF_FONTS = ("/System/Library/Fonts/Supplemental/STIXGeneral.otf", "/usr/share/fonts/opentype/cantarell/Cantarell-Regular.otf",
             "/usr/share/fonts/OTF/Cantarell-Regular.otf")


# RENPY_EDIT_PY's editor: it notes the file it is asked to open, beside itself
PROBE_EDITOR = """
class Editor(object):
    def begin(self, new_window=False, **kwargs):
        pass

    def open(self, filename, line=None, **kwargs):
        with open(__file__ + ".opened", "w") as f:
            f.write(filename)

    def end(self, **kwargs):
        pass
"""


# the game's music, and a click played six times over it (RenpyCoreTest.clicks_over_music); with
# DULCAMARA_TEST_SLOW_READ=<ms>, each click's file (as Python's file, read through Python) waits that long at its first read
CLICKS_LABEL = """
init python:
    import os, time
    dulcamara_slow = {"reads": 0}

    class DulcamaraSlowFile(object):
        def __init__(self, f):
            self.f, self.first = f, True

        def read(self, n=-1):
            if self.first:
                self.first = False
                dulcamara_slow["reads"] += 1
                time.sleep(float(os.environ["DULCAMARA_TEST_SLOW_READ"]) / 1000.0)
            return self.f.read(n)

        def seek(self, *args):
            return self.f.seek(*args)

        def tell(self):
            return self.f.tell()

        def close(self):
            return self.f.close()

    if os.environ.get("DULCAMARA_TEST_SLOW_READ"):
        dulcamara_load = renpy.loader.load

        def dulcamara_slow_load(name, *args, **kwargs):
            f = dulcamara_load(name, *args, **kwargs)
            return DulcamaraSlowFile(f) if "click" in name else f

        renpy.loader.load = dulcamara_slow_load

label dulcamara_clicks:
    scene expression Solid("#203040")
    play music "audio/tone.wav"
    $ renpy.pause(0.5, hard=True)
    $ dulcamara_k = 0
    while dulcamara_k < 6:
        play audio "audio/click.wav"
        $ renpy.pause(0.25, hard=True)
        $ dulcamara_k += 1
    $ renpy.pause(0.5, hard=True)
    $ dulcamara_test("slow reads: %d" % dulcamara_slow["reads"])
    $ dulcamara_test("clicks done")
    $ renpy.pause(hard=True)
"""


class RenpyCoreTest(unittest.TestCase):
    # the build script and its host build (tests/test_renpy7_core.py runs the same tests on Ren'Py 7's)
    SCRIPT = "tools/build-renpy.sh"
    BUILD = ROOT / "build/renpy-host"

    @classmethod
    def setUpClass(cls):
        if not shutil.which(os.environ.get("CC", "cc")) or not shutil.which("cmake"):
            raise unittest.SkipTest("needs a C compiler and cmake")
        build = subprocess.run(["bash", str(ROOT / cls.SCRIPT), "host"], capture_output=True, text=True)
        if build.returncode != 0:
            raise AssertionError(f"{cls.SCRIPT} host failed:\n" + build.stdout[-4000:] + build.stderr[-4000:])
        print(build.stdout.strip().splitlines()[-1])

    def run_core(self, mode, game, data, *more, env=None, cwd=None):
        headless, assets = self.BUILD / "core-check/renpy-headless", self.BUILD / "assets"
        result = subprocess.run([str(headless), str(assets), mode, str(game), str(data), *map(str, more)],
                                capture_output=True, text=True, errors="replace", timeout=600, check=False,
                                env={**os.environ, **(env or {})}, cwd=cwd)
        print(f"\n{mode} {game.name}:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout[-6000:] + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)
        self.assertNotIn("did not stop", result.stdout, "the core stopped Ren'Py within its ten seconds")
        return result.stdout

    def play_and_check(self, game, data):
        output = self.run_core("play", game, data)
        for line in ("XRGB8888", "input: the picture changed after Cross", "sound: playing",
                     "DULCAMARA-TEST: choice=right", "DULCAMARA-TEST: saved", "ended by the game"):
            self.assertIn(line, output)
        return output

    def test_archived_compiled_package(self):
        # Some PC releases keep their complete Python package in an RPA as adjacent Python 3.12 .pyc files.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "CompiledModules")
            python = self.BUILD / "python/bin/python3.12"
            files = {}
            for name, source in (("saga/__init__", "from .story import chapter"),
                                 ("saga/story", "chapter = 42"), ("prefer_source", "value = 0")):
                src, pyc = work / "module.py", work / "module.pyc"
                src.write_text(source)
                subprocess.run([str(python), "-c",
                                "import py_compile,sys; py_compile.compile(*sys.argv[1:], doraise=True)",
                                str(src), str(pyc), name + ".py"], check=True)
                files[name + ".pyc"] = pyc.read_bytes()
            files["prefer_source.py"] = b"value = 1"
            files["wrong_version.pyc"] = b"BAD!" + files["saga/story.pyc"][4:]
            write_rpa(game / "game/lib.rpa", files)
            marker = work / "result"
            (game / "game/+config.rpy").write_text(
                "python early hide:\n"
                "    import saga, prefer_source\n"
                "    assert saga.chapter == 42 and prefer_source.value == 1\n"
                "    try:\n        import wrong_version\n"
                "    except ImportError as e:\n        assert 'bad magic' in str(e)\n"
                "    else:\n        raise AssertionError('accepted wrong Python version')\n"
                f"    with open({str(marker)!r}, 'w') as f:\n        f.write('ok')\n")
            self.run_core("start", game, work / "data")
            self.assertEqual(marker.read_text(), "ok")

    def test_the_core_checks(self):
        # under the sanitizers; leaks are not checked: the OpenGL driver's own allocations would be reported
        result = subprocess.run([str(self.BUILD / "core-check/renpy-unit")], capture_output=True, text=True,
                                timeout=120, check=False, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"})
        print("\ncore checks:")
        print(result.stdout.strip())
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr[-3000:])
        self.assertIn("PASS", result.stdout)

    def test_play_save_and_load(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.play_and_check(game, data)
            self.assertIn("picture: 1280x720", output)
            self.assertTrue(list((data / "saves/renpy/dulcamara-test-game").glob("1-1-*.save")),
                            "the save is in <data>/saves/renpy/<config.save_directory>")
            # Ren'Py saves at the last checkpoint (the menu) and replays the choice after it loads
            output = self.run_core("load", game, data, env={"DULCAMARA_TEST_LOAD": "1"})
            self.assertIn("DULCAMARA-TEST: loaded", output)
            self.assertIn("DULCAMARA-TEST: choice=right", output.split("DULCAMARA-TEST: loaded", 1)[1])

    def test_native_layout_and_first_write_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Unrelated title"), work / "data"
            configured = work / "configured native root"
            extra = work / "extra native root"
            readonly = work / "read only native root"
            readonly.mkdir(); readonly.chmod(0o555)
            (game / "game/native.rpy").write_text(
                "init -999 python:\n"
                "    import renpy as native_renpy\n"
                "    native_renpy.config.savedir = %r\n"
                "    native_renpy.config.extra_savedirs = [%r, %r]\n"
                "    mp = native_renpy.persistent.MultiPersistent('eclipse-shared-test', save_on_quit=True)\n"
                "    mp.value = 42\n" % (str(configured), str(extra), str(readonly)))
            first = data / "saves/renpy/dulcamara-test-game"
            output = self.run_core("play", game, data, env={"DULCAMARA_RUNTIME_PROBE": "1",
                "DULCAMARA_RUNTIME_ABSENT": str(first)})
            self.assertIn("runtime: initial checkpoint held native writes", output)
            self.assertIn("runtime: clean stop finished writers", output)
            self.assertIn(str(configured), output)
            self.assertIn(str(extra), output)
            self.assertIn("native-save: 4 1 1 " + str(readonly.resolve()), output)
            self.assertIn(str(game / "game/saves"), output)
            self.assertRegex(output, r"native-save: 2 2 [12] .*eclipse-shared-test")
            self.assertTrue(list(configured.glob("1-1-*.save")))
            self.assertTrue(list(extra.glob("1-1-*.save")))

    def test_early_multipersistent_worker_protection(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory).resolve()
            game, data = make_game(work / "Early Multi"), work / "data"
            shared = work / "shared"
            (game / "game/early_multi.rpy").write_text(
                "python early:\n"
                "    import renpy as native_renpy\n"
                "    for name in ['eclipse-first', 'eclipse-second']:\n"
                "        mp = native_renpy.persistent.MultiPersistent(name, save_on_quit=True)\n"
                "        mp.value = 42\n"
                "        mp.save()\n")
            env = {"DULCAMARA_RUNTIME_PROBE": "1", "DULCAMARA_SAVE_STORE": str(work / "manager"),
                   "RENPY_MULTIPERSISTENT": str(shared),
                   "DULCAMARA_SAVE_CORE": "2" if "renpy7" in str(self.BUILD) else "1"}
            output = self.run_core("play", game, data, env=env)
            self.assertEqual(output.count("runtime: early native bytes protected by worker"), 2)
            self.assertTrue((shared / "eclipse-first").is_file())
            self.assertTrue((shared / "eclipse-second").is_file())
            from test_saves import build_cli
            expected = {name: (shared / name).read_bytes() for name in ("eclipse-first", "eclipse-second")}
            snapshot = re.search(r"runtime: post snapshot ([^\s]+)", output).group(1)
            for name in expected:
                (shared / name).write_bytes(b"later damage")
            subprocess.run([str(build_cli(work)), str(work / "manager"), "native-fixture", snapshot], check=True)
            for name, contents in expected.items():
                self.assertEqual((shared / name).read_bytes(), contents)
            # A second run exercises existing native MultiPersistent files at the same barriers.
            output = self.run_core("load", game, data, env={**env, "DULCAMARA_TEST_LOAD": "1", "DULCAMARA_SAVE_SESSION": "2"})
            self.assertEqual(output.count("runtime: early native bytes protected by worker"), 2)
            self.assertIn("DULCAMARA-TEST: loaded", output)

    def test_native_load_after_verified_restore(self):
        from test_saves import build_cli
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory).resolve()
            game, data, store = make_game(work / "Restore Native"), work / "data", work / "manager"
            output = self.run_core("play", game, data, env={"DULCAMARA_RUNTIME_PROBE": "1", "DULCAMARA_SAVE_STORE": str(store)})
            snapshot = re.search(r"runtime: post snapshot ([^\s]+)", output).group(1)
            slots = list((data / "saves/renpy/dulcamara-test-game").glob("1-1-*.save"))
            self.assertTrue(slots)
            for slot in slots:
                slot.write_bytes(b"damaged native slot")
            cli = build_cli(work)
            subprocess.run([str(cli), str(store), "native-fixture", snapshot], check=True)
            output = self.run_core("load", game, data, env={"DULCAMARA_TEST_LOAD": "1"})
            self.assertIn("DULCAMARA-TEST: loaded", output)
            self.assertIn("DULCAMARA-TEST: choice=right", output.split("DULCAMARA-TEST: loaded", 1)[1])

    def test_custom_native_selectors_preserve_assets_and_token_neighbors(self):
        from test_saves import build_cli
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory).resolve()
            game, data, store = make_game(work / "Custom suffix"), work / "data", work / "manager"
            native = game / "game"  # Deliberately mixed with scripts/assets, not a dedicated directory.
            (native / "native_selectors.rpy").write_text(
                "python early:\n"
                "    import renpy as native_renpy\n"
                "    native_renpy.config.savedir = %r\n"
                "    native_renpy.savegame_suffix = '.custom'\n"
                "init -998 python:\n"
                "    import renpy as native_renpy\n"
                "    native_renpy.savegame_suffix = '.changed'\n" % str(native))
            script = native / "script.rpy"
            script.write_text(script.read_text().replace('$ renpy.save("1-1")',
                '$ renpy.save("1-1")\n    python:\n        import renpy as native_engine\n        for native_process in range(40):\n            native_engine.savelocation.tmp = ".%d.tmp" % (100000 + native_process)\n            renpy.save("nested/explicit")'))
            for location in (native, native / "saves", native / "sync"):
                (location / "nested").mkdir(parents=True, exist_ok=True)
            unrelated_nested = native / "nested/unrelated.changed"
            unrelated_nested.write_bytes(b"unrelated nested before")
            previous = native / "old.custom"
            previous.write_bytes(b"prior native selector bytes")
            tokens = data / "saves/renpy/tokens"
            tokens.mkdir(parents=True)
            neighbor = tokens / "unrelated.txt"
            neighbor.write_bytes(b"before unrelated")
            env = {"DULCAMARA_RUNTIME_PROBE": "1", "DULCAMARA_SAVE_STORE": str(store),
                   "DULCAMARA_SAVE_CORE": "2" if "renpy7" in str(self.BUILD) else "1"}
            output = self.run_core("play", game, data, env=env)
            self.assertIn("native-selector: .custom ", output)
            self.assertIn("native-selector: .changed ", output)
            slots = list(native.glob("1-1*.changed"))
            self.assertTrue(slots)
            nested_slots = list((native / "nested").glob("explicit*.changed"))
            self.assertTrue(nested_slots)
            expected_nested = {path: path.read_bytes() for path in nested_slots}
            expected_previous = previous.read_bytes()  # Native token upgrade may rewrite it.
            snapshot = re.search(r"runtime: post snapshot ([^\s]+)", output).group(1)
            for slot in slots:
                slot.write_bytes(b"damage")
            for path in nested_slots:
                path.write_bytes(b"damaged nested native save")
            unrelated_nested.write_bytes(b"unrelated nested after")
            newly_added = native / "nested/new-unrelated.changed"
            newly_added.write_bytes(b"new unrelated nested")
            previous.write_bytes(b"damaged old suffix")
            script = native / "script.rpy"
            expected_script = script.read_bytes() + b"\n# newer unrelated asset survives restoration\n"
            script.write_bytes(expected_script)
            neighbor.write_bytes(b"after unrelated")
            (tokens / "security_keys.txt").write_bytes(b"damaged keys")
            subprocess.run([str(build_cli(work)), str(store), "native-fixture", snapshot], check=True)
            self.assertEqual(script.read_bytes(), expected_script)
            self.assertEqual(neighbor.read_bytes(), b"after unrelated")
            self.assertEqual(previous.read_bytes(), expected_previous)
            for path, expected in expected_nested.items():
                self.assertEqual(path.read_bytes(), expected)
            self.assertEqual(unrelated_nested.read_bytes(), b"unrelated nested after")
            self.assertEqual(newly_added.read_bytes(), b"new unrelated nested")
            output = self.run_core("load", game, data, env={"DULCAMARA_TEST_LOAD": "1"})
            self.assertIn("DULCAMARA-TEST: loaded", output)

    def test_initial_union_includes_early_native_locations(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Early roots"), work / "data"
            configured, extra = work / "configured", work / "extra"
            local = game / "game/saves"
            tokens = data / "saves/renpy/tokens"
            # These paths are known when script loading finishes, before init statements.
            (game / "game/early_native.rpy").write_text(
                "python early:\n"
                "    import renpy as native_renpy\n"
                "    native_renpy.config.savedir = %r\n"
                "    native_renpy.config.extra_savedirs = [%r]\n" % (str(configured), str(extra)))
            for root in (configured, extra, local, tokens):
                root.mkdir(parents=True, exist_ok=True)
                (root / "existing-save.fixture").write_bytes(b"pre-launch save bytes\x00")
            expected = work / "initial-roots.txt"
            expected.write_text("".join("4 1 %s\n" % root.resolve() for root in (configured, local, extra)) +
                                "2 2 %s\n2 2 %s\n" % ((tokens / "security_keys.txt").resolve(), (tokens / "upgraded.txt").resolve()))
            output = self.run_core("play", game, data, env={"DULCAMARA_RUNTIME_PROBE": "1",
                "DULCAMARA_RUNTIME_INITIAL_LIST": str(expected),
                "DULCAMARA_RUNTIME_ABSENT": str(tokens / "security_keys.txt")})
            self.assertIn("runtime: initial union contains every expected native root", output)
            for root in (configured, extra, local, tokens):
                self.assertEqual((root / "existing-save.fixture").read_bytes(), b"pre-launch save bytes\x00")
            self.assertTrue((tokens / "security_keys.txt").is_file())
            self.assertTrue(list(extra.glob("1-1-*.save")))

    def test_legacy_frontend_accepts_oversized_native_layout(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Legacy roots")
            extras = [work / ("extra-%02d" % i) for i in range(33)]
            (game / "game/legacy_native.rpy").write_text(
                "python early:\n"
                "    import renpy as native_renpy\n"
                "    native_renpy.config.extra_savedirs = %r\n" % [str(path) for path in extras])
            # With no runtime-probe opt-in this real frontend rejects the optional environment command.
            output = self.run_core("play", game, work / "data", env={"DULCAMARA_RUNTIME_PROBE": ""})
            self.assertNotIn("runtime: initial checkpoint", output)
            for root in extras:
                self.assertTrue(list(root.glob("1-1-*.save")), str(root))

    def test_stop_at_held_initial_checkpoint(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Held")
            self.run_core("held-stop", game, work / "data", env={"DULCAMARA_RUNTIME_PROBE": "1"})
            self.assertFalse(list(work.rglob("*.save")))
            self.assertFalse(list(work.rglob("persistent")))

    def test_native_stop_refusal_is_unsafe(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Refusing")
            (game / "game/refuse.rpy").write_text(
                "init python:\n"
                "    import os\n"
                "    print('DULCAMARA-TEST: refusing')\n"
                "    retained_pipe = os.pipe()\n"
                "    os.read(retained_pipe[0], 1)\n")
            result = subprocess.run([str(self.BUILD / "core-check/renpy-headless"),
                str(self.BUILD / "assets"), "refuse", str(game), str(work / "data")],
                capture_output=True, text=True, timeout=30,
                env={**os.environ, "DULCAMARA_RUNTIME_PROBE": "1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("runtime: timeout retained unsafe writers", result.stdout)

    def test_a_folder_named_in_japanese_with_spaces(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "テスト ゲーム (体験版)"), work / "データ")

    def test_a_second_game_in_the_same_process(self):
        # Ren'Py's compiled modules keep their state in C statics, so Python cannot start over for a second game in
        # the same process: the core refuses it with a reason, and the app starts itself again for it.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            first, second = make_game(work / "First"), make_game(work / "Second", 1920, 1080)
            output = self.run_core("play", first, work / "data", "--then", second)
            self.assertIn("ended by the game", output)
            self.assertIn("Ren'Py runs one game each time Dulcamara starts", output)
            self.assertIn("second game: refused", output)

    def test_a_game_larger_than_the_picture(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Large", 2560, 1440), work / "data")
            self.assertIn("picture: 1920x1080", output)

    def test_a_sound_in_an_archive(self):
        # A sound in a game's archive (.rpa) is read before Ren'Py's sound has it: into memory, or, when it is large,
        # through Python's own file as it plays (DULCAMARA_SOUND_IN_MEMORY=0 makes every sound large). The log says
        # where each sound comes from.
        archived = r"from audio\.rpa at \d+ \(\d+ bytes\), "
        for is_archived, limit, source in ((False, None, r"from the file .*tone\.wav"),
                                           (True, None, archived + "read into memory"),
                                           (True, "0", archived + "read through Python")):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Archived", archived=is_archived)
                env = {"RENPY_HEADLESS_VERBOSE": "1", **({"DULCAMARA_SOUND_IN_MEMORY": limit} if limit else {})}
                output = self.run_core("play", game, work / "data", env=env)
                self.assertIn("sound: playing", output)
                self.assertRegex(output, r"plays u?'audio/tone\.wav' on channel 0, " + source)
                self.assertNotIn("could not read the sound", output)

    def test_a_large_archived_sound_closes_its_file(self):
        # A sound in an archive too large to read into memory is read through Python's own file as it plays
        # (DULCAMARA_SOUND_IN_MEMORY=0 makes every sound large), and Ren'Py's sound closes that file as the sound ends.
        # Ren'Py 7's (pygame_sdl2's python_close) kept it, and the file open, when it could not be closed: a file
        # descriptor more for each play and each loop, until the process had none left and images could not be loaded.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Files", archived=True)
            output = self.run_core("start", game, work / "data",
                                   env={"DULCAMARA_SOUND_IN_MEMORY": "0", "DULCAMARA_TEST_FILES": "20",
                                        "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: files:"})
            more = re.search(r"DULCAMARA-TEST: files: (-?\d+) more open after 20 sounds", output)
            self.assertTrue(more, output[-3000:])
            self.assertLessEqual(int(more.group(1)), 2, "each sound's file is closed as it ends")

    FONT_ROWS = 80  # each line of the font test this many pixels below the one before, from 20

    def test_text_in_each_kind_of_font(self):
        # Ren'Py 7 games on the console showed missing text. Each way a game gives a font is drawn here, a line of white
        # text on black in each: Ren'Py's own (the measure), a font named in another case than its file's (as a
        # game made on Windows names it), one in the game's archive, the second face of a collection (.ttc), one in CFF
        # outlines (.otf, where this system has one); as Ren'Py makes images ready and as the console does (on the
        # CPU, copies written back). Each line must be drawn about as much as Ren'Py's own.
        common = next((self.BUILD / "assets").glob("*/renpy/common"))
        dejavu, bold = (common / "DejaVuSans.ttf").read_bytes(), (common / "DejaVuSans-Bold.ttf").read_bytes()
        otf = next((Path(p) for p in OTF_FONTS if Path(p).is_file()), None)
        rows = [("own", None), ("case", "FONTS/dejavu mixed CASE.ttf"), ("archive", "fonts/archived.ttf"),
                ("collection", "1@fonts/pair.ttc")] + ([("otf", "fonts/cff.otf")] if otf else [])
        script = ["label dulcamara_fonts:", '    scene expression Solid("#000000")']
        for i, (name, font) in enumerate(rows):
            text = f'Text("Hamburgevons {name}", size=44, color="#ffffff"' + (f", font={font!r})" if font else ")")
            script.append(f"    show expression {text} as font_{name} at Position(xpos=20, ypos={20 + i * self.FONT_ROWS}, "
                          "xanchor=0, yanchor=0)")
        script += ["    $ renpy.pause(0.5, hard=True)", '    $ dulcamara_test("fonts shown")',
                   "    $ renpy.pause(5.0, hard=True)", "    return"]
        for console in ("0", "1"):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Fonts")
                fonts = game / "game/fonts"
                fonts.mkdir()
                (fonts / "DejaVu Mixed Case.TTF").write_bytes(dejavu)
                write_rpa(game / "game/fonts.rpa", {"fonts/archived.ttf": dejavu})
                write_ttc(fonts / "pair.ttc", [bold, dejavu])
                if otf:
                    (fonts / "cff.otf").write_bytes(otf.read_bytes())
                (game / "game/fonts.rpy").write_text("\n".join(script) + "\n", encoding="utf-8")
                output = self.run_core("start", game, work / "data", env={
                    "DULCAMARA_TEST_LABEL": "dulcamara_fonts", "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: fonts shown",
                    "RENPY_HEADLESS_PICTURE": str(work / "picture.raw"), "DULCAMARA_GL_CPU_LOAD": console,
                    "DULCAMARA_GL_WRITE_BACK": console, "RENPY_HEADLESS_VERBOSE": "1"})
                self.assertNotIn("Ren'Py reported an error", output)
                picture = (work / "picture.raw").read_bytes()
            self.assertEqual(len(picture), 1280 * 720 * 4)
            drawn = {}
            for i, (name, _font) in enumerate(rows):
                white = 0
                for y in range(20 + i * self.FONT_ROWS, 20 + i * self.FONT_ROWS + 60):
                    line = picture[y * 1280 * 4:(y + 1) * 1280 * 4]
                    white += sum(1 for x in range(0, 900 * 4, 4) if min(line[x], line[x + 1], line[x + 2]) > 200)
                drawn[name] = white
            print(f"fonts (on the CPU: {console}): white pixels of each line {drawn}" + ("" if otf else "; no .otf here"))
            for name, white in drawn.items():
                self.assertGreater(white, drawn["own"] * 0.5, (name, drawn))

    def test_a_refused_thread_does_not_hold_the_stop(self):
        # A thread the system refuses (as the console does when its memory for them runs out): the game is told so and
        # goes on, and when it is closed the core stops Ren'Py at once. CPython 2.7 kept the refused thread's state
        # among its threads for good, and the core, which waits for every thread of Python's before it ends it, then
        # waited out its ten seconds and called the stop unsafe after any refused thread of a Ren'Py 7 game.
        refuse = build_thread_refuser(Path(tempfile.mkdtemp()))
        if refuse is None:
            self.skipTest("needs a C compiler that makes a preloadable library")
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Refused"), work / "data",
                                   env={**refuse, "DULCAMARA_REFUSE_THREADS": f"file:{work / 'refuse'}",
                                        "DULCAMARA_TEST_REFUSE_FILE": str(work / "refuse"),
                                        "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: thread refused"})
            self.assertIn("DULCAMARA-TEST: thread refused: can't start new thread", output)

    def test_a_game_starts_when_its_threads_are_refused(self):
        # The console refuses a thread when its system memory for them runs out, and a tester's Ren'Py 7 games
        # sometimes ended in Ren'Py's error screen as they loaded, which a relaunch put right. Ren'Py starts its own
        # threads as a game loads: its image preloader, its save scanner (twice) and its sound's periodic thread, and
        # it ended in its error screen when any of them was refused ("can't start new thread"). Each refused in turn
        # (tests/core/refuse_threads.c, as the console refuses: EAGAIN), the game starts, shows its picture, plays,
        # and the log says which thread the console refused and what was done instead.
        refuse = build_thread_refuser(Path(tempfile.mkdtemp()))
        if refuse is None:
            self.skipTest("needs a C compiler that makes a preloadable library")
        # The threads Ren'Py asks for as it loads, in order: 1 its own (the core's), 2 the preloader, 3 the save
        # scanner, 4 the scanner again (Ren'Py starts it over once the game's scripts are loaded), 5 the sound's
        # periodic work. A thread refused once is asked for again and given; refused the five times it is asked for,
        # the game goes on without it.
        for refused, said in (("2", r"the console gave a thread \(preloader\) on try 2"),
                              ("2,3,4,5,6", r"the console gave Ren'Py's image preloader no thread"),
                              ("3,4,5,6,7", r"the console gave Ren'Py's save scanner no thread"),
                              ("4,5,6,7,8", r"the console gave Ren'Py's save scanner no thread"),
                              ("5,6,7,8,9", r"the console gave Ren'Py's sound's periodic work no thread")):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                output = self.run_core("start", make_game(work / "Refused"), work / "data",
                                       env={**refuse, "DULCAMARA_REFUSE_THREADS": f"only:{refused}",
                                            "RENPY_HEADLESS_VERBOSE": "1"})
                self.assertIn("DULCAMARA-TEST: started", output, refused)
                self.assertIn("sound: playing", output, refused)
                self.assertRegex(output, said, refused)
                self.assertNotIn("Ren'Py reported an error", output, refused)

    def test_images_are_preloaded_without_the_preloaders_thread(self):
        # with the image preloader's thread refused for good, Ren'Py's own thread preloads the images and keeps the
        # image cache within its size, as the thread would: the soak's scenes are each drawn whole
        refuse = build_thread_refuser(Path(tempfile.mkdtemp()))
        if refuse is None:
            self.skipTest("needs a C compiler that makes a preloadable library")
        try:
            from tests import test_renpy_soak as soak
        except ImportError:
            import test_renpy_soak as soak
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = soak.make_soak_game(work / "Soak")
            env = {**refuse, "DULCAMARA_REFUSE_THREADS": "only:2,3,4,5,6", "DULCAMARA_TEST_SOAK": "60",
                   "RENPY_HEADLESS_FRAME_USEC": str(soak.FRAME_USEC), "DULCAMARA_SOAK_EVERY": "10",
                   "DULCAMARA_SOAK_CACHE_MB": "8", "RENPY_HEADLESS_VERBOSE": "1"}
            output = self.run_core("soak", game, work / "data", env=env)
            soak.Renpy8SoakTest.assert_drawn(self, output, 60)
            self.assertRegex(output, r"the console gave Ren'Py's image preloader no thread")
            table = soak.rows(output)
            self.assertLessEqual(max(row["cache_mb"] for row in table), 8, "the image cache keeps within its size")

    def test_an_archive_over_4_gib(self):
        # "Games up to 20 GB": a game whose one archive (.rpa) is over 4 GiB, its picture and its sound stored past
        # the 4 GiB offset, which a 32-bit offset could not reach. The archive is a sparse file (a hole of 4 GiB
        # before them), so it takes a few kilobytes of disk. The sound is read into memory, and through Python's own
        # file as it plays (DULCAMARA_SOUND_IN_MEMORY=0), both from where the archive's index says.
        four_gib = 4 << 30
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Huge")
            content = game / "game"
            moved = {"images/dulcamara.png": content / "images/dulcamara.png",
                     "audio/tone.wav": content / "audio/tone.wav"}
            archive = content / "big.rpa"
            key, offset, index = 0x42424242, 34 + four_gib, {}
            with open(archive, "wb") as f:
                f.seek(offset)
                for name, path in moved.items():
                    blob = path.read_bytes()
                    index[name] = [(offset ^ key, len(blob) ^ key, b"")]
                    f.write(blob)
                    offset += len(blob)
                    path.unlink()
                f.write(zlib.compress(pickle.dumps(index, protocol=2)))
                f.seek(0)
                f.write(b"RPA-3.0 %016x %08x\n" % (offset, key))
            size, used = archive.stat().st_size, archive.stat().st_blocks * 512
            if used >= 64 << 20:
                self.skipTest(f"this filesystem cannot make a sparse file ({used} bytes used for {size})")
            self.assertGreater(size, four_gib)
            for limit, source in ((None, "read into memory"), ("0", "read through Python")):
                env = {"RENPY_HEADLESS_VERBOSE": "1", **({"DULCAMARA_SOUND_IN_MEMORY": limit} if limit else {})}
                output = self.run_core("play", game, work / f"data-{limit or 'memory'}", env=env)
                for line in ("XRGB8888", "input: the picture changed after Cross", "sound: playing",
                             "DULCAMARA-TEST: saved", "ended by the game"):
                    self.assertIn(line, output)
                at = re.search(r"plays u?'audio/tone\.wav' on channel 0, from big\.rpa at (\d+) \(\d+ bytes\), "
                               + source, output)
                self.assertTrue(at, output[-3000:])
                self.assertGreater(int(at.group(1)), four_gib)
                self.assertNotIn("could not read the sound", output)
                # the picture, only in the archive, past 4 GiB too
                self.assertRegex(output, r"image u?'images/dulcamara\.png' is 256x256 as Ren'Py loads it")

    def test_an_ogg_sound_in_an_archive(self):
        # A game's first OGG (Vorbis) sound, from its archive, ended 0.2.6 to 0.2.8 on the console: FFmpeg's FFT read a
        # table out of the code, which the console cannot read (tools/patches/ffmpeg-4.3.1-fft-tables.diff). This
        # computer reads its code, so this plays one as the game did, through the same steps.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("play", make_game(work / "Vorbis", archived=True), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_MUSIC": "audio/tone.ogg"})
            self.assertIn("sound: playing", output)
            self.assertRegex(output, r"plays u?'audio/tone\.ogg' on channel 0, from audio\.rpa at \d+ \(\d+ bytes\), "
                                     r"read into memory")
            self.assertNotIn("could not read the sound", output)

    def test_sounds_for_this_game(self):
        # Sounds for this game (the core's renpy_sounds option: sounds/<the game's id> in the data folder): the core
        # lists the game's sounds in <base>.list, from its folder or its archive; with one of them off in <base>.txt it
        # plays a second of silence in its place and says so; with none off (an empty file, or none at all) it plays.
        # The archived game plays its music as "<loop 0.5>tone.wav", which Ren'Py finds in audio/.
        header = "# Dulcamara: the sounds of Dulcamara test game switched off\n"
        for is_archived in (False, True):
            music = "<loop 0.5>tone.wav" if is_archived else "audio/tone.wav"
            for off in (None, "", "off = audio/tone.wav\n"):
                with tempfile.TemporaryDirectory() as directory:
                    work = Path(directory)
                    game, base = make_game(work / "Sounds", archived=is_archived), work / "data/sounds/0123abcd"
                    if off is not None:
                        base.parent.mkdir(parents=True)
                        Path(f"{base}.txt").write_text(header + off, encoding="utf-8")
                    output = self.run_core("sounds", game, work / "data", env={
                        "RENPY_HEADLESS_VERBOSE": "1", "renpy_sounds": str(base), "DULCAMARA_TEST_MUSIC": music})
                    self.assertEqual(Path(f"{base}.list").read_text(encoding="utf-8").splitlines(),
                                     ["audio/tone.ogg", "audio/tone.wav"])
                    self.assertFalse(Path(f"{base}.list.new").exists())
                    if off:
                        self.assertRegex(output, r"plays nothing for 'audio/tone\.wav' on channel \d+: it is off "
                                                 r"\(Sounds for this game\)")
                        self.assertIn("sound: silent", output)
                    else:
                        self.assertNotIn("plays nothing", output)
                        self.assertIn("sound: playing", output)

    def test_a_sound_switched_off_as_the_game_runs(self):
        # Switched off in Dulcamara's menu while the game plays it (here the headless frontend writes <base>.txt): the
        # core reads the file again within a second, and the next time the game plays the sound (its music, looped),
        # it is silence.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            base = work / "data/sounds/0123abcd"
            env = {"RENPY_HEADLESS_VERBOSE": "1", "renpy_sounds": str(base),
                   "RENPY_HEADLESS_SWITCH_OFF": "audio/tone.wav"}
            output = self.run_core("sounds", make_game(work / "Sounds"), work / "data", env=env)
            self.assertIn("1 of the game's sounds off in", output)
            self.assertRegex(output, r"plays nothing for 'audio/tone\.wav' on channel \d+")
            self.assertIn("sound: silent", output)

    def test_a_movie_waits_for_its_mask(self):
        # A movie with a mask of its own is two streams: Ren'Py drew a colour frame whose mask frame came late as it
        # was, black wherever the mask makes it clear (a box's animated border, flashing black on the console). Fed
        # frames as the console's late masks come, each colour frame drawn has its mask, and one whose mask has not
        # come waits: the second is drawn a tick late, with its mask, and the last not yet.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Movie"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_MOVIE_MASK": "1"})
            self.assertIn("DULCAMARA-TEST: movie mask: new [True, False, True, False] alpha [0, 0, 255]", output)

    def test_images_made_ready_on_the_cpu(self):
        # On the console some images lost their alpha (a black box around a small cup, black at boxes' borders) as
        # Ren'Py made them ready, drawing them premultiplied and copying that. The core makes them ready on the CPU
        # instead there (gl.c, DULCAMARA_GL_CPU_LOAD): the picture of images partly clear, a large and a small one,
        # is the same either way, each channel of each pixel within 2 (OpenGLs round the draw's premultiplying a
        # little differently; premultiplying wrongly is off by up to 255).
        pictures = {}
        for cpu in ("0", "1"):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Alpha")
                write_png_rgba(game / "game/images/dulcamara_glass.png", 96, 80)
                write_png_rgba(game / "game/images/dulcamara_dot.png", 24, 30)
                output = self.run_core("start", game, work / "data", env={
                    "RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_ALPHA": "1", "DULCAMARA_GL_CPU_LOAD": cpu,
                    "RENPY_HEADLESS_PICTURE": str(work / "picture.raw")})
                self.assertIn("DULCAMARA-TEST: alpha shown", output)
                made = "an image made ready on the CPU, the first" in output
                self.assertEqual(made, cpu == "1", output[-2000:])
                pictures[cpu] = (work / "picture.raw").read_bytes()
        self.assertEqual(len(pictures["0"]), 1280 * 720 * 4)
        self.assertEqual(len(pictures["0"]), len(pictures["1"]))
        worst = max(abs(a - b) for a, b in zip(pictures["0"], pictures["1"]))
        self.assertLessEqual(worst, 2, "a channel of a pixel differs by %d" % worst)

    def test_a_smaller_largest_texture(self):
        # Ren'Py's framebuffer is the size of its largest texture, 4096x4096 (64 MB, and as much for its depth): on
        # the console it is asked for at 2048, DULCAMARA_MAX_TEXTURE_SIZE anywhere, and Ren'Py says what it made.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Textures"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_MAX_TEXTURE_SIZE": "2048"})
            self.assertIn("Maximum texture size: 2048x2048", output)

    def test_a_sound_on_two_channels_at_most(self):
        # Each copy of a sound playing at once is a decode thread of its own, and a tester's fast cursor over a title
        # menu played its hover sound on 35 channels at once (Ren'Py's audio channel opens another while the others
        # play) until the console refused threads: played six times at once, it plays on two, the older stopped.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Copies"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_COPIES": "1"})
            self.assertIn("DULCAMARA-TEST: copies playing: 2", output)
            self.assertRegex(output, r"plays on 3 channels at once: the copy on channel \S+ stopped \(4 so far\)")

    def test_the_frontends_sound_does_not_wait_for_the_mixers_lock(self):
        # Dulcamara's main loop tops its sound up through the core (its sound callback, audio.c's mixer) a frame at a
        # time, ~70 ms ahead. While Ren'Py's thread held the mixer's lock (to start or stop a sound), each top-up waited
        # with it, the loop's picture and its queue of sound too: held long, the game's music ran dry ("music gets
        # interrupted when I click an option", a PS5 tester's). With the lock held 300 ms elsewhere, the top-ups are
        # skipped at once instead, the frontend runs on, and the music plays again once the lock is let go.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("held", make_game(work / "Held"), work / "data",
                                   env={"RENPY_HEADLESS_HOLD_MS": "300"})
            held = re.search(r"held: the sound's lock held (\d+) ms by another thread: (\d+) frames ran meanwhile, "
                             r"(\d+) top-ups, the longest ([\d.]+) ms", output)
            self.assertIsNotNone(held, output)
            self.assertGreaterEqual(int(held[1]), 250, output)
            self.assertLess(float(held[4]), 20.0, "a top-up waited for the mixer's lock")
            self.assertGreater(int(held[3]), 5, "the frontend's loop did not run on while the lock was held")
            self.assertIn("a top-up of the frontend's sound skipped", output)
            self.assertRegex(output, r"held: after it was let go, \d+ batches with sound in them")

    def clicks_over_music(self, work, env):
        """The test game's music, and a click sound (a tenth of a second, 880 Hz) played on Ren'Py's audio channel six
        times, every quarter of a second, as a game plays one at each menu choice: the core's output"""
        game = make_game(work / "Clicks")
        write_tone(game / "game/audio/click.wav", seconds=0.1, hz=880)
        (game / "game/clicks.rpy").write_text(CLICKS_LABEL, encoding="utf-8")
        return self.run_core("start", game, work / "data", env={
            "DULCAMARA_TEST_LABEL": "dulcamara_clicks", "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: clicks done", **env})

    def assert_no_long_hold(self, output):
        holds = [float(ms) for ms in re.findall(r"the sound's lock was held ([\d.]+) ms", output)]
        self.assertFalse([ms for ms in holds if ms >= 150], "the mixer's lock was held as a sound started:\n" + output)
        self.assertIn("sound: playing", output)
        self.assertRegex(output, r"top-ups: \d+, the longest [0-9]\.\d ms")

    def test_a_sound_started_does_not_hold_the_mixer(self):
        # Ren'Py's thread starts a decode thread for each sound it plays (ffmedia.c's media_start, in load_stream).
        # RPS_play did that with the mixer's lock held (RPS_queue opens its sound first): on a console slow to start a
        # thread, every click held the lock, and the frontend's sound with it, for as long as that took, and the
        # game's music cut out (tools/patches/renpy-renpysound-channels.diff). Each thread here takes 300 ms to start.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            slow = build_preloaded(work, "slow_thread_start")
            if slow is None:
                self.skipTest("needs a C compiler that makes a preloaded library")
            output = self.clicks_over_music(work, {**slow, "DULCAMARA_SLOW_THREAD_START": "300"})
            self.assert_no_long_hold(output)

    def test_a_sound_read_slowly_does_not_hold_the_mixer(self):
        # A sound's file is opened and read on its decode thread (through Python, for a file Ren'Py gives as Python's),
        # not under the mixer's lock: the click's first read here takes 300 ms, and the lock is not held meanwhile.
        with tempfile.TemporaryDirectory() as directory:
            output = self.clicks_over_music(Path(directory), {"DULCAMARA_TEST_SLOW_READ": "300"})
            self.assert_no_long_hold(output)
            self.assertIn("DULCAMARA-TEST: slow reads: 6", output)

    def test_an_autosave_without_a_thread(self):
        # 0.2.10's Ren'Py 7 showed its error screen at a choice late in a session: the console refused the thread for
        # its autosave. The autosave is made on Ren'Py's own thread instead, and the next one may still run.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Autosave"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_AUTOSAVE": "1"})
            self.assertIn("DULCAMARA-TEST: autosave made, another may run: True", output)
            self.assertIn("the console gave the autosave no thread: it is made on Ren'Py's own (1 so far)", output)
            self.assertNotIn("Ren'Py reported an error", output)

    def test_a_black_flash_is_logged(self):
        # Testers see boxes flash black for a moment: the core's log finds each in the pictures Ren'Py draws, with its
        # box, so that one not found there is the frontend's. A black box shown for a moment is one, where it was; how
        # many pictures long depends on the machine's frame rate (6 on a fast CI runner), so any length is taken.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Flash"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_FLASH": "1"})
            self.assertIn("DULCAMARA-TEST: flash shown", output)
            self.assertRegex(output, r"black flash 1, picture \d+, \d+ pictures long: 160x120 at 320,240 of the "
                                     r"1280x720 picture")

    def test_a_testers_switches(self):
        # renpy-test.txt in the data folder sets the core's DULCAMARA_ switches for the game, so that a tester tries
        # one on a console without a build of its own: here images without mipmaps; what is not a switch is left.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            data = work / "data"
            data.mkdir()
            (data / "renpy-test.txt").write_text("# the black flashes\nDULCAMARA_MIPMAPS=0\nPATH=/nowhere\n"
                                                 "dulcamara_lower=1\n", encoding="utf-8")
            output = self.run_core("start", make_game(work / "Switches"), data, env={"RENPY_HEADLESS_VERBOSE": "1"})
            self.assertIn("renpy-test.txt sets DULCAMARA_MIPMAPS=0", output)
            self.assertIn("images have no mipmaps (DULCAMARA_MIPMAPS=0)", output)
            self.assertNotIn("sets PATH", output)
            self.assertNotIn("dulcamara_lower", output)

    def test_sound_plays_at_the_games_own_level(self):
        # The core neither raises nor lowers a game's sound: the test tone (a peak of 12000) comes out at 8485, the
        # 3 dB Ren'Py's own mixer takes off a sound in the middle of the two speakers. (A tester heard nothing at half
        # of a game's music slider: Ren'Py puts half at -20 dB, a tenth of full, on every platform.)
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("play", make_game(work / "Level"), work / "data")
            peak = int(re.search(r"the loudest sample (\d+)", output).group(1))
            self.assertTrue(12000 * 0.69 <= peak <= 12000 * 0.73, peak)

    def volume_run(self, setting, game, data, cwd=None):
        """The loudest sample of the test game's music with its sliders at setting ("<main>,<music>", as
        DULCAMARA_TEST_VOLUME; "default=<v>": the sliders left where the game's config.default_music_volume = <v> puts
        them), and the run's output. (Run as "start", a second of the music: "play" fails a run whose sound is as quiet
        as the engine's -40 dB.)"""
        default = setting.startswith("default=")
        env = {"DULCAMARA_TEST_VOLUME": "-,-" if default else setting,
               "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: music played"}
        if default:
            env["DULCAMARA_TEST_DEFAULT_MUSIC"] = setting.split("=")[1]
        output = self.run_core("start", game, data, env=env, cwd=cwd)
        return int(re.search(r"the loudest sample (\d+)", output).group(1)), output

    def volume_peaks(self, renpy_version, *sliders, in_game_folder=False, **game):
        """The loudest sample of the test game's music made with renpy_version (and make_game's other `game` options),
        with both sliders full, then with each of sliders, as fractions of the full one. in_game_folder: run in the
        game's folder, as the console's core does."""
        peaks = []
        for setting in ("1,1", *sliders):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                folder = make_game(work / "Volume", renpy_version=renpy_version, **game)
                peaks.append(self.volume_run(setting, folder, work / "data",
                                             cwd=folder if in_game_folder else None)[0])
        self.assertTrue(12000 * 0.69 <= peaks[0] <= 12000 * 0.73, peaks[0])  # full: the game's own level
        return [peak / peaks[0] for peak in peaks[1:]]

    # Ren'Py 8.1 and 7.6 made volume sliders decibels (half is -20 dB), and Dulcamara runs the newest Ren'Py. Before
    # them a slider was quadratic (Ren'Py's config.quadratic_volumes, which it sets itself for a game that ships
    # game/script_version.txt): half is a quarter of the amplitude (-12 dB), main and music at half a sixteenth. A
    # game made before them sounds as it did on PC.
    OLD_RENPY, NEW_RENPY = "8.0.3.22090809", "8.1.0.23051307"

    def old_quadratic(self, music, both, default, mixer):
        self.assertAlmostEqual(music, 0.25, delta=0.015)
        self.assertAlmostEqual(both, 0.0625, delta=0.006)
        self.assertAlmostEqual(default, 0.25, delta=0.015)  # its default music volume of 0.5 is a quarter too
        self.assertAlmostEqual(mixer, 0.25, delta=0.015)  # and preferences.set_mixer(0.5), Preference("music volume")

    def test_volume_sliders_of_a_game_made_for_an_older_renpy(self):
        self.old_quadratic(*self.volume_peaks(self.OLD_RENPY, "1,0.5", "0.5,0.5", "default=0.5", "1,set=0.5"))

    def test_volume_sliders_of_an_older_game_by_its_script_version(self):
        # no renpy/ folder (some repacks), only the game/script_version.txt Ren'Py's launcher writes into a build
        version = tuple(int(n) for n in self.OLD_RENPY.split(".")[:3])
        self.old_quadratic(*self.volume_peaks(None, "1,0.5", "0.5,0.5", "default=0.5", "1,set=0.5",
                                              script_version=version))

    def test_volume_sliders_of_a_game_made_for_a_newer_renpy(self):
        # the engine's own: half is -20 dB (a tenth), main and music at half -40 dB (a hundredth)
        music, both, default, mixer = self.volume_peaks(self.NEW_RENPY, "1,0.5", "0.5,0.5", "default=0.5",
                                                        "1,set=0.5")
        self.assertAlmostEqual(music, 0.1, delta=0.005)
        self.assertAlmostEqual(both, 0.01, delta=0.002)
        self.assertAlmostEqual(default, 0.1, delta=0.005)
        self.assertAlmostEqual(mixer, 0.1, delta=0.005)

    def test_volume_sliders_of_a_game_without_a_renpy_folder(self):
        # nothing says which Ren'Py the game was made for: the engine's own. A vc_version.py and __init__.py of an
        # older Ren'Py loose in the game's folder, the working folder (as on the console), are not the game's renpy/
        music, both = self.volume_peaks(None, "1,0.5", "0.5,0.5", in_game_folder=True, stray_version=self.OLD_RENPY)
        self.assertAlmostEqual(music, 0.1, delta=0.005)
        self.assertAlmostEqual(both, 0.01, delta=0.002)

    def test_a_volume_saved_before_keeps_its_loudness(self):
        # A Ren'Py setting is an amplitude, so a music volume the linear sliders of the previous build stored (half:
        # an amplitude of 0.5) plays as loud as before; only its slider sits further along (the square root, 0.71).
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            full = self.volume_run("1,1", make_game(work / "Full", renpy_version=self.OLD_RENPY), work / "full")[0]
            game, data = make_game(work / "Volume", renpy_version=self.OLD_RENPY), work / "data"
            stored = self.volume_run("-,amp=0.5", game, data)[0]
            kept, output = self.volume_run("-,-", game, data)
        self.assertAlmostEqual(stored / full, 0.5, delta=0.02)
        self.assertAlmostEqual(kept / full, 0.5, delta=0.02)
        self.assertIn("DULCAMARA-TEST: volumes: main 1.0000, music 0.5000", output)
        self.assertIn("DULCAMARA-TEST: sliders: main 1.0000, music 0.7071", output)

    def test_music_loops(self):
        # A game's music loops (Ren'Py queues it again after itself): a second of it still plays after three, for each
        # kind a game's music comes in, from its own file and from an archive
        for archived in (False, True):
            for sound in ("audio/tone.ogg", "audio/tone.wav"):
                with tempfile.TemporaryDirectory() as directory:
                    work = Path(directory)
                    output = self.run_core("start", make_game(work / "Loop", archived=archived), work / "data",
                                           env={"DULCAMARA_TEST_LOOP": sound,
                                                "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: music after"})
                    self.assertIn(f"DULCAMARA-TEST: music after 3.5 s: {sound}\n", output, (sound, archived))

    def test_music_that_stops_looping_says_why(self):
        # A looping track whose file is really gone (taken away as it plays) is tried again a few times, not for ever:
        # then it stops, and the log says why once, and that the tries ended once.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Gone"), work / "data",
                                   env={"DULCAMARA_TEST_LOOP_GONE": "1", "RENPY_HEADLESS_VERBOSE": "1",
                                        "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: music after it was gone"})
            self.assertIn("DULCAMARA-TEST: music after it was gone: None", output)
            said = re.findall(r"Ren'Py could not load u?'audio/once\.ogg', looping on channel \S+: .*; it stays on "
                              r"the loop and is tried again \(5 times at most\)", output)
            self.assertEqual(len(said), 1, output[-4000:])
            self.assertEqual(len(re.findall(r"u?'audio/once\.ogg' failed 6 times in a row on channel \S+: no "
                                            r"more tries, and Ren'Py stops looping it", output)), 1, output[-4000:])
            self.assertNotIn("once.ogg', and stops looping it if it did", output)

    def listen_to_looped_music(self, fails):
        """The test game's music, a second long, its next `fails` reads failing after its first: the log, with the
        sound the frontend got in one second, four and a half seconds in."""
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            return self.run_core("start", make_game(work / "Flaky"), work / "data",
                                 env={"DULCAMARA_TEST_LOOP_FAIL": str(fails), "RENPY_HEADLESS_VERBOSE": "1",
                                      "RENPY_HEADLESS_UNTIL": "DULCAMARA-TEST: music after 4.5 s, listen",
                                      "RENPY_HEADLESS_LISTEN": "60"})

    def test_music_loops_on_after_a_failure(self):
        # A tester's music played once on the console: Ren'Py takes a track it cannot queue again off the loop for
        # good. A read of it failing once (as an archive read, a decoder or a thread can), the music still plays
        # four loops later, in the sound the frontend gets; the log says what failed once, and that it loops again.
        output = self.listen_to_looped_music(1)
        self.assertIn("listened for 60 frames", output)
        self.assertRegex(output, r"(?m)^sound: playing ")
        said = re.findall(r"Ren'Py could not load u?'audio/flaky\.wav', looping on channel \S+: .*injected: the "
                          r"read of audio/flaky\.wav failed.*; it stays on the loop", output)
        self.assertEqual(len(said), 1, output[-4000:])
        self.assertRegex(output, r"u?'audio/flaky\.wav' loops again on channel \S+, after 1 failure\n")

    def test_music_loops_as_before(self):
        # nothing failing, the loop is Ren'Py's own, as before
        output = self.listen_to_looped_music(0)
        self.assertIn("listened for 60 frames", output)
        self.assertRegex(output, r"(?m)^sound: playing ")
        self.assertNotIn("it stays on the loop", output)
        self.assertNotIn("loops again", output)
        self.assertNotIn("could not load", output)

    def test_a_game_drawn_smaller(self):
        # DULCAMARA_PICTURE_HEIGHT (renpy-test.txt on a console): a game taller than that is drawn that high, its
        # picture read back smaller
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            data = work / "data"
            data.mkdir()
            (data / "renpy-test.txt").write_text("DULCAMARA_PICTURE_HEIGHT=360\n", encoding="utf-8")
            output = self.run_core("start", make_game(work / "Smaller"), data, env={"RENPY_HEADLESS_VERBOSE": "1"})
            self.assertIn("the game's 1280x720 is drawn at 640x360 (DULCAMARA_PICTURE_HEIGHT)", output)
            self.assertIn("the picture is 640x360", output)

    def test_the_presplash(self):
        # A game's presplash (game/presplash.png, or presplash_foreground and _background, a progress bar) is the
        # picture while the game loads, centred on black: Ren'Py draws it into a window of its own the console never
        # shows.
        for files in (("presplash",), ("presplash_foreground", "presplash_background")):
            with tempfile.TemporaryDirectory() as directory:
                work = Path(directory)
                game = make_game(work / "Presplash")
                for name in files:
                    write_png(game / "game" / f"{name}.png", 400, 250)
                output = self.run_core("start", game, work / "data", env={"RENPY_HEADLESS_VERBOSE": "1"})
                self.assertIn("the presplash, 400x250, is the picture (1280x720)", output)
                self.assertNotIn("could not be shown", output)

    # tests/fixtures/presplash.jpg: 400x250, a baseline JPEG in four flat quarters, centred on black in the picture
    PRESPLASH_JPG = ROOT / "tests/fixtures/presplash.jpg"
    PRESPLASH_QUARTERS = {(0, 0): (220, 40, 40), (1, 0): (40, 200, 60), (0, 1): (40, 60, 220), (1, 1): (230, 210, 40)}

    @contextlib.contextmanager
    def case_sensitive_folder(self):
        """A temporary folder that tells Presplash.JPG from presplash.jpg, as the console's does: the temporary folder
        itself where it does, otherwise (macOS) a case-sensitive APFS disk image, attached for the test."""
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            (work / "Probe").touch()
            if not (work / "probe").exists():
                yield work
                return
            if not shutil.which("hdiutil"):
                self.skipTest("needs a case-sensitive temporary folder, or hdiutil to make one")
            image, mount = work / "cs.sparseimage", work / "mnt"
            subprocess.run(["hdiutil", "create", "-quiet", "-size", "256m", "-fs", "Case-sensitive APFS", "-volname",
                            "dulcamara-case", "-type", "SPARSE", str(image)], check=True, timeout=120)
            subprocess.run(["hdiutil", "attach", "-quiet", "-nobrowse", "-mountpoint", str(mount), str(image)],
                           check=True, timeout=120)
            try:
                yield mount
            finally:
                subprocess.run(["hdiutil", "detach", "-quiet", "-force", str(mount)], check=False, timeout=120)

    def first_picture(self, game, work):
        """The run's first picture (RENPY_HEADLESS_FIRST_PICTURE), as rows of (r, g, b), with the log."""
        raw = work / "first.raw"
        output = self.run_core("start", game, work / "data",
                               env={"RENPY_HEADLESS_VERBOSE": "1", "RENPY_HEADLESS_FIRST_PICTURE": str(raw)})
        match = re.search(r"^first picture: (\d+)x(\d+)$", output, re.M)
        self.assertIsNotNone(match, output[-3000:])
        w, h = int(match[1]), int(match[2])
        data = raw.read_bytes()
        self.assertEqual(len(data), w * h * 4)
        return output, w, h, lambda x, y: tuple(data[(y * w + x) * 4 + i] for i in (2, 1, 0))

    def assert_presplash_jpg_shown(self, output, w, h, pixel):
        self.assertIn("the presplash, 400x250, is the picture (1280x720)", output)
        self.assertEqual((w, h), (1280, 720))
        left, top = (w - 400) // 2, (h - 250) // 2
        for (qx, qy), colour in self.PRESPLASH_QUARTERS.items():
            got = pixel(left + 100 + 200 * qx, top + 62 + 125 * qy)
            self.assertLessEqual(max(abs(a - b) for a, b in zip(got, colour)), 16, ((qx, qy), got, colour))
        self.assertEqual(pixel(10, 10), (0, 0, 0))

    def test_a_jpg_presplash(self):
        # The tester's presplash was a JPG. Ren'Py takes presplash.jpg as well as .png; on Windows, where games are
        # made, in any case (Presplash.JPG), which the console's file system tells apart.
        for name in ("presplash.jpg", "Presplash.JPG"):
            with self.case_sensitive_folder() as work:
                game = make_game(work / "Jpg")
                shutil.copyfile(self.PRESPLASH_JPG, game / "game" / name)
                self.assert_presplash_jpg_shown(*self.first_picture(game, work))

    def test_a_jpg_presplash_progress_bar(self):
        # presplash_foreground and presplash_background (here a JPG, its name in another case): the background first
        with self.case_sensitive_folder() as work:
            game = make_game(work / "Bar")
            write_png(game / "game/presplash_foreground.png", 400, 250)
            shutil.copyfile(self.PRESPLASH_JPG, game / "game/Presplash_Background.JPG")
            self.assert_presplash_jpg_shown(*self.first_picture(game, work))

    def test_no_presplash(self):
        # a game without one starts as before: its own picture first
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output, w, h, pixel = self.first_picture(make_game(work / "None"), work)
            self.assertNotIn("presplash", output)
            self.assertEqual((w, h), (1280, 720))

    def test_a_presplash_that_cannot_be_read(self):
        # a damaged presplash is left out, and the game starts
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Damaged")
            (game / "game/presplash.jpg").write_bytes(b"\xff\xd8\xff\xe0 not a picture " * 64)
            output = self.run_core("start", game, work / "data", env={"RENPY_HEADLESS_VERBOSE": "1"})
            self.assertIn("DULCAMARA-TEST: started", output)
            self.assertRegex(output, r"the presplash could not be read, and the game starts without it: ")
            self.assertNotIn("is the picture", output)

    def test_fullscreen_stays_off(self):
        # A game's Fullscreen made Ren'Py's window the display's size and the console's screen black: it reads as off,
        # the game's own value kept, and the picture stays the game's size.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("play", make_game(work / "Fullscreen"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_TEST_FULLSCREEN": "1"})
            self.assertIn("the game asked for fullscreen: its picture stays its own size", output)
            self.assertIn("DULCAMARA-TEST: fullscreen=False", output)
            self.assertIn("picture: 1280x720", output)

    def test_the_memory_line(self):
        # Every few minutes (DULCAMARA_MEMORY_EVERY seconds; here, at each of Ren'Py's periodic calls) the log says what
        # the game holds; the heap and ps5-opengl's memory only on the console.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("play", make_game(work / "Memory"), work / "data",
                                   env={"RENPY_HEADLESS_VERBOSE": "1", "DULCAMARA_MEMORY_EVERY": "0"})
            self.assertRegex(output, r"memory: Ren'Py's textures \d+ \(\d+ MB\), its image cache \d+ MB")

    def test_a_game_at_44100_hz(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            self.play_and_check(make_game(work / "Slow sound", sound_rate=44100), work / "data")

    def test_a_press_of_the_pointer_clicks(self):
        # The touchpad is Ren'Py's mouse: the frontend reports a tap as a press of the pointer.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("click", make_game(work / "Click"), work / "data")
            self.assertIn("pointer: the dialogue moved on after a press", output)

    def test_fast_forward_skips_the_dialogue(self):
        # While the frontend fast-forwards, the core holds Ren'Py's skip (its Ctrl key).
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("skip", make_game(work / "Skip"), work / "data", env={"DULCAMARA_TEST_SKIP": "1"})
            self.assertIn("fast-forward: the dialogue went by without Cross", output)
            self.assertIn("DULCAMARA-TEST: choice=", output)

    def test_a_game_that_catches_its_quit(self):
        # A game whose own handler catches the quit the core raises in Ren'Py's thread: three seconds on, the core logs
        # where Ren'Py is and raises it again, and Ren'Py stops.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("start", make_game(work / "Holds on"), work / "data",
                                   env={"DULCAMARA_TEST_HOLD": "1"})
            self.assertIn("DULCAMARA-TEST: held on to a quit", output)
            self.assertIn("Ren'Py has not stopped; its thread is at:", output)

    def test_a_game_closed_with_its_thread_running(self):
        # A surviving game-owned Python thread can still write native files.
        # A returned main script is not a clean shutdown acknowledgement.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game = make_game(work / "Threaded")
            (game / "game/blocked-worker.rpy").write_text(
                "init python:\n"
                "    import threading as native_threads\n"
                "    import os as native_os\n"
                "    retained_pipe = native_os.pipe()\n"
                "    def native_writer(read=native_os.read, fd=retained_pipe[0]):\n"
                "        read(fd, 1)\n"
                "    retained_worker = native_threads.Thread(target=native_writer)\n"
                "    retained_worker.daemon = True\n"
                "    retained_worker.start()\n")
            result = subprocess.run([str(self.BUILD / "core-check/renpy-headless"),
                str(self.BUILD / "assets"), "start", str(game), str(work / "data")],
                capture_output=True, text=True, timeout=30,
                env={**os.environ, "DULCAMARA_RUNTIME_PROBE": "1",
                     "DULCAMARA_RUNTIME_THREADS_UNSAFE": "1"})
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("runtime: timeout retained unsafe writers", result.stdout)

    def test_an_error_opens_no_editor(self):
        # Ren'Py opens traceback.txt in an editor for an error that escapes it (a process of its own, which the console
        # cannot start): the core's error log tells it not to. RENPY_EDIT_PY's stand-in editor shows whether it would.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            probe = work / "probe.edit.py"
            probe.write_text(PROBE_EDITOR, encoding="utf-8")
            opened = work / "probe.edit.py.opened"
            env = {"RENPY_EDIT_PY": str(probe)}
            self.run_core("start", make_game(work / "Editor"), work / "data1",
                          env={**env, "DULCAMARA_TEST_REPORT": "editor"})
            self.assertTrue(opened.exists(), "the stand-in editor sees Ren'Py open an editor")
            opened.unlink()
            output = self.run_core("start", make_game(work / "Reported"), work / "data2",
                                   env={**env, "DULCAMARA_TEST_REPORT": "error"})
            self.assertIn("Ren'Py reported an error", output)
            self.assertFalse(opened.exists(), "Ren'Py opened an editor for the error")
            # and kept beside dulcamara.log, for a tester to send
            saved = (work / "data2" / "logs" / "renpy-errors.txt").read_text(encoding="utf-8")
            self.assertTrue(saved.startswith("-- Reported\n"), saved[:200])
            self.assertIn("a test error, reported", saved)

    def test_fast_forward_skips_unread_text_when_set(self):
        # Settings > Ren'Py's "skip unread text too": the game's own preference (here, the default: unread text stops
        # the skip) reads as on.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("skip", make_game(work / "Skip all"), work / "data",
                                   env={"RENPY_HEADLESS_SKIP_UNSEEN": "on"})
            self.assertIn("fast-forward: the dialogue went by without Cross", output)

    def test_a_games_own_key_on_l2(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.run_core("keys", game, data, env={"renpy_key_l2": "return"})
            self.assertIn("keys: the dialogue moved on after L2 (Return)", output)

    def test_the_cursor(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            game, data = make_game(work / "Test Game"), work / "data"
            output = self.run_core("cursor", game, data)
            self.assertIn("cursor: drawn on the picture", output)
            self.assertIn("cursor: the dialogue moved on after a click with Cross", output)

    def test_a_name_typed(self):
        # A game asks for a name (renpy.input): the core tells the frontend it wants text and where its input is (here
        # at the foot of the picture, so the on-screen keyboard goes at the top), takes what the keyboard types through
        # its keyboard callback (characters, Backspace, Return), and tells it once Ren'Py has its text. Typing then
        # goes nowhere, and the controller is the game's again.
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("text", make_game(work / "Name"), work / "data", env={"DULCAMARA_TEST_INPUT": "1"})
            self.assertRegex(output, r"text: the core asked for text, its input ([5-9]\d|100)-\d+ % down the picture")
            self.assertRegex(output, r"Ren'Py asks for text, its input \d+-\d+ % down the picture: the frontend's "
                                     r"keyboard opens")
            self.assertIn("DULCAMARA-TEST: name=Ada", output)
            self.assertIn("text: the game got the name typed (Ada)", output)
            self.assertIn("Ren'Py has its text: the frontend's keyboard closes", output)
            self.assertIn("text: a Return typed after the input ended went nowhere", output)
            self.assertIn("text: Cross moved the dialogue on afterwards", output)

    def test_a_game_with_an_error(self):
        with tempfile.TemporaryDirectory() as directory:
            work = Path(directory)
            output = self.run_core("error", make_game(work / "Broken", broken=True), work / "data")
            self.assertIn("Ren'Py reported an error", output)
            self.assertIn("broken.rpy", output)


class PS5LibcTest(unittest.TestCase):
    """The cores' gmtime_r, localtime_r and mbstowcs on the PS5 (src/core/renpy/ps5_time.h, ps5_utf8.h): the host
    build uses the build machine's own, so tests/ps5_libc_check.c checks the console's against them. Dulcamara 0.2.3
    crashed on a PS5 as Ren'Py opened its log: its gmtime_r left tm_zone NULL, which time.gmtime() hands to strlen."""

    def test_times_with_their_zones_and_utf8_text(self):
        cc = os.environ.get("CC", "clang")
        if not shutil.which(cc):
            self.skipTest(f"needs {cc}")
        with tempfile.TemporaryDirectory() as directory:
            executable = Path(directory) / "ps5-libc-check"
            subprocess.run([cc, "-std=c11", "-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                            "-fno-omit-frame-pointer", "-Wall", "-Wextra", "-Wpedantic", "-Werror", "-pthread",
                            "-I" + str(ROOT / "src"), str(ROOT / "tests/ps5_libc_check.c"), "-o", str(executable)],
                           check=True)
            result = subprocess.run([str(executable)], capture_output=True, text=True, timeout=120, check=False)
        self.assertEqual(result.returncode, 0, result.stdout[-3000:] + result.stderr[-3000:])
        self.assertTrue(result.stdout.startswith("ok "), result.stdout)
        print(f"the cores' gmtime_r, localtime_r and mbstowcs: {result.stdout.split()[-1]} checks passed")


if __name__ == "__main__":
    unittest.main()
