#include "Backends/D3D12/D3D12Renderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <Renderer/UIFont.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <SDL3/SDL.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_5.h>   // IDXGIFactory5 + DXGI_FEATURE_PRESENT_ALLOW_TEARING
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cmath>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;
// Triple-buffered: with only 2 buffers + VSync OFF the CPU blocks waiting for a back
// buffer to free, which shows up as uneven/juddery viewport motion. A 3rd buffer gives
// the CPU enough slack to pace frames smoothly. Used for swapchain buffers AND frames
// in flight (allocators/fences/per-frame CBs) — both benefit.
static constexpr UINT k_frameCount = 3;
static constexpr UINT k_maxDraws   = 4096;          // per-object CB ring capacity
static constexpr UINT k_cbSlot     = 256;           // CBV alignment
static constexpr UINT k_maxInstances = 65536;       // instance-transform ring capacity (A3)
static constexpr UINT k_instStride   = 128;         // bytes per instance = 2 × float4x4 (mvp, model)

// ─── Sky 3D noise volume bake ───────────────────────────────────────────────
// CPU-baked RG16 volume the sky's starFbm3 (.r value noise) and worleyFbm
// (.g cellular) sample for the volumetric clouds. Mirrors D3D11Renderer's
// BuildSkyNoise3D exactly — identical math — serial nested loops (one-time init;
// avoids <execution>/<numeric>). Tightly packed: index ((z*n+y)*n+x)*2 into the
// uint16_t buffer.
static std::vector<uint16_t> BuildSkyNoise3D(int n)
{
    auto hash = [](glm::vec3 p) {
        p = glm::fract(p * 0.1031f);
        p += glm::dot(p, glm::vec3(p.z, p.y, p.x) + 31.32f);
        return glm::fract((p.x + p.y) * p.z);
    };
    // Decorrelated per-cell jitter for the Worley feature points (sin-free so it is
    // bit-deterministic across compilers — both backends bake CPU-side).
    auto hash3 = [](glm::vec3 c) {
        glm::vec3 p = glm::fract(c * glm::vec3(0.1031f, 0.1030f, 0.0973f));
        p += glm::dot(p, glm::vec3(p.y, p.z, p.x) + 33.33f);
        return glm::fract(glm::vec3((p.x + p.y) * p.z, (p.x + p.z) * p.y, (p.y + p.z) * p.x));
    };
    const int kWorleyGrid = 48;   // feature cells per axis across the tile
    auto worley = [&](glm::vec3 uv) {
        glm::vec3 pc = uv * static_cast<float>(kWorleyGrid);
        glm::vec3 id = glm::floor(pc);
        glm::vec3 fp = pc - id;
        float f1 = 1e9f;
        for (int k = -1; k <= 1; ++k)
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i)
                {
                    glm::vec3 off(static_cast<float>(i), static_cast<float>(j), static_cast<float>(k));
                    glm::vec3 wrapped = glm::mod(id + off, static_cast<float>(kWorleyGrid)); // seamless tile
                    glm::vec3 d = (off + hash3(wrapped)) - fp;
                    f1 = std::min(f1, glm::dot(d, d));   // nearest feature (squared)
                }
        return glm::clamp(1.0f - std::sqrt(f1), 0.0f, 1.0f);
    };
    std::vector<uint16_t> d(static_cast<size_t>(n) * n * n * 2);
    const float inv = 1.0f / static_cast<float>(n);

    // Serial nested loops (one-time init): each voxel is fully independent.
    for (int z = 0; z < n; ++z)
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const size_t idx = ((static_cast<size_t>(z) * n + y) * n + x) * 2;
                glm::vec3 uv((x + 0.5f) * inv, (y + 0.5f) * inv, (z + 0.5f) * inv);
                d[idx + 0] = static_cast<uint16_t>(
                    glm::clamp(hash(glm::vec3(x, y, z)), 0.0f, 1.0f) * 65535.0f + 0.5f);
                d[idx + 1] = static_cast<uint16_t>(worley(uv) * 65535.0f + 0.5f);
            }
    return d;
}

// ─── Embedded HLSL ──────────────────────────────────────────────────────────
// Unlit Blinn-Phong, flat color only. Textures are intentionally left out of the
// D3D12 path for now (uploading to a DEFAULT-heap texture + descriptor tables is
// the easiest thing to get wrong on a backend that cannot be built here) — TODO
// alongside the render-target work. glm column-major + HLSL default packing.

// ─── Shared sky colour function ─────────────────────────────────────────────
static const char* kSkyFuncHLSL12 = R"HLSL(
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

static const char* kSkyVSHLSL12 = R"HLSL(
struct SkyVSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
SkyVSOut VSSky(uint vid : SV_VertexID)
{
    SkyVSOut o;
    float x = (float)((vid & 1u) << 2u) - 1.0f;
    float y = (float)((vid & 2u) << 1u) - 1.0f;
    o.pos = float4(x, y, 1.0f, 1.0f);
    o.ndc = float2(x, y);
    return o;
}
)HLSL";

static const char* kSkyPSHLSL12 = R"HLSL(
cbuffer SkyEnv : register(b0)
{
    float4x4 uInvViewProj;
    float3   uSunDir;       float  uTimeOfDay;
    float3   uSunColor;     float  uCloudCoverage;
    float3   uWind;         float  uTime;
    float3   uAuroraColor;  float  uAurora;
    float    uMilkyWay;     float  uFlash; int uHasMoonTex; float uNebula;
    float3   uNebulaColor;  float  _skyPad2;
};
Texture2D    uMoonTex   : register(t0);
SamplerState uSkyLinear : register(s0);
Texture3D    uNoise     : register(t1);
SamplerState uSkyWrap   : register(s1);

float starHash(float3 p){p=frac(p*0.1031f);p+=dot(p,p.zyx+31.32f);return frac((p.x+p.y)*p.z);}
// Trilinear value noise sampled from the precomputed uNoise volume (.r channel
// holds starHash at the integer lattice). Pre-smoothstepping the fractional
// coordinate reproduces the old smoothstep interpolation via the hardware linear
// filter; +0.5 lands integer lattice points on texel centres. The 1/256 is the
// tile PERIOD in world units (independent of the texel resolution kNoiseN).
float starNoise3(float3 p)
{
    float3 f = frac(p);
    float3 q = floor(p) + f * f * (3.0f - 2.0f * f) + 0.5f;
    return uNoise.SampleLevel(uSkyWrap, q * (1.0f / 256.0f), 0).r;
}
float starFbm3(float3 p, int oct)
{
    float v=0.0f, a=0.5f;
    for (int i=0;i<oct;++i){v+=a*starNoise3(p);p*=2.03f;a*=0.5f;}
    return v;
}
float cloudHash(float2 p){p=frac(p*float2(127.1f,311.7f));p+=dot(p,p+34.56f);return frac(p.x*p.y);}
float cloudNoise(float2 p){float2 i=floor(p),f=frac(p),u=f*f*(3.0f-2.0f*f);return lerp(lerp(cloudHash(i),cloudHash(i+float2(1,0)),u.x),lerp(cloudHash(i+float2(0,1)),cloudHash(i+float2(1,1)),u.x),u.y);}
float cloudFbm(float2 p){float v=0.0f,a=0.5f;for(int i=0;i<5;++i){v+=a*cloudNoise(p);p*=2.02f;a*=0.5f;}return v;}
float3 celestialDir(float3 dir,float tod){float a=tod*6.2831853f;float3 axis=normalize(float3(0.22f,0.92f,0.32f));float c=cos(a),s=sin(a);return dir*c+cross(axis,dir)*s+axis*dot(axis,dir)*(1.0f-c);}
float galacticBand(float3 cd){float3 gN=normalize(float3(0.46f,0.52f,-0.72f));float d=dot(normalize(cd),gN);return exp(-d*d*7.0f);}

float3 starField(float3 dir,float3 cdir,float3 sunDir,float t,float mw)
{
    float night=1.0f-smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f));
    if(night<=0.0f||dir.y<=0.0f)return (float3)0;
    float band=galacticBand(cdir),mwc=clamp(mw,0.0f,1.0f);
    float thresh=lerp(0.92f,lerp(0.86f,0.72f,mwc),band);
    float3 p=cdir*70.0f,cell=floor(p);float present=starHash(cell);
    if(present<thresh)return (float3)0;
    float3 sp=float3(starHash(cell+1.7f),starHash(cell+4.3f),starHash(cell+8.9f));
    float d=length(frac(p)-sp),sizeH=starHash(cell+5.7f),big=sizeH*sizeH*sizeH;
    float radius=lerp(0.05f,0.17f,big);
    float core=smoothstep(radius,0.0f,d);core*=core;
    float halo=smoothstep(radius*3.0f,radius,d)*(big*big)*0.35f;
    float shape=core+halo;
    float mag=(0.4f+0.6f*smoothstep(thresh,1.0f,present))*lerp(0.7f,2.7f,big);
    float twPhase=starHash(cell+23.5f)*6.2831f,twFreq=2.0f+4.0f*starHash(cell+47.1f);
    float tw=0.7f+0.3f*sin(t*twFreq+twPhase);
    float horizon=smoothstep(0.0f,0.15f,dir.y);
    float3 tint=lerp(float3(0.80f,0.88f,1.0f),float3(1.0f,0.93f,0.82f),starHash(cell+12.1f));
    float bandDim=lerp(1.6f,lerp(0.9f,1.5f,mwc),band);
    return tint*(shape*mag*tw*horizon*night*bandDim);
}
float3 aurora(float3 dir,float3 sunDir,float t,float intensity,float3 auroraCol)
{
    if(intensity<=0.0f)return (float3)0;
    float night=1.0f-smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f));
    if(night<=0.0f||dir.y<=0.04f)return (float3)0;
    float2 P=dir.xz/(dir.y+0.45f);float along=P.x,across=P.y;
    float wave=0.40f*sin(along*0.7f+t*0.15f)+0.30f*cloudFbm(float2(along*0.35f-t*0.04f,3.0f));
    float phase=across*0.30f+wave,f=abs(frac(phase)-0.5f);
    float ribbon=smoothstep(0.10f,0.45f,f);
    float stri=cloudFbm(float2(along*6.0f+t*0.25f,across*1.2f));
    float curtain=ribbon*(0.45f+0.55f*smoothstep(0.30f,0.80f,stri));
    float patches=0.65f+0.35f*smoothstep(0.25f,0.85f,cloudFbm(float2(along*0.45f+t*0.03f,across*0.4f+9.0f)));
    float hcol=smoothstep(0.05f,0.60f,dir.y);
    float3 bCol=auroraCol*float3(0.60f,0.15f,0.90f),tCol=auroraCol*float3(0.30f,0.90f,0.70f);
    float3 col=lerp(lerp(bCol,auroraCol,smoothstep(0.0f,0.5f,hcol)),tCol,smoothstep(0.5f,1.0f,hcol));
    float fade=smoothstep(0.03f,0.16f,dir.y)*(1.0f-smoothstep(0.78f,1.0f,dir.y));
    return col*(curtain*patches*fade*intensity*night*5.0f);
}
float3 moonDisk(float3 dir,float3 sunDir)
{
    float day=smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f)),night=1.0f-day;
    if(night<=0.0f)return (float3)0;
    float3 moonDir2=normalize(float3(-sunDir.x,-sunDir.y,sunDir.z));
    if(dot(dir,moonDir2)<=0.0f)return (float3)0;
    float3 right=normalize(cross(float3(0,1,0),moonDir2)),up=cross(moonDir2,right);
    const float kR=0.030f;
    float2 q=float2(dot(dir,right),dot(dir,up))/kR;float r=length(q);if(r>1.0f)return (float3)0;
    float tex=uHasMoonTex?uMoonTex.Sample(uSkyLinear,q*0.5f+0.5f).r:1.0f;
    float limb=sqrt(max(1.0f-r*r,0.0f)),edge=smoothstep(1.0f,0.90f,r);
    return float3(0.92f,0.94f,1.00f)*(tex*limb*edge*3.0f*night);
}
// ── Volumetric cloud layer (3D noise-volume slab raymarch) ────────────────────
// Cloud slab heights (arbitrary world units in the sky-ray hemisphere model).
// Taller slab than a thin sheet so the billows read as towering cumuli.
static const float kCloudBase  = 1.0f;
static const float kCloudTop   = 2.6f;
static const float kCloudScale = 1.2f;    // spatial frequency of the cloud field
// Worley (cellular) lookup from the noise volume's G channel — bright at the cell
// feature points. fBm of it is the billowy cumulus shape. The bake already tiles,
// so a plain trilinear fetch is enough (Worley is C0-smooth).
float worleyNoise3(float3 p)
{
    return uNoise.SampleLevel(uSkyWrap, p * (1.0f / 256.0f), 0).g;
}
float worleyFbm(float3 p)
{
    return worleyNoise3(p)        * 0.625f
         + worleyNoise3(p * 2.03f) * 0.25f
         + worleyNoise3(p * 4.06f) * 0.125f;
}
// Henyey-Greenstein phase: forward-biased scattering so the cloud edges facing the
// sun glow (the golden sunset rim / silver lining). g>0 peaks toward the light.
float hgPhase(float cosT, float g)
{
    float g2 = g * g;
    return (1.0f - g2) / (12.566371f * pow(max(1.0f + g2 - 2.0f * g * cosT, 1e-4f), 1.5f));
}
// Rounded vertical density taper so the slab reads as puffy bodies, not a sheet.
float cloudHeightGrad(float y)
{
    float hf = clamp((y - kCloudBase) / (kCloudTop - kCloudBase), 0.0f, 1.0f);
    return smoothstep(0.0f, 0.25f, hf) * (1.0f - smoothstep(0.6f, 1.0f, hf));
}
// Full density at a world point: billowy Worley (the cauliflower shape) over a
// large-scale perlin coverage field, thresholded by the coverage slider and shaped
// by the slab height. The slab-height taper is a pure analytic function of pos.y,
// so test it FIRST and bail with zero texture fetches when outside the slab.
float cloudDensity(float3 pos, float time, float coverage, float3 wind)
{
    float hgrad = cloudHeightGrad(pos.y);
    if (hgrad <= 0.0f) return 0.0f;                                // outside slab → no fetches
    float3 p      = pos * kCloudScale + wind * time;
    float  morph  = time * 0.030f;                                 // slow forming/dissolving
    float  perlin = starFbm3(p + float3(0.0f, morph, 0.0f), 4);    // large-scale coverage
    float  billow = worleyFbm(p * 0.9f + float3(morph, 0.0f, 0.0f)); // fine cauliflower detail
    float  base   = perlin * 0.5f + billow * 0.55f;
    float  lo     = lerp(0.70f, 0.22f, clamp(coverage, 0.0f, 1.0f));
    return smoothstep(lo, lo + 0.13f, base) * hgrad;
}
// Density for the sun light-march. Slightly fewer octaves than the view density
// (shadows are lower-frequency); the slab-height test bails with zero fetches when
// the sun-ward sample steps out of the slab.
float cloudShadowDensity(float3 pos, float time, float coverage, float3 wind)
{
    float hgrad = cloudHeightGrad(pos.y);
    if (hgrad <= 0.0f) return 0.0f;
    float3 p      = pos * kCloudScale + wind * time;
    float  morph  = time * 0.030f;
    float  perlin = starFbm3(p + float3(0.0f, morph, 0.0f), 3);
    float  billow = worleyNoise3(p * 0.9f + float3(morph, 0.0f, 0.0f)) * 0.7f
                  + worleyNoise3(p * 1.8f) * 0.3f;
    float  base   = perlin * 0.5f + billow * 0.55f;
    float  lo     = lerp(0.70f, 0.22f, clamp(coverage, 0.0f, 1.0f));
    return smoothstep(lo, lo + 0.13f, base) * hgrad;
}
float3 applyClouds(float3 baseSky, float3 dir, float3 sunDir, float t,
                   float coverage, float3 sunColor, float3 wind)
{
    if(coverage <= 0.0f) return baseSky;          // clear sky → skip the whole raymarch
    dir    = normalize(dir);
    sunDir = normalize(sunDir);
    if(dir.y < 0.02f) return baseSky;             // no clouds at/below the horizon

    // March the view ray through the cloud slab between base and top heights.
    // A deterministic per-ray offset breaks up otherwise coherent sample planes
    // that show up as visible horizontal cloud layers near grazing view angles.
    float s0 = kCloudBase / max(dir.y, 1e-3f);
    float s1 = kCloudTop  / max(dir.y, 1e-3f);
    const int N = 16;
    float ds = (s1 - s0) / float(N);
    float jitter = cloudHash(dir.xz * 173.3f + float2(dir.y * 37.1f, dir.y * 19.7f));

    // Day/night/dusk drive the cloud colour (independent of the drift clock).
    float sunY = clamp(sunDir.y, -0.2f, 1.0f);
    float day  = smoothstep(-0.10f, 0.10f, sunY);
    float dusk = smoothstep(-0.06f, 0.05f, sunY) * (1.0f - smoothstep(0.05f, 0.28f, sunY));

    // Forward-scatter phase (view vs. sun) — constant along the ray, so compute once.
    float costh = max(dot(dir, sunDir), 0.0f);
    float phase = lerp(hgPhase(costh, 0.6f), hgPhase(costh, -0.3f), 0.25f);

    float T = 1.0f;                                 // transmittance along the view ray
    float3 L = (float3)0;                           // accumulated in-scattered colour
    for(int i = 0; i < N; ++i)
    {
        float s   = s0 + (float(i) + jitter) * ds;
        float3 pos = dir * s;
        float dens = cloudDensity(pos, t, coverage, wind);
        if(dens > 0.001f)
        {
            // Light-march toward the sun: Beer's-law self-shadowing (3 steps for a
            // smooth shadow gradient; fewer steps undersample and flicker).
            float shadow = 0.0f;
            for(int j = 1; j <= 3; ++j)
                shadow += cloudShadowDensity(pos + sunDir * (float(j) * 0.25f), t, coverage, wind);
            float sun    = exp(-shadow * 1.7f);
            float powder = 1.0f - exp(-dens * 3.0f); // dark soft edges (powder effect)
            float lit    = sun * powder;

            // Higher-contrast shading: dark cool shaded base, sun-coloured lit tops.
            float3 dayCol   = lerp(float3(0.17f, 0.20f, 0.29f), sunColor * 1.12f, lit);
            float3 nightCol = lerp(float3(0.015f, 0.018f, 0.035f), float3(0.26f, 0.29f, 0.45f), lit);
            float3 cloudCol = lerp(nightCol, dayCol, day);
            float3 duskTop  = sunColor * float3(1.25f, 0.55f, 0.28f);
            cloudCol = lerp(cloudCol, duskTop, dusk * lit * 0.9f);
            // Moonlit silver: moon rises on the opposite arc from the sun.
            float3 cMoonDir = normalize(float3(-sunDir.x, -sunDir.y, sunDir.z));
            float  cMoonUp  = clamp((cMoonDir.y + 0.10f) / 0.25f, 0.0f, 1.0f);
            cloudCol += float3(0.20f, 0.22f, 0.38f) * lit * cMoonUp * (1.0f - day) * 0.25f;
            // Forward-scatter glow: Henyey-Greenstein-weighted direct sunlight makes
            // the sun-facing edges flare gold (the silver lining), strongest when
            // looking toward the sun and where the cloud isn't self-shadowed.
            cloudCol += sunColor * (phase * sun * 0.9f * max(day, dusk));
            // Cheap vertical depth: tops catch the light (bright crown), the base
            // sits in self-shadow (darker, cooler) — fakes the volumetric
            // "cauliflower" relief from just the sample's height in the slab.
            float hTone = smoothstep(kCloudBase, kCloudTop, pos.y);
            cloudCol *= lerp(0.5f, 1.15f, hTone);
            cloudCol += float3(0.07f, 0.10f, 0.17f) * ((1.0f - hTone) * day * 0.25f);

            float opticalDepth = dens * ds * 7.0f;
            float a = 1.0f - exp(-opticalDepth);
            L += T * a * cloudCol;
            T *= 1.0f - a;
            if(T < 0.02f) break;
        }
    }

    // Fade the whole cloud layer out into the horizon haze.
    float horizon = smoothstep(0.02f, 0.16f, dir.y);
    T = 1.0f - (1.0f - T) * horizon;
    L *= horizon;
    return baseSky * T + L;
}

// Space nebula — drifting coloured emission clouds gathered toward the galactic band,
// sampled from the 3D noise volume (reuses starFbm3). Night/horizon gated. Mirrors GL.
float3 nebula(float3 dir, float3 cdir, float3 sunDir, float intensity, float3 nebColor)
{
    if (intensity <= 0.0f) return (float3)0;
    dir = normalize(dir); sunDir = normalize(sunDir);
    float night = 1.0f - smoothstep(-0.10f, 0.10f, clamp(sunDir.y, -0.2f, 1.0f));
    if (night <= 0.0f || dir.y <= 0.0f) return (float3)0;
    float3 cN = normalize(cdir);
    const float3 galN = normalize(float3(0.46f, 0.52f, -0.72f));
    float bd = dot(cN, galN);
    float band = exp(-bd * bd * 1.5f);
    float3 P = cN * 3.4f;
    float big  = starFbm3(P * 0.7f + 11.0f, 4);
    float med  = starFbm3(P * 1.7f + 27.0f, 3);
    float fine = starFbm3(P * 4.0f + 41.0f, 2);
    float blob = smoothstep(0.35f, 0.70f, big * 0.5f + med * 0.6f);
    float charF = starFbm3(P * 0.4f + 150.0f, 2);
    float wispy = smoothstep(0.42f, 0.70f, charF);
    float fila  = smoothstep(0.55f, 0.86f, starFbm3(P * 5.5f + 97.0f, 2));
    float detail = (0.30f + 0.70f * smoothstep(0.32f, 0.86f, fine)) * lerp(1.0f, 0.65f + 0.9f * fila, wispy);
    float dust = 1.0f - 0.5f * smoothstep(0.50f, 0.88f, starFbm3(P * 2.6f + 63.0f, 3));
    float density = blob * detail * dust;
    float core = smoothstep(0.62f, 0.95f, big * 0.55f + med * 0.55f);
    float glow = (band * 0.85f + 0.15f) * (density + 0.6f * core);
    if (glow <= 0.0f) return (float3)0;
    float h = clamp(starFbm3(P * 0.5f + 71.0f, 3) * 1.7f - 0.35f
                  + 0.25f * (starFbm3(P * 1.1f + 83.0f, 2) - 0.5f), 0.0f, 1.0f);
    float warm = smoothstep(0.40f, 0.72f, starFbm3(P * 0.32f + 131.0f, 2));
    h = clamp(h + warm * 0.30f, 0.0f, 1.0f);
    float3 colA = nebColor * float3(0.42f, 0.62f, 1.50f);
    float3 colB = nebColor * float3(0.34f, 1.42f, 1.18f);
    float3 colC = nebColor * float3(0.55f, 1.42f, 0.55f);
    float3 colD = nebColor * float3(1.75f, 1.10f, 0.40f);
    float3 colE = nebColor * float3(1.85f, 0.42f, 0.95f);
    float3 col = colA;
    col = lerp(col, colB, smoothstep(0.14f, 0.36f, h));
    col = lerp(col, colC, smoothstep(0.36f, 0.54f, h));
    col = lerp(col, colD, smoothstep(0.54f, 0.72f, h));
    col = lerp(col, colE, smoothstep(0.72f, 0.92f, h));
    float horizon = smoothstep(0.0f, 0.16f, dir.y);
    return col * (glow * 6.0f * horizon * night * intensity);
}

struct SkyVSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
float4 PSSky(SkyVSOut i) : SV_TARGET
{
    float4 wp1=mul(uInvViewProj,float4(i.ndc,1.0f,1.0f));
    float4 wp0=mul(uInvViewProj,float4(i.ndc,0.0f,1.0f));
    float3 dir=normalize(wp1.xyz/wp1.w - wp0.xyz/wp0.w);
    float3 col=skyColor(dir,uSunDir);
    float nightF=1.0f-smoothstep(-0.10f,0.10f,clamp(normalize(uSunDir).y,-0.2f,1.0f));
    if(nightF>0.0f){
        float3 cdir=celestialDir(dir,uTimeOfDay);
        col+=starField(dir,cdir,uSunDir,uTime,uMilkyWay);
        col+=nebula(dir,cdir,uSunDir,uNebula,uNebulaColor);
        col+=aurora(dir,uSunDir,uTime,uAurora,uAuroraColor);
        col+=moonDisk(dir,uSunDir);
    }
    col=applyClouds(col,dir,uSunDir,uTime,uCloudCoverage,uSunColor,uWind);
    col+=uFlash*float3(0.85f,0.90f,1.0f);
    return float4(col,1.0f);
}
)HLSL";

static const char* kDebugLineHLSL12 = R"HLSL(
cbuffer DebugCB : register(b0) { float4x4 uVP; };
struct LineIn  { float3 pos : POSITION; float3 color : COLOR0; };
struct LineOut { float4 clip : SV_POSITION; float3 color : COLOR0; };
LineOut VSLine(LineIn i){LineOut o;o.clip=mul(uVP,float4(i.pos,1.0f));o.color=i.color;return o;}
float4 PSLine(LineOut i) : SV_TARGET { return float4(i.color,1.0f); }
)HLSL";

static const char* kSceneHLSL = R"HLSL(
cbuffer PerObject : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4   uColor;    // rgb = base color, a = hasTexture (0/1)
    float4   uPBR;      // x = metallic, y = roughness, z = opacity
};
cbuffer PerFrame : register(b1)
{
    float4   uCameraPos;
    int4     uLightCount;
    float4   uLightPos[8];
    float4   uLightDir[8];
    float4   uLightColor[8];
    float4   uLightParams[8];
    float4x4 uLightVP;
    int4     uShadowEnabled;
    float4   uSunDir;    // xyz = sun direction
    float4   uFog;       // x=fogDensity, y=fogHeightFalloff
    float4   uViewport;  // x=W, y=H, z=ssaoEnabled
};
Texture2D    uShadowMap : register(t0);
Texture2D    uAlbedo    : register(t1); // base-color texture (bound per draw; null when untextured)
Texture2D    uAO        : register(t2);
SamplerState uShadowSamp : register(s0);
SamplerState uAOSampler  : register(s1);
SamplerState uAlbedoSamp : register(s2); // linear + wrap, for tiling base-color textures

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_POSITION; float3 worldPos : TEXCOORD0; float3 normal : TEXCOORD1; float2 uv : TEXCOORD2; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.worldPos = mul(uModel, float4(i.pos, 1.0)).xyz;
    o.normal   = mul((float3x3)uModel, i.normal);
    o.uv       = i.uv;
    o.clip     = mul(uMVP, float4(i.pos, 1.0));
    return o;
}
// Instanced geometry (A3): one DrawIndexedInstanced replaces the per-instance draw
// loop. Per-instance mvp + model live in a structured buffer at t3, indexed by
// SV_InstanceID, filled by the CPU exactly like the non-instanced drawOne path
// (same column-major glm bytes as the PerObject cbuffer → identical mul() math).
// uColor/uPBR stay in the shared PerObject cbuffer (batch-constant). Reuses PSMain.
struct InstXform { float4x4 mvp; float4x4 model; };
StructuredBuffer<InstXform> gInstances : register(t3);
VSOut VSMainInstanced(VSIn i, uint iid : SV_InstanceID)
{
    InstXform x = gInstances[iid];
    VSOut o;
    o.worldPos = mul(x.model, float4(i.pos, 1.0)).xyz;
    o.normal   = mul((float3x3)x.model, i.normal);
    o.uv       = i.uv;
    o.clip     = mul(x.mvp, float4(i.pos, 1.0));
    return o;
}
// Depth-only vertex shader for the shadow pass: uMVP carries lightVP * model.
float4 VSDepth(VSIn i) : SV_POSITION { return mul(uMVP, float4(i.pos, 1.0)); }

float shadowFactor(float3 worldPos, float3 N, float3 L)
{
    if (uShadowEnabled.x == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos, 1.0));
    float3 p  = lp.xyz / lp.w;
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5);
    if (p.z > 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float closest = uShadowMap.Sample(uShadowSamp, uv).r;
    return (p.z - bias > closest) ? 0.35 : 1.0;
}

