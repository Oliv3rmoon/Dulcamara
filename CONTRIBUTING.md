# Contributing

Bug reports, fixes and testing of Dulcamara are all welcome. Ask for help on the
[nightly Discord](https://discord.gg/pSqgRU3YP9).

**Reporting a bug**

- Include your firmware, how you loaded etaHEN and kstuff, and the game (its title, and RPG Maker 2000 or 2003).
- Attach `/data/dulcamara/logs/dulcamara.log`. After a crash, the log has a `crash:` section with
  addresses.
- Never attach games, their files or the RTP.

**Changing the code**

- Every push and pull request is built by GitHub Actions (`.github/workflows/build.yml`), host checks included. A
  change should build there; `make host-checks` runs the checks on a computer.
- Keep the SPDX header (`GPL-3.0-or-later`) on new source files.
- Never commit games, test games, the RTP, keys, `.env`, `build/` or `dist/`.
- Release tags are `MAJOR.MINOR.PATCH` (`0.1.0`), matching `NP_VERSION` in `src/nightshade.h` and the
  `contentVersion` of `sce_sys/param.json` (`00.001.000`). A tag pushed to Oliv3rmoon/Dulcamara builds and
  publishes the release (`PPSA99010.zip` and `SHA256SUMS`).
