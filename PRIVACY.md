# Privacy policy

Dulcamara is free, open-source software (GPL-3.0-or-later) with no ads, no accounts, no analytics and no
telemetry. The developer receives no data from your console, except what nightly's insider service gets when you use
insider builds (below). This policy lists everything the app stores and every connection it can make.

Last updated: 2026-11-06.

## What stays on your console

Everything lives under `/data/dulcamara/` (and the app under `/data/homebrew/PPSA99010/`):

- `settings.ini`: the settings; `played.txt`: when each game was last played.
- `easyrpg-player/`: EasyRPG Player's own settings (its OPTIONS menu).
- `logs/dulcamara.log`: a diagnostic log.
- `insider-code`: your insider code, if you typed one in **Settings > System > Insider code** (see below).
- The games you copied, and their saves, which the games write in their own folders.

None of this is sent anywhere by the app. You can delete it at any time with an FTP client.

## Connections the app makes

| To | When | What is sent | Why |
| --- | --- | --- | --- |
| api.github.com and github.com | If "Check for updates at start" is on, or when you check from Settings | A request for the latest release of `Oliv3rmoon/Dulcamara`, then its `SHA256SUMS`, with the User-Agent `Dulcamara/<version>` | The update check; installing an update downloads that release's `PPSA99010.zip` from GitHub and checks it against `SHA256SUMS` |
| nightly's insider service (only in builds made with its address) | When the update check runs with **Test builds** on and an insider code stored | Your insider code and Dulcamara's version, with the User-Agent `Dulcamara/<version>` | Insider builds for nightly's members: when it offers a newer test build, installing it downloads that build from the same service and checks its SHA-256 and size |

These servers receive the usual network metadata (your IP address, and the User-Agent above) and are governed by
their own privacy policies. Nothing else is sent and no game is downloaded. The only server run by the developer the
app ever talks to is nightly's insider service, and only as the table says: without a code stored it is never asked.
The insider code is a code the service gave you when you signed in on its page with Discord; it says which Discord
member you are to that service alone, and it is never written to the log.

## Children

The app collects no personal data and has no social features.

## Contact

Open an issue at <https://github.com/Oliv3rmoon/Dulcamara/issues>.
