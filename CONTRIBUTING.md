# Contributing

Bug reports, fixes and testing of Nightshade Station are all welcome. Ask for help on the
[nightly Discord](https://discord.gg/pSqgRU3YP9).

**Reporting a bug**

- Include your firmware, how you loaded etaHEN and kstuff, and the game (its name and serial, for example `SLUS-00594`).
- Attach `/data/SwanStationPS5/logs/SwanStationPS5.log`. After a crash, the log has a `crash:` section with addresses. Those map to functions with the `SwanStationPS5-symbols` artifact of the same build.

**Changing the code**

- Every pull request is built by GitHub Actions (`.github/workflows/build.yml`). A change should build there.
- Keep the SPDX header (`GPL-3.0-or-later`) on new source files.
- Never commit BIOS files, games, keys, `.env`, `build/` or `dist/`.
- Release tags are `MAJOR.MINOR.PATCH` (`1.6.0`), matching `SwanStationPS5_VERSION` in `src/SwanStationPS5.h` and the
  `contentVersion` of `sce_sys/param.json` (`01.006.000`). A tag pushed to Oliv3rmoon/Nightshade-Station builds
  and publishes the release (`PPSA98510.zip`, `SwanStationPS5-DiscDaemon.elf` and `SHA256SUMS`).