static const float PI12 = 3.14159265;
float D_GGX12(float NdH, float a2) { float d = NdH*NdH*(a2-1.0)+1.0; return a2/(PI12*d*d+1e-6); }
float G_Schlick12(float NdX, float k) { return NdX/(NdX*(1.0-k)+k); }
float3 F_Schlick12(float VdH, float3 F0) { return F0+(1.0-F0)*pow(1.0-VdH,5.0); }
float3 BRDF12(float3 L, float3 V, float3 N, float3 base, float met, float rough)
{
    float a = rough*rough; float a2 = a*a;
    float k = (rough+1.0); k = k*k/8.0;
    float3 H = normalize(L+V);
    float NdL = max(dot(N,L),0.0); float NdV = max(dot(N,V),0.0001);
    float NdH = max(dot(N,H),0.0); float VdH = max(dot(V,H),0.0);
    float3 F0 = lerp(float3(0.04,0.04,0.04),base,met);
    float3 F = F_Schlick12(VdH,F0);
    float D = D_GGX12(NdH,a2);
    float G = G_Schlick12(NdV,k)*G_Schlick12(NdL,k);
    float3 spec = D*F*G/max(4.0*NdV*NdL,1e-6);
    float3 kd = (1.0-F)*(1.0-met);
    return (kd*base/PI12+spec)*NdL;
}
float4 PSMain(VSOut i) : SV_TARGET
{
    // Mirrors GL/Metal/D3D11: a base-color texture (when present, flagged by uColor.a)
    // replaces the flat colour; PBR scalars still come from uPBR.
    float3 base  = (uColor.a > 0.5) ? uAlbedo.Sample(uAlbedoSamp, i.uv).rgb : uColor.rgb;
    float  met   = uPBR.x, rough = max(uPBR.y, 0.04);
    float3 N     = normalize(i.normal);
    if (uLightCount.x == 0)
    {
        float3 L    = normalize(float3(0.5, 0.8, 0.6));
        float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        return float4(base * diff, uPBR.z);
    }
    float3 V      = normalize(uCameraPos.xyz - i.worldPos);
    float3 Nup    = normalize(float3(N.x, max(N.y, 0.1f), N.z));
    float3 Rrough = normalize(lerp(reflect(-V, N), N, rough));
    float3 F0     = lerp(float3(0.04f,0.04f,0.04f), base, met);
    float3 kd     = (1.0f - F0) * (1.0f - met);
    float3 ambDiff = skyColor(Nup,    uSunDir.xyz) * base * kd;
    float3 ambSpec = skyColor(Rrough, uSunDir.xyz) * F0;
    float  ao      = (uViewport.z > 0.5f) ? uAO.SampleLevel(uAOSampler, i.clip.xy / uViewport.xy, 0).r : 1.0f;
    float3 result  = ao * (ambDiff * 0.35f + ambSpec * (1.0f - 0.6f * rough));
    for (int li = 0; li < uLightCount.x; ++li)
    {
        int type = (int)uLightPos[li].w;
        float3 L; float atten = 1.0;
        if (type == 0) { L = normalize(-uLightDir[li].xyz); }
        else
        {
            float3 d = uLightPos[li].xyz - i.worldPos;
            float dist = max(length(d), 1e-4);
            L = d / dist;
            float range = max(uLightParams[li].x, 1e-4);
            atten = saturate(1.0 - dist / range); atten *= atten;
            if (type == 2)
            {
                float c = dot(-L, normalize(uLightDir[li].xyz));
                float cosCone = uLightDir[li].w;
                atten *= smoothstep(cosCone, lerp(cosCone, 1.0, 0.2), c);
            }
        }
        float sh = (type == 0) ? shadowFactor(i.worldPos, N, L) : 1.0;
        result += BRDF12(L, V, N, base, met, rough) * uLightColor[li].rgb * uLightColor[li].w * atten * sh;
    }
    if (uFog.x > 0.0f) {
        float3 ray = i.worldPos - uCameraPos.xyz;
        float dist = max(length(ray), 1e-4f);
        float k    = uFog.y * ray.y;
        float ta   = abs(k) > 1e-4f ? (1.0f - exp(-k)) / k : 1.0f;
        float opt  = uFog.x * dist * exp(-uFog.y * uCameraPos.y) * ta;
        float f    = 1.0f - exp(-opt);
        float3 fogCol = skyColor(ray/dist, uSunDir.xyz);
        result = lerp(result, fogCol, saturate(f));
    }
    return float4(result, uPBR.z);
}
)HLSL";

// ─── SSAO HLSL ──────────────────────────────────────────────────────────────
static const char* kSSAOPosHLSL12 = R"HLSL(
cbuffer SSAOPosCB : register(b0)
{
    float4x4 uPosMVP;
    float4x4 uPosModelView;
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
float4 PSPos(VSOut i) : SV_TARGET { return float4(i.viewPos, 1.0); }
)HLSL";

static const char* kSSAOHLSL12 = R"HLSL(
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
    if (pv.a < 0.5f) return float4(1,1,1,1);
    float3 P = pv.xyz;
    float2 texel = rcp(float2(uSSAONoiseScale.xy * 4.0f));
    float3 Pr = uViewPos.SampleLevel(uPointSamp, i.uv + float2( texel.x, 0), 0).xyz;
    float3 Pl = uViewPos.SampleLevel(uPointSamp, i.uv - float2( texel.x, 0), 0).xyz;
    float3 Pu = uViewPos.SampleLevel(uPointSamp, i.uv + float2(0,  texel.y), 0).xyz;
    float3 Pd = uViewPos.SampleLevel(uPointSamp, i.uv - float2(0,  texel.y), 0).xyz;
    float3 ddx_ = (abs(Pr.z-P.z) < abs(P.z-Pl.z)) ? (Pr-P) : (P-Pl);
    float3 ddy_ = (abs(Pd.z-P.z) < abs(P.z-Pu.z)) ? (Pd-P) : (P-Pu);
    float3 N = normalize(cross(ddx_, ddy_));
    if (N.z < 0.0f) N = -N;
    float radius    = uSSAOParams.x;
    float bias      = uSSAOParams.y;
    float intensity = uSSAOParams.z;
    int   method    = (int)uSSAOParams.w;
    float ao;
    if (method == 1)
    {
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
        float3 randv = uNoise.SampleLevel(uPointSamp, i.uv * uSSAONoiseScale.xy, 0).xyz;
        float3 T = normalize(randv - N * dot(randv, N));
        float3 B = cross(N, T);
        float3x3 TBN = float3x3(T, B, N);
        float occ = 0.0f;
        for (int k = 0; k < 32; ++k)
        {
            float3 sp = P + mul(TBN, uSSAOKernel[k].xyz) * radius;
            float4 clipSP = mul(uSSAOProj, float4(sp, 1.0f));
            float2 suv = float2(clipSP.x/clipSP.w * 0.5f + 0.5f,
                                0.5f - clipSP.y/clipSP.w * 0.5f);
            if (suv.x < 0 || suv.x > 1 || suv.y < 0 || suv.y > 1) continue;
            float4 sv = uViewPos.SampleLevel(uPointSamp, suv, 0);
            if (sv.a < 0.5f) continue;
            float3 toOcc = sv.xyz - P;
            float above = dot(toOcc, N);
            float rangeCheck = smoothstep(0.0f, 1.0f, radius / max(length(toOcc), 1e-4f));
            occ += (above > bias ? 1.0f : 0.0f) * rangeCheck;
        }
        ao = 1.0f - (occ / 32.0f) * intensity;
        ao = max(ao, 0.5f);
    }
    return float4(ao, ao, ao, 1.0f);
}
)HLSL";

// ── GPU Skeletal-Mesh Skinning VS ─────────────────────────────────────────────
static const char* kSkinnedHLSL12 = R"HLSL(
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

static const char* kUIHLSL12 = R"HLSL(
cbuffer UICB : register(b0) {
    float4 uRect;      // xy=top-left px, zw=size px
    float4 uColor;
    float4 uUVRect;    // glyph atlas UVs: xy=uvMin, zw=uvMax
    float2 uViewport;
    float  uMode;      // 0 = solid color, 1 = font-atlas glyph
    float  _upad;
};
Texture2D    uFontAtlas : register(t0);
SamplerState uSamp      : register(s0);
struct UIOut { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
UIOut UIVSMain(uint vid : SV_VertexID)
{
    static const float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    float2 uv = c[vid];
    float2 sp = uRect.xy + uv * uRect.zw;
    UIOut o;
    o.clip = float4(sp.x/uViewport.x*2.0f-1.0f, 1.0f-sp.y/uViewport.y*2.0f, 0.0f, 1.0f);
    o.uv = uv;
    return o;
}
float4 UIPSMain(UIOut i) : SV_TARGET
{
    if (uMode > 0.5f)
    {
        // Glyph coverage lives in the atlas R channel; tint by uColor (mirrors GL).
        float a = uFontAtlas.Sample(uSamp, lerp(uUVRect.xy, uUVRect.zw, i.uv)).r;
        return float4(uColor.rgb, uColor.a * a);
    }
    return uColor;
}
)HLSL";

static const char* kSSAOBlurHLSL12 = R"HLSL(
Texture2D    uAOInput   : register(t0);
SamplerState uPointSamp : register(s0);
cbuffer BlurCB : register(b0) { float2 uBlurTexel; float2 _pad; };
struct FsIn { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
float4 SSAOBlurMain(FsIn i) : SV_TARGET
{
    float sum = 0.0;
    for (int x = -2; x < 2; ++x)
        for (int y = -2; y < 2; ++y)
            sum += uAOInput.SampleLevel(uPointSamp, i.uv + float2(x,y) * uBlurTexel, 0).r;
    float ao = sum / 16.0;
    return float4(ao, ao, ao, 1.0);
}
)HLSL";

// ─── PostProcess HLSL (same logic as D3D11 — identical HLSL, same binding layout)
static const char* kFSTriangleVS12 = R"HLSL(
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
static const char* kTonemapHLSL12 = R"HLSL(
Texture2D    uHDR   : register(t0);
Texture2D    uBloom : register(t1);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float uExposure; float uBloomStrength; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float3 aces(float3 x) { return saturate((x*(2.51*x+0.03))/(x*(2.43*x+0.59)+0.14)); }
float4 main(In i) : SV_Target {
    float3 h=uHDR.Sample(uSamp,i.uv).rgb;
    h+=uBloom.Sample(uSamp,i.uv).rgb*uBloomStrength; h*=uExposure;
    return float4(pow(max(aces(h),0.0001),1.0/2.2),1.0);
}
)HLSL";
static const char* kFxaaHLSL12 = R"HLSL(
Texture2D    uScene : register(t0);
Texture2D    _dummy : register(t1);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float2 uRcpFrame; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float luma(float3 c) { return dot(c, float3(0.299,0.587,0.114)); }
float4 main(In i) : SV_Target {
    const float EMIN=1.0/24.0, EMAX=1.0/8.0, SMAX=8.0;
    float3 M=uScene.Sample(uSamp,i.uv).rgb; float lM=luma(M);
    float lNW=luma(uScene.Sample(uSamp,i.uv+float2(-1,-1)*uRcpFrame).rgb);
    float lNE=luma(uScene.Sample(uSamp,i.uv+float2( 1,-1)*uRcpFrame).rgb);
    float lSW=luma(uScene.Sample(uSamp,i.uv+float2(-1, 1)*uRcpFrame).rgb);
    float lSE=luma(uScene.Sample(uSamp,i.uv+float2( 1, 1)*uRcpFrame).rgb);
    float lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE)));
    float lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE))); float rng=lMax-lMin;
    if(rng<max(EMIN,lMax*EMAX)) return float4(M,1);
    float2 dir; dir.x=-((lNW+lNE)-(lSW+lSE)); dir.y=(lNW+lSW)-(lNE+lSE);
    float dr=max((lNW+lNE+lSW+lSE)*0.25*(1.0/8.0),1.0/128.0);
    dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+dr),-SMAX,SMAX)*uRcpFrame;
    float3 A=0.5*(uScene.Sample(uSamp,i.uv+dir*(1.0/3.0-0.5)).rgb
                 +uScene.Sample(uSamp,i.uv+dir*(2.0/3.0-0.5)).rgb);
    float3 B=A*0.5+0.25*(uScene.Sample(uSamp,i.uv+dir*-0.5).rgb
                         +uScene.Sample(uSamp,i.uv+dir*0.5).rgb);
    float lB=luma(B); return(lB<lMin||lB>lMax)?float4(A,1):float4(B,1);
}
)HLSL";
static const char* kBloomBrightHLSL12 = R"HLSL(
Texture2D    uHDR  : register(t0);
Texture2D    _dummy: register(t1);
SamplerState uSamp : register(s0);
cbuffer CB : register(b0) { float uThreshold; float uKnee; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target {
    float3 c=uHDR.Sample(uSamp,i.uv).rgb; float br=max(c.r,max(c.g,c.b));
    float s=clamp(br-uThreshold+uKnee,0.0,2.0*uKnee);
    s=(s*s)/(4.0*uKnee+1e-4); float contrib=max(s,br-uThreshold)/max(br,1e-4);
    return float4(c*contrib,1.0);
}
)HLSL";
static const char* kBloomBlurHLSL12 = R"HLSL(
Texture2D    uImage : register(t0);
Texture2D    _dummy : register(t1);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float uTexelX; float uTexelY; int uHoriz; float _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target {
    static const float w[5]={0.227027,0.1945946,0.1216216,0.054054,0.016216};
    float2 d=(uHoriz==1)?float2(uTexelX,0):float2(0,uTexelY);
    float3 r=uImage.Sample(uSamp,i.uv).rgb*w[0];
    [unroll] for(int k=1;k<5;++k){r+=uImage.Sample(uSamp,i.uv+d*k).rgb*w[k];r+=uImage.Sample(uSamp,i.uv-d*k).rgb*w[k];}
    return float4(r,1.0);
}
)HLSL";

namespace
{
    struct PerObjectCB { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 pbr; };
    struct PerFrameCB
    {
        glm::vec4  cameraPos;
        glm::ivec4 lightCount;
        glm::vec4  lightPos[8];
        glm::vec4  lightDir[8];
        glm::vec4  lightColor[8];
        glm::vec4  lightParams[8];
        glm::mat4  lightVP;
        glm::ivec4 shadowEnabled;
        glm::vec4  sunDir;     // xyz = sun direction
        glm::vec4  fog;        // x=fogDensity, y=fogHeightFalloff
        glm::vec4  viewport;   // x=W, y=H, z=ssaoEnabled
    };

    struct SkyCB {
        glm::mat4 invViewProj;
        glm::vec3 sunDir;    float timeOfDay;
        glm::vec3 sunColor;  float cloudCoverage;
        glm::vec3 wind;      float time;
        glm::vec3 auroraColor; float aurora;
        float milkyWay;      float flash; int hasMoonTex; float nebula;
        glm::vec3 nebulaColor; float _pad2;
    };

    // Remaps the extractor's GL-convention light projection to D3D clip (z 0..1).
    const glm::mat4 kD3DClipFix(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, 0.0f,
        0.0f, 0.0f, 0.5f, 1.0f);

    struct GpuMesh
    {
        ComPtr<ID3D12Resource>   vbuf;
        ComPtr<ID3D12Resource>   ibuf;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_INDEX_BUFFER_VIEW  ibv{};
        UINT                     indexCount = 0;
        HE::AABB                 localBounds;
        // Base-color texture, resolved from the mesh's baked material on first draw
        // (needs a recording command list to upload).  albedoSlot indexes the mesh-texture
        // region of sceneSrvHeap; -1 = untextured (flat uColor).  albedoTried gates the
        // one-shot resolve so a failed/absent texture isn't retried every frame.
        ComPtr<ID3D12Resource>   albedoTex;
        int                      albedoSlot  = -1;
        bool                     albedoTried = false;
    };

    // GPU buffers for a skeletal mesh — three vertex-buffer slots plus an index buffer.
    // Slot 0: interleaved pos(3)+norm(3)+uv(2), stride 32.
    // Slot 1: boneIds   uint4 per vertex,        stride 16.
    // Slot 2: boneWeights float4 per vertex,     stride 16.
    struct GpuSkeletalMesh12 {
        ComPtr<ID3D12Resource>   vbuf;        // slot 0: pos+norm+uv interleaved
        ComPtr<ID3D12Resource>   boneIdVb;   // slot 1: uint4 boneIds
        ComPtr<ID3D12Resource>   boneWgtVb;  // slot 2: float4 boneWeights
        ComPtr<ID3D12Resource>   ibuf;
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        D3D12_VERTEX_BUFFER_VIEW boneIdVbv{};
        D3D12_VERTEX_BUFFER_VIEW boneWgtVbv{};
        D3D12_INDEX_BUFFER_VIEW  ibv{};
        UINT                     indexCount = 0;
        ComPtr<ID3D12Resource>   albedoTex;             // base-color texture (see GpuMesh)
        int                      albedoSlot  = -1;
        bool                     albedoTried = false;
    };

    UINT alignUp(UINT v, UINT a) { return (v + a - 1u) & ~(a - 1u); }

    struct SsaoRng {
        uint32_t s;
        float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); }
    };
    static std::vector<glm::vec3> BuildSSAOKernel(int n) {
        SsaoRng rng{ 0x9E3779B9u };
        std::vector<glm::vec3> k(n);
        for (int i = 0; i < n; ++i) {
            glm::vec3 s(rng.next()*2-1, rng.next()*2-1, rng.next());
            s = glm::normalize(s) * rng.next();
            float t = float(i)/float(n); s *= 0.1f + 0.9f * t * t;
            k[i] = s;
        }
        return k;
    }
    static std::vector<glm::vec3> BuildSSAONoise(int n) {
        SsaoRng rng{ 0x2545F491u };
        std::vector<glm::vec3> v(n);
        for (int i = 0; i < n; ++i)
            v[i] = glm::vec3(rng.next()*2-1, rng.next()*2-1, 0.0f);
        return v;
    }
}

