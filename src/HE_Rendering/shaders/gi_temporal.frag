#version 450
// GI shadow temporal accumulation: reproject via last frame's (clip-fixed)
// viewProj; history carries the world position (rgb) + shadow scalar (a).
// Tolerance deliberately TIGHT (Metal lesson: loose depth-scaled tolerances
// accept wrong-surface reprojects at cube edges) — a few cm, or one texel's
// world footprint where that is larger. prevViewProj INCLUDES
// kVulkanClipFix, so ndc*0.5+0.5 lands directly in top-left-origin UV space —
// no extra y-flip (matches how the G-buffer itself was rasterized).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uGPos;
layout(set = 0, binding = 1) uniform sampler2D uRaw;
layout(set = 0, binding = 2) uniform sampler2D uHistory;
layout(set = 0, binding = 3) uniform GiTemporalUBO {
    mat4 uPrevViewProj;
    mat4 uCurViewProj; // this frame's, same family as uPrevViewProj (motion-vector reprojection)
    vec4 uBlend; // x = history weight (0 on first GI frame)
};

void main()
{
    vec4  pv   = texture(uGPos, vUV);
    float rawV = texture(uRaw, vUV).r;
    if (pv.a < 0.5) { FragColor = vec4(0.0, 0.0, 0.0, rawV); return; }

    vec4 clip = uPrevViewProj * vec4(pv.xyz, 1.0);
    if (clip.w <= 0.0) { FragColor = vec4(pv.xyz, rawV); return; }
    vec2 ndc    = clip.xy / clip.w;
    vec2 prevUV = ndc * 0.5 + 0.5;
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0))))
    { FragColor = vec4(pv.xyz, rawV); return; }

    vec2  texel     = 1.0 / vec2(textureSize(uRaw, 0)); // uGPos has the same size
    // The history texel can sit up to ~0.7 texel from the exact reprojected
    // spot, so the tolerance must cover one texel's world footprint or plain
    // camera motion throws the history away (Thema 131 §3 C). The footprint
    // per axis is the SMALLER one-sided G-buffer step — the other side may be
    // a different surface — capped so a 1-texel sliver between two other
    // surfaces cannot open it up; the fixed few-cm floor stays the
    // wrong-surface guard everywhere else.
    vec3  gxp = texture(uGPos, vUV + vec2(texel.x, 0.0)).xyz, gxm = texture(uGPos, vUV - vec2(texel.x, 0.0)).xyz;
    vec3  gyp = texture(uGPos, vUV + vec2(0.0, texel.y)).xyz, gym = texture(uGPos, vUV - vec2(0.0, texel.y)).xyz;
    float footprint = max(min(length(gxp - pv.xyz), length(gxm - pv.xyz)),
                          min(length(gyp - pv.xyz), length(gym - pv.xyz)));
    float tolerance = max(clamp(0.02 * clip.w, 0.01, 0.06), min(footprint, 0.5));
    // Bilinear history, reprojected as a MOTION VECTOR from this pixel's centre
    // (Thema 134 §4.2). A point-sampled lookup rounds a sub-half-texel motion
    // onto the same texel every frame, so the history stood still on screen and
    // the shadow trailed ~9x the per-frame motion behind a panning camera.
    // Bilinear on the absolute prevUV is no fix either: the RGBA16F position
    // puts prevUV ~1/4 texel off the centre even for a still camera, and the
    // repeated blend dissolved sharp contact edges. curUV comes from the SAME
    // quantised position through the same formula, so that error cancels and a
    // still camera lands exactly on the texel centre. Each tap only counts if
    // it was written for this surface; the kept weights are renormalised.
    vec4 cclip = uCurViewProj * vec4(pv.xyz, 1.0);
    vec2 curUV = (cclip.xy / cclip.w) * 0.5 + 0.5;
    vec2 hsz   = vec2(textureSize(uHistory, 0));
    vec2 hf    = (vUV + (prevUV - curUV)) * hsz - 0.5;
    vec2 hb    = floor(hf);
    vec2 ht    = hf - hb;
    float hAcc = 0.0, hWsum = 0.0;
    for (int j = 0; j < 4; ++j)
    {
        vec2  o  = vec2(float(j & 1), float(j >> 1));
        vec2  bw = mix(1.0 - ht, ht, o);
        vec4  h  = texture(uHistory, (hb + o + 0.5) / hsz);
        if (length(pv.xyz - h.rgb) < tolerance) { hAcc += h.a * bw.x * bw.y; hWsum += bw.x * bw.y; }
    }
    float histA = hWsum > 1e-3 ? hAcc / hWsum : 0.0;
    float w = hWsum > 1e-3 ? clamp(uBlend.x, 0.0, 0.98) : 0.0;
    // Neighbourhood clamp: guards OCCLUDER motion (the position check above
    // only covers receiver/camera motion) — moved shadows update in 1-2 frames
    // instead of smearing for ~30. The box is the range of the 3x3 MEANS of raw
    // over a 5x5 footprint, widened by 0.1 — not raw min/max: raw is one or two
    // binary rays per pixel, so inside a penumbra a 3x3 of raw taps is all-0 or all-1 by
    // chance every few frames, and clamping to that reset the history to 0/1
    // over and over (the torn, crawling edge of Thema 131).
    float r5[25];
    for (int y = 0; y < 5; ++y)
        for (int x = 0; x < 5; ++x)
            r5[y * 5 + x] = texture(uRaw, vUV + vec2(float(x - 2), float(y - 2)) * texel).r;
    float nMin = 1.0, nMax = 0.0;
    for (int cy = 1; cy <= 3; ++cy)
        for (int cx = 1; cx <= 3; ++cx)
        {
            float s = 0.0;
            for (int y = -1; y <= 1; ++y)
                for (int x = -1; x <= 1; ++x)
                    s += r5[(cy + y) * 5 + cx + x];
            nMin = min(nMin, s / 9.0);
            nMax = max(nMax, s / 9.0);
        }
    FragColor = vec4(pv.xyz, mix(rawV, clamp(histA, nMin - 0.1, nMax + 0.1), w));
}
