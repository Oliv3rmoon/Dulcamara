# Eclipse interface artwork

These production skin components were generated with the built-in OpenAI ImageGen tool on 2026-11-08 for Andrew's request to create every part of Dulcamara's interface with ImageGen and animate it. The existing project license applies to this source artwork. The original outputs are retained separately; these files are byte-for-byte selected outputs, not screenshots of an implemented interface.

`imagegen-prompts.json` records initial component and cleanup prompts. `imagegen-final-atlas-prompts.json` records the selected clean opaque atlas prompts. `asset-metadata.json` records dimensions, hashes, and measured alpha bounds. The background plate and its prompt are in `../../backgrounds/`.

| File | Purpose | Selected ImageGen output |
| --- | --- | --- |
| panel.png | Settings, details, profiles, backups, filters, keyboard, About, and confirmation surfaces | exec-214b9300-624c-4381-8cc1-63445e5e31d9.png |
| cover-frame.png | Shelf selection and cover focus | exec-73787fab-3b16-45f2-9a13-a21c9f6542de.png |
| button.png | Actions, rows, keyboard keys, and selection surfaces | exec-defdd81c-32eb-4c89-8b8b-f9a2de76c0f2.png |
| controller-atlas.png | Controller hints, 4 columns by 3 rows | exec-2140bad6-e06d-4d8e-a3f8-e23e6e2d8d36.png |
| icon-atlas.png | Semantic action icons, 4 columns by 4 rows | exec-ff87178e-aadd-4af3-8d10-f24e48630225.png |
| chrome-strip.png | Header, footer, progress tracks, and separators | exec-d67ce4d9-2b72-46e4-8829-ea8d55a41af0.png |
| loading-ring.png | Scan, load, save, and maintenance progress indicator | exec-4b772164-53b5-45e5-b21d-35152649218a.png |
| placeholder-cover.png | Missing cover and empty-library artwork | exec-1cbaf7c7-8888-4cf3-b634-f1b392e06993.png |

Controller atlas order, left to right then top to bottom: Cross, Circle, Square, Triangle; L1, L2, R1, R2; Options, Touchpad, D-pad, Back. Semantic atlas order: Search, Favorite, Recent, Filter; Settings, Picture, Volume, Controller; Location, Backup, Restore, Play; Info, Warning, Delete, Download. Compute cell edges from the image dimensions using integer fractions; the semantic atlas width is not divisible by four.

Use the original art as cached source textures and choose source rectangles/nine-slice insets around the useful content. The strip has substantial transparent padding: its strong-alpha content is approximately `(27, 298)` to `(2145, 424)`. The square button has a broad plain center suitable for slicing. Alpha bounds are a starting point; verify final rendered corners and tiny controller glyphs. Opaque atlas backgrounds are black, chosen to avoid edge haze in the rejected transparent variants.

Keep text, game artwork, status values, keyboard input, accessibility preferences, and hit targets live in code. Reuse the generated skin across every screen, including errors and confirmations. Use bounded, time-based focus/press/panel/progress animations plus the ASCII background motion. Cache decode/upload, avoid per-frame allocations or image uploads, pause background work during gameplay, dim it behind panels, and provide a persisted still mode. Verify real application captures and matched animation-on/off frame costs; these source images alone do not verify the finished UI.
