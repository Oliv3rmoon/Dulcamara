# Security policy

## Supported versions

Security fixes go into the newest release on the [Releases](https://github.com/Oliv3rmoon/Dulcamara/releases) page. Older releases are not patched; update to get a fix.

## Reporting a vulnerability

Please report it privately: open the **Security** tab of this repository and choose **Report a vulnerability**, or go straight to [the private report form](https://github.com/Oliv3rmoon/Dulcamara/security/advisories/new). Please do not open a public issue for something that could put other users at risk.

A useful report says which Dulcamara version you ran, what happened, how to make it happen again, and includes the logs if you have them.

Dulcamara is maintained by one person. I read every report and aim to answer within a week. A confirmed problem is fixed in the next release, and you are credited in its notes unless you ask not to be.

## In scope

- Bugs in Dulcamara's own code that a crafted file can trigger: a game folder or archive, save data, an update or anything else the app opens.
- The updater and the release pipeline: anything that could make the app install something other than an official release. Each release ships a `SHA256SUMS` file, and the app checks a download against it before installing.
- The app exposing data from the console that it should not, for example through its logs or its network features.

## Out of scope

- The console's firmware, and the methods used to run homebrew on a console. This project does not develop or distribute them.
- Bugs in the bundled engines (EasyRPG Player, Ren'Py and their libraries) that Dulcamara does not change. Report those to the engine's own project too; tell me as well if a Dulcamara release is affected.
- Games, and requests for game files.