// ── DRED (Device Removed Extended Data) ──────────────────────────────────────
// On a device-hung TDR (DXGI_ERROR_DEVICE_HUNG) the GPU stops mid-command. DRED
// records auto-breadcrumbs (the op history per command list) + the faulting VA, so
// we can name the exact operation that hung instead of guessing.
static const char* dredOpName(D3D12_AUTO_BREADCRUMB_OP op)
{
    switch (op)
    {
    case D3D12_AUTO_BREADCRUMB_OP_SETMARKER:              return "SETMARKER";
    case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT:             return "BEGINEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT:               return "ENDEVENT";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED:          return "DRAWINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:   return "DRAWINDEXEDINSTANCED";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT:        return "EXECUTEINDIRECT";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCH:               return "DISPATCH";
    case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION:       return "COPYBUFFERREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION:      return "COPYTEXTUREREGION";
    case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE:           return "COPYRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE:     return "RESOLVESUBRESOURCE";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:  return "CLEARRTV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:return "CLEARUAV";
    case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:  return "CLEARDSV";
    case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER:        return "RESOURCEBARRIER";
    case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE:          return "EXECUTEBUNDLE";
    case D3D12_AUTO_BREADCRUMB_OP_PRESENT:                return "PRESENT";
    case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA:       return "RESOLVEQUERYDATA";
    case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS:           return "DISPATCHRAYS";
    default:                                              return "OTHER";
    }
}
static void logD3D12DredOutput(ID3D12Device* device)
{
    ComPtr<ID3D12DeviceRemovedExtendedData> dred;
    if (!device || FAILED(device->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        Logger::Log(Logger::LogLevel::Error, "D3D12 DRED: not available (no breadcrumb data)");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
    {
        const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode;
        for (int n = 0; node && n < 24; node = node->pNext, ++n)
        {
            const UINT32 done  = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
            const UINT32 total = node->BreadcrumbCount;
            if (done == total && total != 0) continue;   // this list finished — not the culprit
            char hdr[256];
            std::snprintf(hdr, sizeof(hdr),
                "D3D12 DRED node cmdList='%s' completed %u/%u ops (hang after op %u). Full op history:",
                node->pCommandListDebugNameA ? node->pCommandListDebugNameA : "(unnamed)",
                done, total, done);
            Logger::Log(Logger::LogLevel::Error, hdr);
            // Print the WHOLE history (capped): the fullscreen passes (sky/SSAO/PostFX)
            // are DRAWINSTANCED while geometry is DRAWINDEXEDINSTANCED, so the sequence
            // structure reveals which pass the hung op belongs to. Collapse long runs of
            // the same op into a single "xN" line to keep the log readable.
            const UINT32 cap = std::min(total, 220u);
            UINT32 i = 0;
            while (i < cap)
            {
                char line[160];
                if (i == done)   // the hung op — always on its own line
                {
                    std::snprintf(line, sizeof(line), "   op[%u]  <== HUNG HERE = %s",
                                  i, dredOpName(node->pCommandHistory[i]));
                    Logger::Log(Logger::LogLevel::Error, line);
                    ++i; continue;
                }
                const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
                UINT32 j = i + 1;
                while (j < cap && j != done && node->pCommandHistory[j] == op) ++j;
                if (j - i > 1)
                    std::snprintf(line, sizeof(line), "   op[%u..%u] = %s x%u",
                                  i, j - 1, dredOpName(op), j - i);
                else
                    std::snprintf(line, sizeof(line), "   op[%u] = %s", i, dredOpName(op));
                Logger::Log(Logger::LogLevel::Error, line);
                i = j;
            }
        }
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
    {
        char line[128];
        std::snprintf(line, sizeof(line), "D3D12 DRED: GPU page-fault at VA 0x%llX",
            static_cast<unsigned long long>(pf.PageFaultVA));
        Logger::Log(Logger::LogLevel::Error, line);
    }
}

struct D3D12RendererImpl
{
    ComPtr<ID3D12Device>              device;
    ComPtr<ID3D12CommandQueue>        cmdQueue;
    ComPtr<IDXGISwapChain3>           swapchain;
    ComPtr<ID3D12DescriptorHeap>      rtvHeap;
    ComPtr<ID3D12Resource>            renderTargets[k_frameCount];
    ComPtr<ID3D12CommandAllocator>    cmdAllocators[k_frameCount];
    ComPtr<ID3D12GraphicsCommandList> cmdList;
    ComPtr<ID3D12Fence>               fence;
    HANDLE                            fenceEvent  = nullptr;
    UINT64                            fenceValue  = 0;   // single monotonic counter
    UINT64                            fenceValues[k_frameCount]{}; // value each slot last signaled
    UINT                              rtvDescSize = 0;
    UINT                              frameIndex  = 0;
    bool                              vsync       = true;
    bool                              allowTearing = false; // gated on DXGI feature support
    bool                              gpuDebug    = false;  // _DEBUG or env HE_GPU_DEBUG
    bool                              deviceRemovedLogged = false; // dump DRED only once
    bool                              drawCountsLogged = false;    // log pass draw counts once
    int                               width = 0, height = 0;
    ComPtr<ID3D12InfoQueue>           infoQueue;  // drained to Logger each frame when gpuDebug

    // ── GPU frame timing (timestamp query heap) ─────────────────────────────
    // Two timestamps per frame-in-flight slot (list begin/end), resolved into a
    // persistently-mapped READBACK buffer at end-of-list. A slot is only read the
    // next time that slot is reused — i.e. after waitForFrame(slot) — so the value
    // consumed is k_frameCount frames old and the readback never stalls the CPU.
    ComPtr<ID3D12QueryHeap> tsQueryHeap;              // 2 × k_frameCount timestamps
    ComPtr<ID3D12Resource>  tsReadback;               // 2 × k_frameCount × uint64
    const uint64_t*         tsReadbackPtr = nullptr;  // READBACK buffers may stay mapped
    UINT64                  tsFrequency   = 0;        // ticks/s; 0 → timing unavailable
    bool                    tsPending[k_frameCount]{};
    double                  lastGpuFrameMs = -1.0;    // newest reaped whole-frame time
    // CPU counters (draws/tris this frame, cull results) merged by GetFrameGpuStats.
    uint32_t statDraws = 0, statTris = 0, statVisible = 0, statTotal = 0;

    void createGpuTimer()
    {
        if (FAILED(cmdQueue->GetTimestampFrequency(&tsFrequency)) || tsFrequency == 0)
        {
            tsFrequency = 0;
            return;
        }
        D3D12_QUERY_HEAP_DESC qd{};
        qd.Type  = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
        qd.Count = 2 * k_frameCount;
        if (FAILED(device->CreateQueryHeap(&qd, IID_PPV_ARGS(&tsQueryHeap))))
        {
            tsFrequency = 0;
            return;
        }
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = 2ull * k_frameCount * sizeof(uint64_t);
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tsReadback))))
        {
            tsQueryHeap.Reset();
            tsFrequency = 0;
            return;
        }
        void* mapped = nullptr;
        D3D12_RANGE all{ 0, static_cast<SIZE_T>(rd.Width) };
        if (FAILED(tsReadback->Map(0, &all, &mapped)) || !mapped)
        {
            tsReadback.Reset(); tsQueryHeap.Reset();
            tsFrequency = 0;
            return;
        }
        tsReadbackPtr = static_cast<const uint64_t*>(mapped);
    }

    // ── Depth ───────────────────────────────────────────────────────────────
    ComPtr<ID3D12DescriptorHeap> dsvHeap; // [0] = scene depth, [1] = shadow depth
    ComPtr<ID3D12Resource>       depthBuffer;
    UINT                         dsvDescSize = 0;

    // ── Shadow map ──────────────────────────────────────────────────────────
    ComPtr<ID3D12PipelineState>  depthPSO;        // depth-only pass
    ComPtr<ID3D12Resource>       shadowDepth;
    ComPtr<ID3D12DescriptorHeap> shadowSrvHeap;   // shader-visible, 1 SRV
    int                          shadowSize = 2048;
    D3D12_RESOURCE_STATES        shadowState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle(UINT index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = dsvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * dsvDescSize;
        return h;
    }

    // ── Viewport offscreen render target ────────────────────────────────────
    ComPtr<ID3D12Resource>       viewportRT;          // RENDER_TARGET | SHADER_RESOURCE
    ComPtr<ID3D12Resource>       viewportDepth;       // depth for the viewport pass
    ComPtr<ID3D12Resource>       viewportReadback;    // READBACK staging for CaptureViewport
    ComPtr<ID3D12DescriptorHeap> viewportRtvHeap;     // 1-slot RTV heap for viewport
    ComPtr<ID3D12DescriptorHeap> viewportDsvHeap;     // 1-slot DSV heap for viewport
    UINT     viewportW            = 0;
    UINT     viewportH            = 0;
    UINT     viewportReqW         = 0;
    UINT     viewportReqH         = 0;
    bool     viewportResChanged   = false;
    D3D12_RESOURCE_STATES viewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // On resize the OLD viewport RT must not be destroyed immediately: the editor's
    // ImGui descriptor still points at it for the current frame (it only updates next
    // frame on HasViewportResourceChanged), and ImGui samples it later this same frame.
    // Retire it here and free it a few frames later, once the GPU is done. Otherwise
    // ImGui samples a destroyed resource → GPU TDR/device-hung.
    std::vector<std::pair<ComPtr<ID3D12Resource>, int>> retiredViewportRTs;

    void createViewportRT(UINT w, UINT h)
    {
        // Wait for GPU to finish using old resources before destroying them.
        waitForAllFrames();

        // Retire (don't destroy) the old color RT: the editor's ImGui descriptor still
        // references it for the current frame's overlay draw. It's freed a few frames
        // later by the retire-sweep in Render().
        if (viewportRT)
            retiredViewportRTs.emplace_back(std::move(viewportRT), k_frameCount + 2);
        viewportRT.Reset();   // now null (moved-from); the new RT is created below
        viewportDepth.Reset();
        viewportReadback.Reset();
        viewportRtvHeap.Reset();
        viewportDsvHeap.Reset();

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        // Color texture: RENDER_TARGET + SHADER_RESOURCE
        D3D12_RESOURCE_DESC cd{};
        cd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        cd.Width            = w;
        cd.Height           = h;
        cd.DepthOrArraySize = 1;
        cd.MipLevels        = 1;
        cd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        cd.SampleDesc.Count = 1;
        cd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE ccv{};
        ccv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ccv.Color[0] = 0.0f; ccv.Color[1] = 0.0f; ccv.Color[2] = 0.0f; ccv.Color[3] = 1.0f;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &cd,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &ccv, IID_PPV_ARGS(&viewportRT))))
            return;

        // RTV for the color texture
        D3D12_DESCRIPTOR_HEAP_DESC rtvHd{};
        rtvHd.NumDescriptors = 1;
        rtvHd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        device->CreateDescriptorHeap(&rtvHd, IID_PPV_ARGS(&viewportRtvHeap));
        device->CreateRenderTargetView(viewportRT.Get(), nullptr,
            viewportRtvHeap->GetCPUDescriptorHandleForHeapStart());

        // Depth texture
        D3D12_RESOURCE_DESC dd{};
        dd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        dd.Width            = w;
        dd.Height           = h;
        dd.DepthOrArraySize = 1;
        dd.MipLevels        = 1;
        dd.Format           = DXGI_FORMAT_D32_FLOAT;
        dd.SampleDesc.Count = 1;
        dd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE dcv{};
        dcv.Format = DXGI_FORMAT_D32_FLOAT; dcv.DepthStencil.Depth = 1.0f;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &dcv, IID_PPV_ARGS(&viewportDepth));

        D3D12_DESCRIPTOR_HEAP_DESC dsvHd{};
        dsvHd.NumDescriptors = 1;
        dsvHd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        device->CreateDescriptorHeap(&dsvHd, IID_PPV_ARGS(&viewportDsvHeap));
        device->CreateDepthStencilView(viewportDepth.Get(), nullptr,
            viewportDsvHeap->GetCPUDescriptorHandleForHeapStart());

        // Readback buffer for CaptureViewport (row-aligned width × height × 4 bytes)
        UINT rowPitch    = alignUp(w * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        UINT64 totalSize = static_cast<UINT64>(rowPitch) * h;
        D3D12_HEAP_PROPERTIES rbHp{}; rbHp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rbd{};
        rbd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rbd.Width            = totalSize;
        rbd.Height           = 1;
        rbd.DepthOrArraySize = 1;
        rbd.MipLevels        = 1;
        rbd.Format           = DXGI_FORMAT_UNKNOWN;
        rbd.SampleDesc.Count = 1;
        rbd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        device->CreateCommittedResource(&rbHp, D3D12_HEAP_FLAG_NONE, &rbd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&viewportReadback));

        viewportW          = w;
        viewportH          = h;
        viewportState      = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        viewportResChanged = true;

        // Recreate PostFX intermediate RTs to match new viewport size.
        createPostFXResources(w, h);
    }

    // ── PostFX textures (HDR, bloom×2, LDR) ─────────────────────────────────
    ComPtr<ID3D12Resource>       hdrRT;              // RGBA16F scene color
    ComPtr<ID3D12DescriptorHeap> hdrRtvHeap;         // 1-slot RTV
    D3D12_RESOURCE_STATES        hdrState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    ComPtr<ID3D12Resource>       bloomRT[2];         // RGBA16F, half-res
    ComPtr<ID3D12DescriptorHeap> bloomRtvHeap[2];    // 1-slot RTV each
    D3D12_RESOURCE_STATES        bloomState[2] = { D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_RENDER_TARGET };

    ComPtr<ID3D12Resource>       ldrRT;              // RGBA8 tonemap output
    ComPtr<ID3D12DescriptorHeap> ldrRtvHeap;         // 1-slot RTV
    D3D12_RESOURCE_STATES        ldrState = D3D12_RESOURCE_STATE_RENDER_TARGET;

    // Shader-visible SRV heap: [0]=dummy, [1]=hdrSRV, [2]=bloomSRV[0], [3]=bloomSRV[1], [4]=ldrSRV
    ComPtr<ID3D12DescriptorHeap> postFxSrvHeap;      // 5-slot SRV, shader-visible
    UINT                         srvDescSize = 0;

    // ── PostFX pipelines ─────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  postFxRootSig;
    ComPtr<ID3D12PipelineState>  tonemapPSO;
    ComPtr<ID3D12PipelineState>  fxaaPSO;
    ComPtr<ID3D12PipelineState>  bloomBrightPSO;
    ComPtr<ID3D12PipelineState>  bloomBlurPSO;
    bool                         postFxReady = false;
    UINT                         postFxW = 0, postFxH = 0;

    // PostFX settings
    float exposure       = 1.0f;
    float bloomStrength  = 0.25f;
    float bloomThreshold = 1.0f;
    float bloomKnee      = 0.1f;
    bool  bloomEnabled   = true;
    bool  fxaaEnabled    = true;

    D3D12_CPU_DESCRIPTOR_HANDLE srvCpuHandle(UINT slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = postFxSrvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * srvDescSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE srvGpuHandle(UINT slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = postFxSrvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * srvDescSize;
        return h;
    }

    // Creates the RGBA16F HDR RT, bloom[2] RT and RGBA8 LDR RT, plus their RTVs
    // and the PostFX SRV descriptor heap.  Called whenever the viewport is resized.
    void createPostFXResources(UINT w, UINT h)
    {
        waitForAllFrames();
        hdrRT.Reset(); hdrRtvHeap.Reset();
        bloomRT[0].Reset(); bloomRtvHeap[0].Reset();
        bloomRT[1].Reset(); bloomRtvHeap[1].Reset();
        ldrRT.Reset(); ldrRtvHeap.Reset();
        postFxSrvHeap.Reset();

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        srvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

        // Helper: create a 2D DEFAULT-heap RT, 1-slot RTV heap, and fill slot in srvHeap.
        auto makeTex = [&](DXGI_FORMAT fmt, UINT tw, UINT th,
                           ComPtr<ID3D12Resource>& rt,
                           ComPtr<ID3D12DescriptorHeap>& rtvH,
                           UINT srvSlot, float clearR=0, float clearG=0, float clearB=0) -> bool
        {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = tw; rd.Height = th; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
            rd.Format = fmt; rd.SampleDesc.Count = 1;
            rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{}; cv.Format = fmt;
            cv.Color[0]=clearR; cv.Color[1]=clearG; cv.Color[2]=clearB; cv.Color[3]=1.0f;
            if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&rt))))
                return false;
            D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 1;
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvH))))
                return false;
            device->CreateRenderTargetView(rt.Get(), nullptr,
                rtvH->GetCPUDescriptorHandleForHeapStart());
            if (postFxSrvHeap)
            {
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = fmt; sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(rt.Get(), &sv, srvCpuHandle(srvSlot));
            }
            return true;
        };

        // Build the SRV heap first so makeTex can fill slots immediately.
        { D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.NumDescriptors = 5;
          hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
          hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
          device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&postFxSrvHeap)); }

        // Slot 0: null/dummy SRV (used as t1 placeholder for single-texture passes).
        // D3D12 requires pDesc to be non-null when pResource is null.
        D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv0{};
        nullSrv0.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        nullSrv0.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        nullSrv0.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        nullSrv0.Texture2D.MipLevels     = 1;
        if (postFxSrvHeap)
            device->CreateShaderResourceView(nullptr, &nullSrv0, srvCpuHandle(0));

        const UINT bw = std::max(1u, w/2), bh = std::max(1u, h/2);
        makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, w,  h,  hdrRT,     hdrRtvHeap,     1);
        makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, bw, bh, bloomRT[0], bloomRtvHeap[0], 2);
        makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT, bw, bh, bloomRT[1], bloomRtvHeap[1], 3);
        makeTex(DXGI_FORMAT_R8G8B8A8_UNORM,     w,  h,  ldrRT,     ldrRtvHeap,     4);

        // All freshly created in RENDER_TARGET state.
        hdrState      = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bloomState[0] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        bloomState[1] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ldrState      = D3D12_RESOURCE_STATE_RENDER_TARGET;
        postFxW = w; postFxH = h;
    }

    // Compiles and creates all PostFX root signatures + PSOs.
    // Call once after the device is initialized.
    bool createPostFXPipelines()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* src, const char* entry, const char* profile,
                           ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(src, strlen(src), entry, nullptr, nullptr,
                                  entry, profile, flags, 0, &out, &err)))
            {
                Logger::Log(Logger::LogLevel::Error,
                    (std::string("D3D12 PostFX '") + entry + "' failed: "
                    + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };

        // Root signature: 32-bit constants (b0), SRV table t0, SRV table t1, static linear-clamp sampler.
        {
            D3D12_DESCRIPTOR_RANGE r0{}; r0.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r0.NumDescriptors=1; r0.BaseShaderRegister=0;
            D3D12_DESCRIPTOR_RANGE r1{}; r1.RangeType=D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r1.NumDescriptors=1; r1.BaseShaderRegister=1;
            D3D12_ROOT_PARAMETER params[3]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
            params[0].Constants = { 0, 0, 4 }; params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable = { 1, &r0 }; params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            params[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[2].DescriptorTable = { 1, &r1 }; params[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 3; rsd.pParameters = params;
            rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
            ComPtr<ID3DBlob> sig, err;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                                   IID_PPV_ARGS(&postFxRootSig))))
                return false;
        }

        ComPtr<ID3DBlob> vsB, tmB, fxB, brB, blB;
        if (!compile(kFSTriangleVS12,   "main","vs_5_0",vsB)) return false;
        if (!compile(kTonemapHLSL12,    "main","ps_5_0",tmB)) return false;
        if (!compile(kFxaaHLSL12,       "main","ps_5_0",fxB)) return false;
        if (!compile(kBloomBrightHLSL12,"main","ps_5_0",brB)) return false;
        if (!compile(kBloomBlurHLSL12,  "main","ps_5_0",blB)) return false;

        auto makePSO = [&](ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtFmt,
                           ComPtr<ID3D12PipelineState>& out) -> bool
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = postFxRootSig.Get();
            pd.VS = { vs->GetBufferPointer(), vs->GetBufferSize() };
            pd.PS = { ps->GetBufferPointer(), ps->GetBufferSize() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.DepthStencilState.DepthEnable = FALSE;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1; pd.RTVFormats[0] = rtFmt;
            pd.SampleDesc.Count = 1;
            return SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
        };

        if (!makePSO(vsB.Get(), tmB.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, tonemapPSO))   return false;
        if (!makePSO(vsB.Get(), fxB.Get(), DXGI_FORMAT_R8G8B8A8_UNORM, fxaaPSO))      return false;
        if (!makePSO(vsB.Get(), brB.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, bloomBrightPSO)) return false;
        if (!makePSO(vsB.Get(), blB.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, bloomBlurPSO))   return false;

        postFxReady = true;
        return true;
    }

    // Inline resource barrier helper used in the PostFX pass chain.
    void barrier12(ID3D12GraphicsCommandList* cl, ID3D12Resource* res,
                   D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        if (before == after) return;
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    }

    // Run the PostFX chain inside an already-recording command list.
    // Returns the GPU state with viewportRT in PIXEL_SHADER_RESOURCE (ready for ImGui).
    void runPostFX(ID3D12GraphicsCommandList* cl, UINT w, UINT h)
    {
        if (!postFxReady || !postFxSrvHeap || !hdrRT) return;

        const UINT bw = std::max(1u, w/2), bh = std::max(1u, h/2);
        const float tw = 1.0f/float(bw), th = 1.0f/float(bh);

        // Bind PostFX root sig + SRV heap once for all passes.
        cl->SetGraphicsRootSignature(postFxRootSig.Get());
        ID3D12DescriptorHeap* heaps[] = { postFxSrvHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        auto setVP = [&](UINT pw, UINT ph) {
            D3D12_VIEWPORT vp{0,0,(float)pw,(float)ph,0,1};
            D3D12_RECT sc{0,0,(LONG)pw,(LONG)ph};
            cl->RSSetViewports(1,&vp); cl->RSSetScissorRects(1,&sc);
        };
        auto setRTV = [&](ComPtr<ID3D12DescriptorHeap>& rtvH) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvH->GetCPUDescriptorHandleForHeapStart();
            cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        };

        // HDR should already be in RENDER_TARGET after scene writes.
        // Transition to SRV for PostFX reads.
        barrier12(cl, hdrRT.Get(), hdrState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        hdrState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ── Bloom bright pass: hdrSRV → bloomRT[0] ─────────────────────────
        barrier12(cl, bloomRT[0].Get(), bloomState[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        bloomState[0] = D3D12_RESOURCE_STATE_RENDER_TARGET;
        setRTV(bloomRtvHeap[0]); setVP(bw, bh);
        cl->SetPipelineState(bloomBrightPSO.Get());
        { const float cb[4]={bloomThreshold,bloomKnee,0,0};
          cl->SetGraphicsRoot32BitConstants(0,4,cb,0); }
        cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle(1)); // t0=hdrSRV
        cl->SetGraphicsRootDescriptorTable(2, srvGpuHandle(0)); // t1=dummy
        cl->DrawInstanced(3,1,0,0);

        // ── 10 ping-pong blur passes ────────────────────────────────────────
        cl->SetPipelineState(bloomBlurPSO.Get());
        bool horiz = true;
        for (int pass = 0; pass < 10; ++pass)
        {
            const int dst = horiz?1:0, src = horiz?0:1;
            barrier12(cl, bloomRT[dst].Get(), bloomState[dst], D3D12_RESOURCE_STATE_RENDER_TARGET);
            bloomState[dst] = D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier12(cl, bloomRT[src].Get(), bloomState[src], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            bloomState[src] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            setRTV(bloomRtvHeap[dst]); setVP(bw, bh);
            const float cb[4]={tw,th,horiz?1.0f:0.0f,0.0f};
            cl->SetGraphicsRoot32BitConstants(0,4,cb,0);
            cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle(static_cast<UINT>(2+src))); // t0=bloom[src]
            cl->SetGraphicsRootDescriptorTable(2, srvGpuHandle(0)); // t1=dummy
            cl->DrawInstanced(3,1,0,0);
            horiz = !horiz;
        }
        // After 10 passes: bloom result in bloomRT[0] (slot 2), currently RT.
        // Transition to SRV so tonemap can sample it.
        barrier12(cl, bloomRT[0].Get(), bloomState[0], D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        bloomState[0] = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ── Tonemap: (hdrSRV, bloomSRV[0]) → ldrRT ─────────────────────────
        barrier12(cl, ldrRT.Get(), ldrState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ldrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        setRTV(ldrRtvHeap); setVP(w, h);
        cl->SetPipelineState(tonemapPSO.Get());
        { const float cb[4]={exposure, bloomEnabled?bloomStrength:0.0f, 0,0};
          cl->SetGraphicsRoot32BitConstants(0,4,cb,0); }
        cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle(1)); // t0=hdrSRV
        cl->SetGraphicsRootDescriptorTable(2, srvGpuHandle(2)); // t1=bloomSRV[0]
        cl->DrawInstanced(3,1,0,0);

        // ── FXAA: ldrSRV → viewportRT ─────────────────────────────────────
        barrier12(cl, ldrRT.Get(), ldrState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ldrState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barrier12(cl, viewportRT.Get(), viewportState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        viewportState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        { D3D12_CPU_DESCRIPTOR_HANDLE vrtv = viewportRtvHeap->GetCPUDescriptorHandleForHeapStart();
          cl->OMSetRenderTargets(1, &vrtv, FALSE, nullptr); }
        setVP(w, h);
        cl->SetPipelineState(fxaaPSO.Get());
        { const float cb[4]={1.0f/float(w),1.0f/float(h),0,0};
          cl->SetGraphicsRoot32BitConstants(0,4,cb,0); }
        cl->SetGraphicsRootDescriptorTable(1, srvGpuHandle(4)); // t0=ldrSRV
        cl->SetGraphicsRootDescriptorTable(2, srvGpuHandle(0)); // t1=dummy
        cl->DrawInstanced(3,1,0,0);

        // Transition viewport RT to PSR so ImGui can sample it.
        barrier12(cl, viewportRT.Get(), viewportState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        viewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // Reset intermediate RT states for the next frame.
        barrier12(cl, ldrRT.Get(), ldrState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ldrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier12(cl, hdrRT.Get(), hdrState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        hdrState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barrier12(cl, bloomRT[0].Get(), bloomState[0], D3D12_RESOURCE_STATE_RENDER_TARGET);
        bloomState[0] = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }

    // Bake the RG16 3D noise volume on the CPU and upload it into a TEXTURE3D, then
    // create its SRV in skyEnvHeap slot 1 (t1). Mirrors SetMoonTexture's upload path
    // but for a 3-dimensional resource with per-slice/per-row aligned copies.
    bool uploadSkyNoise3D()
    {
        if (!device || !skyEnvHeap) return false;
#ifdef NDEBUG
        constexpr int kNoiseN = 256;   // full resolution in release
#else
        constexpr int kNoiseN = 64;    // cheaper bake for debug iteration
#endif
        auto noise = BuildSkyNoise3D(kNoiseN);   // RG16, 4 bytes/texel, tightly packed

        // DEFAULT-heap TEXTURE3D in COPY_DEST.
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        td.Width  = static_cast<UINT>(kNoiseN);
        td.Height = static_cast<UINT>(kNoiseN);
        td.DepthOrArraySize = static_cast<UINT16>(kNoiseN);
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R16G16_UNORM;
        td.SampleDesc.Count = 1;
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&skyNoiseTex12))))
            return false;

        // UPLOAD-heap staging buffer. Pitch alignment applies per row; the implicit
        // 3D slice pitch in D3D12 is rowPitch * Height, matching our slicePitch.
        const UINT   rowPitch   = alignUp(static_cast<UINT>(kNoiseN) * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        const UINT64 slicePitch = static_cast<UINT64>(rowPitch) * kNoiseN;
        const UINT64 uploadSize = slicePitch * kNoiseN;
        D3D12_HEAP_PROPERTIES uhp{}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC ubd{};
        ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        ubd.Width = uploadSize; ubd.Height = 1; ubd.DepthOrArraySize = 1;
        ubd.MipLevels = 1; ubd.Format = DXGI_FORMAT_UNKNOWN;
        ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> uploadBuf;
        if (FAILED(device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ubd,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf))))
        { skyNoiseTex12.Reset(); return false; }

        void* mapped = nullptr; D3D12_RANGE noRead{0,0};
        uploadBuf->Map(0, &noRead, &mapped);
        if (!mapped) { skyNoiseTex12.Reset(); return false; }
        {
            const uint8_t* srcBytes = reinterpret_cast<const uint8_t*>(noise.data());
            uint8_t*       dstBytes = static_cast<uint8_t*>(mapped);
            for (int z = 0; z < kNoiseN; ++z)
                for (int y = 0; y < kNoiseN; ++y)
                    std::memcpy(dstBytes + z * slicePitch + static_cast<UINT64>(y) * rowPitch,
                                srcBytes + (static_cast<size_t>(z) * kNoiseN + y) * kNoiseN * 4,
                                static_cast<size_t>(kNoiseN) * 4);
        }
        uploadBuf->Unmap(0, nullptr);

        waitForAllFrames();
        cmdAllocators[0]->Reset();
        cmdList->Reset(cmdAllocators[0].Get(), nullptr);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuf.Get(); src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R16G16_UNORM,
            static_cast<UINT>(kNoiseN), static_cast<UINT>(kNoiseN),
            static_cast<UINT>(kNoiseN) /*Depth*/, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = skyNoiseTex12.Get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = skyNoiseTex12.Get();
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cmdList->ResourceBarrier(1, &b);
        cmdList->Close();
        ID3D12CommandList* lists[] = { cmdList.Get() };
        cmdQueue->ExecuteCommandLists(1, lists);
        waitForAllFrames();

        // SRV in skyEnvHeap slot 1 (t1).
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = DXGI_FORMAT_R16G16_UNORM;
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture3D.MipLevels = 1;
        D3D12_CPU_DESCRIPTOR_HANDLE h = skyEnvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(1) *
                 device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateShaderResourceView(skyNoiseTex12.Get(), &sv, h);
        return true;
    }

    bool createSkyPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const std::string& src, const char* entry, const char* profile,
                           ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(src.c_str(), src.size(), entry, nullptr, nullptr,
                                  entry, profile, flags, 0, &out, &err)))
            {
                Logger::Log(Logger::LogLevel::Error,
                    (std::string("D3D12 sky '") + entry + "': " +
                     (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsB, psB;
        const std::string skyPS = std::string(kSkyFuncHLSL12) + kSkyPSHLSL12;
        if (!compile(kSkyVSHLSL12, "VSSky", "vs_5_0", vsB)) return false;
        if (!compile(skyPS, "PSSky", "ps_5_0", psB)) return false;

        // Root sig: [0] CBV b0 (sky env), [1] SRV table t0..t1 (moon + 3D noise),
        // static samplers s0 (CLAMP linear, moon) + s1 (WRAP linear, noise volume).
        {
            D3D12_DESCRIPTOR_RANGE r{};
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 2; r.BaseShaderRegister = 0;
            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor    = { 0, 0 };
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable = { 1, &r };
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderRegister = 0; samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samp1{};
            samp1.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp1.AddressU = samp1.AddressV = samp1.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            samp1.ShaderRegister = 1; samp1.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC samps[2] = { samp, samp1 };
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2; rsd.pParameters = params;
            rsd.NumStaticSamplers = 2; rsd.pStaticSamplers = samps;
            ComPtr<ID3DBlob> sig, err;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&skyRootSig))))
                return false;
        }

        // SRV heap: slot 0 = moon texture (t0), slot 1 = 3D noise volume (t1).
        // Create a null SRV in slot 0 as placeholder until SetMoonTexture runs.
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 2;
            hd.Type  = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&skyEnvHeap));
            // Null SRV so the slot is always valid (hasMoonTex=0 → shader reads it but ignores result).
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
            nullSrv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &nullSrv,
                skyEnvHeap->GetCPUDescriptorHandleForHeapStart());
            // Null Texture3D SRV in slot 1 (t1) as a placeholder. The descriptor
            // table binds the WHOLE heap and the shader samples uNoise unconditionally
            // when coverage>0, so slot 1 must hold a VALID descriptor even if the
            // noise upload below fails. A null SRV reads as 0 → cloudDensity → no
            // clouds (the intended "clouds just won't sample" fallback). Overwritten
            // on a successful uploadSkyNoise3D().
            D3D12_SHADER_RESOURCE_VIEW_DESC nullNoiseSrv{};
            nullNoiseSrv.Format = DXGI_FORMAT_R16G16_UNORM;
            nullNoiseSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            nullNoiseSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullNoiseSrv.Texture3D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE noiseSlot = skyEnvHeap->GetCPUDescriptorHandleForHeapStart();
            noiseSlot.ptr += static_cast<SIZE_T>(1) *
                device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            device->CreateShaderResourceView(nullptr, &nullNoiseSrv, noiseSlot);
        }

        // Bake + upload the 3D cloud noise volume into skyEnvHeap slot 1.
        // (cmdQueue/cmdList/cmdAllocators/fence are all live by the time this runs —
        //  createSkyPipeline is invoked from createPipeline, after Initialize has
        //  created the queue/allocators/closed cmdList/fence.) Non-fatal on failure.
        if (!uploadSkyNoise3D())
            Logger::Log(Logger::LogLevel::Error,
                "D3D12Renderer: sky 3D noise upload failed — volumetric clouds disabled");

        // Upload CBs for sky env (one per frame in flight).
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            skyCBuf[f] = createUploadBuffer(alignUp(sizeof(SkyCB), k_cbSlot),
                                            reinterpret_cast<void**>(&skyCBufPtr[f]));
        }

        // Helper: make a sky PSO for a given RT format.
        auto makeSkyPso = [&](DXGI_FORMAT fmt, ComPtr<ID3D12PipelineState>& out) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = skyRootSig.Get();
            pd.VS = { vsB->GetBufferPointer(), vsB->GetBufferSize() };
            pd.PS = { psB->GetBufferPointer(), psB->GetBufferSize() };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.DepthStencilState.DepthEnable = FALSE;  // sky ignores depth
            pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;       // must match bound DSV even when depth is off
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt;
            pd.SampleDesc.Count = 1;
            return SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        if (!makeSkyPso(DXGI_FORMAT_R16G16B16A16_FLOAT, skyHdrPso)) return false;
        makeSkyPso(DXGI_FORMAT_R8G8B8A8_UNORM, skyLdrPso);  // best-effort fallback

        skyReady = skyRootSig && skyHdrPso && skyEnvHeap && skyCBuf[0];
        return skyReady;
    }

    bool createDebugLinePipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsB, psB, err;
        if (FAILED(D3DCompile(kDebugLineHLSL12, strlen(kDebugLineHLSL12),
                              "dbgline", nullptr, nullptr, "VSLine", "vs_5_0", flags, 0, &vsB, &err)))
        { Logger::Log(Logger::LogLevel::Error, "D3D12 DebugLine VS compile failed"); return false; }
        if (FAILED(D3DCompile(kDebugLineHLSL12, strlen(kDebugLineHLSL12),
                              "dbgline", nullptr, nullptr, "PSLine", "ps_5_0", flags, 0, &psB, &err)))
        { Logger::Log(Logger::LogLevel::Error, "D3D12 DebugLine PS compile failed"); return false; }

        // Root sig: [0] CBV b0 (viewProj mat4), no SRVs.
        {
            D3D12_ROOT_PARAMETER param{};
            param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
            param.Descriptor    = { 0, 0 };
            param.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 1; rsd.pParameters = &param;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> sig, sigErr;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&debugRootSig))))
                return false;
        }

        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };
        auto makeDbgPso = [&](DXGI_FORMAT fmt, ComPtr<ID3D12PipelineState>& out) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = debugRootSig.Get();
            pd.VS = { vsB->GetBufferPointer(), vsB->GetBufferSize() };
            pd.PS = { psB->GetBufferPointer(), psB->GetBufferSize() };
            pd.InputLayout = { layout, 2 };
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.SampleMask = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.DepthStencilState.DepthEnable    = TRUE;
            pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;  // read-only depth
            pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS_EQUAL;
            pd.DSVFormat = DXGI_FORMAT_D32_FLOAT;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
            pd.NumRenderTargets = 1; pd.RTVFormats[0] = fmt;
            pd.SampleDesc.Count = 1;
            return SUCCEEDED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&out)));
        };
        if (!makeDbgPso(DXGI_FORMAT_R16G16B16A16_FLOAT, debugHdrPso)) return false;
        makeDbgPso(DXGI_FORMAT_R8G8B8A8_UNORM, debugLdrPso);

        // Upload vertex buffer (up to 4096 lines = 8192 vertices × 24 bytes).
        debugVBuf = createUploadBuffer(static_cast<UINT64>(8192 * 6 * sizeof(float)),
                                       reinterpret_cast<void**>(&debugVBufPtr));
        debugCBuf = createUploadBuffer(64, reinterpret_cast<void**>(&debugCBufPtr));

        debugReady = debugRootSig && debugHdrPso && debugVBuf && debugCBuf;
        return debugReady;
    }

    void drawSky(ID3D12GraphicsCommandList* cl, UINT fi,
                 const glm::mat4& invVP, const glm::vec3& sunDir,
                 const IRenderer::EnvironmentSettings& env)
    {
        if (!skyReady) return;
        if (!env.skyEnabled) return; // no Sky entity → leave the cleared background
        auto* pso12 = usingHDR ? skyHdrPso.Get() : (skyLdrPso ? skyLdrPso.Get() : nullptr);
        if (!pso12) return;
        SkyCB cb{};
        cb.invViewProj = invVP;
        cb.sunDir    = sunDir;       cb.timeOfDay     = env.timeOfDay;
        cb.sunColor  = env.sunColor; cb.cloudCoverage = env.cloudCoverage;
        // Cloud drift: world-units/sec. The 0.025 factor matches the OpenGL reference
        // (windSpeed * 0.025) — without it the clouds scroll ~40× too fast.
        const float windScale = env.windSpeed * 0.025f;
        cb.wind = glm::vec3(
            std::sin(glm::radians(env.windDirection)) * windScale,
            0.0f,
            std::cos(glm::radians(env.windDirection)) * windScale);
        cb.time      = m_wallTime;
        cb.auroraColor  = env.auroraColor; cb.aurora    = env.auroraIntensity;
        cb.milkyWay     = env.milkyWayIntensity;
        cb.flash        = env.flash;
        cb.hasMoonTex   = moonTex12 ? 1 : 0;
        cb.nebula       = env.nebulaIntensity;
        cb.nebulaColor  = env.nebulaColor;
        if (skyCBufPtr[fi])
            std::memcpy(skyCBufPtr[fi], &cb, sizeof(cb));

        cl->SetGraphicsRootSignature(skyRootSig.Get());
        cl->SetPipelineState(pso12);
        ID3D12DescriptorHeap* heaps[] = { skyEnvHeap.Get() };
        cl->SetDescriptorHeaps(1, heaps);
        cl->SetGraphicsRootConstantBufferView(0, skyCBuf[fi]->GetGPUVirtualAddress());
        cl->SetGraphicsRootDescriptorTable(1, skyEnvHeap->GetGPUDescriptorHandleForHeapStart());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->IASetVertexBuffers(0, 0, nullptr);
        cl->IASetIndexBuffer(nullptr);
        cl->DrawInstanced(3, 1, 0, 0);
    }

    void drawDebugLines(ID3D12GraphicsCommandList* cl,
                        const D3D12_CPU_DESCRIPTOR_HANDLE& dsv,
                        const glm::mat4& viewProj,
                        const std::vector<DebugLine>& lines)
    {
        if (!debugReady || lines.empty()) return;
        auto* pso12 = usingHDR ? debugHdrPso.Get() : (debugLdrPso ? debugLdrPso.Get() : nullptr);
        if (!pso12) return;

        // Fill vertex buffer.
        const UINT64 needed = static_cast<UINT64>(lines.size() * 2 * 6 * sizeof(float));
        if (!debugVBuf || needed > 8192ULL * 6 * sizeof(float)) return;
        if (debugVBufPtr) {
            float* v = reinterpret_cast<float*>(debugVBufPtr);
            for (const DebugLine& l : lines) {
                *v++=l.start.x;*v++=l.start.y;*v++=l.start.z;
                *v++=l.color.r;*v++=l.color.g;*v++=l.color.b;
                *v++=l.end.x;  *v++=l.end.y;  *v++=l.end.z;
                *v++=l.color.r;*v++=l.color.g;*v++=l.color.b;
            }
        }
        if (debugCBufPtr) std::memcpy(debugCBufPtr, glm::value_ptr(viewProj), 64);

        cl->SetGraphicsRootSignature(debugRootSig.Get());
        cl->SetPipelineState(pso12);
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_LINELIST);
        D3D12_VERTEX_BUFFER_VIEW vbv{};
        vbv.BufferLocation = debugVBuf->GetGPUVirtualAddress();
        vbv.SizeInBytes    = static_cast<UINT>(needed);
        vbv.StrideInBytes  = 6 * sizeof(float);
        cl->IASetVertexBuffers(0, 1, &vbv);
        cl->SetGraphicsRootConstantBufferView(0, debugCBuf->GetGPUVirtualAddress());
        cl->DrawInstanced(static_cast<UINT>(lines.size() * 2), 1, 0, 0);
    }

    // ── Scene pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  rootSig;
    ComPtr<ID3D12PipelineState>  pso;
    ComPtr<ID3D12PipelineState>  psoInstanced;    // RGBA8, instanced geometry (A3)
    ComPtr<ID3D12PipelineState>  hdrPsoInstanced; // RGBA16F, instanced geometry (A3)
    ComPtr<ID3D12PipelineState>  transparentPSO; // alpha-blend, depth read-only
    ComPtr<ID3D12Resource>       perFrameCB[k_frameCount];   // upload, persistently mapped
    uint8_t*                     perFramePtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       perObjectRing[k_frameCount]; // upload, persistently mapped
    uint8_t*                     perObjectPtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       perInstanceRing[k_frameCount]; // upload: instance {mvp,model} (A3)
    uint8_t*                     perInstancePtr[k_frameCount]{};

    GpuMesh cube;

    // ── Sky pipeline ─────────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  skyRootSig;
    ComPtr<ID3D12PipelineState>  skyHdrPso;        // RGBA16F target
    ComPtr<ID3D12PipelineState>  skyLdrPso;        // RGBA8 target
    ComPtr<ID3D12Resource>       skyCBuf[k_frameCount];
    uint8_t*                     skyCBufPtr[k_frameCount]{};
    ComPtr<ID3D12DescriptorHeap> skyEnvHeap;       // 2-slot SRV heap: moon (t0) + 3D noise (t1)
    ComPtr<ID3D12Resource>       moonTex12;
    ComPtr<ID3D12Resource>       skyNoiseTex12;    // RG16 TEXTURE3D cloud noise volume (t1)
    // ImGui editor textures (logo + content-browser icons). Held here so the GPU
    // resources outlive ImGui's use of the SRVs the editor creates over them.
    std::vector<ComPtr<ID3D12Resource>> m_imguiTextures;
    bool                         skyReady   = false;
    bool                         usingHDR   = false;  // set by Render() before DrawScene()

    // ── Debug line pipeline ───────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  debugRootSig;
    ComPtr<ID3D12PipelineState>  debugHdrPso;      // RGBA16F
    ComPtr<ID3D12PipelineState>  debugLdrPso;      // RGBA8
    ComPtr<ID3D12Resource>       debugVBuf;
    uint8_t*                     debugVBufPtr = nullptr;
    ComPtr<ID3D12Resource>       debugCBuf;
    uint8_t*                     debugCBufPtr = nullptr;
    bool                         debugReady   = false;
    std::vector<DebugLine>       m_debugLines;
    float                        m_wallTime   = 0.0f;

    // ── Two additional scene PSOs for RGBA16F HDR target ─────────────────────
    // The base pso/transparentPSO use RGBA8 (fallback/swapchain path).
    // hdrPso/hdrTransparentPso use RGBA16F (PostFX path).
    ComPtr<ID3D12PipelineState>  hdrPso;
    ComPtr<ID3D12PipelineState>  hdrTransparentPso;

    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    FrustumCuller   m_culler;
    RenderSorter    m_sorter;
    RenderGraph     m_renderGraph;
    CommandBuffer   m_cmds;
    std::vector<uint8_t>  m_visible;
    std::vector<uint32_t> m_sortedIndices;
    std::unordered_map<HE::UUID, GpuMesh> meshCache;

    // ── Skinned-mesh pipeline + resources ────────────────────────────────────
    ComPtr<ID3D12RootSignature> skinnedRootSig;
    ComPtr<ID3D12PipelineState> m_skinnedPSO;       // RGBA8 (fallback/swapchain path)
    ComPtr<ID3D12PipelineState> m_skinnedHdrPSO;    // RGBA16F (PostFX HDR path)
    // Per-frame bones CB ring: k_maxSkinnedDraws slots × 8192 bytes (128 × 64).
    static constexpr UINT  k_maxSkinnedDraws = 256;
    static constexpr UINT64 k_bonesCBSlot    = 128 * 64; // exactly 8192 — already 256-aligned
    ComPtr<ID3D12Resource>  m_bonesCB[k_frameCount];
    uint8_t*                m_bonesCBPtr[k_frameCount]{};
    std::unordered_map<HE::UUID, GpuSkeletalMesh12> m_skeletalMeshCache;

    // ── 2D UI canvas pipeline ─────────────────────────────────────────────────
    ComPtr<ID3D12PipelineState>  m_uiPSO;
    ComPtr<ID3D12RootSignature>  m_uiRootSig;
    // Per-frame UI CB ring: up to 256 quads per frame × 256 bytes each (48B padded to 256B).
    static constexpr UINT   k_maxUIQuads = 256;
    static constexpr UINT64 k_uiCBSlot   = 256;
    ComPtr<ID3D12Resource>  m_uiCB[k_frameCount];
    uint8_t*                m_uiCBPtr[k_frameCount]{};

    // ── UI font atlas textures (glyph quads, type 2) ─────────────────────────
    // One R8 texture + SRV heap slot per UIFontCache key (0 = the shared default
    // font), created lazily the first frame a glyph references the key. The copy
    // is recorded into the frame's command list ahead of the UI draws (copies are
    // legal mid-frame on a direct list), so the staging buffer must outlive the
    // frames in flight — it is retired and freed a few frames later, the same
    // scheme as the resized viewport RTs.
    static constexpr UINT k_maxUIFontAtlases = 16;
    struct UIFontAtlas12 { ComPtr<ID3D12Resource> tex; UINT slot = 0; };
    ComPtr<ID3D12DescriptorHeap> m_uiAtlasHeap;   // shader-visible SRV heap
    UINT m_uiAtlasDescSize = 0;
    UINT m_uiAtlasNextSlot = 0;
    std::unordered_map<uint32_t, UIFontAtlas12> m_uiFontAtlases;
    std::vector<std::pair<ComPtr<ID3D12Resource>, int>> m_uiAtlasUploads; // retire countdown

    D3D12_CPU_DESCRIPTOR_HANDLE uiAtlasCpu(UINT slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = m_uiAtlasHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * m_uiAtlasDescSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE uiAtlasGpu(UINT slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = m_uiAtlasHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * m_uiAtlasDescSize;
        return h;
    }

    // Heap slot holding `key`'s atlas SRV, creating the R8 texture and recording
    // its upload on `cmd` the first time. Returns -1 when the key is unknown, its
    // bake failed, or the heap is full — callers fall back to the default atlas.
    int uiAtlasSlotFor(ID3D12GraphicsCommandList* cmd, uint32_t key)
    {
        if (auto it = m_uiFontAtlases.find(key); it != m_uiFontAtlases.end())
            return static_cast<int>(it->second.slot);
        if (!m_uiAtlasHeap || m_uiAtlasNextSlot >= k_maxUIFontAtlases) return -1;
        const HE::BakedUIFont* f = key == 0 ? &HE::sharedUIFont() : HE::UIFontCache::find(key);
        if (!f || !f->ok || f->atlasW <= 0 || f->atlasH <= 0 ||
            f->pixels.size() < static_cast<size_t>(f->atlasW) * f->atlasH)
            return -1;

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = static_cast<UINT64>(f->atlasW);
        td.Height           = static_cast<UINT>(f->atlasH);
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> tex;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
            return -1;

        // Row-pitched staging copy (R8: one byte per texel).
        const UINT rowPitch = alignUp(static_cast<UINT>(f->atlasW), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        void* mapped = nullptr;
        ComPtr<ID3D12Resource> uploadBuf =
            createUploadBuffer(static_cast<UINT64>(rowPitch) * f->atlasH, &mapped);
        if (!uploadBuf || !mapped) return -1;
        for (int y = 0; y < f->atlasH; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + static_cast<size_t>(y) * rowPitch,
                        f->pixels.data() + static_cast<size_t>(y) * f->atlasW,
                        static_cast<size_t>(f->atlasW));
        uploadBuf->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuf.Get();
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM,
            static_cast<UINT>(f->atlasW), static_cast<UINT>(f->atlasH), 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = tex.Get();
        dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier12(cmd, tex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                  = DXGI_FORMAT_R8_UNORM;
        sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels     = 1;
        const UINT slot = m_uiAtlasNextSlot++;
        device->CreateShaderResourceView(tex.Get(), &sv, uiAtlasCpu(slot));

        // The staging buffer is read at execute time — keep it alive past every
        // frame in flight before releasing it (swept in Render()).
        m_uiAtlasUploads.emplace_back(std::move(uploadBuf), static_cast<int>(k_frameCount) + 2);
        m_uiFontAtlases[key] = { std::move(tex), slot };
        return static_cast<int>(slot);
    }

    // ── Combined scene SRV heap ──────────────────────────────────────────────
    // Static region: [0]=shadow(t0), [1]=AO-blur(t2), [2]=white-fallback(t2),
    // [3]=null albedo fallback(t1).  Mesh-texture region: [k_sceneStaticSrvs ..]
    // hold one base-color SRV per uploaded mesh, bound per draw as the t1 table.
    static constexpr UINT k_sceneStaticSrvs = 4;   // slots [0..3] above
    static constexpr UINT k_albedoNullSlot  = 3;   // t1 fallback for untextured draws
    static constexpr UINT k_maxMeshTextures = 1024;
    ComPtr<ID3D12DescriptorHeap> sceneSrvHeap;    // CBV_SRV_UAV shader-visible
    UINT                         sceneSrvDescSize = 0;
    UINT                         meshTexNextSlot  = k_sceneStaticSrvs; // running allocator
    // Staging buffers for per-mesh texture uploads, kept alive a few frames past
    // record (the GPU copy runs at execute time), then swept in Render().
    std::vector<std::pair<ComPtr<ID3D12Resource>, int>> meshTexUploads;

    // ── MaterialComponent override + hot-reload (A2) ─────────────────────────
    // Override-material textures cached by material UUID (parallel to the baked per-mesh
    // texture on GpuMesh): a draw's dc.materialAssetId, when it resolves to a texture,
    // wins over the mesh's baked texture — mirrors GL/Metal. slot = -1 means "resolved,
    // no texture" (cached so it isn't re-resolved every frame). Editor edits push UUIDs
    // to the pending lists, drained at the top of the next DrawScene where it is safe to
    // touch the heap. Retired GPU resources are freed a few frames later (past frames in
    // flight). Descriptor-heap slots are monotonic, so an invalidated slot leaks (bounded).
    struct MaterialTex { ComPtr<ID3D12Resource> tex; int slot = -1; };
    std::unordered_map<HE::UUID, MaterialTex> m_materialTexCache;
    std::vector<HE::UUID> m_pendingMatInval;
    std::vector<HE::UUID> m_pendingMeshInval;
    std::vector<std::pair<ComPtr<ID3D12Resource>, int>> m_retiredTextures;
    // Recycle mesh-texture heap slots freed by invalidation so a repeatedly-edited mesh
    // (e.g. per-frame terrain sculpt, TerrainSystem::InvalidateMesh) can't exhaust the
    // 1024-slot region. A freed slot is only reusable once past frames in flight (its old
    // descriptor may still be referenced), so it waits out the same countdown as the resource.
    std::vector<std::pair<UINT, int>> m_freeSlotPending; // (slot, countdown)
    std::vector<UINT>                 m_freeSlots;        // ready to reuse

    D3D12_CPU_DESCRIPTOR_HANDLE sceneSrvCpu(UINT slot) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = sceneSrvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(slot) * sceneSrvDescSize;
        return h;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE sceneSrvGpu(UINT slot) const
    {
        D3D12_GPU_DESCRIPTOR_HANDLE h = sceneSrvHeap->GetGPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<UINT64>(slot) * sceneSrvDescSize;
        return h;
    }

    // Resolve a mesh/skeletal asset's baked base-color texture (material → textureIds[0]),
    // matching D3D11's resolveMesh()/skeletal path. Returns null when the mesh is untextured.
    const TextureAsset* resolveAlbedoAsset(const HE::UUID& materialId, const std::string& materialPath,
                                           ContentManager* cm)
    {
        if (!cm) return nullptr;
        const MaterialAsset* mat = cm->resolveMaterialRef(materialId, materialPath);
        if (!mat) return nullptr;
        const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
        const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
        return cm->resolveTextureRef(texId0, texPath0);
    }

    // Upload a decoded RGBA8 texture to a DEFAULT-heap texture + SRV in the mesh-texture
    // region of sceneSrvHeap, using the frame's recording command list. Mirrors uiAtlasSlotFor
    // (row-pitched staging copy, COPY_DEST→PIXEL_SHADER_RESOURCE). Returns the heap slot, or -1
    // on any miss / heap full. The DEFAULT texture is handed back via outTex so the caller keeps
    // it alive for the mesh's lifetime.
    int allocAlbedoSlot(ID3D12GraphicsCommandList* cl, const TextureAsset* tex,
                        ComPtr<ID3D12Resource>& outTex)
    {
        if (!cl || !sceneSrvHeap || !tex) return -1;
        if (tex->data.empty() || tex->channels != 4 || tex->format != TextureFormat::RGBA8) return -1;
        if (tex->width == 0 || tex->height == 0) return -1;
        const UINT rowBytes = static_cast<UINT>(tex->width) * 4u;
        if (tex->data.size() < static_cast<size_t>(rowBytes) * tex->height) return -1; // truncated
        // A recycled slot (freed by invalidation, past frames in flight) or the next fresh one.
        if (m_freeSlots.empty() && meshTexNextSlot >= k_sceneStaticSrvs + k_maxMeshTextures)
            return -1; // heap full → flat

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{};
        td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width            = static_cast<UINT64>(tex->width);
        td.Height           = static_cast<UINT>(tex->height);
        td.DepthOrArraySize = 1;
        td.MipLevels        = 1;
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        ComPtr<ID3D12Resource> gpuTex;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                   D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&gpuTex))))
            return -1;

        // Row-pitched staging copy (RGBA8: 4 bytes per texel).
        const UINT rowPitch = alignUp(rowBytes, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
        void* mapped = nullptr;
        ComPtr<ID3D12Resource> uploadBuf =
            createUploadBuffer(static_cast<UINT64>(rowPitch) * tex->height, &mapped);
        if (!uploadBuf || !mapped) return -1;
        for (size_t y = 0; y < tex->height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + y * rowPitch,
                        tex->data.data() + y * rowBytes,
                        rowBytes);
        uploadBuf->Unmap(0, nullptr);

        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = uploadBuf.Get();
        src.Type      = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM,
            static_cast<UINT>(tex->width), static_cast<UINT>(tex->height), 1, rowPitch };
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = gpuTex.Get();
        dst.Type      = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        barrier12(cl, gpuTex.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        UINT slot;
        if (!m_freeSlots.empty()) { slot = m_freeSlots.back(); m_freeSlots.pop_back(); }
        else                      { slot = meshTexNextSlot++; }
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
        sv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sv.Texture2D.MipLevels     = 1;
        device->CreateShaderResourceView(gpuTex.Get(), &sv, sceneSrvCpu(slot));

        meshTexUploads.emplace_back(std::move(uploadBuf), static_cast<int>(k_frameCount) + 2);
        outTex = std::move(gpuTex);
        return static_cast<int>(slot);
    }

    // Resolve + upload a static mesh's base-color texture the first time it is drawn (needs a
    // recording command list). On any miss the mesh stays flat (albedoSlot = -1).
    void ensureMeshAlbedo(ID3D12GraphicsCommandList* cl, GpuMesh& mesh,
                          const HE::UUID& assetId, ContentManager* cm)
    {
        if (mesh.albedoTried || !cl || !cm || !sceneSrvHeap) return;
        mesh.albedoTried = true;
        const StaticMeshAsset* asset = cm->getStaticMesh(assetId);
        if (!asset) return;
        const TextureAsset* tex = resolveAlbedoAsset(asset->materialId, asset->materialPath, cm);
        mesh.albedoSlot = allocAlbedoSlot(cl, tex, mesh.albedoTex);
    }

    // Same as ensureMeshAlbedo for a skeletal mesh's baked material.
    void ensureSkeletalAlbedo(ID3D12GraphicsCommandList* cl, GpuSkeletalMesh12& mesh,
                              const HE::UUID& assetId, ContentManager* cm)
    {
        if (mesh.albedoTried || !cl || !cm || !sceneSrvHeap) return;
        mesh.albedoTried = true;
        const SkeletalMeshAsset* asset = cm->getSkeletalMesh(assetId);
        if (!asset) return;
        const TextureAsset* tex = resolveAlbedoAsset(asset->materialId, asset->materialPath, cm);
        mesh.albedoSlot = allocAlbedoSlot(cl, tex, mesh.albedoTex);
    }

    // Resolve + upload an OVERRIDE material's base-color texture (dc.materialAssetId), cached
    // by material UUID. Returns true iff the material asset is loaded (so the override applies,
    // fully replacing the mesh's baked texture — exactly like GL's ResolveMaterialTexture);
    // outSlot is the sceneSrvHeap slot, or -1 when the override material has no usable texture
    // (→ the draw is flat, NOT the baked texture). Returns false only while the material asset
    // isn't loaded yet (retry next frame; the baked texture stays until then). Uses getMaterial
    // (the override is an explicit asset id) and caches even the no-texture result.
    bool resolveMaterialAlbedo(ID3D12GraphicsCommandList* cl, const HE::UUID& materialId,
                               ContentManager* cm, int& outSlot)
    {
        outSlot = -1;
        if (materialId == HE::UUID{} || !cl || !cm || !sceneSrvHeap) return false;
        if (auto it = m_materialTexCache.find(materialId); it != m_materialTexCache.end())
        { outSlot = it->second.slot; return true; }
        const MaterialAsset* mat = cm->getMaterial(materialId);
        if (!mat) return false; // not loaded yet — retry next frame without caching
        const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
        const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
        const TextureAsset* tex = cm->resolveTextureRef(texId0, texPath0);
        MaterialTex entry;
        entry.slot = allocAlbedoSlot(cl, tex, entry.tex); // -1 if the material has no usable texture
        outSlot = entry.slot;
        m_materialTexCache.emplace(materialId, std::move(entry));
        return true;
    }

    // Drain the editor's material/mesh hot-reload requests. Called at the top of DrawScene
    // (render thread, before any draw records) so touching the heap/caches is safe. Retired
    // GPU resources are freed a few frames later; the freed descriptor-heap slot is abandoned
    // (monotonic allocator), a bounded leak acceptable for edit-time invalidation.
    void processPendingInvalidations()
    {
        const int retireN = static_cast<int>(k_frameCount) + 2;
        auto retire     = [&](ComPtr<ID3D12Resource>&& r) { if (r) m_retiredTextures.emplace_back(std::move(r), retireN); };
        auto freeSlot   = [&](int slot) { if (slot >= 0) m_freeSlotPending.emplace_back(static_cast<UINT>(slot), retireN); };

        for (const HE::UUID& id : m_pendingMatInval)
            if (auto it = m_materialTexCache.find(id); it != m_materialTexCache.end())
            {
                freeSlot(it->second.slot);
                retire(std::move(it->second.tex));
                m_materialTexCache.erase(it);
            }
        m_pendingMatInval.clear();

        for (const HE::UUID& id : m_pendingMeshInval)
        {
            if (auto it = meshCache.find(id); it != meshCache.end())
            {
                GpuMesh& m = it->second;
                freeSlot(m.albedoSlot);
                retire(std::move(m.albedoTex));
                retire(std::move(m.vbuf));
                retire(std::move(m.ibuf));
                meshCache.erase(it);
            }
            if (auto it = m_skeletalMeshCache.find(id); it != m_skeletalMeshCache.end())
            {
                GpuSkeletalMesh12& m = it->second;
                freeSlot(m.albedoSlot);
                retire(std::move(m.albedoTex));
                retire(std::move(m.vbuf));
                retire(std::move(m.boneIdVb));
                retire(std::move(m.boneWgtVb));
                retire(std::move(m.ibuf));
                m_skeletalMeshCache.erase(it);
            }
        }
        m_pendingMeshInval.clear();
    }

    // ── SSAO ────────────────────────────────────────────────────────────────
    ComPtr<ID3D12PipelineState>  ssaoPosPSO;
    ComPtr<ID3D12PipelineState>  ssaoPSO;
    ComPtr<ID3D12PipelineState>  ssaoBlurPSO;
    ComPtr<ID3D12RootSignature>  ssaoPosRS;
    ComPtr<ID3D12RootSignature>  ssaoRS;
    // Resources
    ComPtr<ID3D12Resource>       ssaoPosRT;          // RGBA16F positions
    ComPtr<ID3D12Resource>       ssaoPosDepth;        // D16 depth for pos prepass
    ComPtr<ID3D12Resource>       ssaoRT;              // R8 AO
    ComPtr<ID3D12Resource>       ssaoBlurRT;          // R8 blurred AO
    ComPtr<ID3D12Resource>       ssaoNoiseTex;        // 4x4 RGBA32F noise
    ComPtr<ID3D12Resource>       ssaoNoiseUpload;     // upload heap for noise
    ComPtr<ID3D12Resource>       ssaoWhiteTex;        // 1x1 R8 white fallback
    ComPtr<ID3D12Resource>       ssaoWhiteUpload;
    // CBs (persistently mapped, UPLOAD heap, per-frame)
    ComPtr<ID3D12Resource>       ssaoPosPerObjRing[k_frameCount]; // ring: k_maxDraws × 256 bytes
    uint8_t*                     ssaoPosPerObjPtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       ssaoCB[k_frameCount];   // 768-byte: proj+noiseScale+params+kernel32
    void*                        ssaoCBPtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       ssaoBlurCB[k_frameCount]; // 256-byte: texel
    void*                        ssaoBlurCBPtr[k_frameCount]{};
    // Descriptor heap for SSAO SRVs
    ComPtr<ID3D12DescriptorHeap> ssaoSrvHeap;        // CBV_SRV_UAV shader-visible, 4 slots
    // [0]=ssaoPosRT, [1]=ssaoNoiseTex, [2]=ssaoRT, [3]=ssaoBlurRT
    ComPtr<ID3D12DescriptorHeap> ssaoSamplerHeap;    // SAMPLER shader-visible, 1 descriptor
    // RTV/DSV heap for SSAO render targets
    ComPtr<ID3D12DescriptorHeap> ssaoRtvHeap;        // 3 RTVs: [0]=pos, [1]=ao, [2]=aoBlur
    ComPtr<ID3D12DescriptorHeap> ssaoDsvHeap;        // 1 DSV: [0]=pos depth
    UINT                         ssaoSrvDescSize  = 0;
    UINT                         ssaoRtvDescSize  = 0;
    UINT                         ssaoDsvDescSize  = 0;
    UINT                         ssaoSampDescSize = 0;
    // CPU/GPU descriptor handles (cached after heap creation)
    D3D12_CPU_DESCRIPTOR_HANDLE  ssaoPosRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE  ssaoRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE  ssaoBlurRtvCpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE  ssaoDsvCpu{};
    D3D12_GPU_DESCRIPTOR_HANDLE  ssaoPosGpuSrv{};   // SSAO main pass t0
    D3D12_GPU_DESCRIPTOR_HANDLE  ssaoNoiseGpuSrv{};  // SSAO main pass t1
    D3D12_GPU_DESCRIPTOR_HANDLE  ssaoGpuSrv{};       // blur pass t0
    D3D12_GPU_DESCRIPTOR_HANDLE  ssaoBlurGpuSrv{};   // scene pass / sceneSrvHeap[1]
    D3D12_GPU_DESCRIPTOR_HANDLE  ssaoPointSamplerGpu{};
    // Current resource states
    D3D12_RESOURCE_STATES ssaoPosRTState  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES ssaoRTState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    D3D12_RESOURCE_STATES ssaoBlurRTState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    // Settings
    float ssaoRadius    = 0.5f;
    float ssaoBias      = 0.025f;
    float ssaoIntensity = 1.5f;
    bool  ssaoEnabled   = true;
    int   ssaoMethod    = 0;
    bool  ssaoReady     = false;
    int   ssaoW         = 0;
    int   ssaoH         = 0;

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(UINT index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * rtvDescSize;
        return h;
    }

    void waitForFrame(UINT frame)
    {
        const UINT64 target = fenceValues[frame];
        if (fence->GetCompletedValue() < target)
        {
            fence->SetEventOnCompletion(target, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }
    void waitForAllFrames()
    {
        // Flush ALL submitted GPU work: signal one new monotonic value and wait for it.
        const UINT64 val = ++fenceValue;
        cmdQueue->Signal(fence.Get(), val);
        if (fence->GetCompletedValue() < val)
        {
            fence->SetEventOnCompletion(val, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
        // Every slot is now safe up to this value.
        for (UINT i = 0; i < k_frameCount; ++i) fenceValues[i] = val;
    }

    ComPtr<ID3D12Resource> createUploadBuffer(UINT64 bytes, void** mappedOut, const void* initial = nullptr)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width            = bytes;
        rd.Height           = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> res;
        if (FAILED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&res))))
            return nullptr;
        void* ptr = nullptr;
        D3D12_RANGE noRead{ 0, 0 };
        res->Map(0, &noRead, &ptr);
        if (initial && ptr) std::memcpy(ptr, initial, static_cast<size_t>(bytes));
        if (mappedOut) *mappedOut = ptr; else res->Unmap(0, nullptr);
        return res;
    }

    void createDepth(int w, int h)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd{};
        hd.NumDescriptors = 2; // [0] scene depth, [1] shadow depth
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap));
        dsvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width            = static_cast<UINT64>(w);
        rd.Height           = static_cast<UINT>(h);
        rd.DepthOrArraySize = 1;
        rd.MipLevels        = 1;
        rd.Format           = DXGI_FORMAT_D32_FLOAT;
        rd.SampleDesc.Count = 1;
        rd.Flags            = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        D3D12_CLEAR_VALUE cv{}; cv.Format = DXGI_FORMAT_D32_FLOAT; cv.DepthStencil.Depth = 1.0f;
        device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_DEPTH_WRITE, &cv, IID_PPV_ARGS(&depthBuffer));
        device->CreateDepthStencilView(depthBuffer.Get(), nullptr, dsvHandle(0));

        // ── Shadow depth (R32_TYPELESS so it's both a DSV and an SRV) ────────
        D3D12_RESOURCE_DESC sr = rd;
        sr.Width  = static_cast<UINT64>(shadowSize);
        sr.Height = static_cast<UINT>(shadowSize);
        sr.Format = DXGI_FORMAT_R32_TYPELESS;
        if (SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sr,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, &cv, IID_PPV_ARGS(&shadowDepth))))
        {
            D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
            dvd.Format        = DXGI_FORMAT_D32_FLOAT;
            dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            device->CreateDepthStencilView(shadowDepth.Get(), &dvd, dsvHandle(1));

            D3D12_DESCRIPTOR_HEAP_DESC sh{};
            sh.NumDescriptors = 1;
            sh.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            sh.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            device->CreateDescriptorHeap(&sh, IID_PPV_ARGS(&shadowSrvHeap));

            D3D12_SHADER_RESOURCE_VIEW_DESC svd{};
            svd.Format                  = DXGI_FORMAT_R32_FLOAT;
            svd.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            svd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            svd.Texture2D.MipLevels     = 1;
            device->CreateShaderResourceView(shadowDepth.Get(), &svd,
                shadowSrvHeap->GetCPUDescriptorHandleForHeapStart());
            // Also fill slot 0 of the combined scene SRV heap (if already created).
            if (sceneSrvHeap)
                device->CreateShaderResourceView(shadowDepth.Get(), &svd, sceneSrvCpu(0));
        }
    }

    bool createUIPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsB, psB, err;
        if (FAILED(D3DCompile(kUIHLSL12, strlen(kUIHLSL12),
                              "ui", nullptr, nullptr, "UIVSMain", "vs_5_0", flags, 0, &vsB, &err)))
        {
            Logger::Log(Logger::LogLevel::Error,
                (std::string("D3D12 UI VS compile failed: ")
                 + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(kUIHLSL12, strlen(kUIHLSL12),
                              "ui", nullptr, nullptr, "UIPSMain", "ps_5_0", flags, 0, &psB, &err)))
        {
            Logger::Log(Logger::LogLevel::Error,
                (std::string("D3D12 UI PS compile failed: ")
                 + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
            return false;
        }

        // Root signature: root CBV at b0 (VS+PS) + SRV table t0 (font atlas, PS)
        // + static sampler s0 (linear-clamp; glyphs scale the atlas both ways).
        {
            D3D12_DESCRIPTOR_RANGE srvRange{};
            srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvRange.NumDescriptors     = 1;
            srvRange.BaseShaderRegister = 0; // t0

            D3D12_ROOT_PARAMETER params[2]{};
            params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
            params[0].Descriptor       = { 0, 0 }; // b0
            params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            params[1].DescriptorTable  = { 1, &srvRange };
            params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_STATIC_SAMPLER_DESC samp{};
            samp.Filter           = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            samp.ShaderRegister   = 0; // s0
            samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters     = 2; rsd.pParameters     = params;
            rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> sig, sigErr;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &sigErr)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&m_uiRootSig))))
            {
                Logger::Log(Logger::LogLevel::Error, "D3D12 UI root signature failed");
                return false;
            }
        }

        // PSO: alpha blend, no depth test, RGBA8 output (final viewport/swapchain RT).
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature = m_uiRootSig.Get();
            pd.VS = { vsB->GetBufferPointer(), vsB->GetBufferSize() };
            pd.PS = { psB->GetBufferPointer(), psB->GetBufferSize() };
            pd.InputLayout           = { nullptr, 0 };
            pd.SampleMask            = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.DepthStencilState.DepthEnable = FALSE;
            auto& rt = pd.BlendState.RenderTarget[0];
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp        = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
            pd.SampleDesc.Count      = 1;
            if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_uiPSO))))
            {
                Logger::Log(Logger::LogLevel::Error, "D3D12 UI PSO creation failed");
                return false;
            }
        }

        // Per-frame upload CBs, persistently mapped.
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            m_uiCB[f] = createUploadBuffer(
                static_cast<UINT64>(k_maxUIQuads) * k_uiCBSlot,
                reinterpret_cast<void**>(&m_uiCBPtr[f]));
            if (!m_uiCB[f])
            {
                Logger::Log(Logger::LogLevel::Error, "D3D12 UI CB allocation failed");
                return false;
            }
        }

        // Shader-visible font-atlas SRV heap. Every slot starts as a null SRV so
        // the root table is always valid to bind, even before any atlas uploads.
        {
            m_uiAtlasDescSize =
                device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = k_maxUIFontAtlases;
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_uiAtlasHeap))))
            {
                Logger::Log(Logger::LogLevel::Error, "D3D12 UI atlas heap creation failed");
                return false;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
            nullSrv.Format                  = DXGI_FORMAT_R8_UNORM;
            nullSrv.ViewDimension           = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Texture2D.MipLevels     = 1;
            for (UINT s = 0; s < k_maxUIFontAtlases; ++s)
                device->CreateShaderResourceView(nullptr, &nullSrv, uiAtlasCpu(s));
        }

        return true;
    }

    void renderUIPass12(ID3D12GraphicsCommandList* cmd, int fi, int w, int h)
    {
        if (!m_uiPSO || !m_uiAtlasHeap || m_renderWorld.uiObjects.empty()) return;

        // Record any missing atlas uploads BEFORE the pass state is bound so the
        // copies + COPY_DEST→PSR barriers sit ahead of the draws that sample them.
        const int defaultSlot = uiAtlasSlotFor(cmd, 0);
        for (const UIRenderObject& obj : m_renderWorld.uiObjects)
            if (obj.type == 2 && obj.fontAtlasKey != 0)
                uiAtlasSlotFor(cmd, obj.fontAtlasKey);

        cmd->SetPipelineState(m_uiPSO.Get());
        cmd->SetGraphicsRootSignature(m_uiRootSig.Get());
        cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ID3D12DescriptorHeap* heaps[] = { m_uiAtlasHeap.Get() };
        cmd->SetDescriptorHeaps(1, heaps);
        // Solid quads never sample the atlas, but the table must still point at a
        // valid descriptor — slot 0 is at worst a null SRV.
        int boundSlot = std::max(defaultSlot, 0);
        cmd->SetGraphicsRootDescriptorTable(1, uiAtlasGpu(static_cast<UINT>(boundSlot)));

        struct UICB { glm::vec4 rect; glm::vec4 color; glm::vec4 uvRect;
                      glm::vec2 vp; float mode; float pad; };
        int qi = 0;
        for (const UIRenderObject& obj : m_renderWorld.uiObjects) {
            if (qi >= static_cast<int>(k_maxUIQuads)) break;
            if (obj.type == 2)
            {
                // A glyph may use an imported font's atlas; unknown keys fall back
                // to the shared font, and if even that failed to bake, skip.
                const auto it = m_uiFontAtlases.find(obj.fontAtlasKey);
                const int slot = it != m_uiFontAtlases.end()
                    ? static_cast<int>(it->second.slot) : defaultSlot;
                if (slot < 0) continue;
                if (slot != boundSlot)
                {
                    cmd->SetGraphicsRootDescriptorTable(1, uiAtlasGpu(static_cast<UINT>(slot)));
                    boundSlot = slot;
                }
            }
            UICB cb;
            cb.rect   = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
            cb.color  = glm::vec4(obj.color.r, obj.color.g, obj.color.b, obj.color.a);
            cb.uvRect = glm::vec4(obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y);
            cb.vp     = glm::vec2(float(w), float(h));
            cb.mode   = obj.type == 2 ? 1.0f : 0.0f;
            cb.pad    = 0.0f;
            std::memcpy(m_uiCBPtr[fi] + static_cast<size_t>(qi) * k_uiCBSlot, &cb, sizeof(cb));
            D3D12_GPU_VIRTUAL_ADDRESS addr = m_uiCB[fi]->GetGPUVirtualAddress()
                                           + static_cast<UINT64>(qi) * k_uiCBSlot;
            cmd->SetGraphicsRootConstantBufferView(0, addr);
            cmd->DrawInstanced(4, 1, 0, 0);
            ++qi;
        }
    }

    bool createPipeline()
    {
        // Root signature: two root CBVs (b0 per-object, b1 per-frame) +
        // SRV table [t0=shadow, t2=AO] (params[2]) sharing one heap +
        // static samplers s0 (shadow, linear-clamp) + s1 (AO, point-clamp).
        //
        // We declare TWO descriptor ranges inside params[2]:
        //   range0: 1 SRV at t0 (shadow)
        //   range1: 1 SRV at t2 (AO)  -- note BaseShaderRegister=2
        // Both ranges point into the same descriptor table (sceneSrvHeap).
        // Layout of sceneSrvHeap: [0]=shadow(t0), [1]=AO-blur(t2), [2]=white(t2 fallback).
        // Since t2 is at heap slot 1, we set OffsetInDescriptorsFromTableStart=1 for range1.
        D3D12_DESCRIPTOR_RANGE srvRanges[2]{};
        srvRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors                    = 1;
        srvRanges[0].BaseShaderRegister                = 0; // t0
        srvRanges[0].RegisterSpace                     = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = 0; // heap slot 0
        srvRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[1].NumDescriptors                    = 1;
        srvRanges[1].BaseShaderRegister                = 2; // t2
        srvRanges[1].RegisterSpace                     = 0;
        srvRanges[1].OffsetInDescriptorsFromTableStart = 1; // heap slot 1

        // Per-draw base-color table (t1): its own single-descriptor table so the base can be
        // pointed at each mesh's slot (or the null fallback) independently of the shadow/AO table.
        D3D12_DESCRIPTOR_RANGE albedoRange{};
        albedoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedoRange.NumDescriptors                    = 1;
        albedoRange.BaseShaderRegister                = 1; // t1
        albedoRange.RegisterSpace                     = 0;
        albedoRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor       = { 0, 0 }; // b0
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor       = { 1, 0 }; // b1
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 2; // t0 + t2
        params[2].DescriptorTable.pDescriptorRanges   = srvRanges;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[3].DescriptorTable.NumDescriptorRanges = 1; // t1 (albedo)
        params[3].DescriptorTable.pDescriptorRanges   = &albedoRange;
        params[3].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        // param[4]: root SRV (t3) = per-instance {mvp,model} structured buffer for the
        // instanced geometry path (A3). Vertex-only; unused/unbound by non-instanced draws.
        params[4].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[4].Descriptor       = { 3, 0 }; // t3, space0
        params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;

        // s0 = shadow sampler (linear-clamp), s1 = AO sampler (point-clamp),
        // s2 = base-color sampler (linear-wrap, for tiling textures)
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ShaderRegister = 0; // s0
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].ShaderRegister = 1; // s1
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[2].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[2].ShaderRegister = 2; // s2
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 5;
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = 3;
        rsd.pStaticSamplers   = samplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: root signature serialize failed");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                   IID_PPV_ARGS(&rootSig))))
            return false;

        // Build the combined scene SRV heap: [0]=shadow, [1]=AO-blur, [2]=white-fallback.
        // Slots 1 and 2 are filled later by createSSAOPipeline (noise upload) and
        // createSSAOTargets (actual render targets).  For now write null/white to slots 1+2
        // so the heap is always valid.
        {
            sceneSrvDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = k_sceneStaticSrvs + k_maxMeshTextures; // static slots + per-mesh albedo region
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&sceneSrvHeap))))
                return false;
            // Slot 0: shadow map SRV — createDepth() runs before createPipeline(), so fill now.
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrv{};
            nullSrv.Format = DXGI_FORMAT_R32_FLOAT;
            nullSrv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrv.Texture2D.MipLevels = 1;
            if (shadowDepth)
                device->CreateShaderResourceView(shadowDepth.Get(), &nullSrv, sceneSrvCpu(0));
            else
                device->CreateShaderResourceView(nullptr, &nullSrv, sceneSrvCpu(0));
            // Slot 1 (AO-blur / white): null SRV placeholder for R8
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvR8{};
            nullSrvR8.Format = DXGI_FORMAT_R8_UNORM;
            nullSrvR8.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvR8.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvR8.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &nullSrvR8, sceneSrvCpu(1));
            device->CreateShaderResourceView(nullptr, &nullSrvR8, sceneSrvCpu(2));
            // Slot 3: null RGBA8 SRV — the t1 albedo fallback bound for untextured draws.
            // The shader never samples it (uColor.a = 0 selects the flat colour), but the
            // descriptor table must point at a valid descriptor.
            D3D12_SHADER_RESOURCE_VIEW_DESC nullSrvRGBA{};
            nullSrvRGBA.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            nullSrvRGBA.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            nullSrvRGBA.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            nullSrvRGBA.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(nullptr, &nullSrvRGBA, sceneSrvCpu(k_albedoNullSlot));
        }

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        const std::string sceneSource = std::string(kSkyFuncHLSL12) + kSceneHLSL;
        ComPtr<ID3DBlob> vs, ps, cerr;
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "VSMain", "vs_5_0", flags, 0, &vs, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D12Renderer: VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &ps, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D12Renderer: PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }

        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature        = rootSig.Get();
        pd.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout           = { layout, 3 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM;
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count      = 1;
        pd.SampleMask            = UINT_MAX;
        // Rasterizer: solid, no culling (meshes have no guaranteed winding).
        pd.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable       = TRUE;
        // Blend: opaque.
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        // Depth: on, less.
        pd.DepthStencilState.DepthEnable    = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&pso))))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: PSO creation failed");
            return false;
        }

        // HDR variant: RGBA16F for the PostFX scene pass.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC hp = pd;
            hp.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateGraphicsPipelineState(&hp, IID_PPV_ARGS(&hdrPso));
        }

        // Instanced geometry variants (A3): same PS / input layout / root sig, VS =
        // VSMainInstanced (reads per-instance mvp/model from the t3 structured buffer).
        // LDR (RGBA8) + HDR (RGBA16F) to match the two scene targets.
        {
            ComPtr<ID3DBlob> vsi, ierr;
            if (SUCCEEDED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                                     "VSMainInstanced", "vs_5_0", flags, 0, &vsi, &ierr)))
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC ip = pd; // pd still RGBA8 here
                ip.VS = { vsi->GetBufferPointer(), vsi->GetBufferSize() };
                device->CreateGraphicsPipelineState(&ip, IID_PPV_ARGS(&psoInstanced));
                ip.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
                device->CreateGraphicsPipelineState(&ip, IID_PPV_ARGS(&hdrPsoInstanced));
            }
            else
            {
                Logger::Log(Logger::LogLevel::Error, (std::string("D3D12Renderer: VSMainInstanced "
                    "compile failed: ") + (ierr ? static_cast<const char*>(ierr->GetBufferPointer()) : "")).c_str());
            }
        }

        // Depth-only PSO for the shadow pass (VSDepth, no PS / RTV).
        {
            ComPtr<ID3DBlob> dvs;
            if (SUCCEEDED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                                     "VSDepth", "vs_5_0", flags, 0, &dvs, &cerr)))
            {
                D3D12_GRAPHICS_PIPELINE_STATE_DESC dp = pd;
                dp.VS               = { dvs->GetBufferPointer(), dvs->GetBufferSize() };
                dp.PS               = { nullptr, 0 };
                dp.NumRenderTargets = 0;
                dp.RTVFormats[0]    = DXGI_FORMAT_UNKNOWN;
                device->CreateGraphicsPipelineState(&dp, IID_PPV_ARGS(&depthPSO));
            }
        }

        // Alpha-blend PSO for sorted transparency (depth test read-only).
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC tp = pd;
            auto& rt = tp.BlendState.RenderTarget[0];
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp        = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            tp.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            device->CreateGraphicsPipelineState(&tp, IID_PPV_ARGS(&transparentPSO));
        }

        // HDR transparent variant.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC tp12 = pd;
            tp12.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            auto& rt = tp12.BlendState.RenderTarget[0];
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D12_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D12_BLEND_INV_SRC_ALPHA;
            rt.BlendOp        = D3D12_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D12_BLEND_ONE;
            rt.DestBlendAlpha = D3D12_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D12_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            tp12.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
            device->CreateGraphicsPipelineState(&tp12, IID_PPV_ARGS(&hdrTransparentPso));
        }

        // Per-frame + per-object upload rings, one set per frame in flight.
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            perFrameCB[f]    = createUploadBuffer(alignUp(sizeof(PerFrameCB), k_cbSlot),
                                                  reinterpret_cast<void**>(&perFramePtr[f]));
            perObjectRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot,
                                                  reinterpret_cast<void**>(&perObjectPtr[f]));
            perInstanceRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxInstances) * k_instStride,
                                                    reinterpret_cast<void**>(&perInstancePtr[f]));
        }
        createSkyPipeline();
        createDebugLinePipeline();
        createSSAOPipeline();
        createSkinnedPipeline();
        createUIPipeline();
        return rootSig && pso;
    }

    // ── SSAO pipeline creation ───────────────────────────────────────────────
    bool createSSAOPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* src, const char* entry, const char* profile,
                           ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> errBlob;
            if (FAILED(D3DCompile(src, strlen(src), entry, nullptr, nullptr,
                                  entry, profile, flags, 0, &out, &errBlob)))
            {
                Logger::Log(Logger::LogLevel::Error,
                    (std::string("D3D12 SSAO '") + entry + "' compile failed: "
                    + (errBlob ? static_cast<const char*>(errBlob->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };

        // ── Root signature for pos prepass ───────────────────────────────────
        // [0] CBV b0 (per-object: mvp + modelView)
        {
            D3D12_ROOT_PARAMETER p0{};
            p0.ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
            p0.Descriptor       = { 0, 0 }; // b0
            p0.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 1; rsd.pParameters = &p0;
            rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
            ComPtr<ID3DBlob> sig, e;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &e)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&ssaoPosRS))))
            { Logger::Log(Logger::LogLevel::Error, "D3D12 SSAO pos root sig failed"); return false; }
        }

        // ── Root signature for SSAO main pass and blur pass ──────────────────
        // [0] CBV b0  (SSAO params or blur texel)
        // [1] SRV table t0 + t1  (2 descriptors for SSAO; 1 for blur)
        // [2] static POINT-WRAP sampler s0
        {
            D3D12_DESCRIPTOR_RANGE srvR{};
            srvR.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
            srvR.NumDescriptors     = 2; // t0 + t1 (blur pass only uses t0 but declares 2 as max)
            srvR.BaseShaderRegister = 0;
            srvR.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;
            D3D12_ROOT_PARAMETER ssaoParams[2]{};
            ssaoParams[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
            ssaoParams[0].Descriptor       = { 0, 0 };
            ssaoParams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
            ssaoParams[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
            ssaoParams[1].DescriptorTable  = { 1, &srvR };
            ssaoParams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_STATIC_SAMPLER_DESC ssaoSamp{};
            ssaoSamp.Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
            ssaoSamp.AddressU = ssaoSamp.AddressV = ssaoSamp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            ssaoSamp.ShaderRegister  = 0;
            ssaoSamp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2; rsd.pParameters = ssaoParams;
            rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &ssaoSamp;
            ComPtr<ID3DBlob> sig, e;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &e)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&ssaoRS))))
            { Logger::Log(Logger::LogLevel::Error, "D3D12 SSAO root sig failed"); return false; }
        }

        // ── Compile shaders ───────────────────────────────────────────────────
        ComPtr<ID3DBlob> posvs, pops, fsVS, ssaoPS, blurPS;
        if (!compile(kSSAOPosHLSL12,   "VSPos",         "vs_5_0", posvs))  return false;
        if (!compile(kSSAOPosHLSL12,   "PSPos",         "ps_5_0", pops))   return false;
        if (!compile(kFSTriangleVS12,   "main",          "vs_5_0", fsVS))   return false;
        if (!compile(kSSAOHLSL12,       "SSAOMain",      "ps_5_0", ssaoPS)) return false;
        if (!compile(kSSAOBlurHLSL12,   "SSAOBlurMain",  "ps_5_0", blurPS)) return false;

        // ── Position prepass PSO ──────────────────────────────────────────────
        {
            const D3D12_INPUT_ELEMENT_DESC layout[] = {
                { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
                { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            };
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature        = ssaoPosRS.Get();
            pd.VS                    = { posvs->GetBufferPointer(), posvs->GetBufferSize() };
            pd.PS                    = { pops->GetBufferPointer(),  pops->GetBufferSize() };
            pd.InputLayout           = { layout, 3 };
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R16G16B16A16_FLOAT;
            pd.DSVFormat             = DXGI_FORMAT_D16_UNORM;
            pd.SampleDesc.Count      = 1;
            pd.SampleMask            = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.RasterizerState.DepthClipEnable = TRUE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.DepthStencilState.DepthEnable    = TRUE;
            pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
            pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;
            if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&ssaoPosPSO))))
            { Logger::Log(Logger::LogLevel::Error, "D3D12 SSAO pos PSO failed"); return false; }
        }

        // ── SSAO main pass PSO (fullscreen, R8, no depth) ────────────────────
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature        = ssaoRS.Get();
            pd.VS                    = { fsVS->GetBufferPointer(),   fsVS->GetBufferSize() };
            pd.PS                    = { ssaoPS->GetBufferPointer(), ssaoPS->GetBufferSize() };
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R8_UNORM;
            pd.SampleDesc.Count      = 1;
            pd.SampleMask            = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.DepthStencilState.DepthEnable = FALSE;
            if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&ssaoPSO))))
            { Logger::Log(Logger::LogLevel::Error, "D3D12 SSAO PSO failed"); return false; }
        }

        // ── SSAO blur pass PSO (fullscreen, R8, no depth) ────────────────────
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
            pd.pRootSignature        = ssaoRS.Get();
            pd.VS                    = { fsVS->GetBufferPointer(),   fsVS->GetBufferSize() };
            pd.PS                    = { blurPS->GetBufferPointer(), blurPS->GetBufferSize() };
            pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pd.NumRenderTargets      = 1;
            pd.RTVFormats[0]         = DXGI_FORMAT_R8_UNORM;
            pd.SampleDesc.Count      = 1;
            pd.SampleMask            = UINT_MAX;
            pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
            pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
            pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
            pd.DepthStencilState.DepthEnable = FALSE;
            if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&ssaoBlurPSO))))
            { Logger::Log(Logger::LogLevel::Error, "D3D12 SSAO blur PSO failed"); return false; }
        }

        // ── Constant buffers (per-frame rings, persistently mapped) ──────────
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            // Per-object ring for pos prepass: k_maxDraws × 256 bytes
            ssaoPosPerObjRing[f] = createUploadBuffer(
                static_cast<UINT64>(k_maxDraws) * k_cbSlot,
                reinterpret_cast<void**>(&ssaoPosPerObjPtr[f]));
            // SSAO main CB: proj(64) + noiseScale(16) + params(16) + kernel32(512) = 608 → 768 bytes
            ssaoCB[f] = createUploadBuffer(alignUp(608u, k_cbSlot),
                reinterpret_cast<void**>(&ssaoCBPtr[f]));
            // SSAO blur CB: texel(8) padded = 16 → 256 bytes
            ssaoBlurCB[f] = createUploadBuffer(alignUp(16u, k_cbSlot),
                reinterpret_cast<void**>(&ssaoBlurCBPtr[f]));
        }

        // ── Descriptor heaps ──────────────────────────────────────────────────
        ssaoSrvDescSize  = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        ssaoRtvDescSize  = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        ssaoDsvDescSize  = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
        ssaoSampDescSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);

        // SRV heap (4 slots): [0]=ssaoPosRT, [1]=ssaoNoiseTex, [2]=ssaoRT, [3]=ssaoBlurRT
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 4;
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ssaoSrvHeap))))
                return false;
            // Cache GPU handles for each slot
            auto gpuBase = ssaoSrvHeap->GetGPUDescriptorHandleForHeapStart();
            ssaoPosGpuSrv.ptr   = gpuBase.ptr + 0 * ssaoSrvDescSize;
            ssaoNoiseGpuSrv.ptr = gpuBase.ptr + 1 * ssaoSrvDescSize;
            ssaoGpuSrv.ptr      = gpuBase.ptr + 2 * ssaoSrvDescSize;
            ssaoBlurGpuSrv.ptr  = gpuBase.ptr + 3 * ssaoSrvDescSize;
        }

        // Sampler heap (1 slot): POINT WRAP
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 1;
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            hd.Flags          = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ssaoSamplerHeap))))
                return false;
            D3D12_SAMPLER_DESC sd{};
            sd.Filter   = D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
            sd.MaxLOD   = D3D12_FLOAT32_MAX;
            device->CreateSampler(&sd, ssaoSamplerHeap->GetCPUDescriptorHandleForHeapStart());
            ssaoPointSamplerGpu = ssaoSamplerHeap->GetGPUDescriptorHandleForHeapStart();
        }

        // RTV heap (3 slots): [0]=ssaoPosRT, [1]=ssaoRT, [2]=ssaoBlurRT
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 3;
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ssaoRtvHeap))))
                return false;
            auto cpuBase = ssaoRtvHeap->GetCPUDescriptorHandleForHeapStart();
            ssaoPosRtvCpu.ptr  = cpuBase.ptr + 0 * ssaoRtvDescSize;
            ssaoRtvCpu.ptr     = cpuBase.ptr + 1 * ssaoRtvDescSize;
            ssaoBlurRtvCpu.ptr = cpuBase.ptr + 2 * ssaoRtvDescSize;
        }

        // DSV heap (1 slot): [0]=ssaoPosDepth
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 1;
            hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
            if (FAILED(device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&ssaoDsvHeap))))
                return false;
            ssaoDsvCpu = ssaoDsvHeap->GetCPUDescriptorHandleForHeapStart();
        }

        // ── Noise texture (4×4, RGBA32F) ──────────────────────────────────────
        {
            auto noise = BuildSSAONoise(16);
            // Pack vec3→float4 (alpha=0)
            std::vector<float> noiseData(16 * 4);
            for (int i = 0; i < 16; ++i)
            {
                noiseData[i*4+0] = noise[i].x;
                noiseData[i*4+1] = noise[i].y;
                noiseData[i*4+2] = noise[i].z;
                noiseData[i*4+3] = 0.0f;
            }

            D3D12_HEAP_PROPERTIES defHp{}; defHp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width            = 4;
            td.Height           = 4;
            td.DepthOrArraySize = 1;
            td.MipLevels        = 1;
            td.Format           = DXGI_FORMAT_R32G32B32A32_FLOAT;
            td.SampleDesc.Count = 1;
            device->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ssaoNoiseTex));

            // Upload buffer: D3D12_TEXTURE_DATA_PITCH_ALIGNMENT aligned row pitch
            const UINT rowPitch = alignUp(4u * 4u * sizeof(float), D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * 4;
            D3D12_HEAP_PROPERTIES upHp{}; upHp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC ubd{};
            ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            ubd.Width = uploadSize; ubd.Height = 1; ubd.DepthOrArraySize = 1;
            ubd.MipLevels = 1; ubd.Format = DXGI_FORMAT_UNKNOWN;
            ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            device->CreateCommittedResource(&upHp, D3D12_HEAP_FLAG_NONE, &ubd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ssaoNoiseUpload));

            if (ssaoNoiseUpload)
            {
                void* mapped = nullptr; D3D12_RANGE noRead{0,0};
                ssaoNoiseUpload->Map(0, &noRead, &mapped);
                if (mapped)
                {
                    // Fill row by row (each row = 4 texels × 4 floats = 64 bytes, padded to rowPitch)
                    for (int row = 0; row < 4; ++row)
                        std::memcpy(static_cast<uint8_t*>(mapped) + row * rowPitch,
                            noiseData.data() + row * 4 * 4, 4u * 4u * sizeof(float));
                    ssaoNoiseUpload->Unmap(0, nullptr);
                }

                // Execute a one-shot copy command
                waitForAllFrames();
                cmdAllocators[0]->Reset();
                cmdList->Reset(cmdAllocators[0].Get(), nullptr);

                D3D12_TEXTURE_COPY_LOCATION src2{};
                src2.pResource = ssaoNoiseUpload.Get();
                src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src2.PlacedFootprint.Offset = 0;
                src2.PlacedFootprint.Footprint = { DXGI_FORMAT_R32G32B32A32_FLOAT, 4, 4, 1, rowPitch };
                D3D12_TEXTURE_COPY_LOCATION dst2{};
                dst2.pResource = ssaoNoiseTex.Get();
                dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst2.SubresourceIndex = 0;
                cmdList->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);

                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = ssaoNoiseTex.Get();
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &b);
                cmdList->Close();
                ID3D12CommandList* lists[] = { cmdList.Get() };
                cmdQueue->ExecuteCommandLists(1, lists);
                waitForAllFrames();

                // Create SRV for noise in ssaoSrvHeap[1]
                D3D12_CPU_DESCRIPTOR_HANDLE noiseCpu = ssaoSrvHeap->GetCPUDescriptorHandleForHeapStart();
                noiseCpu.ptr += 1 * ssaoSrvDescSize;
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(ssaoNoiseTex.Get(), &sv, noiseCpu);
            }
        }

        // ── White fallback texture (1×1, R8) ─────────────────────────────────
        // Uploaded as a single-texel buffer then transitioned to PSR.
        {
            D3D12_HEAP_PROPERTIES defHp{}; defHp.Type = D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC td{};
            td.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width            = 1;
            td.Height           = 1;
            td.DepthOrArraySize = 1;
            td.MipLevels        = 1;
            td.Format           = DXGI_FORMAT_R8_UNORM;
            td.SampleDesc.Count = 1;
            device->CreateCommittedResource(&defHp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ssaoWhiteTex));

            const UINT rowPitch = alignUp(1u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
            D3D12_HEAP_PROPERTIES upHp{}; upHp.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC ubd{};
            ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            ubd.Width = rowPitch; ubd.Height = 1; ubd.DepthOrArraySize = 1;
            ubd.MipLevels = 1; ubd.Format = DXGI_FORMAT_UNKNOWN;
            ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            device->CreateCommittedResource(&upHp, D3D12_HEAP_FLAG_NONE, &ubd,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ssaoWhiteUpload));

            if (ssaoWhiteUpload && ssaoWhiteTex)
            {
                void* mapped = nullptr; D3D12_RANGE noRead{0,0};
                ssaoWhiteUpload->Map(0, &noRead, &mapped);
                if (mapped) { static_cast<uint8_t*>(mapped)[0] = 0xFF; ssaoWhiteUpload->Unmap(0, nullptr); }

                waitForAllFrames();
                cmdAllocators[0]->Reset();
                cmdList->Reset(cmdAllocators[0].Get(), nullptr);

                D3D12_TEXTURE_COPY_LOCATION src2{};
                src2.pResource = ssaoWhiteUpload.Get();
                src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src2.PlacedFootprint.Footprint = { DXGI_FORMAT_R8_UNORM, 1, 1, 1, rowPitch };
                D3D12_TEXTURE_COPY_LOCATION dst2{};
                dst2.pResource = ssaoWhiteTex.Get();
                dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst2.SubresourceIndex = 0;
                cmdList->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);

                D3D12_RESOURCE_BARRIER b{};
                b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                b.Transition.pResource   = ssaoWhiteTex.Get();
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
                b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                cmdList->ResourceBarrier(1, &b);
                cmdList->Close();
                ID3D12CommandList* lists[] = { cmdList.Get() };
                cmdQueue->ExecuteCommandLists(1, lists);
                waitForAllFrames();

                // Fill sceneSrvHeap[2] (white fallback for t2 when SSAO disabled)
                D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
                sv.Format = DXGI_FORMAT_R8_UNORM;
                sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                sv.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(ssaoWhiteTex.Get(), &sv, sceneSrvCpu(2));
                // Initialize slot 1 to white as well (overwritten when SSAO targets are created)
                device->CreateShaderResourceView(ssaoWhiteTex.Get(), &sv, sceneSrvCpu(1));
            }
        }

        // Kernel seeds
        auto kernel = BuildSSAOKernel(32);

        // Pre-fill ssaoCBPtr[*] kernel section (proj, noiseScale, params come from runSSAO each frame)
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            if (!ssaoCBPtr[f]) continue;
            // Kernel: starts at offset 64+16+16 = 96 bytes (after proj(64)+noiseScale(16)+params(16))
            uint8_t* kptr = static_cast<uint8_t*>(ssaoCBPtr[f]) + 96;
            for (int i = 0; i < 32; ++i)
            {
                glm::vec4 k4(kernel[i], 0.0f);
                std::memcpy(kptr + i * 16, &k4, sizeof(k4));
            }
        }

        ssaoReady = true;
        return true;
    }

    // ── SSAO render target creation/resize ───────────────────────────────────
    void createSSAOTargets(int w, int h)
    {
        waitForAllFrames();
        ssaoPosRT.Reset(); ssaoPosDepth.Reset(); ssaoRT.Reset(); ssaoBlurRT.Reset();

        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

        auto makeRT = [&](DXGI_FORMAT fmt, UINT tw, UINT th, bool isDepth,
                          ComPtr<ID3D12Resource>& res,
                          D3D12_RESOURCE_STATES initState) -> bool
        {
            D3D12_RESOURCE_DESC rd{};
            rd.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width            = tw;
            rd.Height           = th;
            rd.DepthOrArraySize = 1;
            rd.MipLevels        = 1;
            rd.Format           = fmt;
            rd.SampleDesc.Count = 1;
            rd.Flags            = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                                          : D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            D3D12_CLEAR_VALUE cv{};
            cv.Format = fmt;
            if (isDepth) { cv.DepthStencil.Depth = 1.0f; }
            else         { cv.Color[0]=cv.Color[1]=cv.Color[2]=cv.Color[3]=1.0f; }
            return SUCCEEDED(device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                initState, &cv, IID_PPV_ARGS(&res)));
        };

        if (!makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, static_cast<UINT>(w), static_cast<UINT>(h),
                    false, ssaoPosRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return;
        if (!makeRT(DXGI_FORMAT_D16_UNORM, static_cast<UINT>(w), static_cast<UINT>(h),
                    true, ssaoPosDepth, D3D12_RESOURCE_STATE_DEPTH_WRITE)) return;
        if (!makeRT(DXGI_FORMAT_R8_UNORM, static_cast<UINT>(w), static_cast<UINT>(h),
                    false, ssaoRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return;
        if (!makeRT(DXGI_FORMAT_R8_UNORM, static_cast<UINT>(w), static_cast<UINT>(h),
                    false, ssaoBlurRT, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE)) return;

        ssaoPosRTState  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ssaoRTState     = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        ssaoBlurRTState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // RTVs
        device->CreateRenderTargetView(ssaoPosRT.Get(),  nullptr, ssaoPosRtvCpu);
        device->CreateRenderTargetView(ssaoRT.Get(),     nullptr, ssaoRtvCpu);
        device->CreateRenderTargetView(ssaoBlurRT.Get(), nullptr, ssaoBlurRtvCpu);

        // DSV
        D3D12_DEPTH_STENCIL_VIEW_DESC dvd{};
        dvd.Format        = DXGI_FORMAT_D16_UNORM;
        dvd.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(ssaoPosDepth.Get(), &dvd, ssaoDsvCpu);

        // SRVs in ssaoSrvHeap: [0]=ssaoPosRT, [2]=ssaoRT, [3]=ssaoBlurRT
        // (slot [1] = noise, already filled)
        D3D12_CPU_DESCRIPTOR_HANDLE srvBase = ssaoSrvHeap->GetCPUDescriptorHandleForHeapStart();
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h0 = srvBase; h0.ptr += 0 * ssaoSrvDescSize;
            device->CreateShaderResourceView(ssaoPosRT.Get(), &sv, h0);
        }
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE h2 = srvBase; h2.ptr += 2 * ssaoSrvDescSize;
            D3D12_CPU_DESCRIPTOR_HANDLE h3 = srvBase; h3.ptr += 3 * ssaoSrvDescSize;
            device->CreateShaderResourceView(ssaoRT.Get(),     &sv, h2);
            device->CreateShaderResourceView(ssaoBlurRT.Get(), &sv, h3);
        }

        // Also update sceneSrvHeap[1] to point to ssaoBlurRT (the AO result bound at t2)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format = DXGI_FORMAT_R8_UNORM;
            sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sv.Texture2D.MipLevels = 1;
            device->CreateShaderResourceView(ssaoBlurRT.Get(), &sv, sceneSrvCpu(1));
        }

        ssaoW = w; ssaoH = h;
    }

    // ── SSAO 3-pass execution (called each frame before geometry) ────────────
    void runSSAO(ID3D12GraphicsCommandList* cl, UINT fi,
                 const std::vector<const DrawCall*>& opaqueDCs,
                 const glm::mat4& view, const glm::mat4& proj,
                 const glm::mat4& viewProj,
                 int w, int h,
                 ContentManager* cm)
    {
        if (!ssaoReady || !ssaoPosRT) return;

        // ── Pass 1: position prepass ────────────────────────────────────────
        barrier12(cl, ssaoPosRT.Get(), ssaoPosRTState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ssaoPosRTState = D3D12_RESOURCE_STATE_RENDER_TARGET;

        const float clearBlack[4] = { 0,0,0,0 };
        const float clearWhite[4] = { 1,1,1,1 };
        cl->ClearRenderTargetView(ssaoPosRtvCpu, clearBlack, 0, nullptr);
        cl->ClearDepthStencilView(ssaoDsvCpu, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
        cl->OMSetRenderTargets(1, &ssaoPosRtvCpu, FALSE, &ssaoDsvCpu);
        D3D12_VIEWPORT vp{ 0, 0, (float)w, (float)h, 0.0f, 1.0f };
        D3D12_RECT     sc{ 0, 0, w, h };
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->SetPipelineState(ssaoPosPSO.Get());
        cl->SetGraphicsRootSignature(ssaoPosRS.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

        UINT posDrawIdx = 0;
        for (const DrawCall* dc : opaqueDCs)
        {
            if (posDrawIdx >= k_maxDraws) break;
            const GpuMesh* mesh = resolveMesh(dc->meshAssetId, cm);
            const GpuMesh& m    = mesh ? *mesh : cube;
            if (!m.indexCount) continue;

            auto drawPosOne = [&](const glm::mat4& model) {
                if (posDrawIdx >= k_maxDraws) return;
                struct PosCB { glm::mat4 mvp; glm::mat4 modelView; };
                PosCB pcb{};
                pcb.mvp       = viewProj * model;
                pcb.modelView = view     * model;
                if (ssaoPosPerObjPtr[fi])
                    std::memcpy(ssaoPosPerObjPtr[fi] + static_cast<size_t>(posDrawIdx) * k_cbSlot,
                                &pcb, sizeof(pcb));
                cl->SetGraphicsRootConstantBufferView(0,
                    ssaoPosPerObjRing[fi]->GetGPUVirtualAddress()
                    + static_cast<UINT64>(posDrawIdx) * k_cbSlot);
                cl->IASetVertexBuffers(0, 1, &m.vbv);
                cl->IASetIndexBuffer(&m.ibv);
                cl->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
                ++posDrawIdx;
            };

            if (!dc->instanceTransforms.empty())
                for (const glm::mat4& t : dc->instanceTransforms) drawPosOne(t);
            else
                drawPosOne(dc->transform);
        }

        // ── Transition pos RT to SRV, bind ssaoSrvHeap ─────────────────────
        barrier12(cl, ssaoPosRT.Get(), ssaoPosRTState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ssaoPosRTState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ── Pass 2: SSAO ────────────────────────────────────────────────────
        barrier12(cl, ssaoRT.Get(), ssaoRTState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ssaoRTState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ClearRenderTargetView(ssaoRtvCpu, clearWhite, 0, nullptr);
        cl->OMSetRenderTargets(1, &ssaoRtvCpu, FALSE, nullptr);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->SetPipelineState(ssaoPSO.Get());
        cl->SetGraphicsRootSignature(ssaoRS.Get());

        // Upload SSAO CB: proj(64) + noiseScale(16) + params(16) + kernel already set in slots 96+
        if (ssaoCBPtr[fi])
        {
            // proj matrix (column-major, HLSL default = row-major, glm is column-major)
            // D3D12 expects row-major in HLSL unless #pragma pack_matrix or transposed.
            // The scene already passes glm mats directly (kSceneHLSL), so mirror that convention.
            std::memcpy(ssaoCBPtr[fi], glm::value_ptr(proj), 64);
            // noiseScale: (W/4, H/4, 0, 0)
            glm::vec4 noiseScale(float(w)/4.0f, float(h)/4.0f, 0.0f, 0.0f);
            std::memcpy(static_cast<uint8_t*>(ssaoCBPtr[fi]) + 64, &noiseScale, 16);
            // params: (radius, bias, intensity, method)
            glm::vec4 params(ssaoRadius, ssaoBias, ssaoIntensity, float(ssaoMethod));
            std::memcpy(static_cast<uint8_t*>(ssaoCBPtr[fi]) + 80, &params, 16);
            // kernel[32] already filled at init time
        }
        cl->SetGraphicsRootConstantBufferView(0, ssaoCB[fi]->GetGPUVirtualAddress());
        ID3D12DescriptorHeap* ssaoHeaps[] = { ssaoSrvHeap.Get(), ssaoSamplerHeap.Get() };
        cl->SetDescriptorHeaps(2, ssaoHeaps);
        cl->SetGraphicsRootDescriptorTable(1, ssaoPosGpuSrv); // t0=posRT, t1=noise (contiguous)
        cl->DrawInstanced(3, 1, 0, 0);

        // ── Transition AO RT to SRV, AO blur RT to render target ────────────
        barrier12(cl, ssaoRT.Get(), ssaoRTState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ssaoRTState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

        // ── Pass 3: SSAO blur ───────────────────────────────────────────────
        barrier12(cl, ssaoBlurRT.Get(), ssaoBlurRTState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        ssaoBlurRTState = D3D12_RESOURCE_STATE_RENDER_TARGET;
        cl->ClearRenderTargetView(ssaoBlurRtvCpu, clearWhite, 0, nullptr);
        cl->OMSetRenderTargets(1, &ssaoBlurRtvCpu, FALSE, nullptr);
        cl->RSSetViewports(1, &vp);
        cl->RSSetScissorRects(1, &sc);
        cl->SetPipelineState(ssaoBlurPSO.Get());
        cl->SetGraphicsRootSignature(ssaoRS.Get());
        if (ssaoBlurCBPtr[fi])
        {
            glm::vec4 texel(1.0f/float(w), 1.0f/float(h), 0.0f, 0.0f);
            std::memcpy(ssaoBlurCBPtr[fi], &texel, 16);
        }
        cl->SetGraphicsRootConstantBufferView(0, ssaoBlurCB[fi]->GetGPUVirtualAddress());
        // Bind only ssaoSrvHeap (no sampler heap change needed if root sig uses static sampler)
        // — our ssaoRS uses a static sampler, so the sampler heap is not needed here.
        // But we already set the SRV heap above; just update table to point at ssaoRT (slot 2).
        cl->SetGraphicsRootDescriptorTable(1, ssaoGpuSrv); // t0=ssaoRT
        cl->DrawInstanced(3, 1, 0, 0);

        // ── Transition blur result to SRV (ready for scene) ─────────────────
        barrier12(cl, ssaoBlurRT.Get(), ssaoBlurRTState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        ssaoBlurRTState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }

    void uploadMesh(GpuMesh& mesh, const std::vector<float>& interleaved,
                    const std::vector<uint32_t>& indices)
    {
        const UINT vbytes = static_cast<UINT>(interleaved.size() * sizeof(float));
        const UINT ibytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        mesh.vbuf = createUploadBuffer(vbytes, nullptr, interleaved.data());
        mesh.ibuf = createUploadBuffer(ibytes, nullptr, indices.data());
        mesh.vbv  = { mesh.vbuf->GetGPUVirtualAddress(), vbytes, 8u * sizeof(float) };
        mesh.ibv  = { mesh.ibuf->GetGPUVirtualAddress(), ibytes, DXGI_FORMAT_R32_UINT };
        mesh.indexCount = static_cast<UINT>(indices.size());
    }

    void createCube()
    {
        static const float v[] = {
             0.5f,-0.5f,-0.5f, 1,0,0, 0,0,   0.5f, 0.5f,-0.5f, 1,0,0, 0,0,   0.5f, 0.5f, 0.5f, 1,0,0, 0,0,   0.5f,-0.5f, 0.5f, 1,0,0, 0,0,
            -0.5f,-0.5f, 0.5f,-1,0,0, 0,0,  -0.5f, 0.5f, 0.5f,-1,0,0, 0,0,  -0.5f, 0.5f,-0.5f,-1,0,0, 0,0,  -0.5f,-0.5f,-0.5f,-1,0,0, 0,0,
            -0.5f, 0.5f,-0.5f, 0,1,0, 0,0,  -0.5f, 0.5f, 0.5f, 0,1,0, 0,0,   0.5f, 0.5f, 0.5f, 0,1,0, 0,0,   0.5f, 0.5f,-0.5f, 0,1,0, 0,0,
            -0.5f,-0.5f, 0.5f, 0,-1,0,0,0,  -0.5f,-0.5f,-0.5f, 0,-1,0,0,0,   0.5f,-0.5f,-0.5f, 0,-1,0,0,0,   0.5f,-0.5f, 0.5f, 0,-1,0,0,0,
            -0.5f,-0.5f, 0.5f, 0,0,1, 0,0,   0.5f,-0.5f, 0.5f, 0,0,1, 0,0,   0.5f, 0.5f, 0.5f, 0,0,1, 0,0,  -0.5f, 0.5f, 0.5f, 0,0,1, 0,0,
             0.5f,-0.5f,-0.5f, 0,0,-1,0,0,  -0.5f,-0.5f,-0.5f, 0,0,-1,0,0,  -0.5f, 0.5f,-0.5f, 0,0,-1,0,0,   0.5f, 0.5f,-0.5f, 0,0,-1,0,0,
        };
        static const uint32_t idx[] = {
             0, 2, 1,  0, 3, 2,    4, 6, 5,  4, 7, 6,
             8,10, 9,  8,11,10,   12,14,13, 12,15,14,
            16,18,17, 16,19,18,   20,22,21, 20,23,22,
        };
        std::vector<float>    verts(v, v + sizeof(v) / sizeof(float));
        std::vector<uint32_t> indices(idx, idx + sizeof(idx) / sizeof(uint32_t));
        uploadMesh(cube, verts, indices);
        cube.localBounds.expand({ -0.5f, -0.5f, -0.5f });
        cube.localBounds.expand({  0.5f,  0.5f,  0.5f });
    }

    GpuMesh* resolveMesh(const HE::UUID& assetId, ContentManager* cm)
    {
        if (assetId == HE::UUID{} || !cm) return nullptr;
        if (auto it = meshCache.find(assetId); it != meshCache.end()) return &it->second;

        const StaticMeshAsset* asset = cm->getStaticMesh(assetId);
        if (!asset || asset->indices.empty() || (asset->vertices.empty() && !asset->cooked)) return nullptr;

        // Cooked (packaged) assets ship the interleaved pos+norm+uv buffer + baked
        // AABB, built once at pack time. Loose/editor assets interleave on first draw.
        GpuMesh mesh;
        std::vector<float> built;
        const std::vector<float>* vtx = &asset->interleaved;
        if (asset->cooked)
        {
            mesh.localBounds.min = { asset->boundsMin[0], asset->boundsMin[1], asset->boundsMin[2] };
            mesh.localBounds.max = { asset->boundsMax[0], asset->boundsMax[1], asset->boundsMax[2] };
        }
        else
        {
            const size_t vertexCount = asset->vertices.size() / 3;
            built.reserve(vertexCount * 8);
            for (size_t i = 0; i < vertexCount; ++i)
            {
                built.insert(built.end(),
                    { asset->vertices[i*3+0], asset->vertices[i*3+1], asset->vertices[i*3+2] });
                if (i * 3 + 2 < asset->normals.size())
                    built.insert(built.end(),
                        { asset->normals[i*3+0], asset->normals[i*3+1], asset->normals[i*3+2] });
                else
                    built.insert(built.end(), { 0.0f, 0.0f, 0.0f });
                if (i * 2 + 1 < asset->uvs.size())
                    built.insert(built.end(), { asset->uvs[i*2+0], asset->uvs[i*2+1] });
                else
                    built.insert(built.end(), { 0.0f, 0.0f });
            }
            vtx = &built;
            mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
        }
        const std::vector<float>& interleaved = *vtx;
        uploadMesh(mesh, interleaved, asset->indices);
        return &meshCache.emplace(assetId, mesh).first->second;
    }

    // ── Skinned pipeline (separate root sig with b2 for bones CB) ────────────
    bool createSkinnedPipeline()
    {
        // Root signature = scene root sig + an extra root CBV at param[3] for b2 (bones).
        // Params [0..2] and [4] (the t1 base-color table) match the scene root sig so the
        // shared PS works; param[3] (bones) is the skinned-only addition.
        D3D12_DESCRIPTOR_RANGE srvRanges[2]{};
        srvRanges[0].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[0].NumDescriptors                    = 1;
        srvRanges[0].BaseShaderRegister                = 0; // t0
        srvRanges[0].RegisterSpace                     = 0;
        srvRanges[0].OffsetInDescriptorsFromTableStart = 0;
        srvRanges[1].RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRanges[1].NumDescriptors                    = 1;
        srvRanges[1].BaseShaderRegister                = 2; // t2
        srvRanges[1].RegisterSpace                     = 0;
        srvRanges[1].OffsetInDescriptorsFromTableStart = 1;

        // Per-draw base-color table (t1) — matches the scene root sig's param[3].
        D3D12_DESCRIPTOR_RANGE albedoRange{};
        albedoRange.RangeType                         = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        albedoRange.NumDescriptors                    = 1;
        albedoRange.BaseShaderRegister                = 1; // t1
        albedoRange.RegisterSpace                     = 0;
        albedoRange.OffsetInDescriptorsFromTableStart = 0;

        D3D12_ROOT_PARAMETER params[5]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor       = { 0, 0 }; // b0 per-object
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor       = { 1, 0 }; // b1 per-frame
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 2;
        params[2].DescriptorTable.pDescriptorRanges   = srvRanges;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;
        params[3].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[3].Descriptor       = { 2, 0 }; // b2 bones
        params[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
        params[4].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[4].DescriptorTable.NumDescriptorRanges = 1; // t1 (albedo)
        params[4].DescriptorTable.pDescriptorRanges   = &albedoRange;
        params[4].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        // Same static samplers as the scene root sig: s0 shadow linear-clamp, s1 AO point-clamp,
        // s2 base-color linear-wrap. The shared PS references all three.
        D3D12_STATIC_SAMPLER_DESC samplers[3]{};
        samplers[0].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[0].AddressU = samplers[0].AddressV = samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[0].ShaderRegister = 0;
        samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[1].Filter         = D3D12_FILTER_MIN_MAG_MIP_POINT;
        samplers[1].AddressU = samplers[1].AddressV = samplers[1].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samplers[1].ShaderRegister = 1;
        samplers[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        samplers[2].Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samplers[2].AddressU = samplers[2].AddressV = samplers[2].AddressW = D3D12_TEXTURE_ADDRESS_MODE_WRAP;
        samplers[2].ShaderRegister = 2;
        samplers[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 5;
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = 3;
        rsd.pStaticSamplers   = samplers;
        rsd.Flags             = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

        ComPtr<ID3DBlob> sig, err;
        if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: skinned root signature serialize failed");
            return false;
        }
        if (FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                   IID_PPV_ARGS(&skinnedRootSig))))
            return false;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        // Reuse the scene PS (PSMain from kSceneHLSL — it references b0, b1, t0, t1, t2, s0, s1, s2).
        const std::string sceneSource = std::string(kSkyFuncHLSL12) + kSceneHLSL;
        ComPtr<ID3DBlob> ps, cerr;
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &ps, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D12Renderer: skinned PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }
        // Skinned vertex shader.
        ComPtr<ID3DBlob> vs;
        if (FAILED(D3DCompile(kSkinnedHLSL12, strlen(kSkinnedHLSL12), "skinned", nullptr, nullptr,
                              "VSMainSkinned", "vs_5_0", flags, 0, &vs, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D12Renderer: skinned VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }

        // 5-element input layout: slot 0 interleaved (pos+norm+uv), slot 1 boneIds, slot 2 boneWeights.
        const D3D12_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  1,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2,  0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        };

        D3D12_GRAPHICS_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature        = skinnedRootSig.Get();
        pd.VS                    = { vs->GetBufferPointer(), vs->GetBufferSize() };
        pd.PS                    = { ps->GetBufferPointer(), ps->GetBufferSize() };
        pd.InputLayout           = { layout, 5 };
        pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pd.NumRenderTargets      = 1;
        pd.RTVFormats[0]         = DXGI_FORMAT_R8G8B8A8_UNORM; // fallback; HDR variant created below
        pd.DSVFormat             = DXGI_FORMAT_D32_FLOAT;
        pd.SampleDesc.Count      = 1;
        pd.SampleMask            = UINT_MAX;
        pd.RasterizerState.FillMode              = D3D12_FILL_MODE_SOLID;
        pd.RasterizerState.CullMode              = D3D12_CULL_MODE_NONE;
        pd.RasterizerState.DepthClipEnable       = TRUE;
        pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pd.DepthStencilState.DepthEnable    = TRUE;
        pd.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        pd.DepthStencilState.DepthFunc      = D3D12_COMPARISON_FUNC_LESS;

        if (FAILED(device->CreateGraphicsPipelineState(&pd, IID_PPV_ARGS(&m_skinnedPSO))))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: skinned PSO creation failed");
            return false;
        }

        // HDR variant: RGBA16F for the PostFX scene pass.
        {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC hp = pd;
            hp.RTVFormats[0] = DXGI_FORMAT_R16G16B16A16_FLOAT;
            device->CreateGraphicsPipelineState(&hp, IID_PPV_ARGS(&m_skinnedHdrPSO));
        }

        // Bones CB ring: k_maxSkinnedDraws slots × k_bonesCBSlot bytes per frame.
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            m_bonesCB[f] = createUploadBuffer(static_cast<UINT64>(k_maxSkinnedDraws) * k_bonesCBSlot,
                                              reinterpret_cast<void**>(&m_bonesCBPtr[f]));
        }
        return true;
    }

    // ── Upload + cache a SkeletalMeshAsset → GpuSkeletalMesh12 ──────────────
    GpuSkeletalMesh12* resolveSkeletalMesh12(const HE::UUID& assetId, ContentManager* cm)
    {
        if (assetId == HE::UUID{} || !cm) return nullptr;
        if (auto it = m_skeletalMeshCache.find(assetId); it != m_skeletalMeshCache.end())
            return &it->second;

        const SkeletalMeshAsset* asset = cm->getSkeletalMesh(assetId);
        if (!asset || asset->vertices.empty() || asset->indices.empty()) return nullptr;

        const size_t vertexCount = asset->vertices.size() / 3;
        if (asset->boneIDs.empty() || asset->boneWeights.empty() ||
            asset->boneIDs.size() != vertexCount * 4)
            return nullptr;

        // Build interleaved pos+norm+uv for slot 0 (stride 32).
        std::vector<float> interleaved;
        interleaved.reserve(vertexCount * 8);
        for (size_t i = 0; i < vertexCount; ++i)
        {
            interleaved.insert(interleaved.end(),
                { asset->vertices[i*3+0], asset->vertices[i*3+1], asset->vertices[i*3+2] });
            if (i * 3 + 2 < asset->normals.size())
                interleaved.insert(interleaved.end(),
                    { asset->normals[i*3+0], asset->normals[i*3+1], asset->normals[i*3+2] });
            else
                interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
            if (i * 2 + 1 < asset->uvs.size())
                interleaved.insert(interleaved.end(), { asset->uvs[i*2+0], asset->uvs[i*2+1] });
            else
                interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
        }

        GpuSkeletalMesh12 skm;
        // Slot 0: interleaved VB (stride 32).
        {
            const UINT64 bytes = static_cast<UINT64>(interleaved.size()) * sizeof(float);
            skm.vbuf = createUploadBuffer(bytes, nullptr, interleaved.data());
            skm.vbv.BufferLocation = skm.vbuf->GetGPUVirtualAddress();
            skm.vbv.SizeInBytes    = static_cast<UINT>(bytes);
            skm.vbv.StrideInBytes  = 8 * sizeof(float); // pos3+norm3+uv2
        }
        // Slot 1: boneIds (uint32 × 4 per vertex, stride 16).
        {
            const UINT64 bytes = static_cast<UINT64>(vertexCount) * 4 * sizeof(uint32_t);
            skm.boneIdVb = createUploadBuffer(bytes, nullptr, asset->boneIDs.data());
            skm.boneIdVbv.BufferLocation = skm.boneIdVb->GetGPUVirtualAddress();
            skm.boneIdVbv.SizeInBytes    = static_cast<UINT>(bytes);
            skm.boneIdVbv.StrideInBytes  = 4 * sizeof(uint32_t);
        }
        // Slot 2: boneWeights (float × 4 per vertex, stride 16).
        {
            const UINT64 bytes = static_cast<UINT64>(vertexCount) * 4 * sizeof(float);
            skm.boneWgtVb = createUploadBuffer(bytes, nullptr, asset->boneWeights.data());
            skm.boneWgtVbv.BufferLocation = skm.boneWgtVb->GetGPUVirtualAddress();
            skm.boneWgtVbv.SizeInBytes    = static_cast<UINT>(bytes);
            skm.boneWgtVbv.StrideInBytes  = 4 * sizeof(float);
        }
        // Index buffer.
        {
            const UINT64 bytes = static_cast<UINT64>(asset->indices.size()) * sizeof(uint32_t);
            skm.ibuf = createUploadBuffer(bytes, nullptr, asset->indices.data());
            skm.ibv.BufferLocation = skm.ibuf->GetGPUVirtualAddress();
            skm.ibv.SizeInBytes    = static_cast<UINT>(bytes);
            skm.ibv.Format         = DXGI_FORMAT_R32_UINT;
        }
        skm.indexCount = static_cast<UINT>(asset->indices.size());

        return &m_skeletalMeshCache.emplace(assetId, std::move(skm)).first->second;
    }
};

D3D12Renderer::D3D12Renderer()  : m_impl(new D3D12RendererImpl{}) {}
D3D12Renderer::~D3D12Renderer() { delete m_impl; }

void D3D12Renderer::Initialize(HE::Window* window)
{
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: initializing");
    SDL_PropertiesID props = SDL_GetWindowProperties(window->GetNativeWindow());
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        throw std::runtime_error("D3D12Renderer: could not get HWND");

    m_impl->width  = window->GetWidth();
    m_impl->height = window->GetHeight();

    // GPU debug instrumentation (debug layer + InfoQueue drain + DRED breadcrumbs).
    // Opt-in: on in _DEBUG builds, or in any build when HE_GPU_DEBUG is set. These add a
    // per-GPU-op cost (esp. DRED auto-breadcrumbs) that is fine at vsync-capped FPS but
    // tanks the editor at the uncapped FPS you get with vsync off — so keep it gated.
#ifdef _DEBUG
    m_impl->gpuDebug = true;
#else
    m_impl->gpuDebug = (std::getenv("HE_GPU_DEBUG") != nullptr);
#endif
    if (m_impl->gpuDebug)
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
        {
            debugController->EnableDebugLayer();
            // GPU-based validation validates descriptors/resource-states AT DRAW TIME and
            // names the exact offending draw — but it's extremely slow (seconds/frame), so
            // it's opt-in via HE_GPU_GBV. It already pinpointed the viewport-RT lifetime
            // bug; leave it off for normal runs.
            if (std::getenv("HE_GPU_GBV"))
            {
                ComPtr<ID3D12Debug1> debugController1;
                if (SUCCEEDED(debugController.As(&debugController1)))
                {
                    debugController1->SetEnableGPUBasedValidation(TRUE);
                    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: GPU-based validation ENABLED");
                }
            }
        }
        // DRED must be enabled BEFORE device creation. Names the exact op that hung.
        ComPtr<ID3D12DeviceRemovedExtendedDataSettings> dredSettings;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dredSettings))))
        {
            dredSettings->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: GPU debug layer + DRED ENABLED");
        }
        else
            Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: GPU debug layer ENABLED (DRED unavailable)");
    }

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_impl->device))))
        throw std::runtime_error("D3D12Renderer: D3D12CreateDevice failed");

    // The debug layer writes validation errors to the InfoQueue. Without a debugger
    // attached those messages are invisible — grab the queue so we can drain it into
    // the engine Logger each frame and see exactly what the GPU is complaining about.
    if (m_impl->gpuDebug && SUCCEEDED(m_impl->device.As(&m_impl->infoQueue)) && m_impl->infoQueue)
    {
        m_impl->infoQueue->SetMuteDebugOutput(FALSE);
        m_impl->infoQueue->ClearStoredMessages();
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_impl->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_impl->cmdQueue))))
        throw std::runtime_error("D3D12Renderer: CreateCommandQueue failed");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        throw std::runtime_error("D3D12Renderer: CreateDXGIFactory2 failed");

    // ALLOW_TEARING (for unsynced present) is only legal when the adapter+OS support
    // it. Using the flag unconditionally on hardware that lacks it produces visible
    // tearing AND makes Present() return an error every frame — repeated failures
    // remove the device, which reads as "torn image, then crashes after seconds."
    // Query support and only enable the flag when it's actually available.
    {
        ComPtr<IDXGIFactory5> factory5;
        if (SUCCEEDED(factory.As(&factory5)))
        {
            BOOL tearing = FALSE;
            if (SUCCEEDED(factory5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                                                        &tearing, sizeof(tearing))))
                m_impl->allowTearing = (tearing == TRUE);
        }
        Logger::Log(Logger::LogLevel::Info,
            m_impl->allowTearing ? "D3D12Renderer: ALLOW_TEARING supported"
                                 : "D3D12Renderer: ALLOW_TEARING NOT supported — disabled");
    }

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount      = k_frameCount;
    scd.Flags            = m_impl->allowTearing ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    scd.Width            = window->GetWidth();
    scd.Height           = window->GetHeight();
    scd.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferUsage      = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.SwapEffect       = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    scd.SampleDesc.Count = 1;

    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory->CreateSwapChainForHwnd(m_impl->cmdQueue.Get(), hwnd, &scd, nullptr, nullptr, &sc1)))
        throw std::runtime_error("D3D12Renderer: CreateSwapChainForHwnd failed");
    if (FAILED(sc1.As(&m_impl->swapchain)))
        throw std::runtime_error("D3D12Renderer: swapchain cast to IDXGISwapChain3 failed");
    m_impl->frameIndex = m_impl->swapchain->GetCurrentBackBufferIndex();

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.NumDescriptors = k_frameCount;
    hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    if (FAILED(m_impl->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&m_impl->rtvHeap))))
        throw std::runtime_error("D3D12Renderer: CreateDescriptorHeap failed");
    m_impl->rtvDescSize = m_impl->device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    for (UINT i = 0; i < k_frameCount; ++i)
    {
        if (FAILED(m_impl->swapchain->GetBuffer(i, IID_PPV_ARGS(&m_impl->renderTargets[i]))))
            throw std::runtime_error("D3D12Renderer: GetBuffer failed");
        m_impl->device->CreateRenderTargetView(m_impl->renderTargets[i].Get(), nullptr, m_impl->rtvHandle(i));
        if (FAILED(m_impl->device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&m_impl->cmdAllocators[i]))))
            throw std::runtime_error("D3D12Renderer: CreateCommandAllocator failed");
    }

    if (FAILED(m_impl->device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                  m_impl->cmdAllocators[0].Get(), nullptr,
                                                  IID_PPV_ARGS(&m_impl->cmdList))))
        throw std::runtime_error("D3D12Renderer: CreateCommandList failed");
    m_impl->cmdList->Close();

    if (FAILED(m_impl->device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_impl->fence))))
        throw std::runtime_error("D3D12Renderer: CreateFence failed");
    m_impl->fenceEvent = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (!m_impl->fenceEvent)
        throw std::runtime_error("D3D12Renderer: CreateEvent failed");

    m_impl->createGpuTimer();

    m_impl->createDepth(m_impl->width, m_impl->height);
    if (!m_impl->createPipeline())
        Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: scene pipeline creation failed — only clear will work");
    if (!m_impl->createPostFXPipelines())
        Logger::Log(Logger::LogLevel::Error, "D3D12Renderer: PostFX pipeline creation failed — no HDR/bloom/FXAA");
    m_impl->createCube();
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: initialized successfully");
}

