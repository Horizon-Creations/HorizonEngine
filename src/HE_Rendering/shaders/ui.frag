#version 450
// Same block as ui.vert. 128 bytes is all a Vulkan device guarantees, so the
// "Schicht 0" style is packed: the three extra colours as unorm8x4 (the target
// is 8-bit, so this costs nothing visible; out-of-range colours clamp to 0..1).
layout(push_constant) uniform UIPush {
    vec4  uRect;
    vec4  uColor;
    vec4  uUVRect;
    vec2  uViewport;
    vec2  uParams;        // x: 0 = solid color, 1 = font-atlas glyph, 2 = image; y: border width px
    vec4  uRotation;      // w: gradient angle, degrees clockwise from "down"
    vec4  uCornerRadius;  // px per corner: TL, TR, BR, BL
    vec4  uStyle;         // x: blur px (drop shadow), y: inner shadow blur px,
                          // z: gradient on, w: radial gradient
    uvec4 uColors;        // unorm8x4: x border, y gradient, z inner shadow
} pc;
// R8 font atlas (glyph coverage in .r) in mode 1, the quad's RGBA image in
// mode 2 (UNORM: UI colours are sRGB numbers, Thema 107) — solid quads ignore
// it, but a set must stay bound for every draw since the sampler is statically
// used by this shader.
layout(set = 0, binding = 0) uniform sampler2D uFontAtlas;
layout(location = 0) in  vec2 vUV;
layout(location = 1) in  vec2 vLocal;
layout(location = 0) out vec4 FragColor;
// One rounded box, four radii (TL, TR, BR, BL); `p` relative to the centre, y
// down. Same function as heRoundedBoxSDF in the GL kUIFS / the Metal path.
float heRoundedBoxSDF(vec2 p, vec2 halfSz, vec4 radii)
{
    float r = (p.x > 0.0) ? ((p.y > 0.0) ? radii.z : radii.y)
                          : ((p.y > 0.0) ? radii.w : radii.x);
    r = min(r, min(halfSz.x, halfSz.y));
    vec2 q = abs(p) - (halfSz - r);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - r;
}
float heMaxRadius(vec4 radii)
{
    return max(max(radii.x, radii.y), max(radii.z, radii.w));
}
// The rest is the GL kUIFS line for line, so a widget looks the same on both.
void main() {
    if (pc.uParams.x > 0.5 && pc.uParams.x < 1.5) {
        float a = texture(uFontAtlas, vUV).r;
        FragColor = vec4(pc.uColor.rgb, pc.uColor.a * a);
        return;
    }
    if (pc.uParams.x > 1.5) {
        vec4 t = texture(uFontAtlas, vUV);
        vec4 c = vec4(pc.uColor.rgb * t.rgb, pc.uColor.a * t.a);
        if (heMaxRadius(pc.uCornerRadius) <= 0.0) { FragColor = c; return; }
        // A rounded image is the solid path's SDF applied to the sampled alpha.
        float dd = heRoundedBoxSDF((vLocal - 0.5) * pc.uRect.zw, pc.uRect.zw * 0.5, pc.uCornerRadius);
        FragColor = vec4(c.rgb, c.a * clamp(0.5 - dd, 0.0, 1.0));
        return;
    }
    vec4 fill = pc.uColor;
    if (pc.uStyle.z > 0.5)
    {
        float t;
        if (pc.uStyle.w > 0.5)
        {
            // Radial: centre out to the farthest corner, in pixels.
            vec2 dpx = (vLocal - 0.5) * pc.uRect.zw;
            t = clamp(length(dpx) / max(1e-4, length(pc.uRect.zw * 0.5)), 0.0, 1.0);
        }
        else
        {
            float a = pc.uRotation.w * 0.017453292;
            vec2  dir = vec2(sin(a), cos(a));
            t = clamp(dot(vLocal - 0.5, dir) + 0.5, 0.0, 1.0);
        }
        fill = mix(pc.uColor, unpackUnorm4x8(pc.uColors.y), t);
    }
    float borderW = pc.uParams.y, blurPx = pc.uStyle.x, innerBlur = pc.uStyle.y;
    if (heMaxRadius(pc.uCornerRadius) <= 0.0 && borderW <= 0.0 &&
        blurPx <= 0.0 && innerBlur <= 0.0) { FragColor = fill; return; }
    // A blurred quad IS a drop shadow: the producer grew the rect by the blur.
    vec2 halfsz = pc.uRect.zw * 0.5 - blurPx;
    float d = heRoundedBoxSDF((vLocal - 0.5) * pc.uRect.zw, halfsz, pc.uCornerRadius);
    float cov = (blurPx > 0.0) ? (1.0 - smoothstep(-blurPx, blurPx, d))
                               : clamp(0.5 - d, 0.0, 1.0);
    if (blurPx > 0.0) { FragColor = vec4(fill.rgb, fill.a * cov); return; }
    if (innerBlur > 0.0)
    {
        vec4  ic = unpackUnorm4x8(pc.uColors.z);
        float t  = 1.0 - smoothstep(0.0, innerBlur, -d);
        float ia = ic.a * clamp(t, 0.0, 1.0);
        fill = vec4(mix(fill.rgb, ic.rgb, ia), fill.a);
    }
    if (borderW <= 0.0) { FragColor = vec4(fill.rgb, fill.a * cov); return; }
    vec4  bc    = unpackUnorm4x8(pc.uColors.x);
    float inner = clamp(0.5 - (d + borderW), 0.0, 1.0);
    vec3  rgb   = mix(bc.rgb, fill.rgb, inner);
    float a     = mix(bc.a, fill.a, inner);
    FragColor = vec4(rgb, a * cov);
}
