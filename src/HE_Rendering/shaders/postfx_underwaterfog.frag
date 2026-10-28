// Underwater fog post pass (Thema 185 Schritt 2). GLSL twin of HlslSources.h's
// kUnderwaterFogHLSL — see that comment for the full derivation. Summary: per
// pixel, reconstruct the world position the depth buffer names (same
// depthParams convention as MaterialShaderLibrary.cpp's buildDeferredResolveSource),
// then the length of the segment of [camera, that point] lying on the wet side
// of the flat plane y = surfaceY. Output is premultiplied (rgb = tint *
// fogAmount, a = fogAmount) for a ONE/ONE_MINUS_SRC_ALPHA blend straight onto
// the HDR scene colour — no ping-pong target.
//
// The surface is the plane at surfParams.w plus, with a landscape's water tessellation on, the wave AT
// each point (heSurfaceH, the text of HE::underwaterSurfaceFunctions(GLSL) in UnderwaterSurfaceShared.h:
// tests/test_water_query.cpp reads this file back and compares), so the waterline follows the waves.
// The 128 B push-constant block is full: depthParams is not sent, Vulkan's is always 1 / 1 / 0.
#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uDepth;
layout(push_constant) uniform PC
{
    mat4 invViewProj;
    vec4 camPosSurfaceY; // xyz = camera world pos, w = surfaceY (the surface under the camera)
    vec4 tintDensity;    // rgb = resolved tint, a = per-metre density
    vec4 surfWave;       // the wind on the tessellated surface: turn (rad), length / speed / steepness factor
    vec4 surfParams;     // height (0 = flat plane), shore width, wave clock (s), flat level
} pc;

const vec3 kDepthParams = vec3(1.0, 1.0, 0.0);   // uv→NDC y sign, ndc-z scale / bias (kVulkanClipFix)

// SURF-BEGIN
float heTrain(vec2 xz, float dirDeg, float speed, float len, float steep, vec4 wv, vec4 sp)
{
    float L = len * wv.y;
    float k = 6.28318531 / L;
    float rad = dirDeg * 0.0174532925 + wv.x;
    float ph = k * (cos(rad) * xz.x + sin(rad) * xz.y - speed * wv.z * sp.z);
    return steep * wv.w / k * sin(ph);
}
float heSurfaceH(vec2 xz, float groundDepth, vec4 wv, vec4 sp)
{
    if (sp.x <= 0.0) return 0.0;
    float u = clamp(groundDepth / clamp(sp.y, 0.1, 3.0), 0.0, 1.0);
    float shore = u * u * (3.0 - 2.0 * u);
    float h = heTrain(xz, 20.0, 1.0, 14.0, 0.0700000003, wv, sp) + heTrain(xz, 335.0, 0.800000012, 5.0, 0.100000001, wv, sp) + heTrain(xz, 70.0, 0.5, 1.60000002, 0.0900000036, wv, sp);
    return sp.x * shore * h;
}
// SURF-END

void main()
{
    float d = texture(uDepth, vUV).r;
    vec4 clip = vec4(vUV.x * 2.0 - 1.0,
                     (vUV.y * 2.0 - 1.0) * kDepthParams.x,
                     d * kDepthParams.y + kDepthParams.z, 1.0);
    vec4 wp = pc.invViewProj * clip;
    vec3 P = wp.xyz / max(wp.w, 1e-8);

    vec3  camPos   = pc.camPosSurfaceY.xyz;
    float surfaceY = pc.camPosSurfaceY.w;
    float level    = pc.surfParams.w;
    float gDepth   = max(level - P.y, 0.0);
    float sP       = level + heSurfaceH(P.xz, gDepth, pc.surfWave, pc.surfParams);
    bool camBelow = camPos.y < surfaceY;
    bool pBelow   = P.y < sP;

    // This pixel's point on the near plane (d = 0 through the same mapping): a DRY camera
    // (the waterline band, Thema 185 Schritt 5) fogs only where the near plane sliced the
    // water sheet off, never what lies under the translucent surface (kUnderwaterFogHLSL).
    vec4 wn = pc.invViewProj * vec4(clip.xy, kDepthParams.z, 1.0);
    vec3 nearPt = wn.xyz / max(wn.w, 1e-8);
    bool nearBelow = nearPt.y < level + heSurfaceH(nearPt.xz, 1000.0, pc.surfWave, pc.surfParams);

    float segment;
    if (camBelow == pBelow)
    {
        segment = camBelow ? length(camPos - P) : 0.0;
    }
    else
    {
        // Where the ray meets the displaced surface: a few steps of "intersect the plane at the height the
        // wave has where the last guess landed" (the surface is gently sloped, it settles at once).
        float sY = camBelow ? surfaceY : sP;
        vec3 crossPt = camPos;
        for (int it = 0; it < 3; ++it)
        {
            float t = clamp((sY - camPos.y) / (P.y - camPos.y), 0.0, 1.0);
            crossPt = camPos + t * (P - camPos);
            sY = level + heSurfaceH(crossPt.xz, gDepth, pc.surfWave, pc.surfParams);
        }
        segment = camBelow ? length(camPos - crossPt) : (nearBelow ? length(crossPt - P) : 0.0);
    }

    float fogAmount = 1.0 - exp(-pc.tintDensity.a * segment);
    outColor = vec4(pc.tintDensity.rgb * fogAmount, fogAmount);
}
