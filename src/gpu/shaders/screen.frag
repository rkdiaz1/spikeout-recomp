// The screen: black, the 2D layers below the 3D, the 3D picture (opaque, or
// translucent over what is below), then the 2D layers above (as
// m3::TilegenRenderer::composite). The 2D layers are sampled sharp; the 3D
// (premultiplied alpha, gpu::Real3dGpu::display_image) may be drawn larger
// and is filtered, averaged down with its mipmaps where the window is
// smaller than it: supersampling. Then the looks (look.glsl).
//
// Widescreen (wide.x > 0): the 3D is that many screen pixels wider each side
// (its full-screen viewports opened out), and so are the 2D layers (their
// sides filled in by m3::WideLayer). The sides show the 3D only where it
// reaches the edge of the arcade's screen somewhere on that side: on the 2D
// screens (the title, the character select, the map) the 3D is a picture in
// the middle, and what it has past the arcade's screen (a streak waiting to
// cross the title) the arcade never showed.
#version 450
#extension GL_GOOGLE_include_directive : require
#include "look.glsl"

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 colour;

layout(set = 2, binding = 0) uniform sampler2D bottom;
layout(set = 2, binding = 1) uniform sampler2D three_d;
layout(set = 2, binding = 2) uniform sampler2D top;

// Whether the 3D shows anywhere along the arcade screen's edge column at u.
bool reaches(float u) {
    for (int i = 0; i < 48; ++i)
        if (textureLod(three_d, vec2(u, (float(i) + 0.5) / 48.0), 0.0).a > 0.0) return true;
    return false;
}

// A 2D layer at `pos` (its pixels) scaled to the window (layers.x, the 2D scaling setting): sharp-smooth keeps each
// arcade pixel a solid block but blends its edges over one window pixel (`d`: the layer's pixels a window pixel), so
// at a scale that isn't whole every block looks the same size; smooth is plain bilinear, as Supermodel's 2D upscaling.
// The layers are 0 where nothing was drawn and opaque elsewhere, so a blend of them is premultiplied: its alpha the
// share drawn, its edges laid over what is below.
//
// Bicubic (3) and biquintic (4) are Supermodel's 2D upscaling filters (Model 3-Vulkan's Src/Graphics/Shaders2D.h,
// Supermodel, Copyright 2011-2012 Bart Trzynadlowski, Nik Henson, GPL-3.0): a 4x4 B-spline from 4 bilinear reads, and
// bilinear with each pixel's blend eased by a quintic curve (sharper than bilinear, one read).
vec4 cubic(float v) {
    const vec4 n = vec4(1.0, 2.0, 3.0, 4.0) - v;
    const vec4 s = n * n * n;
    const float x = s.x, y = s.y - 4.0 * s.x, z = s.z - 4.0 * s.y + 6.0 * s.x, w = 6.0 - x - y - z;
    return vec4(x, y, z, w) * (1.0 / 6.0);
}
vec4 bicubic(sampler2D t, vec2 pos, ivec2 size) {
    const vec2 inv = 1.0 / vec2(size), p = pos - 0.5, i = floor(p), f = p - i;
    const vec4 xc = cubic(f.x), yc = cubic(f.y);
    const vec4 c = i.xxyy + vec2(-0.5, 1.5).xyxy;
    const vec4 s = vec4(xc.xz + xc.yw, yc.xz + yc.yw);
    const vec4 o = (c + vec4(xc.yw, yc.yw) / s) * inv.xxyy;
    const vec4 s0 = textureLod(t, o.xz, 0.0), s1 = textureLod(t, o.yz, 0.0), s2 = textureLod(t, o.xw, 0.0),
               s3 = textureLod(t, o.yw, 0.0);
    const float sx = s.x / (s.x + s.y), sy = s.z / (s.z + s.w);
    return mix(mix(s3, s2, sx), mix(s1, s0, sx), sy);
}

// Softness (layers.y, 0-1, the 2D softness setting): sharp-smooth's blended edge, 4 window pixels wide at 1 (one at
// the default 0.25, a hard step at 0); the other filters mixed with the sharp picture by it (1: the filter's whole).
vec4 layer(sampler2D t, vec2 pos, ivec2 size, vec2 d) {
    vec2 p = pos;
    vec4 c;
    if (layers.x == 3.0) {
        c = bicubic(t, pos, size);
    } else {
        if (layers.x == 1.0) {
            const vec2 seam = floor(p + 0.5);
            p = seam + clamp((p - seam) / (d * max(4.0 * layers.y, 1e-3)), -0.5, 0.5);
        } else if (layers.x == 4.0) {
            const vec2 q = p + 0.5, i = floor(q);
            vec2 f = q - i;
            f = f * f * f * (f * (f * 6.0 - 15.0) + 10.0) - 0.5;
            p = i + f;
        }
        c = textureLod(t, p / vec2(size), 0.0);
    }
    if (layers.x >= 2.0 && layers.y < 1.0)
        c = mix(texelFetch(t, clamp(ivec2(floor(pos)), ivec2(0), size - 1), 0), c, layers.y);
    return c;
}

void main() {
    vec4 q = texture(three_d, uv);
    const float extend = wide.x, width = wide.y, full = width + 2.0 * extend;
    const ivec2 size = textureSize(top, 0);
    const ivec2 at = min(ivec2(uv * vec2(size)), size - 1);
    const float x = uv.x * full - extend; // where in the arcade's 496 pixels this is
    if (x < 0.0 && !reaches((extend + 0.5) / full)) q = vec4(0.0);
    if (x >= width && !reaches((extend + width - 0.5) / full)) q = vec4(0.0);
    const vec2 d = max(fwidth(uv * vec2(size)), vec2(1e-4)); // (outside any branch: derivatives need every pixel)
    vec3 p;
    if (layers.x == 0.0) { // sharp: each window pixel the arcade pixel it falls in, as before
        // (smooth motion: the layers below the 3D, the sky, where it was between two frames)
        const ivec2 below = clamp(ivec2(floor(uv * vec2(size) + wide.zw)), ivec2(0), size - 1);
        const vec4 b = texelFetch(bottom, below, 0), t = texelFetch(top, at, 0);
        p = b != vec4(0.0) ? b.rgb : vec3(0.0); // nothing drawn below: black
        p = q.rgb + p * (1.0 - q.a);            // over what is below
        if (t != vec4(0.0)) p = t.rgb;
    } else {
        const vec4 b = layer(bottom, uv * vec2(size) + wide.zw, size, d), t = layer(top, uv * vec2(size), size, d);
        p = b.rgb;                   // over black where nothing was drawn
        p = q.rgb + p * (1.0 - q.a); // the 3D over what is below
        p = t.rgb + p * (1.0 - t.a); // the layers above over both, their edges blended
    }
    colour = vec4(look(p), 1.0);
}
