#version 450

// Deferred G-buffer fragment for BUILT-IN materials (Thema 150,
// docs/deferred-renderer-plan.md §10.4 point 4). The Vulkan twin of D3D11's and
// D3D12's GBufPS: surface ATTRIBUTES instead of shading, fed by scene.vert /
// scene_instanced.vert and reading scene.frag's per-draw material block and
// base-colour texture at the same bindings (the pipeline uses
// m_scenePipelineLayout). The fullscreen resolve
// (MaterialShaderLibrary::deferredResolve → heLitP) lights them, the same code
// graph materials shade with.
//
// Base colour by GL's and Metal's rule (kGBufFS / gbufferMain): texture ×
// colour, where the CPU passes 1.0 under a texture and 0.55 grey for a mesh
// without a material (VulkanRenderer::DrawScene, gbufferPass). Specular 0.5 =
// scene.frag's dielectric F0 0.04, emissive 0, material AO 1. GB3 is the
// fragment depth the resolve reconstructs the world position from (plan §10.5
// way B: the depth attachment itself stays a pure attachment).

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;

layout(location = 0) out vec4 oGB0; // rgb BaseColor, a Metallic        (R8G8B8A8_SRGB)
layout(location = 1) out vec4 oGB1; // rg oct normal, b Roughness, a Spec (RGBA16F)
layout(location = 2) out vec4 oGB2; // rgb Emissive, a Material-AO       (RGBA16F)
layout(location = 3) out vec4 oGB3; // r = gl_FragCoord.z                (R32F)

// scene.frag's MatUBO (set 0, binding 2, dynamic offset per draw).
layout(set = 0, binding = 2) uniform MatUBO {
    vec4 baseColorMet;  // rgb = baseColor, a = metallic
    vec4 roughPad;      // x = roughness, y = opacity, z = hasTexture, w = noShadow
} mat_ubo;

layout(set = 2, binding = 0) uniform sampler2D uAlbedo;

// scene.frag's giOctEncode — the signed octahedral map the resolve's
// heOctDecode inverts.
vec2 giOctEncode(vec3 n)
{
    vec2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    vec2 signP = vec2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

void main()
{
    vec3 base = (mat_ubo.roughPad.z > 0.5) ? texture(uAlbedo, vUV).rgb * mat_ubo.baseColorMet.rgb
                                           : mat_ubo.baseColorMet.rgb;
    vec3 N = normalize(vNormal);
    oGB0 = vec4(base, clamp(mat_ubo.baseColorMet.a, 0.0, 1.0));
    oGB1 = vec4(giOctEncode(N) * 0.5 + 0.5, clamp(mat_ubo.roughPad.x, 0.0, 1.0), 0.5);
    oGB2 = vec4(0.0, 0.0, 0.0, 1.0);
    oGB3 = vec4(gl_FragCoord.z, 0.0, 0.0, 0.0);
}
