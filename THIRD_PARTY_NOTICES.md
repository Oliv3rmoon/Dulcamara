# Third-party notices

## Credits and acknowledgements

| Project | Role |
| --- | --- |
| [EasyRPG Player](https://github.com/EasyRPG/Player) and [liblcf](https://github.com/EasyRPG/liblcf), by the [EasyRPG](https://easyrpg.org) team | The core that runs the RPG Maker 2000 / 2003 games, and their file formats |
| [Oliv3rmoon/Mandrake](https://github.com/Oliv3rmoon/Mandrake), Mandrake (formerly Nightshade Station) | The frontend this app is built from |
| [darkxex/SwanStationPS5](https://github.com/darkxex/SwanStationPS5) | Mandrake's origin: its platform layer, Vulkan screen, shaders and interface |
| [BlackBearReloaded's ps5-native-app-boilerplate](https://github.com/blackbearreloaded) | The PS5 native app build, runtime, tooling and update check under all of them |
| [ps5-payload-dev/sdk](https://github.com/ps5-payload-dev/sdk) | Public PS5 headers, libc++ headers, sysroot, and Clang target support |
| [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo) | Optional prebuilt PS5 ports and static libraries |
| [SvenGDK/SharpProspero](https://github.com/SvenGDK/SharpProspero) | Source of the ELF converter and FSELF writer in `tooling/native/` (GPL-3.0) |
| [SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) | Optional UFS2 `.ffpkg` generation |
| [PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) | Optional compressed `.ffpfsc` generation |
| [sinajet/PSFFPKG](https://github.com/sinajet/PSFFPKG) | Public `.ffpkg` procedure used as a format reference |
| [LLVM/Clang](https://github.com/llvm/llvm-project) | Native compiler |
| [GoogleTest](https://github.com/google/googletest) | Pinned host-only C++ unit-test framework |
| [zlib](https://zlib.net/) | Pinned source-built compression library used by the host FSELF tool |
| [Microsoft DirectXTex](https://github.com/microsoft/DirectXTex) | `texconv` presentation-image preparation |
| [FFmpeg](https://ffmpeg.org/) | Developer-supplied selection-audio preparation |
| [ShadowMountPlus](https://github.com/drakmor/ShadowMountPlus) | Directory-style deployment and hardware validation |
| [ArkSama/PS5-Lapy-JB-Daemon](https://github.com/ArkSama/PS5-Lapy-JB-Daemon) | Original Lapy project and owned-root design |
| [mpereiraesaa/PS5-Lapy-JB-Daemon](https://github.com/mpereiraesaa/PS5-Lapy-JB-Daemon) | Exact-title one-shot helper and cooperative elevation protocol |

## Cores shipped

Dulcamara ships **two** cores:

| Core | Upstream | License | Notes |
| --- | --- | --- | --- |
| EasyRPG Player (libretro) | [EasyRPG/Player](https://github.com/EasyRPG/Player) at `096c004def987f3545198304b1b7a8352f284a50`, with [EasyRPG/liblcf](https://github.com/EasyRPG/liblcf) at `6854310c3432e553fd4ae672ce861899c80c3bd0` | GPL-3.0-or-later (`third_party/easyrpg-player/COPYING`); liblcf MIT (`third_party/liblcf/COPYING`) | RPG Maker 2000 and 2003. Built from source into the app by `tools/build-easyrpg.sh` |
| Ren'Py 8 (Dulcamara's own libretro core, `src/core/renpy`) | [renpy/renpy](https://github.com/renpy/renpy) 8.5.3.26051504 at `39895c1e017f0b36ffea2447d97eccd69d76ee1c` (`third_party/renpy`) | Ren'Py: MIT, with LGPL-derived parts (`third_party/renpy/sphinx/source/license.rst`); the core's own code GPL-3.0-or-later | Ren'Py 8 visual novels. Built from source by `tools/build-renpy.sh`; its engine (Ren'Py's Python files, scripts and fonts) is packed as `assets/renpy8` |

EasyRPG Player carries, built in: [FMMidi](http://unhaut.epizy.com/fmmidi) by yuno (Yoshio Uno),
BSD-3-Clause; [dr_wav](https://github.com/mackron/dr_libs) by David Reid, public domain or MIT-0; the
Shinonome (public domain), Baekmuk (Baekmuk License), ttyp0 (ttyp0 license) and WenQuanYi
(GPL-2.0-or-later with font exception) fonts; the EasyRPG logos (CC-BY-SA 4.0); and
[libretro-common](https://github.com/libretro/libretro-common)'s `rthreads` (its permissive MIT-style
licence, in each file). Its authors are listed in `third_party/easyrpg-player/docs/AUTHORS.md`, which the app
folder carries as `licenses/AUTHORS-EasyRPG-Player.md`.

liblcf is patched by `tools/patches/liblcf-iconv.patch` (MIT, as liblcf): with ICU off, its text encodings go
through iconv, and a game's encoding is guessed from its bytes.

Built with the core, from pinned sources:

| Component | Source | License |
| --- | --- | --- |
| [inih](https://github.com/benhoyt/inih) r62 | `third_party/inih` (submodule) | BSD-3-Clause (`third_party/inih/LICENSE.txt`) |
| [pixman](https://pixman.org) 0.46.4 | the release tarball, SHA-256 `d09c44ebc3bd5bee7021c79f922fe8fb2fb57f7320f55e97ff9914d2346a591c`, downloaded by `tools/build-easyrpg.sh` | MIT (its `COPYING`, in the app folder as `licenses/LICENSE-pixman.txt`) |

From the PacBrew prefix (below), linked into the core: fmt (MIT), libpng (libpng license), zlib (zlib),
FreeType (FreeType License), HarfBuzz (MIT), libogg and libvorbis (BSD-3-Clause), mpg123 (LGPL-2.1),
libsndfile (LGPL-2.1-or-later), FLAC (BSD-3-Clause), Opus (BSD-3-Clause), LAME (LGPL-2.0-or-later), bzip2
(bzip2 license), libsamplerate (BSD-2-Clause) and GNU libiconv (LGPL-2.1-or-later), which the frontend links
too. The LGPL libraries are linked statically into a GPL-3.0-or-later program whose whole source is published.

The core is free, and Dulcamara has no paid features, ads or in-app purchases.

Ren'Py's core is built with, from pinned sources (the tarballs are checked against their SHA-256 by
`tools/build-renpy.sh`):

| Component | Source | License |
| --- | --- | --- |
| [CPython](https://www.python.org) 3.12.8 | `Python-3.12.8.tar.xz`, SHA-256 `c909157bb25ec114e5869124cc2a9c4a4d4c1e957ca4ff553f1edc692101154e`; for the PS5 with `tools/patches/python-3.12.8-cross-freebsd.diff` | PSF License (`licenses/LICENSE-Python.txt`); its standard library is packed as `assets/renpy8/lib/python312.zip` |
| [FFmpeg](https://ffmpeg.org) 4.3.1 | `ffmpeg-4.3.1.tar.gz`, SHA-256 `45035f15d6f192772de2309c846e1d60472694f479679354a39c699719e53772`, with Ren'Py's two patches (`tools/patches/ffmpeg-4.3.1-*.diff`) | LGPL-2.1-or-later (`licenses/LICENSE-FFmpeg.txt`). Configured with LGPL parts only (no `--enable-gpl`, no `--enable-nonfree`; the build checks `CONFIG_GPL 0`); decoders only, no network |
| [Assimp](https://github.com/assimp/assimp) 5.4.3 | GitHub's `v5.4.3` archive, SHA-256 `66dfbaee288f2bc43172440a55d0235dfc7bf885dda6435c038e8000e79582cb`; glTF and STL importers only | BSD-3-Clause (`licenses/LICENSE-Assimp.txt`) |
| [ps5-opengl](https://github.com/darkxex/ps5-opengl) 1.0.2 | its SDK release, SHA-256 `30d8e51dd062739b629346102be5932728ba714bc50b1bdcdf08306c8843d9a4` (the one Nightshade pins) | MIT, with Mesa's licences (`licenses/LICENSE-ps5-opengl.txt`, `licenses/NOTICES-ps5-opengl.md`) |
| Cython 3.1.4, setuptools 80.10.2 | PyPI wheels, by SHA-256 (`tools/renpy/wheels.txt`) | Apache-2.0, MIT: run on the build machine only, not shipped |
| six 1.17.0, ecdsa 0.19.1, requests 2.33.0, urllib3 2.7.0, idna 3.11, certifi 2026.1.4, charset-normalizer 3.4.4, rsa 4.9.1, pyasn1 0.6.2, future 1.0.0 | PyPI wheels at the versions of Ren'Py's `uv.lock`, by SHA-256 (`tools/renpy/wheels.txt`) | MIT, MIT, Apache-2.0, MIT, BSD-3-Clause, MPL-2.0, MIT, Apache-2.0, BSD-2-Clause, MIT; each package's licence travels in its `*.dist-info` folder under `assets/renpy8/lib/site-packages` |

From the PacBrew prefix (below), linked into Ren'Py's core: SDL2 and SDL2_image (zlib), FreeType (FreeType
License), HarfBuzz (MIT), GNU FriBidi (LGPL-2.1-or-later), libpng (libpng license), libjpeg-turbo (IJG and
BSD-3-Clause), libwebp (BSD-3-Clause), zlib (zlib), bzip2 (bzip2 license), xz (0BSD and public domain), OpenSSL
(Apache-2.0) and GNU libiconv (LGPL-2.1-or-later). Live2D's Cubism SDK, which Ren'Py can use, is not included.

### Test data, never shipped

The host checks (`tests/test_core.py`, `tests/test_library.py`) run the core on
[EasyRPG's TestGame](https://github.com/EasyRPG/TestGame), fetched by `tools/fetch-testgame.sh` at commit
`4bbedb73492c80b79d8290e0b06ea86875b0bdc8` into the ignored `.deps/testgame`. TestGame is GPL-3.0-or-later
(its `COPYING`), its graphics and sounds CC-BY-4.0, CC0 or public domain as its `AUTHORS.md` lists them. It is
not committed here and not part of the app or its release ZIP, which carries no game files of any kind.

## Native build dependencies

The application build uses LLVM/Clang/lld, zlib 1.3.2, and the public
[PS5 payload SDK](https://github.com/ps5-payload-dev/sdk). The bootstrapper
downloads SDK v0.42 after verifying SHA-256
`8cfbc7cd5811e719eb4f0c47eea668d3dc7b40bc8ab11c4a5031d40c23ec02da`.
It downloads zlib 1.3.2 from the upstream source archive after verifying
SHA-256 `bb329a0a2cd0274d05519d61c667c062e06990d72e125ee2dfa8de64f0119d16`
and compiles its static archive locally. Both dependencies remain under ignored
`.deps/native/`, retain their upstream licenses, and are not distributed by
this repository. No Sony SDK file is included.

Target C++ compilation uses the LLVM libc++ headers distributed by the public
SDK. Those headers retain the Apache-2.0 WITH LLVM-exception license recorded
upstream. The application does not redistribute or dynamically load the
complete libc++ or libc++abi archives.

The PS5 ELF converter and FSELF writer in `tooling/native/` are derived from
[SharpProspero](https://github.com/SvenGDK/SharpProspero), Copyright (C) 2026
SvenGDK, GPL-3.0, and were translated to C++ and modified by BlackBearReloaded.

## Host test dependency

The host unit-test target downloads
[GoogleTest](https://github.com/google/googletest) 1.17.0 after verifying
SHA-256 `65fab701d9829d38cb77c14acdc431d2108bfdbf8979e40eb8ae567edf10b27c`.
It remains under ignored `.deps/test/`, retains its BSD-3-Clause license, and
is not linked into any PS5 application, runtime, or package artifact.

## Optional PS5-Lapy-JB-Daemon integration

The sandbox-elevation build fetches
[blackbearreloaded's PS5-Lapy-JB-Daemon](https://github.com/blackbearreloaded/PS5-Lapy-JB-Daemon)
(a fork of mpereiraesaa's) at commit `57fcc19b7f57e46c823d6f2a6c6233ebf3288079`, invokes its unmodified
`owned-helper` target for the selected application's exact title and packages
the generated helper ELF with Lapy's MIT license. The shared protocol header
published upstream is LGPL-2.1-or-later; this repository's application-side
wire implementation is GPL-3.0-or-later.

Lapy was created by
[ArkSama / Team PHU](https://github.com/ArkSama/PS5-Lapy-JB-Daemon). Credit
belongs to ArkSama, mpereiraesaa and the Lapy contributors. No Lapy kernel
source is copied or modified here.

The helper build also uses the pinned `ps5log/1` header from
[mpereiraesaa/ps5-agc-gears](https://github.com/mpereiraesaa/ps5-agc-gears/tree/1ae1f9182abd2770c131b97419034fb85173c2dc/native/ps5log),
GPL-3.0-or-later, and the official PS5 Payload SDK v0.45. Those build inputs
remain under ignored `.deps/lapy/`; the normal application toolchain remains
the separately pinned Payload SDK v0.42.

## Optional PacBrew dependencies

When selected through `PACBREW_*` build variables, the build downloads the prebuilt ports image
from [ps5-payload-dev/pacbrew-repo](https://github.com/ps5-payload-dev/pacbrew-repo)
release `v0.40.2`, verifies its published SHA-256, and extracts only the
`target/user/homebrew` prefix under ignored `.deps/pacbrew/`. It does not
replace the pinned SDK or install files globally. PacBrew recipes and every
linked third-party library retain their upstream licenses; applications must
review those terms before redistribution.

The update-check example (`make update-check-example`) selects `libcurl`, which
statically links these PacBrew libraries into the example title. Its build
output stays under ignored `dist/` and is not distributed by this repository; an
application that ships the update check ships them and must carry their notices:

| Component | Version in PacBrew v0.40.2 | License |
| --- | --- | --- |
| [libcurl](https://curl.se/) | 8.18.0 | curl license (MIT/X derivative) |
| [OpenSSL](https://www.openssl.org/) | 3.5.2 | Apache License 2.0 |
| [zlib](https://zlib.net/) | 1.3.2 | zlib license |
| [zstd](https://github.com/facebook/zstd) | 1.5.6 | BSD-3-Clause (dual-licensed with GPL-2.0) |
| [libpsl](https://github.com/rockdaboot/libpsl) | 0.21.5 | MIT; built-in Public Suffix List data MPL-2.0 |

`examples/update-check/console_curl.c`, which makes these libraries run in a
native title, is original BlackBearReloaded code (GPL-3.0-or-later), taken from
the ProsperoRadio and ProsperoLichess projects. Its `gmtime_r` follows Howard
Hinnant's public-domain `civil_from_days` algorithm.

## Vendored miniz

`third_party/miniz/` holds [miniz](https://github.com/richgel999/miniz) 3.0.2
(commit `293d4db1b7d0ffee9756d035b9ac6f7431ef8492`), MIT, unmodified, with its
`LICENSE`; `SOURCE.json` records each file's SHA-256. Dulcamara links it
(`src/vendor.c`) to unpack updates and to read `.zip` / `.easyrpg` games and XYZ
pictures for the library; the self-update helper (`examples/self-update-helper`)
and its host test link it too. The
helper's archive validation and file helpers are original BlackBearReloaded
code (GPL-3.0-or-later), taken from the ProsperoStore project.

## Optional UFS2Tool dependency

When `.ffpkg` output is requested, the platform bootstrapper fetches
[SvenGDK/UFS2Tool](https://github.com/SvenGDK/UFS2Tool) at commit
`b5307a60d5b4e3a68ba680e0e33cfadf05017c77` into the ignored
`.deps/UFS2Tool` cache and builds it with the host .NET SDK. UFS2Tool is
BSD-2-Clause software and is not distributed by this repository.

## Optional MkPFS dependency

When `.ffpfsc` output is requested, the platform bootstrapper fetches
[PSBrew/MkPFS](https://github.com/PSBrew/MkPFS) at commit
`6cb8313dfe0c988ac52617794553f343243d3a56` into the ignored `.deps/MkPFS`
cache and installs its Python dependencies into an ignored virtual environment
there. MkPFS and its dependencies retain their own licenses and are not
distributed by this repository.

## Independently authored runtime shim

`tooling/native/libc_builder.cpp` and the manifests under
`tooling/native/runtime/` are independently authored for this project and
licensed under GPL-3.0-or-later. The generated `runtime/libc.prx` contains
project-authored compatibility stubs, startup code, and semantic loader
metadata. It contains no Sony runtime implementation.

Original ps5-native-app-boilerplate code is Copyright (C) 2026
BlackBearReloaded and licensed under GPL-3.0-or-later. Source and script files
carry matching SPDX identifiers.

## Original presentation assets

The BlackBear icon, selection artwork, and default selection track
`sce_sys/snd0.at9` are original assets supplied by BlackBearReloaded, Copyright
(C) 2026 BlackBearReloaded, and distributed under GPL-3.0-or-later. The track
is titled `Night Drive`.

Dulcamara's icon and home-screen pictures (`sce_sys/icon0.png`, `pic0.dds` and `pic1.dds`
with their source PNGs, `background-source.png` and `launch-background-source.png`) are drawn by
`tools/render-home-art.py`, after Mandrake's and Nightshade's (Oliv3rmoon/Nightshade), Copyright
(C) 2026 Oliv3rmoon, and distributed under GPL-3.0-or-later. The name in them is set in Inter (SIL Open Font
License 1.1).

No proprietary runtime module, encryption key, RTP or game file is included.

## From SwanStationPS5 and Mandrake

The frontend is Mandrake's (formerly Nightshade Station), which is [darkxex's SwanStationPS5](https://github.com/darkxex/SwanStationPS5)
(GPL-3.0-or-later, from [SynoPiia's PSXS5](https://github.com/SynoPiia/PSXS5)). What it brings, and still uses:

| Component | Use | License |
| --- | --- | --- |
| [stb_image, stb_truetype](https://github.com/nothings/stb) | title picture decoding, font rendering (`third_party/stb`) | Public domain / MIT |
| [Inter](https://github.com/rsms/inter) | interface font (`assets/fonts`) | SIL Open Font License 1.1 (`assets/fonts/OFL.txt`) |
| [SDL2](https://www.libsdl.org/) (PacBrew port) | events, controllers, audio, software renderer | zlib |
| [dlmalloc](https://gee.cs.oswego.edu/dl/html/malloc.html) 2.8.6 by Doug Lea | allocator for the direct-memory heap (`third_party/dlmalloc`) | Public domain (CC0) |
| [Noto Sans JP](https://github.com/notofonts/noto-cjk) (subset made for Mandrake) | Japanese text (`assets/fonts/NotoSansJP-subset.ttf`) | SIL Open Font License 1.1 (`assets/fonts/OFL-NotoSansJP.txt`) |
| [Tabler Icons](https://tabler.io/icons) 3.31.0 (subset by `tools/make_icon_font.py`) | interface icons (`assets/fonts/tabler-icons-subset.ttf`) | MIT (`assets/fonts/LICENSE-tabler-icons.txt`) |
| [lcd3x](https://github.com/libretro/slang-shaders) by Gigaherz | the LCD3x shader, ported to GLSL in `src/platform/vk/shaders/lcd3x.frag` | Public domain |
| [crt-royale](https://github.com/libretro/slang-shaders) (crt-royale-fast) by TroggleMonkey, adapted by Hyllian | the CRT Royale Fast shader, compiled to SPIR-V by `tools/make-royale.py` (`third_party/crt-royale`) | GPL-2.0-or-later (`third_party/crt-royale/LICENSE.TXT`) |
| [crt-royale](https://github.com/libretro/slang-shaders) by TroggleMonkey (the full preset, with his MIT-licensed blur and gamma includes) | the CRT Royale shader, compiled to SPIR-V by `tools/make-crt.py` (`third_party/crt-shaders`); the vertex stage of its two halation blurs was finished by hand, as the libretro copy is half converted from Cg | GPL-2.0-or-later (`third_party/crt-shaders/crt/shaders/crt-royale/LICENSE.TXT`), MIT (the includes) |
| [newpixie-crt](https://github.com/libretro/slang-shaders) by Mattias Gustavsson, adapted to slang by hunterk | the CRT Newpixie shader (`third_party/crt-shaders`) | MIT or Unlicense, as the shader's own header says |
| [crt-hyllian](https://github.com/libretro/slang-shaders) and its NTSC-Adaptive-Lite pass, by Hyllian (the latter based on Themaister's NTSC shader) | the CRT Hyllian NTSC shader (`third_party/crt-shaders`) | MIT (Hyllian's headers) |
| [crt-mattias](https://www.shadertoy.com/view/lsB3DV) by Mattias, adapted to slang in libretro's slang-shaders | the CRT Mattias shader (`third_party/crt-shaders`) | see the libretro/slang-shaders repository (license to be confirmed) |
| [crt-lottes](https://github.com/libretro/slang-shaders) by Timothy Lottes, as shipped with Apotris | the CRT Lottes shader, ported to Vulkan GLSL in `src/platform/vk/shaders/crt_lottes.frag` | Public domain |
| [crt-pi](https://github.com/libretro/glsl-shaders) by davej, as shipped with Apotris | the CRT Pi shader, ported to Vulkan GLSL in `src/platform/vk/shaders/crt_pi.frag` | GPL-2.0-or-later |
| [SameBoy LCD shader](https://github.com/LIJI32/SameBoy) by Lior Halphon, as shipped with Apotris | the Game Boy LCD shader, ported to Vulkan GLSL in `src/platform/vk/shaders/sameboy_lcd.frag` | MIT |
| [zfast_lcd](https://github.com/libretro/glsl-shaders) by Greg Hogan (SoltanGris42), as shipped with Apotris | the zfast LCD shader, ported to Vulkan GLSL in `src/platform/vk/shaders/zfast_lcd.frag` | GPL-2.0-or-later |
| [NTSC shaders](https://github.com/libretro/slang-shaders) (ntsc/shaders/maister) by Hans-Kristian Arntzen (Themaister) | the four NTSC presets (320px / 256px, S-Video / composite), compiled to SPIR-V by `tools/make-ntsc.py` (`third_party/ntsc`) | see the libretro/slang-shaders repository (license to be confirmed) |
| [AMD FidelityFX Super Resolution 1](https://github.com/GPUOpen-Effects/FidelityFX-FSR) (EASU and RCAS), in the libretro [slang-shaders](https://github.com/libretro/slang-shaders) port (`edge-smoothing/fsr`) | the FSR 1 shader, compiled to SPIR-V by `tools/make-ntsc.py` (`third_party/fsr`) | MIT (AMD's headers in `third_party/fsr/shaders`), Unlicense (the port) |
| Frame interpolation from [PS5SX2](https://github.com/Swordpdf/PS5SX2) by Swordpdf (from his RPCS3-PS5 / ps5-framegen), as integrated in [PSXS5](https://github.com/SynoPiia/PSXS5) by SynoPiia | frame interpolation on Vulkan (`third_party/framegen`, built by `tools/build-framegen.sh`) | GPL-3.0-or-later |
| [AMD FidelityFX SDK](https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK) 2.3.0 (FSR 3 optical flow and frame interpolation) | the frame interpolation shaders (`third_party/framegen/framegen`) | MIT (`third_party/framegen/framegen/LICENSE.txt`) |

