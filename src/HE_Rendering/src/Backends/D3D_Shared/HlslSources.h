#pragma once

// ─── HLSL shared by the D3D11 and D3D12 backends ─────────────────────────────
// Both backends embedded these ~750 lines of shader source as string literals,
// byte-identical down to the comments (audit 1a: "größter Einzelgewinn, rein
// mechanisch"). Two hand-maintained copies is exactly the drift the audit
// flagged — a fix applied to one backend's copy silently leaves the other
// behind, which is how the sky PS and the scene PS (listed below as NOT shared)
// already diverged.
//
// What is deliberately NOT in here, and why. These stay in the backend .cpp
// files because the two versions genuinely differ, not because nobody got round
// to them:
//   kSceneHLSL   — different resource bindings. D3D11: t0 = base colour,
//                  t1 = shadow map, samplers s0..s2. D3D12: t0 = shadow map,
//                  t1 = albedo in its own root-signature range, plus a 4th
//                  sampler s3. A root signature is not something an #ifdef can
//                  paper over.
//   kSkyPSHLSL   — the D3D12 copy has the space-nebula pass (nebula(), plus
//                  uNebula/uNebulaColor in its SkyCB); the D3D11 copy predates
//                  it and its SkyCB has no room for the two extra fields. Real
//                  feature drift, not formatting — porting it changes what
//                  D3D11 renders and needs the C++ constant buffer widened too.
//   kUIHLSL      — same maths, but D3D11 does the glyph-UV lerp in the VS and
//                  D3D12 in the PS.
//   kFxaaHLSL,
//   kBloom*HLSL  — the D3D12 copies declare `Texture2D _dummy : register(t1)`
//                  because their root signature always binds two SRV tables.
//
// Header-only string constants: no .cpp, so no CMake source entry. Every TU that
// includes this gets the same `inline` definitions, merged at link time.
namespace HE::hlsl
{

// ─── Shared sky colour function ─────────────────────────────────────────────
// Mirrors kSkyFuncGLSL in OpenGLRenderer.cpp exactly (GLSL→HLSL: lerp/frac/float3).
// Prepended to the sky PS AND to each backend's own scene source, so skyColor()
// is in scope for the IBL ambient term there too.
inline constexpr const char* kSkyFuncHLSL = R"HLSL(
float3 skyColor(float3 dir, float3 sunDir)
{
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    float sunY = clamp(sunDir.y, -0.2f, 1.0f);
    float day  = smoothstep(-0.10f, 0.10f, sunY);
    float dusk = smoothstep(-0.06f, 0.05f, sunY)
               * (1.0f - smoothstep(0.05f, 0.28f, sunY));
    float3 zenithDay  = float3(0.08f, 0.28f, 0.72f);
    float3 horizDay   = float3(0.42f, 0.62f, 0.88f);
    float3 zenithNite = float3(0.003f, 0.005f, 0.015f);
    float3 horizNite  = float3(0.006f, 0.009f, 0.024f);
    float3 zenith  = lerp(zenithNite, zenithDay, day);
    float3 horizon = lerp(horizNite,  horizDay,  day);
    float2 sunAz  = normalize(sunDir.xz + 1e-5f);
    float toward  = dot(normalize(dir.xz + 1e-5f), sunAz) * 0.5f + 0.5f;
    toward = pow(clamp(toward, 0.0f, 1.0f), 1.5f);
    float3 duskHoriz = lerp(float3(0.52f,0.30f,0.52f), float3(1.20f,0.50f,0.16f), toward);
    horizon = lerp(horizon, duskHoriz, dusk);
    zenith  = lerp(zenith,  float3(0.20f,0.16f,0.40f), dusk * 0.6f);
    float  h    = clamp(dir.y, 0.0f, 1.0f);
    float  grad = pow(1.0f - h, 2.5f);
    float3 sky  = lerp(zenith, horizon, grad);
    float band = pow(1.0f - h, 8.0f) * toward;
    sky += float3(1.25f,0.62f,0.26f) * (band * dusk * 0.8f);
    float3 ground = lerp(float3(0.02f,0.02f,0.03f), float3(0.24f,0.23f,0.21f), day);
    sky = lerp(sky, ground, smoothstep(0.0f, -0.25f, dir.y));
    float3 sunTint = lerp(float3(1.0f,0.42f,0.20f), float3(1.0f,0.96f,0.88f),
                          smoothstep(0.0f, 0.25f, sunY));
    float  s      = max(dot(dir, sunDir), 0.0f);
    float  sunVis = max(day, dusk);
    sky += sunTint * (pow(s, 1800.0f) * 14.0f) * day;
    sky += sunTint * (pow(s, 180.0f)  * 2.2f)  * sunVis;
    sky += sunTint * (pow(s, 22.0f)   * 0.7f)  * sunVis;
    sky += float3(1.0f,0.5f,0.25f) * (pow(s, 5.0f) * 0.5f) * dusk;
    float  night   = 1.0f - day;
    float3 moonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
    float  m       = max(dot(dir, moonDir), 0.0f);
    sky += float3(0.80f,0.86f,1.00f) * (pow(m, 60.0f) * 0.05f) * night;
    sky += float3(0.015f,0.018f,0.030f) * night;
    return sky;
}
)HLSL";

