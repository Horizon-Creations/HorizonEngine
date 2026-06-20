#version 450

// Fullscreen SSAO pass.  Vertex shader: postfx.vert (attribute-less triangle).
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

// UBO layout mirrors BuildSSAOKernel / SSAOCB struct in VulkanRenderer.cpp.
// std140: mat4(64) + vec4(16) + vec4(16) + vec4[32](512) = 608 bytes.
layout(set = 0, binding = 0) uniform SSAOCB {
    mat4 uSSAOProj;           // kVulkanClipFix * camera.projection  (view already applied in position prepass)
    vec4 uSSAONoiseScale;     // xy = viewport / 4.0  (tiles the 4x4 noise texture)
    vec4 uSSAOParams;         // x = radius, y = bias, z = intensity, w = unused
    vec4 uSSAOKernel[32];     // hemisphere kernel in view space (w unused)
};
layout(set = 0, binding = 1) uniform sampler2D uViewPos;   // RGBA16F: view-space pos, a = valid
layout(set = 0, binding = 2) uniform sampler2D uNoise;     // 4x4 random rotation vectors (xy in [-1,1])

void main()
{
    vec4 pv = texture(uViewPos, vUV);
    if (pv.a < 0.5) { FragColor = vec4(1.0); return; }  // background -> unoccluded
    vec3 P = pv.xyz;

    // View-space normal reconstructed from screen-space position differences.
    // Pick the nearer neighbour on each axis so silhouette edges don't bleed.
    vec2 texel = 1.0 / vec2(textureSize(uViewPos, 0));
    vec3 Pr = texture(uViewPos, vUV + vec2(texel.x, 0.0)).xyz;
    vec3 Pl = texture(uViewPos, vUV - vec2(texel.x, 0.0)).xyz;
    vec3 Pu = texture(uViewPos, vUV + vec2(0.0, texel.y)).xyz;
    vec3 Pd = texture(uViewPos, vUV - vec2(0.0, texel.y)).xyz;
    vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddx, ddy));
    if (N.z < 0.0) N = -N;   // ensure the normal faces the camera (+Z in view space)

    // TBN basis from the noise texture (random rotation per pixel).
    vec3 randv = texture(uNoise, vUV * uSSAONoiseScale.xy).xyz;
    vec3 T  = normalize(randv - N * dot(randv, N));  // Gram-Schmidt orthogonalisation
    vec3 B  = cross(N, T);
    mat3 TBN = mat3(T, B, N);

    float radius    = uSSAOParams.x;
    float bias      = uSSAOParams.y;
    float intensity = uSSAOParams.z;

    // SSAO: slope-invariant tangent-plane kernel (matches OpenGL/Metal backends).
    float occ = 0.0;
    for (int i = 0; i < 32; ++i)
    {
        // Orient the kernel sample into view space via TBN.
        vec3 sp = P + (TBN * uSSAOKernel[i].xyz) * radius;

        // Project sample to UV.
        // uSSAOProj = kVulkanClipFix * camera.projection  (no view factor needed:
        // both P and sp are already in view space, and kVulkanClipFix bakes Vulkan's
        // Y-flip so there is no manual ndc.y negation here).
        vec4 clip = uSSAOProj * vec4(sp, 1.0);
        vec2 suv  = clip.xy / clip.w * 0.5 + 0.5;
        if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;

        vec4 sv = texture(uViewPos, suv);
        if (sv.a < 0.5) continue;   // sample hit the background

        // Slope-invariant test: the neighbour must rise ABOVE the fragment's tangent plane.
        vec3  toOcc    = sv.xyz - P;
        float above    = dot(toOcc, N);
        float rangeChk = smoothstep(0.0, 1.0, radius / max(length(toOcc), 1e-4));
        occ += (above > bias ? 1.0 : 0.0) * rangeChk;
    }

    float ao = 1.0 - (occ / 32.0) * intensity;
    ao = max(ao, 0.5);   // conservative backstop -- matches GL/Metal
    FragColor = vec4(ao, ao, ao, 1.0);
}
