# Build-time fixture for the shared Ren'Py scripts shipped in the read-only app.
# SPDX-License-Identifier: GPL-3.0-or-later
init 990 python hide:
    import os
    common = renpy.config.commondir
    for directory, subdirs, files in os.walk(common):
        subdirs.sort()
        for name in sorted(files):
            if name.endswith(".rpym"):
                relative = os.path.relpath(os.path.join(directory, name), common)
                # Compile the module without executing its optional layout or changing the active one.
                renpy.game.script.load_file(common, relative.replace(os.sep, "/"))

label start:
    return