// ─── Sky background pass VS ─────────────────────────────────────────────────
// VSSky: fullscreen triangle at D3D far plane (z=1 so geometry draws over it).
// The sky PIXEL shader is NOT shared — see the file header.
inline constexpr const char* kSkyVSHLSL = R"HLSL(
struct SkyVSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyVSOut VSSky(uint vid : SV_VertexID)
{
    SkyVSOut o;
    float x = (float)((vid & 1u) << 2u) - 1.0f;
    float y = (float)((vid & 2u) << 1u) - 1.0f;
    o.pos = float4(x, y, 1.0f, 1.0f); // z=1 = D3D far plane
    o.ndc = float2(x, y);
    return o;
}
)HLSL";

// ─── Debug line pass HLSL ───────────────────────────────────────────────────
inline constexpr const char* kDebugLineHLSL = R"HLSL(
cbuffer DebugCB : register(b0) { float4x4 uVP; };
struct LineIn  { float3 pos : POSITION; float3 color : COLOR0; };
struct LineOut { float4 clip : SV_POSITION; float3 color : COLOR0; };
LineOut VSLine(LineIn i)
{
    LineOut o; o.clip=mul(uVP,float4(i.pos,1.0f)); o.color=i.color; return o;
}
float4 PSLine(LineOut i) : SV_TARGET { return float4(i.color,1.0f); }
)HLSL";

// ─── Skinned vertex shader HLSL ─────────────────────────────────────────────
// Only contains the VS entry; each backend pairs it with its OWN scene PSMain,
// which is pre-bound. (That the scene PS differs is exactly why it is not here.)
inline constexpr const char* kSkinnedHLSL = R"HLSL(
cbuffer PerObject : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4   uColor;
    float4   uPBR;
};
cbuffer BonesCB : register(b2)
{
    float4x4 uBoneMatrices[128];
};
struct SkinnedIn
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD0;
    uint4  boneIds : BLENDINDICES;
    float4 boneWgt : BLENDWEIGHT;
};
struct VSOut { float4 clip : SV_POSITION; float3 worldPos : TEXCOORD0; float3 normal : TEXCOORD1; float2 uv : TEXCOORD2; };
VSOut VSMainSkinned(SkinnedIn i)
{
    float4x4 skin = i.boneWgt.x * uBoneMatrices[i.boneIds.x]
                  + i.boneWgt.y * uBoneMatrices[i.boneIds.y]
                  + i.boneWgt.z * uBoneMatrices[i.boneIds.z]
                  + i.boneWgt.w * uBoneMatrices[i.boneIds.w];
    float4 sp  = mul(skin, float4(i.pos, 1.0f));
    VSOut o;
    o.worldPos = mul(uModel, sp).xyz;
    o.normal   = mul((float3x3)uModel, mul((float3x3)skin, i.normal));
    o.uv       = i.uv;
    o.clip     = mul(uMVP, sp);
    return o;
}
)HLSL";

// ─── SSAO HLSL ──────────────────────────────────────────────────────────────

// Position prepass: outputs view-space position (alpha=1 marks valid geometry).
inline constexpr const char* kSSAOPosHLSL = R"HLSL(
cbuffer SSAOPosCB : register(b0)
{
    float4x4 uPosMVP;        // viewProj * model
    float4x4 uPosModelView;  // view * model
};
struct VSIn  { float3 pos : POSITION; float3 n : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_POSITION; float3 viewPos : TEXCOORD0; };
VSOut VSPos(VSIn i)
{
    VSOut o;
    o.viewPos = mul(uPosModelView, float4(i.pos, 1.0)).xyz;
    o.clip    = mul(uPosMVP,       float4(i.pos, 1.0));
    return o;
}
float4 PSPos(VSOut i) : SV_TARGET
{
    return float4(i.viewPos, 1.0);  // a=1 marks valid geometry
}
)HLSL";

