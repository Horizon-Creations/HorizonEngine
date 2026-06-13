#version 450

// Scene vertex shader for the Vulkan backend. Mirrors the GL/Metal/D3D unlit
// pipeline. Per-object transforms arrive as push constants; the projection is
// already remapped to Vulkan clip space on the CPU (see kVulkanClipFix).

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    mat4 uModel;
} pc;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

void main()
{
    vWorldPos   = (pc.uModel * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(pc.uModel) * aNormal;
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
}
