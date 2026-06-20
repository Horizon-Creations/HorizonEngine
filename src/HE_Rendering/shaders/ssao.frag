#version 450

// Fullscreen SSAO/HBAO/GTAO pass.  Vertex shader: postfx.vert (attribute-less triangle).
layout(location = 0) in  vec2 vUV;
layout(location = 0) out vec4 FragColor;

// UBO layout mirrors SSAOCB struct in VulkanRenderer.cpp.
// std140: mat4(64) + vec4(16) + vec4(16) + vec4[32](512) = 608 bytes.
layout(set = 0, binding = 0) uniform SSAOCB {
    mat4 uSSAOProj;           // kVulkanClipFix * camera.projection
    vec4 uSSAONoiseScale;     // xy = viewport / 4.0
    vec4 uSSAOParams;         // x=radius, y=bias, z=intensity, w=method(0=SSAO,1=HBAO,2=GTAO)
    vec4 uSSAOKernel[32];
};
layout(set = 0, binding = 1) uniform sampler2D uViewPos;
layout(set = 0, binding = 2) uniform sampler2D uNoise;

const float PI      = 3.14159265359;
const float TWO_PI  = 6.28318530718;
const float HALF_PI = 1.57079632679;

uint hbaoSectors(float minH, float maxH, uint mask)
{
    uint startBit = min(uint(clamp(minH, 0.0, 1.0) * 32.0), 31u);
    uint count    = uint(ceil(clamp(maxH - minH, 0.0, 1.0) * 32.0));
    uint bits     = (count > 0u) ? (0xFFFFFFFFu >> (32u - count)) : 0u;
    return mask | (bits << startBit);
}
float ign(vec2 p) { return fract(52.9829189 * fract(0.06711056 * p.x + 0.00583715 * p.y)); }