// SSAO fullscreen pass: slope-invariant tangent-plane hemisphere kernel.
// Mirrors the GL/Metal reference; D3D y-flip applied to UV reprojection only.
// Also carries the HBAO (32-sector bitmask) and GTAO (analytic horizon arc)
// methods — the uParams.w selector picks between them.
inline constexpr const char* kSSAOHLSL = R"HLSL(
Texture2D    uViewPos   : register(t0);
Texture2D    uNoise     : register(t1);
SamplerState uPointSamp : register(s0);
cbuffer SSAOCB : register(b0)
{
    float4x4 uSSAOProj;
    float4   uSSAONoiseScale;
    float4   uSSAOParams;  // x=radius, y=bias, z=intensity, w=method(0=SSAO,1=HBAO,2=GTAO)
    float4   uSSAOKernel[32];
};
struct FsIn { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
static const float HE_PI      = 3.14159265359f;
static const float HE_TWO_PI  = 6.28318530718f;
static const float HE_HALF_PI = 1.57079632679f;
uint hbaoSectors(float minH, float maxH, uint mask)
{
    uint startBit = min(uint(clamp(minH, 0.0f, 1.0f) * 32.0f), 31u);
    uint count    = uint(ceil(clamp(maxH - minH, 0.0f, 1.0f) * 32.0f));
    uint bits     = (count > 0u) ? (0xFFFFFFFFu >> (32u - count)) : 0u;
    return mask | (bits << startBit);
}
float hbaoIgn(float2 p) { return frac(52.9829189f * frac(0.06711056f * p.x + 0.00583715f * p.y)); }
float4 SSAOMain(FsIn i) : SV_TARGET
{
    float4 pv = uViewPos.SampleLevel(uPointSamp, i.uv, 0);
    if (pv.a < 0.5f) { return float4(1,1,1,1); }
    float3 P = pv.xyz;
    float2 texel = rcp(float2(uSSAONoiseScale.xy * 4.0f));
    float3 Pr = uViewPos.SampleLevel(uPointSamp, i.uv + float2( texel.x, 0), 0).xyz;
    float3 Pl = uViewPos.SampleLevel(uPointSamp, i.uv - float2( texel.x, 0), 0).xyz;
    float3 Pu = uViewPos.SampleLevel(uPointSamp, i.uv + float2(0,  texel.y), 0).xyz;
    float3 Pd = uViewPos.SampleLevel(uPointSamp, i.uv - float2(0,  texel.y), 0).xyz;
    float3 ddx_ = (abs(Pr.z - P.z) < abs(P.z - Pl.z)) ? (Pr - P) : (P - Pl);
    float3 ddy_ = (abs(Pd.z - P.z) < abs(P.z - Pu.z)) ? (Pd - P) : (P - Pu);
    float3 N = normalize(cross(ddx_, ddy_));
    if (N.z < 0.0f) N = -N;
    float radius    = uSSAOParams.x;
    float bias      = uSSAOParams.y;
    float intensity = uSSAOParams.z;
    int   method    = (int)uSSAOParams.w;
    float ao;
    if (method == 1)
    {
        // HBAO: horizon-based AO via 32-sector visibility bitmask
        const int   SLICES    = 3;
        const int   STEPS     = 8;
        const float THICKNESS = 0.5f;
        float3 V = normalize(-P);
        float  jitter = hbaoIgn(i.clip.xy) - 0.5f;
        float  depthScale = 0.5f * radius / max(-P.z, 1e-4f);
        float  visibility = 0.0f;
        for (int s = 0; s < SLICES; ++s)
        {
            float  phi     = (float(s) + jitter) * (HE_TWO_PI / float(SLICES));
            float2 omega   = float2(cos(phi), sin(phi));
            float3 dir     = float3(omega, 0.0f);
            float3 orthoDir = dir - dot(dir, V) * V;
            float3 axis    = cross(dir, V);
            float3 projN   = N - axis * dot(N, axis);
            float  projLen = length(projN);
            if (projLen < 1e-5f) { visibility += 1.0f; continue; }
            float  nAng    = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, 0.0f, 1.0f));
            float2 omegaUV = float2(uSSAOProj[0][0] * omega.x, uSSAOProj[1][1] * omega.y);
            uint   occ     = 0u;
            for (int k = 0; k < STEPS; ++k)
            {
                float  t   = (float(k) + jitter) / float(STEPS) + 0.01f;
                float2 sUV = i.uv - t * depthScale * omegaUV;
                float4 sp  = uViewPos.SampleLevel(uPointSamp, sUV, 0);
                if (sp.a < 0.5f) continue;
                float3 d   = sp.xyz - P;
                float  len = length(d);
                float2 fb;
                fb.x = dot(d / max(len, 1e-5f), V);
                fb.y = dot(normalize(d - V * THICKNESS), V);
                fb   = acos(clamp(fb, -1.0f, 1.0f));
                fb   = clamp((fb + nAng + HE_HALF_PI) / HE_PI, 0.0f, 1.0f);
                occ  = hbaoSectors(min(fb.x, fb.y), max(fb.x, fb.y), occ);
            }
            visibility += 1.0f - float(countbits(occ)) / 32.0f;
        }
        visibility /= float(SLICES);
        ao = 1.0f - (1.0f - visibility) * intensity;
        ao = max(ao, 0.1f);
    }
    else if (method == 2)
    {
        // GTAO: analytic horizon-arc ambient occlusion
        const int SLICES = 3;
        const int STEPS  = 8;
        float3 V = normalize(-P);
        float  jitter = hbaoIgn(i.clip.xy);
        float  depthScale = 0.5f * radius / max(-P.z, 1e-4f);
        float  visAccum = 0.0f;
        for (int s = 0; s < SLICES; ++s)
        {
            float  phi     = (float(s) + jitter) * (HE_PI / float(SLICES));
            float2 omega   = float2(cos(phi), sin(phi));
            float3 dir     = float3(omega, 0.0f);
            float3 axis    = cross(dir, V);
            float  axisLen = length(axis);
            if (axisLen < 1e-5f) { visAccum += 1.0f; continue; }
            axis /= axisLen;
            float3 orthoDir = normalize(dir - dot(dir, V) * V);
            float3 projN    = N - axis * dot(N, axis);
            float  projLen  = length(projN);
            if (projLen < 1e-5f) continue;
            float  gamma    = sign(dot(orthoDir, projN)) * acos(clamp(dot(projN, V) / projLen, -1.0f, 1.0f));
            float2 omegaUV  = float2(uSSAOProj[0][0] * omega.x, uSSAOProj[1][1] * omega.y);
            float  cH1 = 0.0f;
            float  cH2 = 0.0f;
            for (int k = 0; k < STEPS; ++k)
            {
                float  t   = (float(k) + jitter) / float(STEPS) + 0.02f;
                float4 sp1 = uViewPos.SampleLevel(uPointSamp, i.uv + t * depthScale * omegaUV, 0);
                if (sp1.a >= 0.5f) {
                    float3 d = sp1.xyz - P; float len = length(d);
                    float fall = clamp(1.0f - len / radius, 0.0f, 1.0f);
                    cH1 = max(cH1, (dot(d, V) / max(len, 1e-5f)) * fall);
                }
                float4 sp2 = uViewPos.SampleLevel(uPointSamp, i.uv - t * depthScale * omegaUV, 0);
                if (sp2.a >= 0.5f) {
                    float3 d = sp2.xyz - P; float len = length(d);
                    float fall = clamp(1.0f - len / radius, 0.0f, 1.0f);
                    cH2 = max(cH2, (dot(d, V) / max(len, 1e-5f)) * fall);
                }
            }
            float h1 =  acos(clamp(cH1, -1.0f, 1.0f));
            float h2 = -acos(clamp(cH2, -1.0f, 1.0f));
            h1 = gamma + min(h1 - gamma,  HE_HALF_PI);
            h2 = gamma + max(h2 - gamma, -HE_HALF_PI);
            float cosG = cos(gamma), sinG = sin(gamma);
            float arc  = (-cos(2.0f * h1 - gamma) + cosG + 2.0f * h1 * sinG)
                       + (-cos(2.0f * h2 - gamma) + cosG + 2.0f * h2 * sinG);
            visAccum += projLen * 0.25f * arc;
        }
        float visibility = clamp(visAccum / float(SLICES), 0.0f, 1.0f);
        ao = 1.0f - (1.0f - visibility) * intensity;
        ao = max(ao, 0.1f);
    }
    else
    {
        // SSAO: slope-invariant tangent-plane kernel
        float3 randv = uNoise.SampleLevel(uPointSamp, i.uv * uSSAONoiseScale.xy, 0).xyz;
        float3 T  = normalize(randv - N * dot(randv, N));
        float3 B  = cross(N, T);
        float3x3 TBN = float3x3(T, B, N);
        float occ = 0.0f;
        for (int k = 0; k < 32; ++k)
        {
            float3 sp = P + mul(TBN, uSSAOKernel[k].xyz) * radius;
            float4 clipSP = mul(uSSAOProj, float4(sp, 1.0f));
            float2 suv = float2(clipSP.x / clipSP.w * 0.5f + 0.5f,
                                0.5f - clipSP.y / clipSP.w * 0.5f);
            if (suv.x < 0.0f || suv.x > 1.0f || suv.y < 0.0f || suv.y > 1.0f) continue;
            float4 sv = uViewPos.SampleLevel(uPointSamp, suv, 0);
            if (sv.a < 0.5f) continue;
            float3 toOcc = sv.xyz - P;
            float  above = dot(toOcc, N);
            float  rangeCheck = smoothstep(0.0f, 1.0f, radius / max(length(toOcc), 1e-4f));
            occ += (above > bias ? 1.0f : 0.0f) * rangeCheck;
        }
        ao = 1.0f - (occ / 32.0f) * intensity;
        ao = max(ao, 0.5f);
    }
    return float4(ao, ao, ao, 1.0f);
}
)HLSL";

