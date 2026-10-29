# Privacy policy

Nightshade Player is free, open-source software (GPL-3.0-or-later) with no ads, no accounts, no analytics and no
telemetry. The developer receives no data from your console. This policy lists everything the app stores and every
connection it can make.

Last updated: 2026-10-29.

## What stays on your console

Everything lives under `/data/nightshade-player/` (and the app under `/data/homebrew/PPSA99010/`):

- `settings.ini`: the settings; `played.txt`: when each game was last played.
- `easyrpg-player/`: EasyRPG Player's own settings (its OPTIONS menu).
- `logs/nightshade-player.log`: a diagnostic log.
- The games you copied, and their saves, which the games write in their own folders.

None of this is sent anywhere by the app. You can delete it at any time with an FTP client.

## Connections the app makes

| To | When | What is sent | Why |
| --- | --- | --- | --- |
| api.github.com and github.com | If "Check for updates at start" is on, or when you check from Settings | A request for the latest release of `Oliv3rmoon/Nightshade-Player`, then its `SHA256SUMS`, with the User-Agent `NightshadePlayer/<version>` | The update check; installing an update downloads that release's `PPSA99010.zip` from GitHub and checks it against `SHA256SUMS` |

These servers receive the usual network metadata (your IP address, and the User-Agent above) and are governed by
their own privacy policies. Nothing else is sent, no game is downloaded, and the app talks to no server run by the
developer.

## Children

The app collects no personal data and has no social features.

## Contact

Open an issue at <https://github.com/Oliv3rmoon/Nightshade-Player/issues>.