void main()
{
    vec4 pv = texture(uViewPos, vUV);
    if (pv.a < 0.5) { FragColor = vec4(1.0); return; }
    vec3 P = pv.xyz;

    vec2 texel = 1.0 / vec2(textureSize(uViewPos, 0));
    vec3 Pr = texture(uViewPos, vUV + vec2(texel.x, 0.0)).xyz;
    vec3 Pl = texture(uViewPos, vUV - vec2(texel.x, 0.0)).xyz;
    vec3 Pu = texture(uViewPos, vUV + vec2(0.0, texel.y)).xyz;
    vec3 Pd = texture(uViewPos, vUV - vec2(0.0, texel.y)).xyz;
    vec3 ddx = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    vec3 ddy = (abs(Pu.z - P.z) < abs(P.z - Pd.z)) ? (Pu - P) : (P - Pd);
    vec3 N = normalize(cross(ddx, ddy));
    if (N.z < 0.0) N = -N;

    float radius    = uSSAOParams.x;
    float bias      = uSSAOParams.y;
    float intensity = uSSAOParams.z;
    int   method    = int(uSSAOParams.w);
    float ao;

    if (method == 1)
    {
        // HBAO: horizon-based AO via 32-sector visibility bitmask
        const int   SLICES    = 3;
        const int   STEPS     = 8;
        const float THICKNESS = 0.5;
        vec3  V = normalize(-P);
        float jitter = ign(gl_FragCoord.xy) - 0.5;
        float depthScale = 0.5 * radius / max(-P.z, 1e-4);
        float visibility = 0.0;
        for (int s = 0; s < SLICES; ++s)
        {
            float phi     = (float(s) + jitter) * (TWO_PI / float(SLICES));
            vec2  omega   = vec2(cos(phi), sin(phi));
            vec3  dir     = vec3(omega, 0.0);
            vec3  orthoDir = dir - dot(dir, V) * V;
            vec3  axis    = cross(dir, V);
            vec3  projN   = N - axis * dot(N, axis);
            float projLen = length(projN);
            if (projLen < 1e-5) { visibility += 1.0; continue; }
            float nAng    = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, 0.0, 1.0));
            vec2  omegaUV = vec2(uSSAOProj[0][0] * omega.x, uSSAOProj[1][1] * omega.y);
            uint  occ     = 0u;
            for (int k = 0; k < STEPS; ++k)
            {
                float t   = (float(k) + jitter) / float(STEPS) + 0.01;
                vec2  sUV = vUV - t * depthScale * omegaUV;
                vec4  sp  = texture(uViewPos, sUV);
                if (sp.a < 0.5) continue;
                vec3  d   = sp.xyz - P;
                float len = length(d);
                vec2  fb;
                fb.x = dot(d / max(len, 1e-5), V);
                fb.y = dot(normalize(d - V * THICKNESS), V);
                fb   = acos(clamp(fb, -1.0, 1.0));
                fb   = clamp((fb + nAng + HALF_PI) / PI, 0.0, 1.0);
                occ  = hbaoSectors(min(fb.x, fb.y), max(fb.x, fb.y), occ);
            }
            visibility += 1.0 - float(bitCount(occ)) / 32.0;
        }
        visibility /= float(SLICES);
        ao = 1.0 - (1.0 - visibility) * intensity;
        ao = max(ao, 0.1);
    }
    else if (method == 2)
    {
        // GTAO: analytic horizon-arc ambient occlusion
        const int SLICES = 3;
        const int STEPS  = 8;
        vec3  V = normalize(-P);
        float jitter = ign(gl_FragCoord.xy);
        float depthScale = 0.5 * radius / max(-P.z, 1e-4);
        float visAccum = 0.0;
        for (int s = 0; s < SLICES; ++s)
        {
            float phi     = (float(s) + jitter) * (PI / float(SLICES));
            vec2  omega   = vec2(cos(phi), sin(phi));
            vec3  dir     = vec3(omega, 0.0);
            vec3  axis    = cross(dir, V);
            float axisLen = length(axis);
            if (axisLen < 1e-5) { visAccum += 1.0; continue; }
            axis /= axisLen;
            vec3  orthoDir = normalize(dir - dot(dir, V) * V);
            vec3  projN    = N - axis * dot(N, axis);
            float projLen  = length(projN);
            if (projLen < 1e-5) continue;
            float gamma    = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, -1.0, 1.0));
            vec2  omegaUV  = vec2(uSSAOProj[0][0] * omega.x, uSSAOProj[1][1] * omega.y);
            float cH1 = 0.0;
            float cH2 = 0.0;
            for (int k = 0; k < STEPS; ++k)
            {
                float t = (float(k) + jitter) / float(STEPS) + 0.02;
                vec4  sp1 = texture(uViewPos, vUV + t * depthScale * omegaUV);
                if (sp1.a >= 0.5) {
                    vec3 d = sp1.xyz - P; float len = length(d);
                    float fall = clamp(1.0 - len / radius, 0.0, 1.0);
                    cH1 = max(cH1, (dot(d, V) / max(len, 1e-5)) * fall);
                }
                vec4  sp2 = texture(uViewPos, vUV - t * depthScale * omegaUV);
                if (sp2.a >= 0.5) {
                    vec3 d = sp2.xyz - P; float len = length(d);
                    float fall = clamp(1.0 - len / radius, 0.0, 1.0);
                    cH2 = max(cH2, (dot(d, V) / max(len, 1e-5)) * fall);
                }
            }
            float h1 =  acos(clamp(cH1, -1.0, 1.0));
            float h2 = -acos(clamp(cH2, -1.0, 1.0));
            h1 = gamma + min(h1 - gamma,  HALF_PI);
            h2 = gamma + max(h2 - gamma, -HALF_PI);
            float cosG = cos(gamma), sinG = sin(gamma);
            float arc  = (-cos(2.0 * h1 - gamma) + cosG + 2.0 * h1 * sinG)
                       + (-cos(2.0 * h2 - gamma) + cosG + 2.0 * h2 * sinG);
            visAccum += projLen * 0.25 * arc;
        }
        float visibility = clamp(visAccum / float(SLICES), 0.0, 1.0);
        ao = 1.0 - (1.0 - visibility) * intensity;
        ao = max(ao, 0.1);
    }
    else
    {
        // SSAO: slope-invariant tangent-plane kernel
        vec3 randv = texture(uNoise, vUV * uSSAONoiseScale.xy).xyz;
        vec3 T  = normalize(randv - N * dot(randv, N));
        vec3 B  = cross(N, T);
        mat3 TBN = mat3(T, B, N);
        float occ = 0.0;
        for (int i = 0; i < 32; ++i)
        {
            vec3 sp = P + (TBN * uSSAOKernel[i].xyz) * radius;
            vec4 clip = uSSAOProj * vec4(sp, 1.0);
            vec2 suv  = clip.xy / clip.w * 0.5 + 0.5;
            if (suv.x < 0.0 || suv.x > 1.0 || suv.y < 0.0 || suv.y > 1.0) continue;
            vec4 sv = texture(uViewPos, suv);
            if (sv.a < 0.5) continue;
            vec3  toOcc    = sv.xyz - P;
            float above    = dot(toOcc, N);
            float rangeChk = smoothstep(0.0, 1.0, radius / max(length(toOcc), 1e-4));
            occ += (above > bias ? 1.0 : 0.0) * rangeChk;
        }
        ao = 1.0 - (occ / 32.0) * intensity;
        ao = max(ao, 0.5);
    }
    FragColor = vec4(ao, ao, ao, 1.0);
}
