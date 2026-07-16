#version 450
// GI world-space G-buffer pre-pass (position + normal MRT, half-res). Rendered
// with the SAME camera/clip-fix as the scene pass (Metal aspect lesson: a
// mismatched camera misaligns the screen-space shadow mask → swimming).
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

// Same 128-byte push-constant shape as the scene/ssao_pos passes:
// uMVP = kVulkanClipFix * proj * view * model, uModel = model.
layout(push_constant) uniform GiGBufPC {
    mat4 uMVP;
    mat4 uModel;
};

layout(location = 0) out vec3 vWorldPos;
layout(location = 1) out vec3 vNormal;

void main()
{
    vWorldPos   = (uModel * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(uModel) * aNormal;
    gl_Position = uMVP * vec4(aPos, 1.0);
}
