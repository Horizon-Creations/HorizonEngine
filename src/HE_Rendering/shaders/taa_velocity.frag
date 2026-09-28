#version 450

// Screen-space motion in TEXTURE-UV units (uvNow - uvPrev), so the resolve can
// subtract it from its own uv. Vulkan clip space already points y down
// (kVulkanClipFix), like texture v and like postfx.vert's vUV, so unlike the
// D3D/Metal twins there is no y flip here — the same formula as GL's, for the
// opposite reason (GL flips neither).

layout(location = 0) in vec4 vClipNow;
layout(location = 1) in vec4 vClipPrev;
layout(location = 0) out vec2 outVelocity;   // RG16F

void main()
{
    vec2 ndcNow  = vClipNow.xy  / max(vClipNow.w,  1e-6);
    vec2 ndcPrev = vClipPrev.xy / max(vClipPrev.w, 1e-6);
    outVelocity = (ndcNow - ndcPrev) * 0.5;
}