void D3D12Renderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: shutdown — waiting for GPU");
    m_impl->waitForAllFrames();
    if (m_impl->fenceEvent) { CloseHandle(m_impl->fenceEvent); m_impl->fenceEvent = nullptr; }

    m_impl->meshCache.clear();
    m_impl->m_skeletalMeshCache.clear();
    m_impl->cube = {};
    m_impl->viewportRT.Reset();
    m_impl->viewportDepth.Reset();
    m_impl->viewportReadback.Reset();
    m_impl->viewportRtvHeap.Reset();
    m_impl->viewportDsvHeap.Reset();
    m_impl->m_uiFontAtlases.clear();
    m_impl->m_uiAtlasUploads.clear();
    m_impl->m_uiAtlasHeap.Reset();
    m_impl->meshTexUploads.clear(); // per-mesh base-color staging buffers (GpuMesh SRVs die with meshCache)
    m_impl->meshTexNextSlot = D3D12RendererImpl::k_sceneStaticSrvs; // reset the albedo-slot allocator for a possible re-Initialize
    m_impl->m_materialTexCache.clear(); // override-material textures
    m_impl->m_retiredTextures.clear();  // hot-reload retire list
    m_impl->m_pendingMatInval.clear();
    m_impl->m_pendingMeshInval.clear();
    m_impl->m_freeSlotPending.clear();
    m_impl->m_freeSlots.clear();
    m_impl->m_skinnedPSO.Reset();
    m_impl->m_skinnedHdrPSO.Reset();
    m_impl->skinnedRootSig.Reset();
    for (UINT i = 0; i < k_frameCount; ++i)
        m_impl->m_bonesCB[i].Reset();
    m_impl->pso.Reset();
    m_impl->transparentPSO.Reset();
    m_impl->depthPSO.Reset();
    m_impl->shadowDepth.Reset();
    m_impl->shadowSrvHeap.Reset();
    m_impl->rootSig.Reset();
    m_impl->depthBuffer.Reset();
    m_impl->dsvHeap.Reset();
    for (UINT i = 0; i < k_frameCount; ++i)
    {
        m_impl->perFrameCB[i].Reset();
        m_impl->perObjectRing[i].Reset();
        m_impl->perInstanceRing[i].Reset();
    }
    if (m_impl->tsReadbackPtr) { m_impl->tsReadback->Unmap(0, nullptr); m_impl->tsReadbackPtr = nullptr; }
    m_impl->tsReadback.Reset();
    m_impl->tsQueryHeap.Reset();
    m_impl->cmdList.Reset();
    for (UINT i = 0; i < k_frameCount; ++i)
    {
        m_impl->cmdAllocators[i].Reset();
        m_impl->renderTargets[i].Reset();
    }
    m_impl->fence.Reset();
    m_impl->rtvHeap.Reset();
    m_impl->swapchain.Reset();
    m_impl->cmdQueue.Reset();
    m_impl->device.Reset();
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: all resources released");
}

