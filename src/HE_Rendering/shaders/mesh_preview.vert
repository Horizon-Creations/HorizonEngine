#version 450

// Mesh preview / thumbnail vertex shader for the Vulkan backend.
// Twin of kMeshPreviewVS (OpenGLRenderer.cpp:690) and of the VSMain half of
// kMeshPreviewHLSL (D3D_Shared/HlslSources.h). The projection is already
// remapped to Vulkan clip space on the CPU (see kVulkanClipFix), exactly as
// scene.vert expects it — HE::meshOrbit hands back a GL-convention matrix and
// the backend pre-multiplies the fix-up.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Push constants rather than a UBO for the two matrices, mirroring scene.vert:
// a thumbnail is one draw, so a descriptor set for per-draw transforms would be
// pure ceremony. The shading parameters ride in the set-0 UBO below because the
// 128-byte push-constant floor cannot hold them as well.
layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    mat4 uModel;
} pc;

layout(location = 0) out vec3 vNormal;
layout(location = 1) out vec2 vUV;
layout(location = 2) out vec3 vWorldPos;

void main()
{
    vNormal     = mat3(pc.uModel) * aNormal;
    vUV         = aUV;
    vWorldPos   = (pc.uModel * vec4(aPos, 1.0)).xyz;
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
}
