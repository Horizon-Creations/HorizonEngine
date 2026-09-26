#version 450

// Instanced twin of ssao_pos.vert: one draw per run of same-mesh AO
// contributors. The CPU writes the SAME two products the push-constant path
// carries, per instance, into the scene pass's 128-byte instance buffer
// (binding 1, VK_VERTEX_INPUT_RATE_INSTANCE):
//   locations 3..6  = kVulkanClipFix * proj * view * model  → clip-space position
//   locations 7..10 = view * model                          → view-space position
// so the instanced pre-pass is the looped pre-pass, bit for bit.

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

layout(location = 3)  in vec4 iMvp0;
layout(location = 4)  in vec4 iMvp1;
layout(location = 5)  in vec4 iMvp2;
layout(location = 6)  in vec4 iMvp3;
layout(location = 7)  in vec4 iModelView0;
layout(location = 8)  in vec4 iModelView1;
layout(location = 9)  in vec4 iModelView2;
layout(location = 10) in vec4 iModelView3;

layout(location = 0) out vec3 vViewPos;

void main()
{
    mat4 mvp       = mat4(iMvp0, iMvp1, iMvp2, iMvp3);
    mat4 modelView = mat4(iModelView0, iModelView1, iModelView2, iModelView3);
    vViewPos    = (modelView * vec4(aPos, 1.0)).xyz;
    gl_Position = mvp * vec4(aPos, 1.0);
}
