# Native save path declarations for both Ren'Py engines.
# SPDX-License-Identifier: GPL-3.0-or-later
from __future__ import absolute_import
import os
import sys
import _dulcamara
import renpy


def entry(path, kind=4, shared=False):
    # Resolve filesystem spelling; do not derive anything from display names.
    path = os.path.realpath(os.path.abspath(path))
    probe = path if os.path.exists(path) else os.path.dirname(path)
    while not os.path.exists(probe) and os.path.dirname(probe) != probe:
        probe = os.path.dirname(probe)
    policy = 2 if os.access(probe, os.W_OK) else 1
    if sys.version_info[0] < 3 and not isinstance(path, bytes):
        path = path.encode('utf-8')
    suffix = renpy.savegame_suffix if kind == 4 else ""
    if sys.version_info[0] < 3 and not isinstance(suffix, bytes):
        suffix = suffix.encode('utf-8')
    return (path, kind, 2 if shared else 1, policy, suffix)


def declare(path, kind=4, shared=False):
    if path is not None:
        _dulcamara.save_path(*entry(path, kind, shared))


def slot(directory, slotname, temporary_suffix):
    # scan() lists direct children. A concrete nested native operation grants only
    # its exact filename and bounded native artifacts, independent of process time.
    # temporary_suffix is deliberately not part of identity: native rename timestamps
    # vary by process, while the completed filename and artifact grammar are stable.
    declare(directory)
    filename = os.path.join(directory, renpy.exports.fsencode(slotname + renpy.savegame_suffix))
    if os.path.realpath(os.path.dirname(filename)) != os.path.realpath(directory):
        declare(filename, kind=5)


def tokens(path):
    for name in ("security_keys.txt", "upgraded.txt"):
        declare(os.path.join(path, name), kind=2, shared=True)


def initial(renpy):
    # Mirror native savelocation.init and savetoken.init_tokens before either can write.
    # Script loading (including python early) and --savedir resolution have finished.
    paths = [entry(renpy.config.savedir)]
    if not renpy.mobile and not renpy.macapp:
        paths.append(entry(os.path.join(renpy.config.gamedir, "saves")))
    paths.extend(entry(path) for path in renpy.config.extra_savedirs)
    if renpy.config.save_directory is not None:
        tokens = renpy.__main__.path_to_saves(renpy.config.gamedir, "tokens")
        if tokens is not None:
            paths.extend(entry(os.path.join(tokens, name), kind=2, shared=True)
                         for name in ("security_keys.txt", "upgraded.txt"))
    # Deduplicate before applying the bounded C protocol, preserving exact file/directory kinds.
    unique, indices = [], {}
    for path in paths:
        key = path[:2] + path[4:]
        if key not in indices:
            indices[key] = len(unique)
            unique.append(path)
        elif path[2] == 2:
            unique[indices[key]] = path  # A shared alias stays shared.
    _dulcamara.save_paths(unique)
