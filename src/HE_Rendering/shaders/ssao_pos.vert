#version 450

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Push constants carry per-draw MVP and ModelView matrices (128 bytes total).
// The same PushConstants layout as the scene pass (mvp + model) is reused:
// uMVP       = kVulkanClipFix * proj * view * model  → clip-space position
// uModelView = view * modelMatrix                     → view-space position
layout(push_constant) uniform PosPC {
    mat4 uMVP;        // clip-space transform  (kVulkanClipFix * proj * view * model)
    mat4 uModelView;  // view-space transform  (view * model)
};

layout(location = 0) out vec3 vViewPos;

void main()
{
    vViewPos    = (uModelView * vec4(aPos, 1.0)).xyz;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