// SSAO 4x4 box blur pass.
inline constexpr const char* kSSAOBlurHLSL = R"HLSL(
Texture2D    uAOInput   : register(t0);
SamplerState uPointSamp : register(s0);
cbuffer BlurCB : register(b0) { float2 uBlurTexel; float2 _pad; };
struct FsIn { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
float4 SSAOBlurMain(FsIn i) : SV_TARGET
{
    float sum = 0.0;
    for (int x = -2; x < 2; ++x)
        for (int y = -2; y < 2; ++y)
            sum += uAOInput.SampleLevel(uPointSamp, i.uv + float2(x, y) * uBlurTexel, 0).r;
    float ao = sum / 16.0;
    return float4(ao, ao, ao, 1.0);
}
)HLSL";

// ─── PostProcess HLSL ───────────────────────────────────────────────────────
// Fullscreen triangle generated from SV_VertexID — no vertex buffer needed.
// UV convention: y=0 top, y=1 bottom (D3D texture coordinates).
// Only these two post passes are shared: FXAA and the two bloom passes declare an
// extra dummy SRV on D3D12 (see the file header) and stay backend-local.
inline constexpr const char* kFSTriangleVS = R"HLSL(
struct Out { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
Out main(uint vid : SV_VertexID)
{
    Out o;
    float x = (float)((vid & 1u) << 2u) - 1.0;
    float y = (float)((vid & 2u) << 1u) - 1.0;
    o.pos = float4(x, y, 0.0, 1.0);
    o.uv  = float2(x * 0.5 + 0.5, 0.5 - y * 0.5);
    return o;
}
)HLSL";

// ACES filmic tonemapping + bloom composite.  Reads an RGBA16F HDR scene
// color (t0) and a half-res blurred bloom texture (t1), applies exposure,
// ACES, and sRGB gamma.  cbuffer b0 carries { exposure, bloomStrength }.
inline constexpr const char* kTonemapHLSL = R"HLSL(
Texture2D    uHDR   : register(t0);
Texture2D    uBloom : register(t1);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float uExposure; float uBloomStrength; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float3 aces(float3 x) {
    return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14));
}
float4 main(In i) : SV_Target {
    float3 h = uHDR.Sample(uSamp, i.uv).rgb;
    h += uBloom.Sample(uSamp, i.uv).rgb * uBloomStrength;
    h *= uExposure;
    return float4(pow(max(aces(h), 0.0001), 1.0/2.2), 1.0);
}
)HLSL";

// ─── Ray-traced GI (software BVH) HLSL ──────────────────────────────────────
// Port of the GL-4.3 compute GI (kGi* in OpenGLRenderer.cpp), which in turn
// mirrors the Metal reference. SSBOs → StructuredBuffers, image store →
// RWTexture2D, floatBitsToInt → asint(). All five stages compile as SM 5.0.
// Only the HOST binding model differs between the two D3D backends (D3D11 binds
// SRVs/CBs directly; D3D12 passes giNodes/giTris/giInsts as ROOT SRVs t2/t3/t4
// with the textures and UAVs in a dedicated shader-visible GI descriptor heap),
// which is why the shader text itself is identical.

