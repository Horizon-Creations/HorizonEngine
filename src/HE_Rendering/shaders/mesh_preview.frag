#version 450

// Mesh preview / thumbnail fragment shader for the Vulkan backend.
// Twin of kMeshPreviewFS (OpenGLRenderer.cpp:710) and of the PSMain half of
// kMeshPreviewHLSL (D3D_Shared/HlslSources.h). Kept line-for-line comparable
// with those two on purpose: this shader exists so a Content-Browser tile can
// be A/B'd against OpenGL's, and a divergence here would make that comparison
// unreadable.

layout(location = 0) in vec3 vNormal;
layout(location = 1) in vec2 vUV;
layout(location = 2) in vec3 vWorldPos;

layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform MeshPreviewCB {
    vec4 uColor;    // rgb = base colour
    vec4 uCamPos;   // xyz = camera position
    vec4 uPbr;      // x = metallic, y = roughness, z = hasTexture (0/1)
    vec4 uSun;      // xyz points TOWARD the light, w > 0 arms it
    vec4 uSunColor; // rgb
    vec4 uAmbient;  // rgb
};
layout(set = 0, binding = 1) uniform sampler2D uTex;

void main()
{
    // uSun.w == 0 keeps the fixed studio light every cached tile on disk was
    // rendered with, so a tile does not change the day someone puts a sky
    // behind a preview. w > 0 arms the world's real sun (world preview).
    bool lit  = uSun.w > 0.0;
    vec3 L    = lit ? normalize(uSun.xyz) : normalize(vec3(0.45, 0.75, 0.55));
    vec3 lc   = lit ? uSunColor.rgb : vec3(1.0);
    vec3 amb  = lit ? uAmbient.rgb  : vec3(0.32);

    vec3  N = normalize(vNormal);
    vec3  V = normalize(uCamPos.xyz - vWorldPos);
    vec3  H = normalize(L + V);
    float diff  = max(dot(N, L), 0.0);
    float rough = clamp(uPbr.y, 0.05, 1.0);
    float spec  = pow(max(dot(N, H), 0.0), mix(128.0, 8.0, rough))
                * (1.0 - rough) * mix(0.25, 1.0, clamp(uPbr.x, 0.0, 1.0));

    vec3 albedo  = (uPbr.z > 0.5) ? texture(uTex, vUV).rgb * uColor.rgb
                                  : uColor.rgb;
    vec3 lightIn = amb + lc * (lit ? diff : 0.68 * diff);
    FragColor = vec4(albedo * lightIn + vec3(spec) * lc, 1.0);
}
