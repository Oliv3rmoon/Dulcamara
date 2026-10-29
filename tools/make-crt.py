#!/usr/bin/env python3
# SwanStationPS5 - compiles four more CRT presets of libretro's slang-shaders (third_party/crt-shaders: CRT Royale,
# newpixie-crt, crt-hyllian-ntsc and crt-mattias) to SPIR-V and writes src/platform/vk/shaders_crt.h: each preset's
# passes, in the same form as shaders_royale.h (RoyalePass), so the renderer in vk_present.c runs them the same way.
# Committed, so builds don't need a shader compiler:
#
#   GLSLANG=/path/to/glslang python3 tools/make-crt.py
#
# Three textures are not embedded as the originals have them, to keep the header small; none of them is read with the
# presets' own settings: CRT Royale's three large phosphor masks (512x512, only read by mask_sample_mode 1 and 2) are the
# 64x64 ones, the second Hyllian LUT (only read when LUT_selector_param is 2) is the first, and newpixie's frame image
# (only read when use_frame is 1) is a 4x4 transparent one.
# SPDX-License-Identifier: GPL-3.0-or-later
import hashlib
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

from_dir = os.path.dirname(os.path.abspath(__file__))
root = os.path.normpath(os.path.join(from_dir, ".."))
base = os.environ.get("CRT_SHADERS_BASE") or os.path.join(root, "third_party", "crt-shaders")
out_path = os.environ.get("CRT_HEADER_OUT") or os.path.join(root, "src", "platform", "vk", "shaders_crt.h")
glslang = os.environ.get("GLSLANG") or shutil.which("glslang") or shutil.which("glslangValidator")
if not glslang:
    sys.exit("glslang not found (set GLSLANG)")

# the order of Settings > Display > Shader, from 14 on
PRESETS = ["crt-royale", "newpixie-crt", "crt-hyllian-ntsc", "crt-mattias"]
SUBSTITUTE = {"mask_grille_texture_large": "mask_grille_texture_small", "mask_slot_texture_large": "mask_slot_texture_small",
              "mask_shadow_texture_large": "mask_shadow_texture_small", "SamplerLUT2": "SamplerLUT1", "frametexture": None}

MAX_PASSES, MAX_PUSH, MAX_SAMPLERS, MAX_UBO_SEM = 12, 24, 8, 8


def read_preset(path):
    values = {}
    for line in open(path, encoding="utf-8"):
        line = line.split("#", 1)[0].strip()
        m = re.match(r'(\w+)\s*=\s*"?([^"]*?)"?\s*$', line)
        if m:
            values[m.group(1)] = m.group(2).strip()
    return values


def expand(text, directory):
    def one(m):
        path = os.path.normpath(os.path.join(directory, m.group(1)))
        return expand(open(path, encoding="utf-8").read(), os.path.dirname(path))
    return re.sub(r'^[ \t]*#include "([^"]+)"[ \t]*$', one, text, flags=re.M)


def parse_parameters(text):
    return {m.group(1): float(m.group(2))
            for m in re.finditer(r'^#pragma parameter (\w+)\s+"[^"]*"\s+(\S+)', text, flags=re.M)}


def split_stages(text):
    parts = re.split(r"^#pragma stage (vertex|fragment)\s*$", text, flags=re.M)
    stages = {parts[k]: parts[k + 1] for k in range(1, len(parts), 2)}
    return parts[0], stages["vertex"], stages["fragment"]


TYPES = {"mat4": (16, 64), "vec4": (16, 16), "vec3": (16, 12), "vec2": (8, 8), "float": (4, 4), "uint": (4, 4), "int": (4, 4)}


def block_members(text, pattern):
    """(size, [(offset, type, name)]) of a push constant or std140 block, in declaration order."""
    m = re.search(pattern, text, flags=re.S)
    if not m:
        return 0, []
    members, offset = [], 0
    for line in re.sub(r"//[^\n]*", "", m.group(1)).split(";"):
        line = line.strip()
        if not line:
            continue
        mm = re.match(r"(mat4|vec4|vec3|vec2|float|uint|int)\s+(\w+(?:\s*,\s*\w+)*)$", line)
        if not mm:
            sys.exit("unsupported block member: " + line)
        kind, names = mm.groups()
        align, size = TYPES[kind]
        for name in (n.strip() for n in names.split(",")):
            offset = (offset + align - 1) // align * align
            members.append((offset, kind, name))
            offset += size
    return offset, members


