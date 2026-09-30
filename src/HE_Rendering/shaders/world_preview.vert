#version 450

// World-preview vertex shader (IRenderer::RenderWorldPreview, Vulkan).
// The preview's own small pipeline, not scene.vert: per-draw data comes from a
// dynamic UBO (set 0, binding 0) instead of push constants, because the
// preview fragment shader needs the colour and PBR scalars next to the two
// matrices and 128 bytes of push constants hold only the matrices.
// Outputs match world_preview.frag.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(set = 0, binding = 0) uniform PerObject {
    mat4 uMVP;
    mat4 uModel;
    vec4 uColor;   // rgb = base colour, a = has texture
    vec4 uPBR;     // x = metallic, y = roughness, w = 1 → skinned lighting
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main()
{
    vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(uModel) * aNormal;
    vUV         = aUV;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
