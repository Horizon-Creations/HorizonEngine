#version 450

layout(location = 0) in vec3 vWorldPos;
layout(location = 1) in vec3 vNormal;
layout(location = 2) in vec2 vUV;
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
    vec4  sunDir;   // xyz = sun direction
    vec4  fog;      // x=fogDensity, y=fogHeightFalloff
    vec4  viewport; // x=W, y=H, z=ssaoEnabled(1.0), w=unused — must match FrameUBOData exactly
} uf;

layout(set = 0, binding = 1) uniform sampler2D uShadowMap;

// Per-draw PBR material scalars uploaded via vkCmdUpdateBuffer before each draw.
layout(set = 0, binding = 2) uniform MatUBO {
    vec4 baseColorMet;  // rgb = baseColor, a = metallic
    vec4 roughPad;      // x = roughness, y = opacity, z = hasTexture (0/1)
} mat_ubo;

// Per-mesh base-color texture (set = 2, binding = 0), bound per draw. Untextured
// meshes bind a 1x1 white default, and roughPad.z = 0 selects the flat colour, so
// sampling is always safe. Set index 2 is shared by the scene + skinned pipelines
// (skinned keeps its bones UBO at set 1).
layout(set = 2, binding = 0) uniform sampler2D uAlbedo;

// SSAO occlusion texture (1x1 white when SSAO is disabled so ao = 1.0).
// Binding 3 must match the scene descriptor set layout in VulkanRenderer.cpp.
layout(set = 0, binding = 3) uniform sampler2D uAO;

// ── Procedural sky ────────────────────────────────────────────────────────────
vec3 skyColor(vec3 dir, vec3 sunDir)
{
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    float sunY = clamp(sunDir.y, -0.2, 1.0);
    float day  = smoothstep(-0.10, 0.10, sunY);
    float dusk = smoothstep(-0.06, 0.05, sunY) * (1.0 - smoothstep(0.05, 0.28, sunY));
    vec3 zenithDay  = vec3(0.08, 0.28, 0.72);
    vec3 horizDay   = vec3(0.42, 0.62, 0.88);
    vec3 zenithNite = vec3(0.003, 0.005, 0.015);
    vec3 horizNite  = vec3(0.006, 0.009, 0.024);
    vec3 zenith  = mix(zenithNite, zenithDay, day);
    vec3 horizon = mix(horizNite,  horizDay,  day);
    vec2  sunAz  = normalize(sunDir.xz + vec2(1e-5));
    float toward = dot(normalize(dir.xz + vec2(1e-5)), sunAz) * 0.5 + 0.5;
    toward = pow(clamp(toward, 0.0, 1.0), 1.5);
    vec3  duskHoriz = mix(vec3(0.52, 0.30, 0.52), vec3(1.20, 0.50, 0.16), toward);
    horizon = mix(horizon, duskHoriz, dusk);
    zenith  = mix(zenith,  vec3(0.20, 0.16, 0.40), dusk * 0.6);
    float h    = clamp(dir.y, 0.0, 1.0);
    float grad = pow(1.0 - h, 2.5);
    vec3 sky = mix(zenith, horizon, grad);
    float band = pow(1.0 - h, 8.0) * toward;
    sky += vec3(1.25, 0.62, 0.26) * (band * dusk * 0.8);
    vec3 ground = mix(vec3(0.02, 0.02, 0.03), vec3(0.24, 0.23, 0.21), day);
    sky = mix(sky, ground, smoothstep(0.0, -0.25, dir.y));
    vec3  sunTint = mix(vec3(1.0, 0.42, 0.20), vec3(1.0, 0.96, 0.88), smoothstep(0.0, 0.25, sunY));
    float s = max(dot(dir, sunDir), 0.0);
    float sunVis = max(day, dusk);
    sky += sunTint * (pow(s, 1800.0) * 14.0) * day;
    sky += sunTint * (pow(s, 180.0)  * 2.2) * sunVis;
    sky += sunTint * (pow(s, 22.0)   * 0.7) * sunVis;
    sky += vec3(1.0, 0.5, 0.25) * (pow(s, 5.0) * 0.5) * dusk;
    float night   = 1.0 - day;
    vec3  moonDir = normalize(vec3(-sunDir.x, -sunDir.y, sunDir.z));
    float m       = max(dot(dir, moonDir), 0.0);
    sky += vec3(0.80, 0.86, 1.00) * (pow(m, 60.0) * 0.05) * night;
    sky += vec3(0.015, 0.018, 0.030) * night;
    return sky;
}

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
    // A base-color texture (flagged by roughPad.z) replaces the flat colour, mirroring
    // GL/Metal/D3D11. PBR scalars still come from the material UBO.
    vec3  base  = (mat_ubo.roughPad.z > 0.5) ? texture(uAlbedo, vUV).rgb
                                             : mat_ubo.baseColorMet.rgb;
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
    vec3 Nup    = normalize(vec3(N.x, max(N.y, 0.1), N.z));
    vec3 Rrough = normalize(mix(reflect(-V, N), N, rough));
    vec3 F0     = mix(vec3(0.04), base, met);
    vec3 kd     = (1.0 - F0) * (1.0 - met);
    vec3 ambDiff = skyColor(Nup,    uf.sunDir.xyz) * base * kd;
    vec3 ambSpec = skyColor(Rrough, uf.sunDir.xyz) * F0;
    // AO only darkens the ambient (sky IBL) terms; direct BRDF lights are unaffected.
    // When SSAO is disabled, uf.viewport.z == 0 and we use a 1x1 white fallback texture
    // so ao = 1.0 and the ambient result is unchanged.
    float ao = (uf.viewport.z > 0.5)
        ? texture(uAO, gl_FragCoord.xy / uf.viewport.xy).r
        : 1.0;
    vec3 result  = ao * (ambDiff * 0.35 + ambSpec * (1.0 - 0.6 * rough));

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
    // Atmospheric fog
    if (uf.fog.x > 0.0) {
        vec3  ray  = vWorldPos - uf.cameraPos.xyz;
        float dist = max(length(ray), 1e-4);
        float k    = uf.fog.y * ray.y;
        float ta   = abs(k) > 1e-4 ? (1.0 - exp(-k)) / k : 1.0;
        float opt  = uf.fog.x * dist * exp(-uf.fog.y * uf.cameraPos.y) * ta;
        float f    = 1.0 - exp(-opt);
        vec3  fogCol = skyColor(ray/dist, uf.sunDir.xyz);
        result = mix(result, fogCol, clamp(f, 0.0, 1.0));
    }
    FragColor = vec4(result, mat_ubo.roughPad.y);
}
