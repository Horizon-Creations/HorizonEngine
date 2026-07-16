#version 450
// GI shadow temporal accumulation: reproject via last frame's (clip-fixed)
// viewProj; history carries the world position (rgb) + shadow scalar (a).
// Tolerance deliberately TIGHT (Metal lesson: loose depth-scaled tolerances
// accept wrong-surface reprojects at cube edges). prevViewProj INCLUDES
// kVulkanClipFix, so ndc*0.5+0.5 lands directly in top-left-origin UV space —
// no extra y-flip (matches how the G-buffer itself was rasterized).
layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform sampler2D uGPos;
layout(set = 0, binding = 1) uniform sampler2D uRaw;
layout(set = 0, binding = 2) uniform sampler2D uHistory;
layout(set = 0, binding = 3) uniform GiTemporalUBO {
    mat4 uPrevViewProj;
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

    vec4  hist      = texture(uHistory, prevUV);
    float posError  = length(pv.xyz - hist.rgb);
    float tolerance = clamp(0.02 * clip.w, 0.01, 0.06);
    float w = (posError < tolerance) ? clamp(uBlend.x, 0.0, 0.98) : 0.0;
    FragColor = vec4(pv.xyz, mix(rawV, hist.a, w));
}
