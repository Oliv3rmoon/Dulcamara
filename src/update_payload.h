/* Eclipse payload format v1. SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef NP_UPDATE_PAYLOAD_H
#define NP_UPDATE_PAYLOAD_H
#define UPDATE_PAYLOAD_FILE "eclipse-payload.sha256"
#define UPDATE_PAYLOAD_MAGIC "DULCAMARA-ECLIPSE-PAYLOAD-1\n"
/* The package generator reads these literal anchors from this header. The manifest
 * then binds EVERY packaged regular file, not just these minimum boot anchors. */
static const char *const update_payload_required[] = {
    "eboot.bin",
    "sce_sys/param.json",
    "sce_sys/icon0.png",
    "sce_module/libc.prx",
    "assets/fonts/Inter-400.ttf",
    "assets/fonts/Inter-600.ttf",
    "assets/fonts/tabler-icons-subset.ttf",
    "assets/fonts/NotoSansJP-subset.ttf",
    "assets/ui/eclipse/panel.png",
    "assets/ui/eclipse/button.png",
    "assets/ui/eclipse/controller-atlas.png",
    "assets/ui/eclipse/icon-atlas.png",
    "assets/ui/eclipse/loading-ring.png",
    "assets/ui/eclipse/placeholder-cover.png",
    "assets/ui/eclipse/cover-frame.png",
    "assets/ui/eclipse/chrome-strip.png",
    "assets/renpy8/dulcamara_boot.py",
    "assets/renpy8/dulcamara_savepaths.py",
    "assets/renpy8/lib/python312.zip",
    "assets/renpy8/renpy/__init__.py",
    "assets/renpy8/renpy/common/00start.rpy",
    "assets/renpy7/dulcamara_boot.py",
    "assets/renpy7/dulcamara_savepaths.py",
    "assets/renpy7/lib/python27.zip",
    "assets/renpy7/renpy/__init__.py",
    "assets/renpy7/renpy/common/00start.rpy",
    "licenses/LICENSE-Dulcamara.txt",
    "licenses/LICENSE-EasyRPG-Player.txt",
    "licenses/LICENSE-liblcf.txt",
    "licenses/THIRD-PARTY.txt",
};
#endif