void D3D12Renderer::DrawScene(void* cmdListPtr, int width, int height)
{
    if (!m_world || !m_impl->pso || width <= 0 || height <= 0) return;
    auto& p = *m_impl;
    auto* cl = static_cast<ID3D12GraphicsCommandList*>(cmdListPtr);

    // Drop caches for materials/meshes edited since last frame (before the mesh-resolve
    // pre-pass below re-creates them fresh). Safe here: render thread, no draws recorded yet.
    p.processPendingInvalidations();

    // Feed time-of-day to the extractor so it recomputes the sun/moon direction from the
    // day-night clock (otherwise m_timeOfDay stays at its 0.5 default and the sky never
    // responds to the time slider). Mirrors the OpenGL/Metal backends.
    p.m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                              m_environment.sunColor, m_environment.sunIntensity,
                              m_environment.moonColor, m_environment.moonIntensity,
                              m_environment.cloudCoverage);
    p.m_extractor.setContentManager(m_contentManager);
    p.m_extractor.extract(*m_world, p.m_renderWorld,
                          static_cast<float>(width) / static_cast<float>(height),
                          &m_editorCamera);

    // Sky is independent of scene geometry — draw it before any early returns so
    // it always renders even when objects/sortedIndices is empty.
    {
        const glm::mat4 svp = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;
        p.drawSky(cl, p.frameIndex, glm::inverse(svp), p.m_renderWorld.sunDirection, m_environment);
    }

    if (p.m_renderWorld.objects.empty()) return;

    for (RenderObject& obj : p.m_renderWorld.objects)
    {
        if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);
        if (m_contentManager)
        {
            const HE::UUID matId = obj.materialAssetId;
            if (const MaterialAsset* mat = (matId == HE::UUID{}) ? nullptr
                                           : m_contentManager->getMaterial(matId))
            {
                obj.baseColor = { mat->baseColor[0], mat->baseColor[1], mat->baseColor[2] };
                obj.metallic  = mat->metallic;
                obj.roughness = mat->roughness;
                obj.opacity   = mat->opacity;
            }
        }
    }

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
    p.statTotal   = static_cast<uint32_t>(p.m_renderWorld.objects.size());
    p.statVisible = static_cast<uint32_t>(p.m_sortedIndices.size());
    if (p.m_sortedIndices.empty()) return;

    if (p.m_renderGraph.empty())
    {
        p.m_renderGraph.addPass(std::make_unique<ShadowPass>());
        p.m_renderGraph.addPass(std::make_unique<GeometryPass>());
    }

    const glm::mat4 viewProj  = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;
    const bool      shadows   = p.m_renderWorld.shadow.enabled && p.shadowDepth && p.depthPSO;
    const glm::mat4 lightClip = kD3DClipFix * p.m_renderWorld.shadow.viewProj;

    // Per-frame constants for this frame slot.
    {
        PerFrameCB f{};
        f.cameraPos     = glm::vec4(p.m_renderWorld.camera.position, 1.0f);
        const int count = std::min(static_cast<int>(p.m_renderWorld.lights.size()), 8);
        f.lightCount    = glm::ivec4(count, 0, 0, 0);
        for (int i = 0; i < count; ++i)
        {
            const LightData& l = p.m_renderWorld.lights[i];
            f.lightPos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
            f.lightDir[i]    = glm::vec4(l.direction, l.spotAngleCos);
            f.lightColor[i]  = glm::vec4(l.color,     l.intensity);
            f.lightParams[i] = glm::vec4(l.range, 0.0f, 0.0f, 0.0f);
        }
        f.lightVP       = lightClip;
        f.shadowEnabled = glm::ivec4(shadows ? 1 : 0, 0, 0, 0);
        f.sunDir = glm::vec4(p.m_renderWorld.sunDirection, 0.0f);
        f.fog    = glm::vec4(m_environment.fogDensity, m_environment.fogHeightFalloff, 0, 0);
        f.viewport = glm::vec4(float(width), float(height),
                               (p.ssaoEnabled && p.ssaoReady) ? 1.0f : 0.0f, 0.0f);
        if (p.perFramePtr[p.frameIndex])
            std::memcpy(p.perFramePtr[p.frameIndex], &f, sizeof(f));
    }

    cl->SetGraphicsRootSignature(p.rootSig.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());

    const D3D12_GPU_VIRTUAL_ADDRESS ringBase = p.perObjectRing[p.frameIndex]->GetGPUVirtualAddress();
    uint8_t* ringPtr = p.perObjectPtr[p.frameIndex];
    // Instance-transform ring (A3): sub-allocated per instanced batch, one instanced draw each.
    const D3D12_GPU_VIRTUAL_ADDRESS instRingBase = p.perInstanceRing[p.frameIndex]->GetGPUVirtualAddress();
    uint8_t* instRingPtr = p.perInstancePtr[p.frameIndex];
    UINT instCursor = 0; // next free instance slot in the ring

    auto transition = [&](ID3D12Resource* res, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
    {
        D3D12_RESOURCE_BARRIER b{};
        b.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource   = res;
        b.Transition.StateBefore = before;
        b.Transition.StateAfter  = after;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        cl->ResourceBarrier(1, &b);
    };

    // One ring slot per draw across BOTH passes (the CPU writes are recorded
    // now; the GPU reads them at execute time, so shadow and geometry draws
    // must use distinct slots).
    UINT drawIdx = 0;
    p.m_renderGraph.execute(p.m_renderWorld, p.m_sortedIndices,
        [&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
    {
        // ── Shadow pass: depth from the light's POV ─────────────────────────
        if (io.output.id == kShadowMapTarget)
        {
            if (!shadows) return;
            transition(p.shadowDepth.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE,
                       D3D12_RESOURCE_STATE_DEPTH_WRITE);
            D3D12_CPU_DESCRIPTOR_HANDLE sdsv = p.dsvHandle(1);
            cl->OMSetRenderTargets(0, nullptr, FALSE, &sdsv);
            cl->ClearDepthStencilView(sdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);
            cl->SetPipelineState(p.depthPSO.Get());
            D3D12_VIEWPORT svp{ 0, 0, (float)p.shadowSize, (float)p.shadowSize, 0.0f, 1.0f };
            D3D12_RECT     ssc{ 0, 0, p.shadowSize, p.shadowSize };
            cl->RSSetViewports(1, &svp);
            cl->RSSetScissorRects(1, &ssc);
            for (const DrawCall& dc : cmds.drawCalls())
            {
                if (drawIdx >= k_maxDraws) break;
                const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
                const GpuMesh& m    = mesh ? *mesh : p.cube;
                if (!m.indexCount) continue;
                PerObjectCB o{};
                o.mvp = lightClip * dc.transform; o.model = dc.transform;
                if (ringPtr)
                    std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
                cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);
                cl->IASetVertexBuffers(0, 1, &m.vbv);
                cl->IASetIndexBuffer(&m.ibv);
                cl->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
                ++p.statDraws; p.statTris += m.indexCount / 3;
                ++drawIdx;
            }
            transition(p.shadowDepth.Get(), D3D12_RESOURCE_STATE_DEPTH_WRITE,
                       D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            // Restore the backbuffer RTV + scene DSV + viewport.
            auto rtv  = p.rtvHandle(p.frameIndex);
            auto dsv0 = p.dsvHandle(0);
            cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv0);
            D3D12_VIEWPORT vp{ 0, 0, (float)width, (float)height, 0.0f, 1.0f };
            D3D12_RECT     sc{ 0, 0, width, height };
            cl->RSSetViewports(1, &vp);
            cl->RSSetScissorRects(1, &sc);
            return;
        }

        if (io.output.id != kBackbufferTarget) return;

        // Collect draw calls split by opacity (needed for SSAO and for transparency sort).
        const glm::vec3 camPos = p.m_renderWorld.camera.position;
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        for (const DrawCall& dc : cmds.drawCalls())
            (dc.opacity < 0.999f ? transparentDCs : opaqueDCs).push_back(&dc);

        // One-time: log the per-pass draw counts so a DRED op index can be mapped to the
        // exact pass (shadow → ssaoPos → [ssao fullscreen] → opaque → skinned → transparent).
        if (!p.drawCountsLogged)
        {
            p.drawCountsLogged = true;
            char c[200];
            std::snprintf(c, sizeof(c),
                "D3D12 draw counts: shadow=%zu ssaoPos=%zu opaque=%zu skinned=%zu transparent=%zu",
                cmds.drawCalls().size(), opaqueDCs.size(), opaqueDCs.size(),
                cmds.skinnedDrawCalls().size(), transparentDCs.size());
            Logger::Log(Logger::LogLevel::Info, c);
        }

        // ── SSAO: run 3-pass (pos prepass → ssao → blur) before sky/geometry ─
        if (p.ssaoEnabled && p.ssaoReady)
        {
            if (p.ssaoW != width || p.ssaoH != height)
                p.createSSAOTargets(width, height);
            p.runSSAO(cl, p.frameIndex, opaqueDCs,
                      p.m_renderWorld.camera.view, p.m_renderWorld.camera.projection,
                      viewProj, width, height, m_contentManager);
            // Restore the scene RTV + depth after SSAO changed them.
            // (The RTV that was active before depends on HDR vs LDR path;
            //  Render() will have already called OMSetRenderTargets to the correct one.
            //  Re-bind it here so subsequent sky/geometry writes go to the right target.)
            // We store the active RTV handle in a local so we can restore it.
            // SSAO doesn't know the active RTV, so we rebuild from known state:
            // In the HDR path: hdrRT RTV; in LDR path: viewportRT or swapchain RTV.
            // The simplest correct approach is to re-issue OMSetRenderTargets using the
            // same targets that Render() bound before calling DrawScene().
            // Since we don't have that handle here, we leave it to the sky/geometry
            // stage below, which will use the already-bound RTV (sky doesn't re-set it).
            // DrawScene is called with the correct RTV already set by Render().
            // The SSAO pass changes RTV but leaves the command list in a drawable state.
            // We must rebind here.
            // Determine which RTV is active: hdrRT if usingHDR, else viewportRT if present.
            if (p.usingHDR && p.hdrRtvHeap)
            {
                auto hrtv = p.hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
                auto vdsv = p.viewportDsvHeap->GetCPUDescriptorHandleForHeapStart();
                cl->OMSetRenderTargets(1, &hrtv, FALSE, &vdsv);
            }
            else if (p.viewportRtvHeap)
            {
                auto vrtv = p.viewportRtvHeap->GetCPUDescriptorHandleForHeapStart();
                auto vdsv = p.viewportDsvHeap->GetCPUDescriptorHandleForHeapStart();
                cl->OMSetRenderTargets(1, &vrtv, FALSE, &vdsv);
            }
            else
            {
                auto rtv  = p.rtvHandle(p.frameIndex);
                auto dsv0 = p.dsvHandle(0);
                cl->OMSetRenderTargets(1, &rtv, FALSE, &dsv0);
            }
            D3D12_VIEWPORT vvp{ 0, 0, (float)width, (float)height, 0.0f, 1.0f };
            D3D12_RECT     vsc{ 0, 0, width, height };
            cl->RSSetViewports(1, &vvp);
            cl->RSSetScissorRects(1, &vsc);
        }

        // Restore scene root signature + per-frame CBV after SSAO changed them.
        cl->SetGraphicsRootSignature(p.rootSig.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());

        // ── Geometry pass: bind combined sceneSrvHeap (shadow t0 + AO t2) ───
        if (p.sceneSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { p.sceneSrvHeap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            // sceneSrvHeap[0]=shadow(t0), [1]=AO-blur(t2); the descriptor table has 2 ranges
            // starting at heap slot 0, so one SetGraphicsRootDescriptorTable covers both.
            cl->SetGraphicsRootDescriptorTable(2, p.sceneSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }
        std::sort(transparentDCs.begin(), transparentDCs.end(),
            [&](const DrawCall* a, const DrawCall* b) {
                return glm::length(glm::vec3(a->transform[3]) - camPos) >
                       glm::length(glm::vec3(b->transform[3]) - camPos);
            });

        // Real GPU instancing (A3) applies to the opaque pass only; the transparent
        // pass reuses drawDC12 with a blend PSO + per-instance depth sort, so it keeps
        // the per-instance loop (allowInstancing is set false before that pass).
        bool allowInstancing = true;
        auto drawDC12 = [&](const DrawCall& dc) {
            if (drawIdx >= k_maxDraws) return;
            GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.indexCount) return;
            // Base color: an explicit MaterialComponent override (dc.materialAssetId) wins
            // over the mesh's own baked texture; else the mesh's baked texture (A1); else flat.
            if (mesh) p.ensureMeshAlbedo(cl, m, dc.meshAssetId, m_contentManager);
            int albedoSlot = (mesh && m.albedoSlot >= 0) ? m.albedoSlot : -1; // baked (A1)
            int ovrSlot = -1;
            if (p.resolveMaterialAlbedo(cl, dc.materialAssetId, m_contentManager, ovrSlot))
                albedoSlot = ovrSlot; // override fully replaces the baked texture (may be flat)
            const bool textured = albedoSlot >= 0;
            cl->SetGraphicsRootDescriptorTable(3, p.sceneSrvGpu(
                textured ? static_cast<UINT>(albedoSlot) : p.k_albedoNullSlot));
            cl->IASetVertexBuffers(0, 1, &m.vbv);
            cl->IASetIndexBuffer(&m.ibv);
            auto drawOne = [&](const glm::mat4& model) {
                if (drawIdx >= k_maxDraws) return;
                PerObjectCB o{};
                o.mvp   = viewProj * model;
                o.model = model;
                o.color = glm::vec4(dc.baseColor, textured ? 1.0f : 0.0f);
                o.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity, 0.0f);
                if (ringPtr)
                    std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
                cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);
                cl->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
                ++p.statDraws; p.statTris += m.indexCount / 3;
                ++drawIdx;
            };
            if (!dc.instanceTransforms.empty())
            {
                // A3: real instancing — upload every instance's {mvp,model} to the
                // instance ring and issue ONE DrawIndexedInstanced. Falls back to the
                // per-instance loop when instancing isn't allowed (transparent pass) or
                // a ring is exhausted.
                static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be mvp+model");
                const UINT count = static_cast<UINT>(dc.instanceTransforms.size());
                // Pick the instanced PSO matching the active scene target — must equal the
                // format scenePso resolves to. If that variant is null → fits=false → the
                // per-instance fallback (never bind an LDR PSO to the HDR target).
                const bool useHdrInst = p.usingHDR && p.hdrPso;
                auto* instPso = useHdrInst ? p.hdrPsoInstanced.Get() : p.psoInstanced.Get();
                const bool fits = allowInstancing && instPso && ringPtr && instRingPtr
                                  && (static_cast<UINT64>(instCursor) + count) <= k_maxInstances
                                  && drawIdx < k_maxDraws;
                if (fits)
                {
                    uint8_t* dst = instRingPtr + static_cast<size_t>(instCursor) * k_instStride;
                    for (UINT k = 0; k < count; ++k)
                    {
                        const glm::mat4& t = dc.instanceTransforms[k];
                        const glm::mat4 xf[2] = { viewProj * t, t }; // mvp, model (column-major, as InstXform)
                        std::memcpy(dst + static_cast<size_t>(k) * k_instStride, xf, sizeof(xf));
                    }
                    // One PerObject CB carries the batch-constant colour/pbr (the instanced
                    // VS reads mvp/model from t3, so the CB's mvp/model are unused here).
                    PerObjectCB o{};
                    o.color = glm::vec4(dc.baseColor, textured ? 1.0f : 0.0f);
                    o.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity, 0.0f);
                    std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
                    cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);
                    cl->SetGraphicsRootShaderResourceView(4,
                        instRingBase + static_cast<UINT64>(instCursor) * k_instStride);
                    cl->SetPipelineState(instPso);
                    cl->DrawIndexedInstanced(m.indexCount, count, 0, 0, 0);
                    // Restore the opaque scene PSO (instancing runs in the opaque pass only).
                    cl->SetPipelineState((p.usingHDR && p.hdrPso) ? p.hdrPso.Get() : p.pso.Get());
                    ++p.statDraws; p.statTris += (m.indexCount / 3) * count;
                    ++drawIdx;
                    instCursor += count;
                }
                else
                {
                    for (const glm::mat4& t : dc.instanceTransforms) drawOne(t); // fallback
                }
            }
            else
                drawOne(dc.transform);
        };

        auto* scenePso = p.usingHDR && p.hdrPso ? p.hdrPso.Get() : p.pso.Get();
        auto* transePso = p.usingHDR && p.hdrTransparentPso ? p.hdrTransparentPso.Get() : p.transparentPSO.Get();
        cl->SetPipelineState(scenePso);
        for (const DrawCall* dc : opaqueDCs) drawDC12(*dc);

        // ── Skinned draw calls ────────────────────────────────────────────────
        if (p.m_skinnedPSO && p.skinnedRootSig && !cmds.skinnedDrawCalls().empty())
        {
            auto* activeSkinnedPso = (p.usingHDR && p.m_skinnedHdrPSO)
                                         ? p.m_skinnedHdrPSO.Get()
                                         : p.m_skinnedPSO.Get();

            UINT skinnedIdx = 0;
            const D3D12_GPU_VIRTUAL_ADDRESS bonesBase =
                p.m_bonesCB[p.frameIndex] ? p.m_bonesCB[p.frameIndex]->GetGPUVirtualAddress() : 0;
            uint8_t* bonesPtr = p.m_bonesCBPtr[p.frameIndex];

            // Switch to skinned root sig + PSO, re-bind per-frame CB and scene SRV table.
            cl->SetGraphicsRootSignature(p.skinnedRootSig.Get());
            cl->SetPipelineState(activeSkinnedPso);
            cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());
            if (p.sceneSrvHeap)
                cl->SetGraphicsRootDescriptorTable(2, p.sceneSrvHeap->GetGPUDescriptorHandleForHeapStart());

            for (const SkinnedDrawCall& sdc : cmds.skinnedDrawCalls())
            {
                if (drawIdx >= k_maxDraws) break;
                if (skinnedIdx >= p.k_maxSkinnedDraws) break;

                GpuSkeletalMesh12* skm = p.resolveSkeletalMesh12(sdc.meshAssetId, m_contentManager);
                if (!skm || !skm->indexCount) continue;

                // Base color: MaterialComponent override wins over the baked texture, else baked, else flat.
                p.ensureSkeletalAlbedo(cl, *skm, sdc.meshAssetId, m_contentManager);
                int albedoSlot = skm->albedoSlot; // baked (A1)
                int ovrSlot = -1;
                if (p.resolveMaterialAlbedo(cl, sdc.materialAssetId, m_contentManager, ovrSlot))
                    albedoSlot = ovrSlot; // override fully replaces the baked texture
                const bool textured = albedoSlot >= 0;
                cl->SetGraphicsRootDescriptorTable(4, p.sceneSrvGpu(
                    textured ? static_cast<UINT>(albedoSlot) : p.k_albedoNullSlot));

                // Upload bone matrices (clamped to 128) into the bones ring slot.
                if (bonesPtr && bonesBase)
                {
                    const size_t boneCount = std::min(sdc.boneMatrices.size(), size_t(128));
                    uint8_t* dst = bonesPtr + static_cast<size_t>(skinnedIdx) * p.k_bonesCBSlot;
                    std::memcpy(dst, sdc.boneMatrices.data(), boneCount * sizeof(glm::mat4));
                }

                // Per-object CB (reuses perObjectRing / drawIdx).
                PerObjectCB o{};
                o.mvp   = viewProj * sdc.transform;
                o.model = sdc.transform;
                o.color = glm::vec4(sdc.baseColor, textured ? 1.0f : 0.0f);
                o.pbr   = glm::vec4(sdc.metallic, sdc.roughness, sdc.opacity, 0.0f);
                if (ringPtr)
                    std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
                cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);

                // Bones CB at param index 3 (b2).
                if (bonesBase)
                    cl->SetGraphicsRootConstantBufferView(3, bonesBase + static_cast<UINT64>(skinnedIdx) * p.k_bonesCBSlot);

                // Bind 3 VB slots + IB.
                D3D12_VERTEX_BUFFER_VIEW vbvs[3] = { skm->vbv, skm->boneIdVbv, skm->boneWgtVbv };
                cl->IASetVertexBuffers(0, 3, vbvs);
                cl->IASetIndexBuffer(&skm->ibv);
                cl->DrawIndexedInstanced(skm->indexCount, 1, 0, 0, 0);
                ++p.statDraws; p.statTris += skm->indexCount / 3;

                ++drawIdx;
                ++skinnedIdx;
            }

            // Restore scene root sig + PSO for subsequent transparent/debug draws.
            cl->SetGraphicsRootSignature(p.rootSig.Get());
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());
            if (p.sceneSrvHeap)
                cl->SetGraphicsRootDescriptorTable(2, p.sceneSrvHeap->GetGPUDescriptorHandleForHeapStart());
            cl->SetPipelineState(scenePso);
        }

        if (!transparentDCs.empty() && transePso) {
            allowInstancing = false; // transparent batches keep the per-instance loop (blend + depth sort)
            cl->SetPipelineState(transePso);
            for (const DrawCall* dc : transparentDCs) drawDC12(*dc);
            cl->SetPipelineState(scenePso); // restore for next draw
        }

        // Debug lines on top of geometry but before PostFX tonemap.
        if (!p.m_debugLines.empty()) {
            auto dsv12 = p.viewportDsvHeap ?
                p.viewportDsvHeap->GetCPUDescriptorHandleForHeapStart() :
                p.dsvHandle(0);
            p.drawDebugLines(cl, dsv12, viewProj, p.m_debugLines);
            // Restore scene state for any future draws.
            cl->SetGraphicsRootSignature(p.rootSig.Get());
            cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());
        }
    });
}

