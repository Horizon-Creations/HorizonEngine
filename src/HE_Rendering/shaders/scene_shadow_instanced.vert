#version 450

// Instanced twin of scene_shadow.vert: one vkCmdDrawIndexed per run of
// same-mesh shadow casters instead of one draw per caster. The per-instance
// mvp (cascadeVP * model, already in Vulkan clip space via kVulkanClipFix —
// the product the push-constant path gets) arrives as four vec4 attributes at
// locations 3..6 from binding 1 (VK_VERTEX_INPUT_RATE_INSTANCE, the scene
// pass's 128-byte instance buffer; the model half at bytes 64..127 is not read
// here). mat4(c0,c1,c2,c3) is column-major, so the glm columns map directly.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 3) in vec4 iMvp0;
layout(location = 4) in vec4 iMvp1;
layout(location = 5) in vec4 iMvp2;
layout(location = 6) in vec4 iMvp3;

void main()
{
    gl_Position = mat4(iMvp0, iMvp1, iMvp2, iMvp3) * vec4(aPos, 1.0);
}
