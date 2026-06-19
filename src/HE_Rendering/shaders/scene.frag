#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 0) out vec4 FragColor;

layout(set = 0, binding = 0) uniform Frame {
    vec4  cameraPos;
    ivec4 lightCount;
    vec4  lightPos[8];
    vec4  lightDir[8];
    vec4  lightColor[8];
    vec4  lightParams[8];
    mat4  lightVP;
    ivec4 shadowEnabled;
} uf;

layout(set = 0, binding = 1) uniform sampler2D uShadowMap;

// Per-draw PBR material scalars uploaded via vkCmdUpdateBuffer before each draw.
layout(set = 0, binding = 2) uniform MatUBO {
    vec4 baseColorMet;  // rgb = baseColor, a = metallic
    vec4 roughPad;      // x = roughness, y = opacity
} mat_ubo;

// ── Cook-Torrance BRDF ────────────────────────────────────────────────────────
const float PI = 3.14159265;
float D_GGX(float NdH, float a2) { float d = NdH*NdH*(a2-1.0)+1.0; return a2/(PI*d*d+1e-6); }
float G_Schlick(float NdX, float k) { return NdX/(NdX*(1.0-k)+k); }
vec3  F_Schlick(float VdH, vec3 F0) { return F0+(1.0-F0)*pow(1.0-VdH,5.0); }

vec3 BRDF(vec3 L, vec3 V, vec3 N, vec3 base, float met, float rough)
{
    float a = rough*rough; float a2 = a*a;
    float k = (rough+1.0); k = k*k/8.0;
    vec3  H = normalize(L+V);
    float NdL = max(dot(N,L),0.0);
    float NdV = max(dot(N,V),0.0001);
    float NdH = max(dot(N,H),0.0);
    float VdH = max(dot(V,H),0.0);
    vec3 F0 = mix(vec3(0.04), base, met);
    vec3 F  = F_Schlick(VdH, F0);
    float D = D_GGX(NdH, a2);
    float G = G_Schlick(NdV,k)*G_Schlick(NdL,k);
    vec3 spec = D*F*G / max(4.0*NdV*NdL, 1e-6);
    vec3 kd = (1.0-F)*(1.0-met);
    return (kd*base/PI + spec)*NdL;
}

float shadowFactor(vec3 worldPos, vec3 N, vec3 L)
{
    if (uf.shadowEnabled.x == 0) return 1.0;
    vec4 lp = uf.lightVP * vec4(worldPos, 1.0);
    vec3 p  = lp.xyz / lp.w;
    vec2 uv = p.xy * 0.5 + 0.5;
    if (p.z > 1.0 || any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0))))
        return 1.0;
    float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float closest = texture(uShadowMap, uv).r;
    return (p.z - bias > closest) ? 0.35 : 1.0;
}

void main()
{
    vec3  base  = mat_ubo.baseColorMet.rgb;
    float met   = mat_ubo.baseColorMet.a;
    float rough = max(mat_ubo.roughPad.x, 0.04);
    vec3  N     = normalize(vNormal);

    if (uf.lightCount.x == 0)
    {
        vec3  L    = normalize(vec3(0.5, 0.8, 0.6));
        float diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        FragColor  = vec4(base * diff, mat_ubo.roughPad.y);
        return;
    }

    vec3 V      = normalize(uf.cameraPos.xyz - vWorldPos);
    vec3 result = 0.03 * base * (1.0 - met);  // ambient

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
        float sh = (type == 0) ? shadowFactor(vWorldPos, N, L) : 1.0;
        result += BRDF(L, V, N, base, met, rough) * uf.lightColor[i].rgb * uf.lightColor[i].w * atten * sh;
    }
    FragColor = vec4(result, mat_ubo.roughPad.y);
}