void D3D12Renderer::Render()
{
    auto& p = *m_impl;
    p.m_wallTime = static_cast<float>(SDL_GetTicks()) * 0.001f;

    // Free retired viewport RTs once enough frames have passed that the GPU is done
    // with them (countdown set when the RT was retired on resize).
    for (auto it = p.retiredViewportRTs.begin(); it != p.retiredViewportRTs.end(); )
    {
        if (--it->second <= 0) it = p.retiredViewportRTs.erase(it);
        else                   ++it;
    }
    // Same for font-atlas staging buffers recorded into an earlier frame's list.
    for (auto it = p.m_uiAtlasUploads.begin(); it != p.m_uiAtlasUploads.end(); )
    {
        if (--it->second <= 0) it = p.m_uiAtlasUploads.erase(it);
        else                   ++it;
    }
    // Same for per-mesh base-color texture staging buffers.
    for (auto it = p.meshTexUploads.begin(); it != p.meshTexUploads.end(); )
    {
        if (--it->second <= 0) it = p.meshTexUploads.erase(it);
        else                   ++it;
    }
    // GPU resources retired by material/mesh hot-reload — free once past frames in flight.
    for (auto it = p.m_retiredTextures.begin(); it != p.m_retiredTextures.end(); )
    {
        if (--it->second <= 0) it = p.m_retiredTextures.erase(it);
        else                   ++it;
    }
    // Heap slots freed by invalidation become reusable once past frames in flight.
    for (auto it = p.m_freeSlotPending.begin(); it != p.m_freeSlotPending.end(); )
    {
        if (--it->second <= 0) { p.m_freeSlots.push_back(it->first); it = p.m_freeSlotPending.erase(it); }
        else                   ++it;
    }

    // Resize viewport RT if the editor requested a different size.
    if (p.viewportReqW > 0 && p.viewportReqH > 0 &&
        (p.viewportReqW != p.viewportW || p.viewportReqH != p.viewportH))
        p.createViewportRT(p.viewportReqW, p.viewportReqH);

    const bool useViewport = p.viewportRT && p.viewportW > 0 && p.viewportH > 0;

    p.waitForFrame(p.frameIndex);

    // Reap this slot's timestamps from k_frameCount frames ago: the fence wait
    // above guarantees the resolve completed, so the mapped read never stalls.
    if (p.tsPending[p.frameIndex] && p.tsReadbackPtr && p.tsFrequency)
    {
        const uint64_t t0 = p.tsReadbackPtr[2 * p.frameIndex];
        const uint64_t t1 = p.tsReadbackPtr[2 * p.frameIndex + 1];
        p.lastGpuFrameMs = (t1 > t0)
            ? static_cast<double>(t1 - t0) * 1000.0 / static_cast<double>(p.tsFrequency)
            : 0.0;
        p.tsPending[p.frameIndex] = false;
    }

    p.cmdAllocators[p.frameIndex]->Reset();
    p.cmdList->Reset(p.cmdAllocators[p.frameIndex].Get(), nullptr);
    if (p.tsQueryHeap)
        p.cmdList->EndQuery(p.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * p.frameIndex);

    // CPU counters restart each frame; DrawScene fills them back in.
    p.statDraws = p.statTris = p.statVisible = p.statTotal = 0;

    const float bgColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    if (useViewport)
    {
        const bool useHDR = p.postFxReady && p.hdrRT && p.ldrRT;

        if (useHDR)
        {
            // ── Scene → HDR RT (RGBA16F) ─────────────────────────────────────
            // hdrRT is already in RENDER_TARGET state (initial or restored by runPostFX).
            auto hrtv = p.hdrRtvHeap->GetCPUDescriptorHandleForHeapStart();
            auto vdsv = p.viewportDsvHeap->GetCPUDescriptorHandleForHeapStart();
            p.cmdList->OMSetRenderTargets(1, &hrtv, FALSE, &vdsv);
            p.cmdList->ClearRenderTargetView(hrtv, bgColor, 0, nullptr);
            p.cmdList->ClearDepthStencilView(vdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            D3D12_VIEWPORT vvp{ 0, 0, (float)p.viewportW, (float)p.viewportH, 0.0f, 1.0f };
            D3D12_RECT     vsc{ 0, 0, (LONG)p.viewportW, (LONG)p.viewportH };
            p.cmdList->RSSetViewports(1, &vvp);
            p.cmdList->RSSetScissorRects(1, &vsc);
            p.usingHDR = true;
            DrawScene(p.cmdList.Get(), static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));
            p.hdrState = D3D12_RESOURCE_STATE_RENDER_TARGET;

            // PostFX chain: HDR→bloom→tonemap→FXAA→viewportRT (leaves viewportRT in PSR).
            p.runPostFX(p.cmdList.Get(), p.viewportW, p.viewportH);

            // ── 2D UI canvas: draw on top of the final tonemapped image ─────────
            // runPostFX left viewportRT in PSR; transition back to RT for UI draw.
            p.barrier12(p.cmdList.Get(), p.viewportRT.Get(),
                        p.viewportState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            p.viewportState = D3D12_RESOURCE_STATE_RENDER_TARGET;
            {
                auto uirtv = p.viewportRtvHeap->GetCPUDescriptorHandleForHeapStart();
                p.cmdList->OMSetRenderTargets(1, &uirtv, FALSE, nullptr);
                D3D12_VIEWPORT uivp{ 0, 0, (float)p.viewportW, (float)p.viewportH, 0.0f, 1.0f };
                D3D12_RECT     uisc{ 0, 0, (LONG)p.viewportW, (LONG)p.viewportH };
                p.cmdList->RSSetViewports(1, &uivp);
                p.cmdList->RSSetScissorRects(1, &uisc);
            }
            p.renderUIPass12(p.cmdList.Get(), p.frameIndex,
                             static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));
            // Transition back to PSR so ImGui can sample viewportRT.
            p.barrier12(p.cmdList.Get(), p.viewportRT.Get(),
                        p.viewportState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            p.viewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
        else
        {
            // ── Fallback: Scene → viewport RT directly (no PostFX) ───────────
            p.barrier12(p.cmdList.Get(), p.viewportRT.Get(),
                        p.viewportState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            p.viewportState = D3D12_RESOURCE_STATE_RENDER_TARGET;

            auto vrtv = p.viewportRtvHeap->GetCPUDescriptorHandleForHeapStart();
            auto vdsv = p.viewportDsvHeap->GetCPUDescriptorHandleForHeapStart();
            p.cmdList->OMSetRenderTargets(1, &vrtv, FALSE, &vdsv);
            p.cmdList->ClearRenderTargetView(vrtv, bgColor, 0, nullptr);
            p.cmdList->ClearDepthStencilView(vdsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

            D3D12_VIEWPORT vvp{ 0, 0, (float)p.viewportW, (float)p.viewportH, 0.0f, 1.0f };
            D3D12_RECT     vsc{ 0, 0, (LONG)p.viewportW, (LONG)p.viewportH };
            p.cmdList->RSSetViewports(1, &vvp);
            p.cmdList->RSSetScissorRects(1, &vsc);
            p.usingHDR = false;
            DrawScene(p.cmdList.Get(), static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));

            // ── 2D UI canvas on the viewport RT (still in RENDER_TARGET state) ─
            {
                auto uirtv = p.viewportRtvHeap->GetCPUDescriptorHandleForHeapStart();
                p.cmdList->OMSetRenderTargets(1, &uirtv, FALSE, nullptr);
                D3D12_VIEWPORT uivp{ 0, 0, (float)p.viewportW, (float)p.viewportH, 0.0f, 1.0f };
                D3D12_RECT     uisc{ 0, 0, (LONG)p.viewportW, (LONG)p.viewportH };
                p.cmdList->RSSetViewports(1, &uivp);
                p.cmdList->RSSetScissorRects(1, &uisc);
            }
            p.renderUIPass12(p.cmdList.Get(), p.frameIndex,
                             static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));

            p.barrier12(p.cmdList.Get(), p.viewportRT.Get(),
                        p.viewportState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            p.viewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        }
    }

    // ── Swapchain → transition to RTV, clear, run ImGui overlay ────────────
    D3D12_RESOURCE_BARRIER swapBarrier{};
    swapBarrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    swapBarrier.Transition.pResource   = p.renderTargets[p.frameIndex].Get();
    swapBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    swapBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    swapBarrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &swapBarrier);

    auto rtv = p.rtvHandle(p.frameIndex);
    auto dsv = p.dsvHeap ? p.dsvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    p.cmdList->OMSetRenderTargets(1, &rtv, FALSE, p.dsvHeap ? &dsv : nullptr);
    p.cmdList->ClearRenderTargetView(rtv, bgColor, 0, nullptr);
    if (p.dsvHeap)
        p.cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    if (!useViewport)
    {
        D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0.0f, 1.0f };
        D3D12_RECT     sc{ 0, 0, p.width, p.height };
        p.cmdList->RSSetViewports(1, &vp);
        p.cmdList->RSSetScissorRects(1, &sc);
        p.usingHDR = false;
        DrawScene(p.cmdList.Get(), p.width, p.height);

        // ── 2D UI canvas on swapchain RT (already bound, in RENDER_TARGET state) ─
        p.cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
        {
            D3D12_VIEWPORT uivp{ 0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0.0f, 1.0f };
            D3D12_RECT     uisc{ 0, 0, p.width, p.height };
            p.cmdList->RSSetViewports(1, &uivp);
            p.cmdList->RSSetScissorRects(1, &uisc);
        }
        p.renderUIPass12(p.cmdList.Get(), p.frameIndex, p.width, p.height);
    }

    // Overlay (ImGui) records into this command list and binds its own SRV heap.
    if (m_overlayCallback) m_overlayCallback(p.cmdList.Get());

    swapBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    swapBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    p.cmdList->ResourceBarrier(1, &swapBarrier);
    if (p.tsQueryHeap && p.tsReadback)
    {
        p.cmdList->EndQuery(p.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * p.frameIndex + 1);
        p.cmdList->ResolveQueryData(p.tsQueryHeap.Get(), D3D12_QUERY_TYPE_TIMESTAMP,
            2 * p.frameIndex, 2, p.tsReadback.Get(),
            static_cast<UINT64>(2 * p.frameIndex) * sizeof(uint64_t));
        p.tsPending[p.frameIndex] = true;
    }
    p.cmdList->Close();

    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    // PRESENT_ALLOW_TEARING is only valid on a swapchain created with the matching
    // flag AND when unsynced (sync interval 0). Gate it on the queried capability.
    HRESULT prHr;
    if (p.vsync)
        prHr = p.swapchain->Present(1, 0);
    else
        prHr = p.swapchain->Present(0, p.allowTearing ? DXGI_PRESENT_ALLOW_TEARING : 0);

    // A failed Present usually means the device was removed/reset. Log the reason +
    // a DRED breadcrumb dump (which op hung) ONCE — a permanently-removed device fails
    // Present every frame, which would otherwise flood the log with thousands of lines.
    const bool firstRemoval = FAILED(prHr) && !p.deviceRemovedLogged;
    if (firstRemoval)
    {
        p.deviceRemovedLogged = true;
        HRESULT removed = p.device->GetDeviceRemovedReason();
        char msg[256];
        std::snprintf(msg, sizeof(msg),
            "D3D12Renderer: Present failed hr=0x%08X, DeviceRemovedReason=0x%08X "
            "(0x887A0006=DEVICE_HUNG) — dumping DRED:",
            static_cast<unsigned>(prHr), static_cast<unsigned>(removed));
        Logger::Log(Logger::LogLevel::Error, msg);
        logD3D12DredOutput(p.device.Get());
    }

    // Drain validation messages every healthy frame, and once on the first device
    // removal (the message explaining the hang is often the last thing queued).
    if (p.gpuDebug && p.infoQueue && (!FAILED(prHr) || firstRemoval))
    {
        const UINT64 n = p.infoQueue->GetNumStoredMessages();
        for (UINT64 i = 0; i < n; ++i)
        {
            SIZE_T len = 0;
            if (FAILED(p.infoQueue->GetMessage(i, nullptr, &len)) || len == 0) continue;
            std::vector<char> buf(len);
            auto* dm = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(p.infoQueue->GetMessage(i, dm, &len)) && dm->pDescription)
            {
                const bool err = dm->Severity == D3D12_MESSAGE_SEVERITY_ERROR ||
                                 dm->Severity == D3D12_MESSAGE_SEVERITY_CORRUPTION;
                const std::string line = std::string("D3D12 debug layer: ") + dm->pDescription;
                Logger::Log(err ? Logger::LogLevel::Error : Logger::LogLevel::Warning,
                            line.c_str());
            }
        }
        p.infoQueue->ClearStoredMessages();
    }

    // Signal a UNIQUE monotonic value and remember it for THIS slot. Per-slot counters
    // would collide on the shared fence (both reach 1,2,3…), letting waitForFrame return
    // when a DIFFERENT slot's signal completed → the allocator gets reset mid-execution.
    const UINT64 signalValue = ++p.fenceValue;
    p.cmdQueue->Signal(p.fence.Get(), signalValue);
    p.fenceValues[p.frameIndex] = signalValue;
    p.frameIndex = p.swapchain->GetCurrentBackBufferIndex();
}