// World-space G-buffer pre-pass (position + normal MRT). CRITICAL: rendered
// with the SAME extraction/camera as the scene pass (Metal lesson 5846efc) or
// the screen-space mask misaligns and shadows swim with camera rotation.
// Reuses the PerObject cbuffer layout so each backend's per-object upload feeds it.
inline constexpr const char* kGiGBufHLSL = R"HLSL(
cbuffer PerObject : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4   uColor;
    float4   uPBR;
};
struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_POSITION; float3 worldPos : TEXCOORD0; float3 normal : TEXCOORD1; };
VSOut GiGBufVS(VSIn i)
{
    VSOut o;
    o.worldPos = mul(uModel, float4(i.pos, 1.0)).xyz;
    o.normal   = mul((float3x3)uModel, i.normal);
    o.clip     = mul(uMVP, float4(i.pos, 1.0));
    return o;
}
struct GiGBufOut { float4 pos : SV_Target0; float4 norm : SV_Target1; };
GiGBufOut GiGBufPS(VSOut i)
{
    GiGBufOut o;
    o.pos  = float4(i.worldPos, 1.0);          // a = 1 → valid geometry
    o.norm = float4(normalize(i.normal), 0.0);
    return o;
}
)HLSL";

// Shared BVH declarations + traversal, string-prepended into both compute
// kernels. Same data layout as the GL/Vulkan SSBOs: 32B nodes (int bits in
// .w lanes, read via asint), 48B triangles, instances = inverse transform +
// baseColor + BLAS offsets. glm writes column-major bytes and D3DCompile
// defaults to column_major, so float4x4 in the structured buffer needs no
// transpose (the A3 instancing buffer relies on the same fact).
inline constexpr const char* kGiTraversalHLSL = R"HLSL(
struct GiNode { float4 d0; float4 d1; }; // d0.xyz bmin, d0.w leftFirst (int bits), d1.xyz bmax, d1.w triCount (int bits)
struct GiTri  { float4 v0; float4 v1; float4 v2; };
struct GiInst { float4x4 invTransform; float4 baseColor; int4 offsets; }; // offsets.x = nodeOffset, .y = triOffset
StructuredBuffer<GiNode> giNodes : register(t2);
StructuredBuffer<GiTri>  giTris  : register(t3);
StructuredBuffer<GiInst> giInsts : register(t4);
cbuffer GiCountCB : register(b1) { int4 uGiCount; }; // x = instance count

// Möller-Trumbore, both faces — mirrors GiBvh.cpp's triHit().
bool giTriHit(GiTri tri, float3 o, float3 d, float tMin, float tMax, out float tOut)
{
    tOut = 0.0;
    float3 e1 = tri.v1.xyz - tri.v0.xyz;
    float3 e2 = tri.v2.xyz - tri.v0.xyz;
    float3 p  = cross(d, e2);
    float det = dot(e1, p);
    if (abs(det) < 1e-9) return false;
    float invDet = 1.0 / det;
    float3 s = o - tri.v0.xyz;
    float u = dot(s, p) * invDet;
    if (u < 0.0 || u > 1.0) return false;
    float3 q = cross(s, e1);
    float v = dot(d, q) * invDet;
    if (v < 0.0 || u + v > 1.0) return false;
    float t = dot(e2, q) * invDet;
    if (t <= tMin || t >= tMax) return false;
    tOut = t;
    return true;
}

// BLAS traversal (one instance), object-space ray — mirrors giBvhIntersect().
bool giBlasHit(int nodeOfs, int triOfs, float3 o, float3 d, float tMin, float tMax,
               bool anyHit, out float tOut)
{
    tOut = tMax;
    float3 invD = 1.0 / d;
    int stack[64];
    int sp = 0;
    stack[sp++] = nodeOfs;
    bool hit = false;
    float best = tMax;
    while (sp > 0)
    {
        GiNode n = giNodes[stack[--sp]];
        float3 t0 = (n.d0.xyz - o) * invD;
        float3 t1 = (n.d1.xyz - o) * invD;
        float3 lo = min(t0, t1);
        float3 hi = max(t0, t1);
        float tN = max(max(lo.x, lo.y), max(lo.z, tMin));
        float tF = min(min(hi.x, hi.y), min(hi.z, best));
        if (tN > tF) continue;
        int leftFirst = asint(n.d0.w);
        int triCount  = asint(n.d1.w);
        if (triCount > 0)
        {
            for (int i = 0; i < triCount; ++i)
            {
                float t;
                if (giTriHit(giTris[triOfs + leftFirst + i], o, d, tMin, best, t))
                {
                    hit = true; best = t; tOut = t;
                    if (anyHit) return true;
                }
            }
        }
        else if (sp + 2 <= 64)
        {
            stack[sp++] = nodeOfs + leftFirst;
            stack[sp++] = nodeOfs + leftFirst + 1;
        }
    }
    return hit;
}

// TLAS analogue: linear instance loop; unnormalised object-space direction
// keeps the parametric t world-comparable across instances.
bool giSceneAnyHit(float3 o, float3 d, float tMin, float tMax)
{
    for (int i = 0; i < uGiCount.x; ++i)
    {
        float3 oL = mul(giInsts[i].invTransform, float4(o, 1.0)).xyz;
        float3 dL = mul((float3x3)giInsts[i].invTransform, d);
        float t;
        if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, tMax, true, t))
            return true;
    }
    return false;
}