tmp = tempfile.mkdtemp()


def compile_stage(source, stage, name, include_dir):
    path = os.path.join(tmp, name + "." + ("vert" if stage == "vertex" else "frag"))
    open(path, "w", encoding="utf-8").write(source)
    spv = path + ".spv"
    r = subprocess.run([glslang, "-V", "--target-env", "vulkan1.1", "-o", spv, path], capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit("glslang failed on %s:\n%s%s" % (path, r.stdout, r.stderr))
    data = open(spv, "rb").read()
    return struct.unpack("<%dI" % (len(data) // 4), data)


def c_words(name, words):
    lines = ["static const uint32_t %s[] = {" % name]
    for i in range(0, len(words), 8):
        lines.append("    " + ", ".join("0x%08x" % w for w in words[i:i + 8]) + ",")
    lines.append("};")
    return "\n".join(lines)


def c_bytes(name, data, per_line=32):
    lines = ["static const uint8_t %s[%d] = {" % (name, len(data))]
    for i in range(0, len(data), per_line):
        lines.append("    " + ",".join(str(b) for b in data[i:i + per_line]) + ",")
    lines.append("};")
    return "\n".join(lines)


SCALE = {"source": "SCALE_SOURCE", "viewport": "SCALE_VIEWPORT", "absolute": "SCALE_ABSOLUTE"}
WRAP = {"clamp_to_border": 0, "clamp_to_edge": 1, "repeat": 2, "mirrored_repeat": 2}
SIMPLE = {"SourceSize": "SOURCE_SIZE", "OriginalSize": "ORIGINAL_SIZE", "OutputSize": "OUTPUT_SIZE",
          "FinalViewportSize": "FINAL_SIZE", "FrameCount": "FRAME"}

blobs = {}      # content hash -> identifier of a byte array
decls = []      # the arrays, in order
modules = {}    # (preset, pass) -> info


def array_of(prefix, data):
    key = (prefix, hashlib.sha1(bytes(data)).hexdigest())
    if key not in blobs:
        blobs[key] = "%s%d" % (prefix, len(blobs))
        decls.append(c_bytes(blobs[key], data))
    return blobs[key]


def read_png(path):
    from PIL import Image
    image = Image.open(path).convert("RGBA")
    return image.width, image.height, image.tobytes()


preset_defs = []
for name in PRESETS:
    path = os.path.join(base, "crt", name + ".slangp")
    pdir = os.path.dirname(path)
    preset = read_preset(path)
    count = int(preset["shaders"])
    if count > MAX_PASSES:
        sys.exit("%s: %d passes" % (name, count))
    aliases = {preset["alias%d" % i]: i for i in range(count) if preset.get("alias%d" % i)}
    texture_names = [t for t in preset.get("textures", "").split(";") if t]

    # textures
    textures = []
    for t in texture_names:
        source = SUBSTITUTE.get(t, t)
        if source is None:
            width, height, rgba = 4, 4, bytes(64)
        else:
            width, height, rgba = read_png(os.path.normpath(os.path.join(pdir, preset[source])))
        linear = 1 if preset.get(t + "_linear", "true") == "true" else 0
        wrap = WRAP[preset.get(t + "_wrap_mode", "clamp_to_border")]
        textures.append((array_of("CRT_TEX", rgba), width, height, linear, wrap))

    passes = []
    for i in range(count):
        shader = os.path.normpath(os.path.join(pdir, preset["shader%d" % i]))
        text = expand(open(shader, encoding="utf-8").read(), os.path.dirname(shader))
        common, vertex, fragment = split_stages(text)
        local = parse_parameters(text)
        for k in list(local):
            if k in preset:
                local[k] = float(preset[k])
        # no vertex buffer: the quad's corners come from the vertex index
        vertex = re.sub(r"layout\(location\s*=\s*0\)\s*in\s+vec4\s+Position;\s*layout\(location\s*=\s*1\)\s*in\s+vec2\s+TexCoord;", "", vertex)
        vertex = ("#define TexCoord vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1))\n"
                  "#define Position vec4(TexCoord * 2.0 - 1.0, 0.0, 1.0)\n") + vertex
        uniform_text = re.sub(r"//[^\n]*", "", common)
        push_size, push_members = block_members(uniform_text, r"layout\(push_constant\)\s+uniform\s+Push\s*\{(.*?)\}\s*\w+;")
        ubo_size, ubo_members = block_members(
            uniform_text, r"layout\(std140,\s*set\s*=\s*0,\s*binding\s*=\s*0\)\s+uniform\s+UBO\s*\{(.*?)\}\s*\w+;")

        def kind_of(member):
            if member in SIMPLE:
                return SIMPLE[member], -1, 0.0
            m = re.match(r"PassOutput(\d+)Size$", member)
            if m:
                return "PASS_SIZE", int(m.group(1)), 0.0
            if member.endswith("Size") and member[:-4] in aliases:
                return "PASS_SIZE", aliases[member[:-4]], 0.0
            if member.endswith("Size") and member[:-4] in texture_names:
                return "TEXTURE_SIZE", texture_names.index(member[:-4]), 0.0
            if member in local:
                return "PARAM", -1, local[member]
            return "NONE", -1, 0.0  # no meaning: it stays 0, as in RetroArch

        push = []
        for offset, _kind, member in push_members:
            kind, index, value = kind_of(member)
            push.append((offset, kind, index, value))
        ubo = bytearray((ubo_size + 15) // 16 * 16 or 16)
        ubo_sem = []
        for offset, kind_t, member in ubo_members:
            if kind_t == "mat4":
                struct.pack_into("16f", ubo, offset, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1)
                continue
            kind, index, value = kind_of(member)
            if kind == "PARAM":
                struct.pack_into("f", ubo, offset, value)
            elif kind != "NONE":
                ubo_sem.append((offset, kind, index, 0.0))
        samplers = []
        for m in re.finditer(r"layout\(set\s*=\s*0,\s*binding\s*=\s*(\d+)\)\s+uniform\s+sampler2D\s+(\w+);",
                             re.sub(r"//[^\n]*", "", common + fragment)):
            binding, sname = int(m.group(1)), m.group(2)
            m2 = re.match(r"PassOutput(\d+)$", sname)
            m3 = re.match(r"PassFeedback(\d+)$", sname)
            if sname == "Source":
                kind, index = "SOURCE", -1
            elif sname == "Original":
                kind, index = "ORIGINAL", -1
            elif m2:
                kind, index = "PASS", int(m2.group(1))
            elif m3:
                kind, index = "FEEDBACK", int(m3.group(1))
            elif sname in aliases:
                kind, index = "PASS", aliases[sname]
            elif sname.endswith("Feedback") and sname[:-8] in aliases:
                kind, index = "FEEDBACK", aliases[sname[:-8]]
            elif sname in texture_names:
                kind, index = "TEXTURE", texture_names.index(sname)
            else:
                sys.exit("%s pass %d: sampler %s is not known" % (name, i, sname))
            samplers.append((binding, kind, index))
        if len(push) > MAX_PUSH or len(samplers) > MAX_SAMPLERS or len(ubo_sem) > MAX_UBO_SEM or (push_size + 15) // 16 * 16 > 128:
            sys.exit("%s pass %d is too big for RoyalePass (push %d members, %d bytes; %d samplers; %d UBO members)" %
                     (name, i, len(push), push_size, len(samplers), len(ubo_sem)))
        ident = "CRT_P%d_%d" % (len(preset_defs), i)
        vert = compile_stage(common + vertex, "vertex", ident, base)
        frag = compile_stage(common + fragment, "fragment", ident, base)
        decls.append("/* %s: %s */" % (name, os.path.relpath(shader, base)))
        decls.append(c_words(ident + "_VERT", vert))
        decls.append(c_words(ident + "_FRAG", frag))
        blob = array_of("CRT_UBO", ubo)

        def scale_of(axis):
            typ = preset.get("scale_type_%s%d" % (axis, i)) or preset.get("scale_type%d" % i) or "source"
            val = (preset.get("scale_%s%d" % (axis, i)) or preset.get("scale%d" % i) or preset.get("scale_%d" % i) or "1.0")
            return SCALE[typ], float(val)

        (sx, vx), (sy, vy) = scale_of("x"), scale_of("y")
        fmt = 0
        fm = re.search(r"^#pragma format (\w+)", fragment, flags=re.M)
        if fm:
            fmt = {"R8G8B8A8_SRGB": 1, "R16G16B16A16_SFLOAT": 2}.get(fm.group(1), 0)
        if preset.get("srgb_framebuffer%d" % i) == "true":
            fmt = 1
        if preset.get("float_framebuffer%d" % i) == "true":
            fmt = 2
        passes.append(dict(ident=ident, vert=vert, frag=frag, sx=sx, vx=vx, sy=sy, vy=vy,
                           linear=1 if preset.get("filter_linear%d" % i, "false") == "true" else 0,
                           wrap=WRAP[preset.get("wrap_mode%d" % i, "clamp_to_border")], fmt=fmt,
                           push_size=(push_size + 15) // 16 * 16, push=push, samplers=samplers,
                           frames=int(preset.get("frame_count_mod%d" % i, "0")), blob=blob, ubo_size=len(ubo),
                           ubo_sem=ubo_sem))
        print("%-18s pass %2d %-52s push %3d, %d samplers, ubo %3d" % (name, i, os.path.basename(shader), push_size, len(samplers), ubo_size))
    preset_defs.append((name, passes, textures))

out = ["/* Generated by tools/make-crt.py from third_party/crt-shaders (libretro's slang-shaders). Do not edit. */",
       "#ifndef SwanStationPS5_SHADERS_CRT_H", "#define SwanStationPS5_SHADERS_CRT_H", "#include <stddef.h>", "#include <stdint.h>",
       '#include "shaders_royale.h"', ""]
out += decls
out.append("")
for n, (name, passes, textures) in enumerate(preset_defs):
    out.append("/* %s */" % name)
    if textures:
        out.append("static const RoyaleTexture CRT_TEXTURES%d[%d] = {" % (n, len(textures)))
        for data, w, h, linear, wrap in textures:
            out.append("    {%d, %d, %d, %d, %s}," % (w, h, linear, wrap, data))
        out.append("};")
    out.append("static const RoyalePass CRT_CHAIN%d[%d] = {" % (n, len(passes)))
    for p in passes:
        out.append("    {%s_VERT, %s_FRAG, %d, %d, ROYALE_%s, ROYALE_%s, %.8ff, %.8ff, %d, %d, %d, %d, %d, %d," %
                   (p["ident"], p["ident"], len(p["vert"]) * 4, len(p["frag"]) * 4, p["sx"], p["sy"], p["vx"], p["vy"],
                    p["linear"], p["wrap"], p["fmt"], p["push_size"], len(p["push"]), len(p["samplers"])))
        out.append("     {%s}," % ", ".join("{%d, ROYALE_PUSH_%s, %d, %.8ff}" % m for m in p["push"]))
        out.append("     {%s}, %d," % (", ".join("{%d, ROYALE_SAMPLER_%s, %d}" % s for s in p["samplers"]), p["frames"]))
        out.append("     %s, %d, %d, {%s}}," % (p["blob"], p["ubo_size"], len(p["ubo_sem"]),
                                                ", ".join("{%d, ROYALE_PUSH_%s, %d, %.8ff}" % m for m in p["ubo_sem"])))
    out.append("};")
out.append("")
out.append("static const struct { const RoyalePass *passes; int count; const RoyaleTexture *textures; int tex_count; } CRT_PRESETS[%d] = {" % len(preset_defs))
for n, (name, passes, textures) in enumerate(preset_defs):
    out.append("    {CRT_CHAIN%d, %d, %s, %d}, /* %s */" % (n, len(passes), "CRT_TEXTURES%d" % n if textures else "NULL", len(textures), name))
out.append("};")
out.append("#endif")
open(out_path, "w").write("\n".join(out) + "\n")
shutil.rmtree(tmp)
print("wrote", out_path)
