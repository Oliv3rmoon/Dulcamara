# Ren'Py core, milestone 3: Ren'Py in the app

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:executing-plans. Steps use checkbox (`- [ ]`) syntax.

**Goal:** Dulcamara finds Ren'Py games, runs them on milestone 2's core with the PS5's controls, keeps their saves,
and says what it ships. **Spec:** `docs/superpowers/specs/2026-10-29-renpy-core-design.md` (milestone 3). Milestone
1's Global Constraints hold. Written as it was carried out; the rulings are in the ledger
(`.superpowers/sdd/2026-10-29-renpy-core/progress.md`, lines starting `M3:`).

## Tasks

- [x] **M3.1 Library.** `src/library.c` recognises a folder with `game/` holding scripts or archives as a Ren'Py game:
  its version from `renpy/vc_version.py` or `renpy/__init__.py`, its title from its launcher's name, its cover from
  `game/gui/main_menu.png` or `window_icon.png`. Check: `tests/library_check.c` (`check_renpy_games`).
- [x] **M3.2 Core table and frontend.** `CORE_RENPY` in `src/core/cores.c` with Ren'Py's buttons (Cross confirm,
  Circle game menu, L1/R1 roll back and forward); `src/core/host.c` answers the core assets folder, turns the
  touchpad into the pointer (a tap is a click) and keeps each core's own button map (`settings.ini`'s keys:
  EasyRPG's `button_<n>` as before, `<core>_button_<n>` for the others).
- [x] **M3.3 One Ren'Py game a process.** A second Ren'Py game restarts the app (`src/main.c`: `restart-game.txt`,
  `sceSystemServiceLoadExec`), which opens it at start.
- [x] **M3.4 Saves and skip.** Saves in `<data>/saves/renpy/<config.save_directory>`; R2 (fast-forward) holds
  Ren'Py's skip, pressed again every 30 frames. Checks: `tests/test_renpy_core.py` (save and load, skip).
- [x] **M3.5 Settings, notices, packaging.** Settings > About and Controls name Ren'Py; `THIRD_PARTY_NOTICES.md`,
  `docs/THIRD-PARTY.txt` and the licences in the package; `tools/build.sh` packs `assets/renpy8`; CI checks the
  ZIP's engine and licences and that no game files are packed.
- [x] **M3.6 CI.** The PS5 job builds the core, links the app with it and runs the host checks. Evidence: CI run
  41043194846 (green, 4630e49).
