# Changelog

What changed in each release of Nightshade Station, newest first. A release's notes on GitHub are in
`docs/releases/<version>.md`. What changed in SwanStationPS5 before Nightshade Station is in
[its releases](https://github.com/darkxex/SwanStationPS5/releases).

## Nightshade Station 1.6.0: the first Nightshade Station, based on SwanStationPS5 1.5.9

Nightshade Station is Oliv3rmoon's release of SwanStationPS5 by darkxex, built on SwanStation:
SwanStationPS5 1.5.9 with a new name, Nightshade's look and updates from
Oliv3rmoon/Nightshade-Station. Its version, 1.6.0 (contentVersion 01.006.000), is the next minor
after 1.5.9 (01.005.009), so it installs over it as an update. What changed:

- **The name.** The PS5 home screen, the shelf's header, the About page, the update texts, the
  notifications and every message, tip and help say Nightshade Station, in all five languages, and
  so does the phone page. Under the About page's QR code, which opens this repository, are its
  address, the nightly Discord and "Nightshade Station is Oliv3rmoon's release of SwanStationPS5 by
  darkxex, built on SwanStation." "Created by" still says AngelXex.
- **The look.** A seventh theme, Nightshade, in Nightshade's palette (its "nightly" theme):
  near-black pages and panels, one red accent (#e5484d) for the focus, the switches and the selected
  cover, cool white text, an amber for warnings, and a near-black sky with a low red glow behind the
  shelf. It is the default for a new install and after Factory settings; Classic, Neon Arcade,
  Memory Card, Memory Card Dark, Record Shelf and Grayrat are still there. The phone page has the
  same colours.
- **The art.** The icon (a black disc lit red, the nightshade flower at its hub), the home
  background (the disc risen like an eclipsed moon, the nightshade climbing beside it) and the launch
  picture (a disc-shaped ring of red light round the flower) are drawn by
  `tools/render-home-art.py`, in Nightshade's style.
- **Updates from this repository.** The app asks
  [Oliv3rmoon/Nightshade-Station](https://github.com/Oliv3rmoon/Nightshade-Station/releases) for its
  newest release that is not a pre-release, and offers it when its tag is a newer version and it
  carries `PPSA98510.zip` and `SHA256SUMS`, both this repository's own files; the download is
  checked against `SHA256SUMS` before anything is replaced. darkxex's releases are never offered.
- **Release builds.** A version tag pushed to Oliv3rmoon/Nightshade-Station builds
  `PPSA98510.zip` and `SwanStationPS5-DiscDaemon.elf` and publishes them with `SHA256SUMS` as the
  latest release, with a build attestation.
- **What stays.** The title ID (PPSA98510) and the data folder (`/data/SwanStationPS5`), so it
  installs over SwanStationPS5 and keeps your memory cards, saves, save states, covers and settings.
  The disc daemon keeps its file name. Everything else is SwanStationPS5 1.5.9.

Built and checked by the host tests; not yet run on a console.

### Install

Close SwanStationPS5, copy this release's `PPSA98510` folder (from `PPSA98510.zip`) over your
current one (usually `/data/homebrew/PPSA98510`), and start it. SwanStationPS5 doesn't offer this
release itself, so this one is copied by hand (or installed by Nightshade's Settings > Apps); from it
on, the update check applies the next one. To go back, copy an official
[SwanStationPS5 release](https://github.com/darkxex/SwanStationPS5/releases) over it the same way.
