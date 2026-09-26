#version 450

// World-preview fragment shader (IRenderer::RenderWorldPreview, Vulkan).
// GL's kMeshPreviewFS and kSkelPreviewFS in one (uPBR.w picks), the same
// numbers as the D3D backends' kWorldPreviewPSHLSL: the preview's fixed
// lighting — no shadow map, SSAO, IBL or fog is bound in a preview target.
// uSun.w > 0 arms the sun (xyz points TOWARD it); w == 0 keeps the studio light.

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(set = 0, binding = 0) uniform PerObject {
    mat4 uMVP;
    mat4 uModel;
    vec4 uColor;   // rgb = base colour, a = has texture
    vec4 uPBR;     // x = metallic, y = roughness, w = 1 → skinned lighting
};
layout(set = 0, binding = 1) uniform PreviewLight {
    vec4 uCamPos;
    vec4 uSun;
    vec4 uSunColor;
    vec4 uAmbient;
};
// The scene's per-mesh base-colour sets (m_albedoSetLayout), bound at set 1.
layout(set = 1, binding = 0) uniform sampler2D uAlbedo;

layout(location = 0) out vec4 outColor;

void main()
{
    bool  lit     = uSun.w > 0.0;
    bool  skinned = uPBR.w > 0.5;
    vec3  L   = lit ? normalize(uSun.xyz) : normalize(vec3(0.45, 0.75, 0.55));
    vec3  lc  = lit ? uSunColor.rgb : vec3(1.0);
    vec3  amb = lit ? uAmbient.rgb  : vec3(skinned ? 0.35 : 0.32);
    vec3  N   = normalize(vNormal);
    float diff = max(dot(N, L), 0.0);
    vec3  albedo = uColor.a > 0.5 ? texture(uAlbedo, vUV).rgb * uColor.rgb : uColor.rgb;
    if (skinned)
    {
        outColor = vec4(albedo * (amb + lc * (lit ? diff : 0.65 * diff)), 1.0);
        return;
    }
    vec3  V     = normalize(uCamPos.xyz - vWorldPos);
    vec3  H     = normalize(L + V);
    float rough = clamp(uPBR.y, 0.05, 1.0);
    float spec  = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough))
                * (1.0 - rough) * mix(0.25, 1.0, clamp(uPBR.x, 0.0, 1.0));
    vec3  lightIn = amb + lc * (lit ? diff : 0.68 * diff);
    outColor = vec4(albedo * lightIn + vec3(spec) * lc, 1.0);
}