IRenderer::Capabilities D3D12Renderer::GetCapabilities() const { return { true, m_impl->postFxReady, false }; }

void* D3D12Renderer::GetDevice()       const { return m_impl->device.Get(); }
void* D3D12Renderer::GetCommandQueue() const { return m_impl->cmdQueue.Get(); }

void D3D12Renderer::SetVSync(bool enabled)
{
    Logger::Log(Logger::LogLevel::Info, enabled ? "D3D12Renderer: VSync enabled" : "D3D12Renderer: VSync disabled");
    m_impl->vsync = enabled;
}

void D3D12Renderer::SetViewportSize(uint32_t width, uint32_t height)
{
    m_impl->viewportReqW = static_cast<UINT>(width);
    m_impl->viewportReqH = static_cast<UINT>(height);
}

bool D3D12Renderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH)
{
    auto& p = *m_impl;
    if (!p.viewportRT || !p.viewportReadback || p.viewportW == 0 || p.viewportH == 0)
        return false;

    // Execute a one-shot command list to copy the viewport RT to the readback buffer.
    p.waitForAllFrames();
    p.cmdAllocators[0]->Reset();
    p.cmdList->Reset(p.cmdAllocators[0].Get(), nullptr);

    // Transition to COPY_SOURCE
    D3D12_RESOURCE_BARRIER toSrc{};
    toSrc.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    toSrc.Transition.pResource   = p.viewportRT.Get();
    toSrc.Transition.StateBefore = p.viewportState;
    toSrc.Transition.StateAfter  = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toSrc.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &toSrc);

    UINT rowPitch = alignUp(p.viewportW * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource        = p.viewportRT.Get();
    src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource                          = p.viewportReadback.Get();
    dst.Type                               = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset             = 0;
    dst.PlacedFootprint.Footprint.Format   = DXGI_FORMAT_R8G8B8A8_UNORM;
    dst.PlacedFootprint.Footprint.Width    = p.viewportW;
    dst.PlacedFootprint.Footprint.Height   = p.viewportH;
    dst.PlacedFootprint.Footprint.Depth    = 1;
    dst.PlacedFootprint.Footprint.RowPitch = rowPitch;
    p.cmdList->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    D3D12_RESOURCE_BARRIER toShader = toSrc;
    toShader.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    toShader.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    p.cmdList->ResourceBarrier(1, &toShader);
    p.viewportState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

    p.cmdList->Close();
    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    p.waitForAllFrames();

    void* mapped = nullptr;
    D3D12_RANGE readRange{ 0, static_cast<SIZE_T>(rowPitch) * p.viewportH };
    if (FAILED(p.viewportReadback->Map(0, &readRange, &mapped))) return false;

    outW = p.viewportW;
    outH = p.viewportH;
    rgba.resize(static_cast<size_t>(outW) * outH * 4);
    const uint8_t* src2 = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < outH; ++y)
        std::memcpy(rgba.data() + y * outW * 4, src2 + y * rowPitch, outW * 4);

    D3D12_RANGE noWrite{ 0, 0 };
    p.viewportReadback->Unmap(0, &noWrite);
    return true;
}

