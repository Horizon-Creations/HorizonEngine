#version 450
// Instanced twin of gi_gbuf.vert: one draw per run of same-mesh AO
// contributors. Per instance the CPU writes the push-constant path's two
// matrices into the scene pass's 128-byte instance buffer (binding 1,
// VK_VERTEX_INPUT_RATE_INSTANCE): uMVP at locations 3..6, uModel at 7..10 —
// the same layout scene_instanced.vert reads, and the same products, so the
// instanced G-buffer is the looped one.
layout(location = 0) in vec3 aPos;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec2 aUV;

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

void main()
{
    mat4 model  = mat4(iModel0, iModel1, iModel2, iModel3);
    mat4 mvp    = mat4(iMvp0, iMvp1, iMvp2, iMvp3);
    vWorldPos   = (model * vec4(aPos, 1.0)).xyz;
    vNormal     = mat3(model) * aNormal;
    gl_Position = mvp * vec4(aPos, 1.0);
}
