#version 450

// TAA resolve (docs/anti-aliasing-plan.md A3), the Vulkan twin of GL's
// kTaaResolveFS — same maths, keep them in step. Blends this frame's tonemapped
// image with the reprojected history. Current and velocity are read with
// texelFetch (point, clamped by hand); the history is sampled linear on purpose
// (subpixel reprojection) through the postFx sampler.
// params: x/y = 1/resolution, z = history blend weight (0 = history unusable
// this frame — resize, first frame, a still from another camera), w unused.

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D uCurrent;
layout(set = 0, binding = 1) uniform sampler2D uHistory;
layout(set = 0, binding = 2) uniform sampler2D uVelocity;
layout(push_constant) uniform PC { vec4 params; } pc;

void main()
{
    ivec2 size = textureSize(uCurrent, 0);
    ivec2 px   = ivec2(gl_FragCoord.xy);
    vec3  cur  = texelFetch(uCurrent, px, 0).rgb;
    if (pc.params.z <= 0.0) { outColor = vec4(cur, 1.0); return; }

    // Motion of the FASTEST fragment in the 3x3 neighbourhood, not this pixel's
    // own: on a silhouette the pixel may carry the background's motion while
    // the eye follows the object.
    vec2  vel  = texelFetch(uVelocity, px, 0).rg;
    float best = length(vel);
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), size - 1);
            vec2  v = texelFetch(uVelocity, q, 0).rg;
            float l = length(v);
            if (l > best) { best = l; vel = v; }
        }

    vec2 histUV = vUV - vel;
    // Off-screen history is no history: nothing was ever accumulated there.
    if (any(lessThan(histUV, vec2(0.0))) || any(greaterThan(histUV, vec2(1.0))))
    {
        outColor = vec4(cur, 1.0);
        return;
    }
    vec3 hist = texture(uHistory, histUV).rgb;

    // Neighbourhood clamp — the whole defence against ghosting.
    vec3 lo = cur, hi = cur;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
        {
            ivec2 q = clamp(px + ivec2(x, y), ivec2(0), size - 1);
            vec3  c = texelFetch(uCurrent, q, 0).rgb;
            lo = min(lo, c);
            hi = max(hi, c);
        }
    hist = clamp(hist, lo, hi);

    // Fast motion means less history.
    float motion = clamp(length(vel / pc.params.xy) / 32.0, 0.0, 1.0);
    float blend  = mix(pc.params.z, 0.0, motion);
    outColor = vec4(mix(cur, hist, blend), 1.0);
}