// Closest hit across all instances; returns instance index (-1 = miss).
int giSceneClosestHit(float3 o, float3 d, float tMin, float tMax, out float tOut)
{
    int   bestInst = -1;
    float best     = tMax;
    for (int i = 0; i < uGiCount.x; ++i)
    {
        float3 oL = mul(giInsts[i].invTransform, float4(o, 1.0)).xyz;
        float3 dL = mul((float3x3)giInsts[i].invTransform, d);
        float t;
        if (giBlasHit(giInsts[i].offsets.x, giInsts[i].offsets.y, oL, dL, tMin, best, false, t))
        {
            best = t; bestInst = i;
        }
    }
    tOut = best;
    return bestInst;
}
)HLSL";

// Shadow-ray kernel: 1 cone-jittered ray/pixel toward the dominant directional
// light (NEVER the sky-dome sun — Metal lesson 5e45643). Same hash/cone/bias
// constants as the GL/Metal kernels.
inline constexpr const char* kGiShadowCSHLSL = R"HLSL(
cbuffer GiShadowCB : register(b0)
{
    float4 uSunDirRadius; // xyz = direction TOWARD the light, w = angular radius (radians)
    float4 uFrame;        // x = jitter seed, y = tex width, z = tex height
    float4 uLocalPosRange[4]; // xyz = local (point/spot) light position, w = range
    float4 uLocalExtra;       // x = local light count
};
Texture2D<float4>   uGPos     : register(t0);
Texture2D<float4>   uGNorm    : register(t1);
RWTexture2D<float>  uOut      : register(u0);
RWTexture2D<float4> uOutLocal : register(u1); // per-pixel local-light visibility (1 channel per light, first 4)

float2 giHash2(uint2 gid, float seed)
{
    float2 p = float2(gid) + seed * 13.37;
    return float2(frac(sin(dot(p, float2(12.9898, 78.233))) * 43758.5453),
                  frac(sin(dot(p, float2(39.3468, 11.1352))) * 24634.6345));
}
float3 giConeSample(float3 L, float angleRad, float2 xi)
{
    float3 up = (abs(L.y) < 0.99) ? float3(0.0, 1.0, 0.0) : float3(1.0, 0.0, 0.0);
    float3 T  = normalize(cross(up, L));
    float3 B  = cross(L, T);
    float r   = sin(angleRad) * sqrt(xi.x);
    float phi = 6.28318530718 * xi.y;
    return normalize(L + T * (r * cos(phi)) + B * (r * sin(phi)));
}

[numthreads(8, 8, 1)]
void GiShadowCS(uint3 gid : SV_DispatchThreadID)
{
    if (float(gid.x) >= uFrame.y || float(gid.y) >= uFrame.z) return;
    float4 pv = uGPos.Load(int3(gid.xy, 0));
    if (pv.a < 0.5) // background → everything unoccluded
    {
        uOut[gid.xy]      = 1.0;
        uOutLocal[gid.xy] = float4(1.0, 1.0, 1.0, 1.0);
        return;
    }
    float3 N = normalize(uGNorm.Load(int3(gid.xy, 0)).xyz);
    float3 L = uSunDirRadius.xyz;

    // ── Directional light (cone-jittered, temporally accumulated) ─────────
    float sunVis = 0.0;
    // Grazing/back-facing relative to the light: direct lighting's dot(N,L)
    // term already zeroes this out, so skip the trace entirely.
    if (dot(N, L) > 0.0)
    {
        float2 xi  = giHash2(gid.xy, uFrame.x);
        float3 dir = giConeSample(L, max(uSunDirRadius.w, 1e-4), xi);
        // Same self-intersection guards as Metal: normal-offset origin + min t.
        float3 origin = pv.xyz + N * 0.05;
        sunVis = giSceneAnyHit(origin, dir, 0.02, 10000.0) ? 0.0 : 1.0;
    }
    uOut[gid.xy] = sunVis;

    // ── Local (point/spot) lights: one HARD occlusion ray each toward the
    // first 4 (see the Metal kernels) — deliberately UNjittered: deterministic,
    // no temporal pass, one visibility channel per light; the scene shader
    // indexes by its local-light counter.
    float4 localVis = float4(1.0, 1.0, 1.0, 1.0);
    int localCount = clamp(int(uLocalExtra.x), 0, 4);
    // Fixed-trip unrolled loop (i is a literal per iteration) so the dynamic
    // vector-component write localVis[i] stays FXC/SM5.0-safe.
    [unroll] for (int i = 0; i < 4; ++i)
    {
        if (i >= localCount) break;
        float3 toL   = uLocalPosRange[i].xyz - pv.xyz;
        float  distL = length(toL);
        if (distL <= 0.05) continue; // on top of the light → lit
        if (distL >= uLocalPosRange[i].w) continue; // outside the attenuation radius → contributes nothing, skip the ray
        float3 dirL = toL / distL;
        if (dot(N, dirL) <= 0.0) { localVis[i] = 0.0; continue; }
        if (giSceneAnyHit(pv.xyz + N * 0.05, dirL, 0.02, max(distL - 0.1, 0.02)))
            localVis[i] = 0.0;
    }
    uOutLocal[gid.xy] = localVis;
}
)HLSL";

