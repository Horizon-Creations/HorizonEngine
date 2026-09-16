#version 450

// Depth-only vertex shader for the Vulkan cascade shadow pass (one draw per
// caster per cascade layer) and the camera depth pre-pass of the decals. The
// push constant uMVP carries cascadeVP * model — or cameraVP * model — already
// in Vulkan clip space via kVulkanClipFix.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(push_constant) uniform PushConstants {
    mat4 uMVP;
    mat4 uModel;
} pc;

void main()
{
    gl_Position = pc.uMVP * vec4(aPos, 1.0);
}
