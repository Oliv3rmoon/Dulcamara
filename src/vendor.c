/*
 * Nightshade Player - third-party C libraries compiled into the app (from SwanStationPS5's):
 *   miniz 3.0.2 (MIT): unzips updates, reads .zip / .easyrpg games for the library and XYZ pictures
 * Their warnings are theirs; keep them out of ours.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#pragma clang diagnostic ignored "-Weverything"
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"

#define NDEBUG /* no __assert in the PS5 libc */
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.c"
#include "../third_party/miniz/miniz_tdef.c"
#include "../third_party/miniz/miniz_tinfl.c"
#include "../third_party/miniz/miniz_zip.c"