// Temporal accumulation: reproject via last frame's viewProj; history carries
// world position (rgb) + shadow scalar (a). Tolerance deliberately TIGHT
// (Metal lesson 58ee312). D3D NDC → UV includes the y-flip (unlike GL).
inline constexpr const char* kGiTemporalHLSL = R"HLSL(
Texture2D    uGPos    : register(t0);
Texture2D    uRaw     : register(t1);
Texture2D    uHistory : register(t2);
SamplerState uPointSamp : register(s0);
cbuffer GiTemporalCB : register(b0)
{
    float4x4 uPrevViewProj;
    float4   uParams; // x = blend (0 on first GI frame), y = tex width, z = tex height
};
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target
{
    float4 pv   = uGPos.Sample(uPointSamp, i.uv);
    float  rawV = uRaw.Sample(uPointSamp, i.uv).r;
    if (pv.a < 0.5) return float4(0.0, 0.0, 0.0, rawV);

    float4 clip = mul(uPrevViewProj, float4(pv.xyz, 1.0));
    if (clip.w <= 0.0) return float4(pv.xyz, rawV);
    float2 ndc    = clip.xy / clip.w;
    float2 prevUV = float2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (any(prevUV < 0.0) || any(prevUV > 1.0)) return float4(pv.xyz, rawV);

    float4 hist      = uHistory.Sample(uPointSamp, prevUV);
    float  posError  = length(pv.xyz - hist.rgb);
    float  tolerance = clamp(0.02 * clip.w, 0.01, 0.06);
    float  w = (posError < tolerance) ? clamp(uParams.x, 0.0, 0.98) : 0.0;
    // Neighbourhood clamp: guards OCCLUDER motion (the position check above
    // only covers receiver/camera motion).
    float2 texel = 1.0 / uParams.yz;
    float nMin = rawV, nMax = rawV;
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
        {
            float r = uRaw.Sample(uPointSamp, i.uv + float2(x, y) * texel).r;
            nMin = min(nMin, r);
            nMax = max(nMax, r);
        }
    return float4(pv.xyz, lerp(rawV, clamp(hist.a, nMin, nMax), w));
}
)HLSL";

// 3x3 spatial blur of the accumulated shadow scalar → the mask the scene
// shader samples (R16F, linear = free bilinear upsample to full res).
inline constexpr const char* kGiBlurHLSL = R"HLSL(
Texture2D    uSrc : register(t0); // temporal history: rgb = world pos, a = shadow
SamplerState uPointSamp : register(s0);
cbuffer GiBlurCB : register(b0) { float4 uTexel; }; // xy = 1/size
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target
{
    float sum = 0.0;
    [unroll] for (int x = -1; x <= 1; ++x)
        [unroll] for (int y = -1; y <= 1; ++y)
            sum += uSrc.Sample(uPointSamp, i.uv + float2(x, y) * uTexel.xy).a;
    return float4(sum / 9.0, 0.0, 0.0, 1.0);
}
)HLSL";

// DDGI probe update — gather formulation (one thread per octahedral texel, no
// atomics), one threadgroup per probe in the frame's round-robin batch.
// The D3D twist: typed UAV loads of RGBA16F/RG16F are an optional cap on BOTH
// D3D11 (11.3) and D3D12, so the previous atlas values arrive as SRV COPIES
// (t5/t6, refreshed by CopyResource before the dispatch) instead of imageLoad
// on the UAV.
inline constexpr const char* kGiProbeCSHLSL = R"HLSL(
cbuffer GiProbeCB : register(b0)
{
    float4 uGridOrigin;   // xyz = grid origin, w = spacing
    float4 uGridCounts;   // xyz = probe counts, w = probesPerRow
    float4 uRayParams;    // x = max dist, y = hysteresis, z = cursor start, w = probes this batch
    float4 uSunDirRadius; // xyz = direction TOWARD the light, w = local light count
    float4 uSunColor;     // rgb = colour * intensity
    float4 uSkyAmbient;   // rgb = miss colour
    float4 uLightPosRange[8];  // xyz pos, w range
    float4 uLightColorType[8]; // rgb colour*intensity, w type (1 point, 2 spot)
    float4 uLightDirCos[8];    // xyz spot travel dir, w cos(half angle)
};
Texture2D<float4>    uIrrPrev : register(t5);
Texture2D<float2>    uVisPrev : register(t6);
RWTexture2D<float4>  uIrr     : register(u0);
RWTexture2D<float2>  uVis     : register(u1);

static const int kOctSize = 8; // must match the host's kGIProbeOctSize

float3 octDecode(float2 e)
{
    float3 n = float3(e.xy, 1.0 - abs(e.x) - abs(e.y));
    if (n.z < 0.0)
    {
        float2 signN = float2(n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0);
        n.xy = (1.0 - abs(n.yx)) * signN;
    }
    return normalize(n);
}

