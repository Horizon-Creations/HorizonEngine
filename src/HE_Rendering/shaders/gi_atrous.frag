#version 450
// GI shadow spatial filter: ONE edge-aware a-trous iteration (B3-spline 5x5,
// holes = uStep texels), run twice (step 1 from history.a into a scratch R16F
// target, step 2 from that into the mask the scene samples). Replaces the old
// 3x3 box, which widened every penumbra by ~3 screen pixels at half res — at
// 0.5 deg and at contact edges the largest single error in the mask, which no
// ray count could reduce (Thema 134 §2/§4.3).
//
// Three stops per tap: the receiver's PLANE (distance of the tap from this
// pixel's tangent plane, in texel footprints), its NORMAL (power 32) and the
// VALUE — mandatory, without it the filter softens every shadow (rmse x7).
// The value stop's scale is the Bernoulli sigma sqrt(c(1-c)/N_eff): binary
// visibility knows its own variance from its mean, so unlike SVGF no variance
// buffer is needed. The history itself stays unfiltered (no feedback), which
// keeps the filter's bias out of the accumulation.
//
// SYNC: one of FOUR hand-kept copies — kGiAtrousHLSL (HlslSources.h, D3D11/12),
// kGiAtrousFS (OpenGLRenderer.cpp), giShadowAtrous (MetalRenderer.mm). The
// constants are pinned by tests/test_culling.cpp, "GI kernels: ...", subcase
// "shadow spatial filter".
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uSrc;  // history (.a) or the previous iteration (.r)
layout(set = 0, binding = 1) uniform sampler2D uGPos; // world position, a = 0 on background
layout(set = 0, binding = 2) uniform sampler2D uGNorm;
layout(push_constant) uniform GiAtrousPC {
    vec4 uAtrous; // x = step in texels (0 = plain copy), y = 1 read .a else .r, z = N_eff (history samples)
};

// Explicit LOD: the early outs and the background skip make the taps divergent.
vec4  tap(sampler2D s, vec2 uv) { return textureLod(s, uv, 0.0); }
float atrousValue(vec4 s) { return uAtrous.y > 0.5 ? s.a : s.r; }

void main()
{
    float c  = atrousValue(tap(uSrc, vUV));
    vec4  pv = tap(uGPos, vUV);
    float st = uAtrous.x;
    if (st < 0.5 || pv.a < 0.5) { FragColor = vec4(c, 0.0, 0.0, 1.0); return; }

    vec2 texel = 1.0 / vec2(textureSize(uSrc, 0));
    // Fully lit / fully shadowed and so are the 4 neighbours a hole further out:
    // nothing to filter — most of the screen (Thema 134 §3.4, filter 1.45 -> 0.37 ms).
    if (c < 1e-3 || c > 1.0 - 1e-3)
    {
        bool allSame = true;
        for (int k = 0; k < 4 && allSame; ++k)
        {
            vec2 o = vec2(k == 0 ? 1.0 : k == 1 ? -1.0 : 0.0, k == 2 ? 1.0 : k == 3 ? -1.0 : 0.0);
            allSame = abs(atrousValue(tap(uSrc, vUV + o * (2.0 * st) * texel)) - c) < 1e-3;
        }
        if (allSame) { FragColor = vec4(c, 0.0, 0.0, 1.0); return; }
    }

    vec3 n = normalize(tap(uGNorm, vUV).xyz);
    // World size of one texel: per axis the SMALLER one-sided G-buffer step (the
    // other side may be another surface), as in the temporal pass.
    vec3  gxp = tap(uGPos, vUV + vec2(texel.x, 0.0)).xyz, gxm = tap(uGPos, vUV - vec2(texel.x, 0.0)).xyz;
    vec3  gyp = tap(uGPos, vUV + vec2(0.0, texel.y)).xyz, gym = tap(uGPos, vUV - vec2(0.0, texel.y)).xyz;
    float fp  = max(max(min(length(gxp - pv.xyz), length(gxm - pv.xyz)),
                        min(length(gyp - pv.xyz), length(gym - pv.xyz))), 1e-4);
    float sig = sqrt(max(c * (1.0 - c), 0.0) / max(uAtrous.z, 1.0));
    const float h[5] = float[5](1.0 / 16.0, 1.0 / 4.0, 3.0 / 8.0, 1.0 / 4.0, 1.0 / 16.0);
    float sum = 0.0, wsum = 0.0;
    for (int y = -2; y <= 2; ++y)
        for (int x = -2; x <= 2; ++x)
        {
            vec2 uv = vUV + vec2(float(x), float(y)) * st * texel;
            vec4 q  = tap(uGPos, uv);
            if (q.a < 0.5) continue;
            float v = atrousValue(tap(uSrc, uv));
            float w = h[x + 2] * h[y + 2];
            w *= exp(-abs(dot(n, q.xyz - pv.xyz)) / (1.0 * fp));
            w *= pow(max(dot(n, normalize(tap(uGNorm, uv).xyz)), 0.0), 32.0);
            w *= exp(-abs(v - c) / (2.0 * sig + 1e-3));
            sum += v * w; wsum += w;
        }
    FragColor = vec4(wsum > 0.0 ? sum / wsum : c, 0.0, 0.0, 1.0);
}
