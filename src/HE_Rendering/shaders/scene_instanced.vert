#version 450

// Instanced scene vertex shader for the Vulkan backend (A3). Same interface as
// scene.vert, but the per-instance mvp + model arrive as per-instance vertex
// attributes (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE) instead of push constants,
// so ONE vkCmdDrawIndexed(indexCount, instanceCount) replaces the per-instance
// draw loop. Each mat4 is four vec4 attributes = the four glm columns; GLSL's
// mat4(c0,c1,c2,c3) constructor is column-major, so they map directly (no
// transpose), matching the push-constant path's math. The CPU already remaps the
// projection to Vulkan clip space (see kVulkanClipFix), so mvp is used as-is.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Per-instance (binding 1): mvp columns at 3..6, model columns at 7..10.
layout(location = 3)  in vec4 iMvp0;
layout(location = 4)  in vec4 iMvp1;
layout(location = 5)  in vec4 iMvp2;
layout(location = 6)  in vec4 iMvp3;
layout(location = 7)  in vec4 iModel0;
layout(location = 8)  in vec4 iModel1;
layout(location = 9)  in vec4 iModel2;
layout(location = 10) in vec4 iModel3;

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUV;

void main()
{
    mat4 model = mat4(iModel0, iModel1, iModel2, iModel3);
    mat4 mvp   = mat4(iMvp0, iMvp1, iMvp2, iMvp3);
    vWorldPos   = (model * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(model) * aNormal;
    vUV         = aUV;
    gl_Position = mvp * vec4(aPos, 1.0);
}