// direction -> octahedral UV, inverse of octDecode (needed for the
// multi-bounce field lookup below; matches the scene shader's giOctEncode
// byte-for-byte).
float2 octEncodeP(float3 n)
{
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

// PREVIOUS-frame irradiance field at an arbitrary surface point: trilinear
// over the 8 surrounding probes, point-read of each probe's octahedral tile
// in the hit normal's direction. No Chebyshev here — this feeds the low-
// frequency multi-bounce term, where leaking is dampened by albedo anyway.
// Reads uIrrPrev (the SRV copy of last frame's atlas — typed UAV loads are
// an optional cap on D3D11 and D3D12 alike, so the UAV is never read).
float3 giSampleFieldIrradiance(float3 pos, float3 n)
{
    int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0.0, 0.0, 0.0);
    int probesPerRow = max(1, int(uGridCounts.w));
    float spacing = max(uGridOrigin.w, 1e-4);
    float3 gridSpace = (pos - uGridOrigin.xyz) / spacing;
    float3 base  = floor(gridSpace);
    float3 fracP = gridSpace - base;
    float2 oct = octEncodeP(n) * 0.5 + 0.5;
    int2 octTexel = int2(clamp(oct * float(kOctSize), 0.0, float(kOctSize) - 1.0));
    float3 sum  = float3(0.0, 0.0, 0.0);
    float  sumW = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float3 offs = float3(float(i & 1), float((i >> 1) & 1), float((i >> 2) & 1));
        float3 cell = base + offs;
        if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
            continue;
        float3 tri = lerp(1.0 - fracP, fracP, offs);
        float w = tri.x * tri.y * tri.z;
        if (w <= 1e-5) continue;
        int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;
        int2 tile = int2((probeIndex % probesPerRow) * kOctSize,
                         (probeIndex / probesPerRow) * kOctSize);
        sum  += uIrrPrev.Load(int3(tile + octTexel, 0)).rgb * w;
        sumW += w;
    }
    return sum / max(sumW, 1e-4);
}

[numthreads(8, 8, 1)]
void GiProbeCS(uint3 gtid : SV_GroupThreadID, uint3 groupId : SV_GroupID)
{
    int2 texel    = int2(gtid.xy);
    int  batchIdx = int(groupId.x);
    int gx = int(uGridCounts.x), gy = int(uGridCounts.y), gz = int(uGridCounts.z);
    int probeCount = gx * gy * gz;
    if (probeCount <= 0 || batchIdx >= int(uRayParams.w)) return;
    int probeIndex = (int(uRayParams.z) + batchIdx) % probeCount;

    int pz = probeIndex / (gx * gy);
    int py = (probeIndex / gx) % gy;
    int px = probeIndex % gx;
    float3 probePos = uGridOrigin.xyz + float3(px, py, pz) * uGridOrigin.w;

    float2 uv  = (float2(texel) + 0.5) / float(kOctSize) * 2.0 - 1.0;
    float3 dir = octDecode(uv);

    float dist;
    int hitInst = giSceneClosestHit(probePos, dir, 0.01, max(uRayParams.x, 1.0), dist);

    float3 radiance;
    if (hitInst < 0)
    {
        radiance = uSkyAmbient.rgb;
        dist     = uRayParams.x;
    }
    else
    {
        float3 albedo    = giInsts[hitInst].baseColor.rgb;
        float3 hitNormal = -dir;
        float3 hitPos    = probePos + dir * dist;
        float ndl = max(dot(hitNormal, uSunDirRadius.xyz), 0.0);
        // Secondary shadow ray — hit surfaces are NOT assumed fully sun-lit
        // (otherwise probes flood shadowed regions with bright sun bounce).
        if (ndl > 0.0 && giSceneAnyHit(hitPos + hitNormal * 0.05, uSunDirRadius.xyz, 0.02, 10000.0))
            ndl = 0.0;
        radiance = albedo * uSunColor.rgb * ndl;
        int lightCount = int(uSunDirRadius.w);
        for (int i = 0; i < lightCount; ++i)
        {
            float3 toL = uLightPosRange[i].xyz - hitPos;
            float d    = max(length(toL), 1e-4);
            float range = max(uLightPosRange[i].w, 1e-4);
            if (d >= range) continue;
            float3 L = toL / d;
            float ndl2 = max(dot(hitNormal, L), 0.0);
            if (ndl2 <= 0.0) continue;
            float atten = 1.0 - d / range;
            atten *= atten;
            if (uLightColorType[i].w > 1.5)
            {
                float c       = dot(-L, normalize(uLightDirCos[i].xyz));
                float cosCone = uLightDirCos[i].w;
                atten *= smoothstep(cosCone, lerp(cosCone, 1.0, 0.2), c);
            }
            if (atten <= 0.0) continue;
            if (giSceneAnyHit(hitPos + hitNormal * 0.05, L, 0.02, max(d - 0.1, 0.02)))
                continue;
            radiance += albedo * uLightColorType[i].rgb * ndl2 * atten;
        }
        // Multi-bounce feedback (DDGI recursion): light already gathered in the
        // probe field re-reflects off this surface — a red wall visibly bleeds
        // red onto neighbouring geometry, and the series converges toward
        // infinite bounces through the EMA. albedo < 1 keeps it stable.
        radiance += albedo * giSampleFieldIrradiance(hitPos, hitNormal);
    }

    int probesPerRow = max(1, int(uGridCounts.w));
    int2 outCoord = int2((probeIndex % probesPerRow) * kOctSize + texel.x,
                         (probeIndex / probesPerRow) * kOctSize + texel.y);

    // Adaptive hysteresis: deterministic gather rays → deltas are real scene
    // changes; converge fast on change, stay smooth otherwise.
    float baseH = clamp(uRayParams.y, 0.0, 0.98);
    float4 oldIrr = uIrrPrev.Load(int3(outCoord, 0));
    float hIrr = lerp(baseH, 0.3, saturate(length(radiance - oldIrr.rgb) * 4.0));
    uIrr[outCoord] = float4(lerp(radiance, oldIrr.rgb, hIrr), 1.0);
    float2 oldVis = uVisPrev.Load(int3(outCoord, 0));
    float2 newVisSample = float2(dist, dist * dist);
    float hVis = lerp(baseH, 0.3, saturate(abs(dist - oldVis.x) / max(uGridOrigin.w, 1.0)));
    uVis[outCoord] = lerp(newVisSample, oldVis.xy, hVis);
}
)HLSL";

} // namespace HE::hlsl
