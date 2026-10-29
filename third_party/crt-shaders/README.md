# crt-shaders

The sources of the CRT Royale, CRT Newpixie, CRT Hyllian NTSC and CRT Mattias shaders, copied from
[libretro/slang-shaders](https://github.com/libretro/slang-shaders) with its folder layout (`crt/`, `include/`, `blurs/`,
`stock.slang`), so the shaders' includes resolve as they do there. `tools/make-crt.py` compiles them to
`src/platform/vk/shaders_crt.h`. Their licenses are in each file's header and in `crt/shaders/crt-royale/LICENSE.TXT`.

Changes to the originals:

- `blurs/shaders/royale/vertex-shader-blur-fast-*.h`: the libretro copy is half converted from Cg (`float2`, `IN.video_size`,
  an `#endif` without its `#ifndef`), so it does not compile; the vertex stage was finished by hand. Both blurs only feed the
  halation, whose weight is 0 in the preset.
- The generator does not embed three textures as they are, to keep the header small; none is read with the presets' own
  settings (see the top of `tools/make-crt.py`).
