#version 450

// Skinned vertex shader for the Vulkan backend.
// Inputs: same per-vertex attributes as scene.vert plus bone indices/weights.
// Per-object data arrives as push constants (MVP + model) — identical to scene.vert.
// Bone matrices arrive in a separate UBO at set=1, binding=0 (VERTEX stage).
// Outputs: vWorldPos (loc=0), vNormal (loc=1) — exactly what scene.frag expects.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;
layout(location = 3) in uvec4 aBoneIds;
layout(location = 4) in vec4  aBoneWgts;

// Per-object transforms via push constants (identical range to scene.vert).
layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    mat4 uModel;
} pc;

// Per-draw bone matrices: set=1, binding=0. Max 128 joints.
layout(set = 1, binding = 0) uniform BonesCB {
    mat4 uBoneMatrices[128];
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

void main()
{
    // Build the weighted skin matrix from up to 4 influences.
    mat4 skin = aBoneWgts.x * uBoneMatrices[aBoneIds.x]
              + aBoneWgts.y * uBoneMatrices[aBoneIds.y]
              + aBoneWgts.z * uBoneMatrices[aBoneIds.z]
              + aBoneWgts.w * uBoneMatrices[aBoneIds.w];

    // Skin position and normal in local space, then apply model/MVP.
    vec4 skinnedPos    = skin * vec4(aPos, 1.0);
    vec3 skinnedNormal = mat3(skin) * aNormal;

    vWorldPos   = (pc.uModel * skinnedPos).xyz;
    vNormal     = mat3(pc.uModel) * skinnedNormal;
    gl_Position = pc.uMVP * skinnedPos;
}
