# Ren'Py core, milestone 4: Ren'Py 7

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** games made with Ren'Py 7 and 6 run on a Ren'Py 7 core with Python 2.7, beside Ren'Py 8's. **Spec:**
`docs/superpowers/specs/2026-10-29-renpy-core-design.md` (milestone 4). Milestone 1's Global Constraints hold.
Written as it was carried out; the rulings are in the ledger (lines starting `M4:`).

## Tasks

- [x] **M4.1 Sources.** Submodules `third_party/renpy7` (7.8.7, `b38cc6a5`) and `third_party/pygame_sdl2`
  (`7e61ebdb`); CPython 2.7.18 pinned by SHA-256 with renpy-build's two UTF-8 patches and
  `tools/patches/python-2.7.18-configure.diff`; `tools/renpy7/Setup.local` and `wheels.txt`.
- [x] **M4.2 Host build.** `tools/build-renpy7.sh host` (sharing `tools/renpy/common.sh`): Python 2.7, Cython
  0.29.32's C for pygame_sdl2's and Ren'Py's 63 modules, the engine `assets/renpy7`, the core from the same
  `src/core/renpy` with `python2.c`, `boot2.py`, `errors2.py`, and Ren'Py's common scripts compiled.
- [x] **M4.3 Tests.** `tests/test_renpy7_core.py` runs `tests/test_renpy_core.py`'s tests on it.
- [x] **M4.4 PS5 build.** `tools/build-renpy7.sh ps5` (`tools/renpy7/ps5.sh`): Python 2.7 for a FreeBSD host,
  the modules, `build/renpy7-ps5/librenpy7.a` with the API renamed `renpy7_retro_*`, reusing Ren'Py 8's PS5 build's
  SDK, PacBrew, FFmpeg and ps5-opengl; every symbol it leaves is one the app's link gives.
- [x] **M4.5 App.** `CORE_RENPY7`; the library sends Ren'Py 7 and 6 games to it (by `renpy/`, else `lib/`);
  `assets/renpy7` and its licences packed; CI builds and checks both cores.
