#version 450

// Scene fragment shader for the Vulkan backend. Blinn-Phong over up to 8 lights,
// flat base color (textures are a follow-up, matching the D3D12 simplification).

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform Frame {
    vec4  cameraPos;
    ivec4 lightCount;       // x = active light count
    vec4  lightPos[8];      // xyz pos,  w type (0 dir / 1 point / 2 spot)
    vec4  lightDir[8];      // xyz dir,  w cos(spot half angle)
    vec4  lightColor[8];    // rgb,      w intensity
    vec4  lightParams[8];   // x range
    mat4  lightVP;          // directional-light view-proj (Vulkan clip)
    ivec4 shadowEnabled;    // x = 0/1
} uf;

layout(set = 0, binding = 1) uniform sampler2D uShadowMap;

float shadowFactor(vec3 worldPos, vec3 N, vec3 L)
{
    if (uf.shadowEnabled.x == 0) return 1.0;
    vec4 lp = uf.lightVP * vec4(worldPos, 1.0);
    vec3 p  = lp.xyz / lp.w;                 // z already [0,1]; xy in Vulkan NDC
    vec2 uv = p.xy * 0.5 + 0.5;              // clip fix already flipped Y
    if (p.z > 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float closest = texture(uShadowMap, uv).r;
    return (p.z - bias > closest) ? 0.35 : 1.0;
}

void main()
{
    vec3 base = vec3(0.85, 0.55, 0.25);
    vec3 N    = normalize(vNormal);

    if (uf.lightCount.x == 0)
    {
        vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
        float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        FragColor  = vec4(base * diff, 1.0);
        return;
    }

    vec3 V      = normalize(uf.cameraPos.xyz - vWorldPos);
    vec3 result = 0.08 * base;

    for (int i = 0; i < uf.lightCount.x; ++i)
    {
        int   type  = int(uf.lightPos[i].w);
        vec3  L;
        float atten = 1.0;
        if (type == 0)
        {
            L = normalize(-uf.lightDir[i].xyz);
        }
        else
        {
            vec3  d    = uf.lightPos[i].xyz - vWorldPos;
            float dist = max(length(d), 1e-4);
            L = d / dist;
            float range = max(uf.lightParams[i].x, 1e-4);
            atten = clamp(1.0 - dist / range, 0.0, 1.0);
            atten *= atten;
            if (type == 2)
            {
                float c       = dot(-L, normalize(uf.lightDir[i].xyz));
                float cosCone = uf.lightDir[i].w;
                atten *= smoothstep(cosCone, mix(cosCone, 1.0, 0.2), c);
            }
        }
        float sh   = (type == 0) ? shadowFactor(vWorldPos, N, L) : 1.0;
        float diff = max(dot(N, L), 0.0);
        vec3  H    = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;
        result += (base * diff + vec3(spec)) * uf.lightColor[i].rgb * uf.lightColor[i].w * atten * sh;
    }
    FragColor = vec4(result, 1.0);
}
