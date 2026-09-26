#version 450

// Skinned world-preview vertex shader (IRenderer::RenderWorldPreview, Vulkan).
// world_preview.vert plus the skin: the same vertex bindings as skinned.vert
// (interleaved pos/normal/uv, bone ids, bone weights), bone matrices from a
// dynamic UBO at set 0, binding 2 — one 128-matrix block per skinned draw.

layout(location = 0) in vec3  aPos;
layout(location = 1) in vec3  aNormal;
layout(location = 2) in vec2  aUV;
layout(location = 3) in uvec4 aBoneIds;
layout(location = 4) in vec4  aBoneWgts;

layout(set = 0, binding = 0) uniform PerObject {
    mat4 uMVP;
    mat4 uModel;
    vec4 uColor;
    vec4 uPBR;
};
layout(set = 0, binding = 2) uniform BonesCB {
    mat4 uBoneMatrices[128];
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main()
{
    mat4 skin = aBoneWgts.x * uBoneMatrices[aBoneIds.x]
              + aBoneWgts.y * uBoneMatrices[aBoneIds.y]
              + aBoneWgts.z * uBoneMatrices[aBoneIds.z]
              + aBoneWgts.w * uBoneMatrices[aBoneIds.w];
    vec4 skinnedPos = skin * vec4(aPos, 1.0);
    vWorldPos   = (uModel * skinnedPos).xyz;
    vNormal     = mat3(uModel) * mat3(skin) * aNormal;
    vUV         = aUV;
    gl_Position = uMVP * skinnedPos;
}
