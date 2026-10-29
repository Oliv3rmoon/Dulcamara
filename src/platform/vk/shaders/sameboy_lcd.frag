// SwanStationPS5 - Game Boy LCD: every pixel split into RGB sub-pixels, with faint scanline gaps.
// Ported from the SameBoy LCD shader (as shipped with Apotris).
// SPDX-License-Identifier: GPL-3.0-or-later
#version 450

layout(set = 0, binding = 0) uniform sampler2D picture;
layout(push_constant) uniform Quad
{
    vec4 dst, uv, info, colour; // info: texture width, height, the PS1's line count
} quad;
layout(location = 0) in vec2 in_uv;
layout(location = 0) out vec4 out_color;

// the Display settings' brightness, saturation and warmth (1, 1, 0: unchanged)
vec3 grade(vec3 c)
{
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = mix(vec3(l), c, quad.colour.y) * quad.colour.x;
    return clamp(c * vec3(1.0 + quad.colour.z, 1.0, 1.0 - quad.colour.z), 0.0, 1.0);
}

// the PS1's own grid, whatever the internal resolution: its lines, and columns at 4:3
// (320 for 240 lines, 640 for 480)
vec2 grid_size()
{
    float lines = max(quad.info.z, 1.0);
    return vec2(lines * (4.0 / 3.0), lines);
}

// this pixel's place in the visible picture, 0 to 1 on both axes
vec2 place()
{
    return vec2(in_uv.x, (in_uv.y - quad.uv.y) / max(quad.uv.w - quad.uv.y, 1e-5));
}

// the picture at a place (0 to 1), kept inside the visible part
vec3 fetch(vec2 p)
{
    p = clamp(p, 0.0, 1.0);
    return texture(picture, vec2(p.x, mix(quad.uv.y, quad.uv.w, p.y))).rgb;
}

const float COLOR_LOW = 0.8;
const float COLOR_HIGH = 1.0;
const float SCANLINE_DEPTH = 0.1;

void main()
{
    vec2 size = grid_size();
    vec2 inv = 1.0 / size;
    vec2 t = place();
    vec2 pos = fract(t * size);
    vec2 sub_pos = fract(t * size * 6.0);

    vec4 center = vec4(fetch(t), 1.0);
    vec4 left = vec4(fetch(t - vec2(inv.x, 0.0)), 1.0);
    vec4 right = vec4(fetch(t + vec2(inv.x, 0.0)), 1.0);

    if (pos.y < 1.0 / 6.0)
    {
        float k = 0.5 - sub_pos.y / 2.0, d = sub_pos.y * SCANLINE_DEPTH + (1.0 - SCANLINE_DEPTH);
        center = mix(center, vec4(fetch(t + vec2(0.0, -inv.y)), 1.0), k) * d;
        left = mix(left, vec4(fetch(t + vec2(-inv.x, -inv.y)), 1.0), k) * d;
        right = mix(right, vec4(fetch(t + vec2(inv.x, -inv.y)), 1.0), k) * d;
    }
    else if (pos.y > 5.0 / 6.0)
    {
        float k = sub_pos.y / 2.0, d = (1.0 - sub_pos.y) * SCANLINE_DEPTH + (1.0 - SCANLINE_DEPTH);
        center = mix(center, vec4(fetch(t + vec2(0.0, inv.y)), 1.0), k) * d;
        left = mix(left, vec4(fetch(t + vec2(-inv.x, inv.y)), 1.0), k) * d;
        right = mix(right, vec4(fetch(t + vec2(inv.x, inv.y)), 1.0), k) * d;
    }

    vec4 midleft = mix(left, center, 0.5);
    vec4 midright = mix(right, center, 0.5);

    vec4 ret;
    if (pos.x < 1.0 / 6.0)
        ret = mix(vec4(COLOR_HIGH * center.r, COLOR_LOW * center.g, COLOR_HIGH * left.b, 1.0),
                  vec4(COLOR_HIGH * center.r, COLOR_LOW * center.g, COLOR_LOW * left.b, 1.0), sub_pos.x);
    else if (pos.x < 2.0 / 6.0)
        ret = mix(vec4(COLOR_HIGH * center.r, COLOR_LOW * center.g, COLOR_LOW * left.b, 1.0),
                  vec4(COLOR_HIGH * center.r, COLOR_HIGH * center.g, COLOR_LOW * midleft.b, 1.0), sub_pos.x);
    else if (pos.x < 3.0 / 6.0)
        ret = mix(vec4(COLOR_HIGH * center.r, COLOR_HIGH * center.g, COLOR_LOW * midleft.b, 1.0),
                  vec4(COLOR_LOW * midright.r, COLOR_HIGH * center.g, COLOR_LOW * center.b, 1.0), sub_pos.x);
    else if (pos.x < 4.0 / 6.0)
        ret = mix(vec4(COLOR_LOW * midright.r, COLOR_HIGH * center.g, COLOR_LOW * center.b, 1.0),
                  vec4(COLOR_LOW * right.r, COLOR_HIGH * center.g, COLOR_HIGH * center.b, 1.0), sub_pos.x);
    else if (pos.x < 5.0 / 6.0)
        ret = mix(vec4(COLOR_LOW * right.r, COLOR_HIGH * center.g, COLOR_HIGH * center.b, 1.0),
                  vec4(COLOR_LOW * right.r, COLOR_LOW * midright.g, COLOR_HIGH * center.b, 1.0), sub_pos.x);
    else
        ret = mix(vec4(COLOR_LOW * right.r, COLOR_LOW * midright.g, COLOR_HIGH * center.b, 1.0),
                  vec4(COLOR_HIGH * right.r, COLOR_LOW * right.g, COLOR_HIGH * center.b, 1.0), sub_pos.x);

    out_color = vec4(grade(clamp(ret.rgb, 0.0, 1.0)), 1.0);
}
