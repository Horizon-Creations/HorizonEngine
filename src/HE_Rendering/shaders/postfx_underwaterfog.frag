// Underwater fog post pass (Thema 185 Schritt 2). GLSL twin of HlslSources.h's
// kUnderwaterFogHLSL — see that comment for the full derivation. Summary: per
// pixel, reconstruct the world position the depth buffer names (same
// depthParams convention as MaterialShaderLibrary.cpp's buildDeferredResolveSource),
// then the length of the segment of [camera, that point] lying on the wet side
// of the flat plane y = surfaceY. Output is premultiplied (rgb = tint *
// fogAmount, a = fogAmount) for a ONE/ONE_MINUS_SRC_ALPHA blend straight onto
// the HDR scene colour — no ping-pong target.
#version 450
layout(location=0) in vec2 vUV;
layout(location=0) out vec4 outColor;
layout(set=0, binding=0) uniform sampler2D uDepth;
layout(push_constant) uniform PC
{
    mat4 invViewProj;
    vec4 depthParams;    // x = uv→NDC y sign, y/z = ndc-z scale/bias, w unused
    vec4 camPosSurfaceY; // xyz = camera world pos, w = surfaceY
    vec4 tintDensity;    // rgb = resolved tint, a = per-metre density
} pc;

void main()
{
    float d = texture(uDepth, vUV).r;
    vec4 clip = vec4(vUV.x * 2.0 - 1.0,
                     (vUV.y * 2.0 - 1.0) * pc.depthParams.x,
                     d * pc.depthParams.y + pc.depthParams.z, 1.0);
    vec4 wp = pc.invViewProj * clip;
    vec3 P = wp.xyz / max(wp.w, 1e-8);

    vec3  camPos   = pc.camPosSurfaceY.xyz;
    float surfaceY = pc.camPosSurfaceY.w;
    bool camBelow = camPos.y < surfaceY;
    bool pBelow   = P.y < surfaceY;

    float segment;
    if (camBelow == pBelow)
    {
        segment = camBelow ? length(camPos - P) : 0.0;
    }
    else
    {
        float t = (surfaceY - camPos.y) / (P.y - camPos.y);
        vec3 crossPt = camPos + t * (P - camPos);
        segment = camBelow ? length(camPos - crossPt) : length(crossPt - P);
    }

    float fogAmount = 1.0 - exp(-pc.tintDensity.a * segment);
    outColor = vec4(pc.tintDensity.rgb * fogAmount, fogAmount);
}