void* D3D12Renderer::GetViewportD3DResource()    const { return m_impl->viewportRT.Get(); }
bool  D3D12Renderer::HasViewportResourceChanged() const { return m_impl->viewportResChanged; }
void  D3D12Renderer::ClearViewportResourceChanged()     { m_impl->viewportResChanged = false; }

void D3D12Renderer::SetDebugLines(const std::vector<DebugLine>& lines)
{
    m_impl->m_debugLines = lines;
}

void D3D12Renderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
    auto& p = *m_impl;
    p.moonTex12.Reset();
    if (!rgba8Pixels || width <= 0 || height <= 0 || !p.device || !p.skyEnvHeap) return;

    // Create an upload + default heap texture, copy via upload buffer.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width  = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&p.moonTex12))))
        return;

    const UINT rowPitch = alignUp(static_cast<UINT>(width) * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;
    D3D12_HEAP_PROPERTIES uhp{}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ubd{};
    ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubd.Width = uploadSize; ubd.Height = 1; ubd.DepthOrArraySize = 1;
    ubd.MipLevels = 1; ubd.Format = DXGI_FORMAT_UNKNOWN;
    ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> uploadBuf;
    if (FAILED(p.device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ubd,
               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf))))
        return;

    void* mapped = nullptr; D3D12_RANGE noRead{0,0};
    uploadBuf->Map(0, &noRead, &mapped);
    if (mapped) {
        const uint8_t* src = static_cast<const uint8_t*>(rgba8Pixels);
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + y * rowPitch,
                        src + y * width * 4, static_cast<size_t>(width) * 4);
        uploadBuf->Unmap(0, nullptr);
    }

    p.waitForAllFrames();
    p.cmdAllocators[0]->Reset();
    p.cmdList->Reset(p.cmdAllocators[0].Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION src2{};
    src2.pResource = uploadBuf.Get(); src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src2.PlacedFootprint.Offset = 0;
    src2.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<UINT>(width), static_cast<UINT>(height), 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION dst2{};
    dst2.pResource = p.moonTex12.Get(); dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst2.SubresourceIndex = 0;
    p.cmdList->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = p.moonTex12.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &b);
    p.cmdList->Close();
    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    p.waitForAllFrames();

    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    p.device->CreateShaderResourceView(p.moonTex12.Get(), &sv,
        p.skyEnvHeap->GetCPUDescriptorHandleForHeapStart());
}

// ─────────────────────────────────────────────────────────────────────────────
// ImGui editor textures (content-browser icons + logo)
// ─────────────────────────────────────────────────────────────────────────────
// Mirrors SetMoonTexture's upload path (DEFAULT-heap R8G8B8A8_UNORM texture in
// COPY_DEST, UPLOAD-heap staging buffer, CopyTextureRegion, transition to
// PIXEL_SHADER_RESOURCE) but does NOT create the SRV — the editor's registrar
// (which links ImGui) allocates a slot in ImGui's descriptor heap and creates the
// SRV there. The ID3D12Resource is retained in m_imguiTextures so it outlives the
// SRV ImGui samples from.
void* D3D12Renderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
    auto& p = *m_impl;
    if (!rgba8Pixels || width <= 0 || height <= 0 || !p.device) return nullptr;

    // Create an upload + default heap texture, copy via upload buffer.
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width  = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_NONE;
    ComPtr<ID3D12Resource> tex;
    if (FAILED(p.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
               D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex))))
        return nullptr;

    const UINT rowPitch = alignUp(static_cast<UINT>(width) * 4u, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const UINT64 uploadSize = static_cast<UINT64>(rowPitch) * height;
    D3D12_HEAP_PROPERTIES uhp{}; uhp.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC ubd{};
    ubd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    ubd.Width = uploadSize; ubd.Height = 1; ubd.DepthOrArraySize = 1;
    ubd.MipLevels = 1; ubd.Format = DXGI_FORMAT_UNKNOWN;
    ubd.SampleDesc.Count = 1; ubd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> uploadBuf;
    if (FAILED(p.device->CreateCommittedResource(&uhp, D3D12_HEAP_FLAG_NONE, &ubd,
               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadBuf))))
        return nullptr;

    void* mapped = nullptr; D3D12_RANGE noRead{0,0};
    uploadBuf->Map(0, &noRead, &mapped);
    if (mapped) {
        const uint8_t* src = static_cast<const uint8_t*>(rgba8Pixels);
        for (int y = 0; y < height; ++y)
            std::memcpy(static_cast<uint8_t*>(mapped) + y * rowPitch,
                        src + y * width * 4, static_cast<size_t>(width) * 4);
        uploadBuf->Unmap(0, nullptr);
    }

    p.waitForAllFrames();
    p.cmdAllocators[0]->Reset();
    p.cmdList->Reset(p.cmdAllocators[0].Get(), nullptr);

    D3D12_TEXTURE_COPY_LOCATION src2{};
    src2.pResource = uploadBuf.Get(); src2.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src2.PlacedFootprint.Offset = 0;
    src2.PlacedFootprint.Footprint = { DXGI_FORMAT_R8G8B8A8_UNORM,
        static_cast<UINT>(width), static_cast<UINT>(height), 1, rowPitch };
    D3D12_TEXTURE_COPY_LOCATION dst2{};
    dst2.pResource = tex.Get(); dst2.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dst2.SubresourceIndex = 0;
    p.cmdList->CopyTextureRegion(&dst2, 0, 0, 0, &src2, nullptr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource   = tex.Get();
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &b);
    p.cmdList->Close();
    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    p.waitForAllFrames();

    // Retain the resource so it outlives ImGui's SRV; the editor builds the SRV.
    p.m_imguiTextures.push_back(tex);
    return m_imguiTexRegistrar ? m_imguiTexRegistrar(tex.Get(), nullptr) : nullptr;
}

void D3D12Renderer::DestroyImGuiTexture(void* /*handle*/)
{
    // No-op: the editor textures live until renderer shutdown, at which point the
    // ComPtrs in m_impl->m_imguiTextures release the GPU resources. The ImGui SRV
    // descriptor is owned by the editor's heap allocator and freed on ImGui
    // shutdown, so there is nothing to free here.
}

void D3D12Renderer::SetSSAOSettings(const SSAOSettings& s)
{
    m_impl->ssaoEnabled   = s.enabled;
    m_impl->ssaoRadius    = s.radius;
    m_impl->ssaoBias      = 0.025f;   // no bias field in SSAOSettings; keep default
    m_impl->ssaoIntensity = s.intensity;
    m_impl->ssaoMethod    = s.method;
}

void D3D12Renderer::SetBloomSettings(const BloomSettings& s)
{
    // Same field mapping as OpenGL: intensity drives the tonemap add-back strength.
    // bloomKnee stays at its default — the settings struct has no knee field.
    m_impl->bloomEnabled   = s.enabled;
    m_impl->bloomThreshold = s.threshold;
    m_impl->bloomStrength  = s.intensity;
}

void D3D12Renderer::InvalidateMaterial(const HE::UUID& materialId)
{
    // Deferred to the next DrawScene (render thread), where touching the heap is safe.
    if (m_impl && materialId != HE::UUID{})
        m_impl->m_pendingMatInval.push_back(materialId);
}

void D3D12Renderer::InvalidateMesh(const HE::UUID& meshId)
{
    if (m_impl && meshId != HE::UUID{})
        m_impl->m_pendingMeshInval.push_back(meshId);
}

IRenderer::FrameGpuStats D3D12Renderer::GetFrameGpuStats() const
{
    // GPU time comes from the newest reaped timestamp pair (k_frameCount frames
    // late; -1 before the first reap / when timestamps are unavailable). CPU
    // counters are this frame's — mirrors OpenGL's merge in GetFrameGpuStats.
    FrameGpuStats s;
    s.gpuFrameMs     = m_impl->lastGpuFrameMs;
    s.gpuTimingMode  = m_impl->lastGpuFrameMs >= 0.0 ? "whole-frame" : "";
    s.drawCalls      = m_impl->statDraws;
    s.triangles      = m_impl->statTris;
    s.visibleObjects = m_impl->statVisible;
    s.totalObjects   = m_impl->statTotal;
    return s;
}
