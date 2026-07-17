#include "Backends/D3D11/D3D11Renderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/RenderWorld.h>
#include <Renderer/UIRenderObject.h>
#include <Renderer/UIFont.h>
#include <Diagnostics/EngineProfiler.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <HorizonRendering/GiBvh.h>          // GI: CPU BLAS (shared with GL/Vulkan/Metal-SW)
#include <ContentManager/DefaultAssets.h>    // GI: default-cube occluder fallback
#include <material/MaterialShaderLibrary.h> // A4: shared cross-backend material shader layer (unguarded, like Vulkan/D3D12)
#include <SDL3/SDL.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <functional>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;

// [blind] added D3D11 sky+IBL+debuglines parity

// ─── Sky 3D noise volume bake ───────────────────────────────────────────────
// CPU-baked RG16 volume the sky's starFbm3 (.r value noise) and worleyFbm
// (.g cellular) sample for the volumetric clouds. Mirrors OpenGLRenderer's
// BuildSkyNoise3D exactly — identical math — but serial nested loops instead of
// std::execution::par_unseq (one-time init; avoids <execution>/<numeric>).
// Tightly packed: index ((z*n+y)*n+x)*2 into the uint16_t buffer.
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

// ─── Shared sky colour function ─────────────────────────────────────────────
// Mirrors kSkyFuncGLSL in OpenGLRenderer.cpp exactly (GLSL→HLSL: lerp/frac/float3).
static const char* kSkyFuncHLSL = R"HLSL(
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

// ─── Sky background pass HLSL ───────────────────────────────────────────────
// VSSky: fullscreen triangle at D3D far plane (z=1 so geometry draws over it).
static const char* kSkyVSHLSL = R"HLSL(
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

// PSSky: reconstruct world ray from inv(viewProj), evaluate sky + effects.
// Prepend kSkyFuncHLSL when compiling so skyColor() is in scope.
static const char* kSkyPSHLSL = R"HLSL(
cbuffer SkyEnv : register(b0)
{
    float4x4 uInvViewProj;
    float3   uSunDir;       float  uTimeOfDay;
    float3   uSunColor;     float  uCloudCoverage;
    float3   uWind;         float  uTime;
    float3   uAuroraColor;  float  uAurora;
    float    uMilkyWay;     float  uFlash; int uHasMoonTex; float _skyPad;
};
Texture2D    uMoonTex   : register(t0);
SamplerState uSkyLinear : register(s0);
Texture3D    uNoise      : register(t1);
SamplerState uSkyWrap    : register(s1);

// ── Hash / noise (pure math) ─────────────────────────────────────────────────
float starHash(float3 p)
{
    p = frac(p * 0.1031f); p += dot(p, p.zyx + 31.32f);
    return frac((p.x + p.y) * p.z);
}
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
float cloudHash(float2 p)
{
    p=frac(p*float2(127.1f,311.7f)); p+=dot(p,p+34.56f); return frac(p.x*p.y);
}
float cloudNoise(float2 p)
{
    float2 i=floor(p),f=frac(p),u=f*f*(3.0f-2.0f*f);
    return lerp(lerp(cloudHash(i),cloudHash(i+float2(1,0)),u.x),
                lerp(cloudHash(i+float2(0,1)),cloudHash(i+float2(1,1)),u.x),u.y);
}
float cloudFbm(float2 p)
{
    float v=0.0f,a=0.5f;
    for(int i=0;i<5;++i){v+=a*cloudNoise(p);p*=2.02f;a*=0.5f;}
    return v;
}

// ── Celestial rotation ────────────────────────────────────────────────────────
float3 celestialDir(float3 dir, float tod)
{
    float a=tod*6.2831853f;
    float3 axis=normalize(float3(0.22f,0.92f,0.32f));
    float c=cos(a),s=sin(a);
    return dir*c+cross(axis,dir)*s+axis*dot(axis,dir)*(1.0f-c);
}
float galacticBand(float3 cd)
{
    float3 gN=normalize(float3(0.46f,0.52f,-0.72f));
    float d=dot(normalize(cd),gN); return exp(-d*d*7.0f);
}

// ── Star field ────────────────────────────────────────────────────────────────
float3 starField(float3 dir, float3 cdir, float3 sunDir, float t, float mw)
{
    float night=1.0f-smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f));
    if(night<=0.0f||dir.y<=0.0f) return (float3)0;
    float band=galacticBand(cdir), mwc=clamp(mw,0.0f,1.0f);
    float thresh=lerp(0.92f,lerp(0.86f,0.72f,mwc),band);
    float3 p=cdir*70.0f, cell=floor(p);
    float present=starHash(cell);
    if(present<thresh) return (float3)0;
    float3 sp=float3(starHash(cell+1.7f),starHash(cell+4.3f),starHash(cell+8.9f));
    float d=length(frac(p)-sp);
    float sizeH=starHash(cell+5.7f), big=sizeH*sizeH*sizeH;
    float radius=lerp(0.05f,0.17f,big);
    float core=smoothstep(radius,0.0f,d); core*=core;
    float halo=smoothstep(radius*3.0f,radius,d)*(big*big)*0.35f;
    float shape=core+halo;
    float mag=(0.4f+0.6f*smoothstep(thresh,1.0f,present))*lerp(0.7f,2.7f,big);
    float twPhase=starHash(cell+23.5f)*6.2831f, twFreq=2.0f+4.0f*starHash(cell+47.1f);
    float tw=0.7f+0.3f*sin(t*twFreq+twPhase);
    float horizon=smoothstep(0.0f,0.15f,dir.y);
    float3 tint=lerp(float3(0.80f,0.88f,1.0f),float3(1.0f,0.93f,0.82f),starHash(cell+12.1f));
    float bandDim=lerp(1.6f,lerp(0.9f,1.5f,mwc),band);
    return tint*(shape*mag*tw*horizon*night*bandDim);
}

// ── Aurora ────────────────────────────────────────────────────────────────────
float3 aurora(float3 dir, float3 sunDir, float t, float intensity, float3 auroraCol)
{
    if(intensity<=0.0f) return (float3)0;
    float night=1.0f-smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f));
    if(night<=0.0f||dir.y<=0.04f) return (float3)0;
    float2 P=dir.xz/(dir.y+0.45f);
    float along=P.x, across=P.y;
    float wave=0.40f*sin(along*0.7f+t*0.15f)+0.30f*cloudFbm(float2(along*0.35f-t*0.04f,3.0f));
    float phase=across*0.30f+wave;
    float f=abs(frac(phase)-0.5f);
    float ribbon=smoothstep(0.10f,0.45f,f);
    float stri=cloudFbm(float2(along*6.0f+t*0.25f,across*1.2f));
    float curtain=ribbon*(0.45f+0.55f*smoothstep(0.30f,0.80f,stri));
    float patches=0.65f+0.35f*smoothstep(0.25f,0.85f,cloudFbm(float2(along*0.45f+t*0.03f,across*0.4f+9.0f)));
    float hcol=smoothstep(0.05f,0.60f,dir.y);
    float3 bCol=auroraCol*float3(0.60f,0.15f,0.90f), tCol=auroraCol*float3(0.30f,0.90f,0.70f);
    float3 col=lerp(lerp(bCol,auroraCol,smoothstep(0.0f,0.5f,hcol)),tCol,smoothstep(0.5f,1.0f,hcol));
    float fade=smoothstep(0.03f,0.16f,dir.y)*(1.0f-smoothstep(0.78f,1.0f,dir.y));
    return col*(curtain*patches*fade*intensity*night*5.0f);
}

// ── Moon disk ─────────────────────────────────────────────────────────────────
float3 moonDisk(float3 dir, float3 sunDir)
{
    float day=smoothstep(-0.10f,0.10f,clamp(sunDir.y,-0.2f,1.0f)), night=1.0f-day;
    if(night<=0.0f) return (float3)0;
    float3 moonDir2=normalize(float3(-sunDir.x,-sunDir.y,sunDir.z));
    if(dot(dir,moonDir2)<=0.0f) return (float3)0;
    float3 right=normalize(cross(float3(0,1,0),moonDir2)), up=cross(moonDir2,right);
    const float kR=0.030f;
    float2 q=float2(dot(dir,right),dot(dir,up))/kR;
    float r=length(q); if(r>1.0f) return (float3)0;
    float tex=uHasMoonTex?uMoonTex.Sample(uSkyLinear,q*0.5f+0.5f).r:1.0f;
    float limb=sqrt(max(1.0f-r*r,0.0f)), edge=smoothstep(1.0f,0.90f,r);
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

struct SkyVSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
float4 PSSky(SkyVSOut i) : SV_TARGET
{
    // Reconstruct world-space ray. D3D NDC z in [0,1]: 0=near, 1=far.
    float4 wp1=mul(uInvViewProj,float4(i.ndc,1.0f,1.0f)); // far
    float4 wp0=mul(uInvViewProj,float4(i.ndc,0.0f,1.0f)); // near
    // Normalize: applyClouds/starField/aurora/moonDisk all assume unit-length dir.
    // Without this, ds is scaled by the far-plane distance making cloud opacity ~0.
    float3 dir=normalize(wp1.xyz/wp1.w - wp0.xyz/wp0.w);
    float3 col=skyColor(dir,uSunDir);
    float nightF=1.0f-smoothstep(-0.10f,0.10f,clamp(normalize(uSunDir).y,-0.2f,1.0f));
    if(nightF>0.0f)
    {
        float3 cdir=celestialDir(dir,uTimeOfDay);
        col+=starField(dir,cdir,uSunDir,uTime,uMilkyWay);
        col+=aurora(dir,uSunDir,uTime,uAurora,uAuroraColor);
        col+=moonDisk(dir,uSunDir);
    }
    col=applyClouds(col,dir,uSunDir,uTime,uCloudCoverage,uSunColor,uWind);
    col+=uFlash*float3(0.85f,0.90f,1.0f);
    return float4(col,1.0f);
}
)HLSL";

// ─── Debug line pass HLSL ───────────────────────────────────────────────────
static const char* kDebugLineHLSL = R"HLSL(
cbuffer DebugCB : register(b0) { float4x4 uVP; };
struct LineIn  { float3 pos : POSITION; float3 color : COLOR0; };
struct LineOut { float4 clip : SV_POSITION; float3 color : COLOR0; };
LineOut VSLine(LineIn i)
{
    LineOut o; o.clip=mul(uVP,float4(i.pos,1.0f)); o.color=i.color; return o;
}
float4 PSLine(LineOut i) : SV_TARGET { return float4(i.color,1.0f); }
)HLSL";

// ─── Embedded HLSL ──────────────────────────────────────────────────────────
// Same unlit Blinn-Phong as the GL/Metal backends. Matrices come straight from
// glm (column-major); HLSL's default cbuffer matrix packing is column_major, so
// mul(M, v) reproduces the GLSL `uMVP * vec4(pos,1)` without transposing.
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
    float4   uCameraPos;        // xyz
    int4     uLightCount;       // x = count
    float4   uLightPos[8];      // xyz pos,  w type (0 dir / 1 point / 2 spot)
    float4   uLightDir[8];      // xyz dir,  w cos(spot half angle)
    float4   uLightColor[8];    // rgb,      w intensity
    float4   uLightParams[8];   // x range
    float4x4 uLightVP;          // directional-light view-proj (D3D clip)
    int4     uShadowEnabled;    // x = 0/1
    float4   uSunDir;           // xyz = sun direction toward sky, w unused
    float4   uFog;              // x = fogDensity, y = fogHeightFalloff
    float4   uViewport;        // x=width, y=height, z=ssaoEnabled (0/1)
    float4   uGIParams;        // x = GI enabled (0/1), y = indirect intensity
    float4   uGIGridOrigin;    // xyz = probe grid origin, w = spacing
    float4   uGIGridCounts;    // xyz = probe counts, w = probesPerRow
};

Texture2D    uTexture   : register(t0);
Texture2D    uShadowMap : register(t1);
Texture2D    uAO        : register(t2);
Texture2D    uGIShadow  : register(t4); // half-res ray-traced sun-shadow mask
Texture2D    uGIIrr     : register(t5); // DDGI irradiance atlas (RGBA16F)
Texture2D    uGIVis     : register(t6); // DDGI visibility atlas (RG16F)
Texture2D    uGILocal   : register(t7); // half-res local-light visibility mask (1 channel per light, first 4)
SamplerState uSampler   : register(s0);
SamplerState uAOSampler : register(s1);
SamplerState uGISampler : register(s2); // linear clamp (mask upsample + atlases)

// Signed-octahedral mapping (direction → texel UV) — must match the probe
// kernel's octDecode and the GL/Metal implementations byte-for-byte.
float2 giOctEncode(float3 n)
{
    float2 p = n.xy * (1.0 / (abs(n.x) + abs(n.y) + abs(n.z)));
    float2 signP = float2(p.x >= 0.0 ? 1.0 : -1.0, p.y >= 0.0 ? 1.0 : -1.0);
    return (n.z <= 0.0) ? ((1.0 - abs(p.yx)) * signP) : p;
}

static const int GI_PROBE_OCT = 8; // must match the host's kGiProbeOctSize

// The ray-traced masks are binary 0/1 (one shadow ray, hit or miss), so a fully
// occluded surface loses its direct term outright. Let a sliver through to stand in for
// the light the single ray can't carry — bounced and area-light spill. Much lower than
// shadowFactor()'s 0.35: that floor propped up the sky-ambient path, whereas here probe
// indirect already lights the shadow, and a high floor would flatten the ray-traced
// shadows this pass exists to produce.
static const float GI_SHADOW_FLOOR = 0.1f;

// DDGI probe sampling — trilinear over the 8 surrounding probes × soft
// backface × Chebyshev visibility. Direct port of the GL/Metal version.
// `coverage` reports how much of the trilinear footprint landed on real probes: 1 well
// inside the grid, falling to 0 as P leaves it (and 0 outright before the grid is built).
// The probe volume only spans kGiMaxProbesPerAxis × spacing, so a terrain reaches far
// past it; the caller cross-fades back to sky ambient on that signal. Deliberately NOT
// sumWeight — that folds in backface + Chebyshev, so a probe-lit surface legitimately in
// shadow would read as "uncovered" and get sky ambient painted over its indirect shadow.
float3 sampleDDGIIrradiance(float3 P, float3 N, out float coverage)
{
    coverage = 0.0;
    int gx = int(uGIGridCounts.x), gy = int(uGIGridCounts.y), gz = int(uGIGridCounts.z);
    if (gx <= 0 || gy <= 0 || gz <= 0) return float3(0, 0, 0);
    int probesPerRow = max(1, int(uGIGridCounts.w));
    int probeRows    = int(ceil(float(gx * gy * gz) / float(probesPerRow)));
    float2 atlasSizeTexels = float2(probesPerRow, probeRows) * float(GI_PROBE_OCT);
    float spacing = max(uGIGridOrigin.w, 1e-4);

    float3 gridSpace = (P - uGIGridOrigin.xyz) / spacing;
    float3 base      = floor(gridSpace);
    float3 fracP     = gridSpace - base;

    float3 sumColor  = float3(0, 0, 0);
    float  sumWeight = 0.0;
    for (int i = 0; i < 8; ++i)
    {
        float3 offs = float3(i & 1, (i >> 1) & 1, (i >> 2) & 1);
        float3 cell = base + offs;
        if (any(cell < 0.0) || cell.x >= float(gx) || cell.y >= float(gy) || cell.z >= float(gz))
            continue;
        int probeIndex = int(cell.x) + int(cell.y) * gx + int(cell.z) * gx * gy;

        float3 trilinear = lerp(1.0 - fracP, fracP, offs);
        float weight = trilinear.x * trilinear.y * trilinear.z;
        coverage += weight; // in-bounds share of the footprint, before any shadowing terms
        if (weight <= 1e-5) continue;

        float3 probePos   = uGIGridOrigin.xyz + cell * spacing;
        float3 toProbe    = probePos - P;
        float  dist       = max(length(toProbe), 1e-4);
        float3 dirToProbe = toProbe / dist;

        weight *= max(0.05, dot(N, dirToProbe) * 0.5 + 0.5);

        float2 tileOrigin = float2(probeIndex % probesPerRow,
                                   probeIndex / probesPerRow) * float(GI_PROBE_OCT);

        float2 visUV = (tileOrigin + (giOctEncode(-dirToProbe) * 0.5 + 0.5) * float(GI_PROBE_OCT)) / atlasSizeTexels;
        float2 visSample = uGIVis.SampleLevel(uGISampler, visUV, 0).rg;
        float mean = visSample.x, mean2 = visSample.y;
        float variance = abs(mean2 - mean * mean);
        float chebyshev = 1.0;
        if (dist > mean)
        {
            float dd = dist - mean;
            chebyshev = variance / (variance + dd * dd);
            chebyshev = chebyshev * chebyshev * chebyshev;
        }
        weight *= max(chebyshev, 0.05);

        float2 irrUV = (tileOrigin + (giOctEncode(N) * 0.5 + 0.5) * float(GI_PROBE_OCT)) / atlasSizeTexels;
        sumColor  += uGIIrr.SampleLevel(uGISampler, irrUV, 0).rgb * weight;
        sumWeight += weight;
    }
    coverage = saturate(coverage);
    return sumColor / max(sumWeight, 1e-4);
}

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
// SV_InstanceID, filled by the CPU exactly like uploadObject (same column-major glm
// bytes as the PerObject cbuffer → identical mul() math). uColor/uPBR stay in the
// shared PerObject cbuffer (batch-constant). Reuses PSMain.
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
float4 VSDepth(VSIn i) : SV_POSITION
{
    return mul(uMVP, float4(i.pos, 1.0));
}

float shadowFactor(float3 worldPos, float3 N, float3 L)
{
    if (uShadowEnabled.x == 0) return 1.0;
    float4 lp = mul(uLightVP, float4(worldPos, 1.0));
    float3 p  = lp.xyz / lp.w;                       // z already [0,1] (D3D clip)
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); // top-left origin
    if (p.z > 1.0 || any(uv < 0.0) || any(uv > 1.0)) return 1.0;
    float bias    = max(0.0015 * (1.0 - dot(N, L)), 0.0004);
    float closest = uShadowMap.Sample(uSampler, uv).r;
    return (p.z - bias > closest) ? 0.35 : 1.0;
}

// Cook-Torrance PBR helpers.
static const float PI11 = 3.14159265;
float D_GGX(float NdH, float a2) { float d = NdH*NdH*(a2-1.0)+1.0; return a2/(PI11*d*d+1e-6); }
float G_Schlick(float NdX, float k) { return NdX/(NdX*(1.0-k)+k); }
float3 F_Schlick(float VdH, float3 F0) { return F0+(1.0-F0)*pow(1.0-VdH, 5.0); }
float3 BRDF(float3 L, float3 V, float3 N, float3 base, float metallic, float roughness)
{
    float a   = roughness*roughness;
    float a2  = a*a;
    float k   = (roughness+1.0); k = k*k/8.0;
    float3 H  = normalize(L+V);
    float NdL = max(dot(N,L),0.0);
    float NdV = max(dot(N,V),0.0001);
    float NdH = max(dot(N,H),0.0);
    float VdH = max(dot(V,H),0.0);
    float3 F0 = lerp(float3(0.04,0.04,0.04), base, metallic);
    float3 F  = F_Schlick(VdH, F0);
    float  D  = D_GGX(NdH, a2);
    float  G  = G_Schlick(NdV,k)*G_Schlick(NdL,k);
    float3 spec = D*F*G / max(4.0*NdV*NdL, 1e-6);
    float3 kd = (1.0-F)*(1.0-metallic);
    return (kd*base/PI11 + spec)*NdL;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 base = (uColor.a > 0.5) ? uTexture.Sample(uSampler, i.uv).rgb : uColor.rgb;
    float  met  = uPBR.x, rough = max(uPBR.y, 0.04);
    float3 N    = normalize(i.normal);

    if (uLightCount.x == 0)
    {
        float3 L    = normalize(float3(0.5, 0.8, 0.6));
        float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        return float4(base * diff, uPBR.z);
    }

    float3 V      = normalize(uCameraPos.xyz - i.worldPos);
    // IBL ambient: sample sky in surface normal and reflection direction.
    float3 Nup    = normalize(float3(N.x, max(N.y, 0.1f), N.z));
    float3 Rrough = normalize(lerp(reflect(-V, N), N, rough));
    float3 F0     = lerp(float3(0.04f,0.04f,0.04f), base, met);
    float3 kd     = (1.0f - F0) * (1.0f - met);
    float3 ambDiff = skyColor(Nup,    uSunDir.xyz) * base * kd;
    float3 ambSpec = skyColor(Rrough, uSunDir.xyz) * F0;
    float ao = (uViewport.z > 0.5f) ? uAO.SampleLevel(uAOSampler, i.clip.xy / uViewport.xy, 0).r : 1.0f;
    // GI replaces the AO-gated IBL diffuse with probe-grid indirect (spec IBL
    // stays in both branches) — mirrors the GL/Metal gi.enabled branch.
    // Where the probe volume doesn't reach, `coverage` fades the sky diffuse back in
    // rather than leaving the surface with no diffuse ambient at all.
    float3 skyDiff  = ao * ambDiff * 0.35f;
    float3 indirect = skyDiff;
    float3 indSpec  = ao * ambSpec * (1.0f - 0.6f * rough);
    if (uGIParams.x > 0.5f)
    {
        float  coverage;
        float3 giDiff = sampleDDGIIrradiance(i.worldPos, N, coverage) * base * kd * uGIParams.y;
        indirect = lerp(skyDiff, giDiff, coverage);
        indSpec  = ambSpec * (1.0f - 0.6f * rough); // GI branch leaves specular un-AO'd, as before
    }
    float3 result = indirect + indSpec;

    int giLocalIdx = 0; // counter over non-directional lights → local-mask channel
    for (int li = 0; li < uLightCount.x; ++li)
    {
        int   type  = (int)uLightPos[li].w;
        float3 L;
        float atten = 1.0;
        if (type == 0)
        {
            L = normalize(-uLightDir[li].xyz);
        }
        else
        {
            float3 d    = uLightPos[li].xyz - i.worldPos;
            float  dist = max(length(d), 1e-4);
            L = d / dist;
            float range = max(uLightParams[li].x, 1e-4);
            atten = saturate(1.0 - dist / range);
            atten *= atten;
            if (type == 2)
            {
                float c       = dot(-L, normalize(uLightDir[li].xyz));
                float cosCone = uLightDir[li].w;
                atten *= smoothstep(cosCone, lerp(cosCone, 1.0, 0.2), c);
            }
        }
        // Directional lights: ray-traced screen-space mask when GI is on
        // (replaces the single shadow map entirely), else the classic lookup.
        float sh = 1.0;
        if (type == 0)
        {
            sh = (uGIParams.x > 0.5f)
               ? lerp(GI_SHADOW_FLOOR, 1.0f,
                      uGIShadow.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0).r)
               : shadowFactor(i.worldPos, N, L);
        }
        else
        {
            // Local (point/spot) lights: ray-traced hard shadows when GI is
            // active — one visibility channel per light (first 4), written by
            // the shadow kernel from unjittered secondary rays (previously
            // local lights had no shadowing at all).
            if (uGIParams.x > 0.5f && giLocalIdx < 4)
                sh = lerp(GI_SHADOW_FLOOR, 1.0f,
                          uGILocal.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0)[giLocalIdx]);
            giLocalIdx++;
        }
        result += BRDF(L, V, N, base, met, rough) * uLightColor[li].rgb * uLightColor[li].w * atten * sh;
    }
    // Atmospheric fog
    if (uFog.x > 0.0f) {
        float3 ray = i.worldPos - uCameraPos.xyz;
        float dist = max(length(ray), 1e-4f);
        float k = uFog.y * ray.y;
        float ta = abs(k) > 1e-4f ? (1.0f - exp(-k)) / k : 1.0f;
        float opt = uFog.x * dist * exp(-uFog.y * uCameraPos.y) * ta;
        float f = 1.0f - exp(-opt);
        float3 fogCol = skyColor(ray/dist, uSunDir.xyz);
        result = lerp(result, fogCol, clamp(f, 0.0f, 1.0f));
    }
    return float4(result, uPBR.z);
}
)HLSL";

// ─── Skinned vertex shader HLSL ─────────────────────────────────────────────
// Only contains the VS entry; PSMain from kSceneHLSL is shared and pre-bound.
static const char* kSkinnedHLSL = R"HLSL(
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
static const char* kSSAOPosHLSL = R"HLSL(
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
static const char* kSSAOHLSL = R"HLSL(
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
static const char* kSSAOBlurHLSL = R"HLSL(
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
static const char* kFSTriangleVS = R"HLSL(
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
static const char* kTonemapHLSL = R"HLSL(
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

// Lottes FXAA — classic 3x3 neighbourhood edge blend, run on the
// tonemapped LDR image (t0).  cbuffer b0: { rcpFrame.xy }.
static const char* kFxaaHLSL = R"HLSL(
Texture2D    uScene : register(t0);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float2 uRcpFrame; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float luma(float3 c) { return dot(c, float3(0.299,0.587,0.114)); }
float4 main(In i) : SV_Target {
    const float EMIN=1.0/24.0, EMAX=1.0/8.0, SMAX=8.0;
    float3 M  = uScene.Sample(uSamp, i.uv).rgb;
    float  lM = luma(M);
    float  lNW= luma(uScene.Sample(uSamp, i.uv+float2(-1,-1)*uRcpFrame).rgb);
    float  lNE= luma(uScene.Sample(uSamp, i.uv+float2( 1,-1)*uRcpFrame).rgb);
    float  lSW= luma(uScene.Sample(uSamp, i.uv+float2(-1, 1)*uRcpFrame).rgb);
    float  lSE= luma(uScene.Sample(uSamp, i.uv+float2( 1, 1)*uRcpFrame).rgb);
    float  lMin=min(lM,min(min(lNW,lNE),min(lSW,lSE)));
    float  lMax=max(lM,max(max(lNW,lNE),max(lSW,lSE)));
    float  rng =lMax-lMin;
    if (rng < max(EMIN, lMax*EMAX)) return float4(M,1);
    float2 dir; dir.x=-((lNW+lNE)-(lSW+lSE)); dir.y=(lNW+lSW)-(lNE+lSE);
    float  dr=max((lNW+lNE+lSW+lSE)*0.25*(1.0/8.0),1.0/128.0);
    float  rdr=1.0/(min(abs(dir.x),abs(dir.y))+dr);
    dir=clamp(dir*rdr,-SMAX,SMAX)*uRcpFrame;
    float3 A=0.5*(uScene.Sample(uSamp,i.uv+dir*(1.0/3.0-0.5)).rgb
                 +uScene.Sample(uSamp,i.uv+dir*(2.0/3.0-0.5)).rgb);
    float3 B=A*0.5+0.25*(uScene.Sample(uSamp,i.uv+dir*-0.5).rgb
                         +uScene.Sample(uSamp,i.uv+dir* 0.5).rgb);
    float  lB=luma(B);
    return (lB<lMin||lB>lMax)?float4(A,1):float4(B,1);
}
)HLSL";

// Bloom bright-pass: soft-knee threshold, feeds the blur chain (t0 = HDR).
// cbuffer b0: { threshold, knee }.
static const char* kBloomBrightHLSL = R"HLSL(
Texture2D    uHDR  : register(t0);
SamplerState uSamp : register(s0);
cbuffer CB : register(b0) { float uThreshold; float uKnee; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target {
    float3 c=uHDR.Sample(uSamp,i.uv).rgb;
    float  br=max(c.r,max(c.g,c.b));
    float  s=clamp(br-uThreshold+uKnee,0.0,2.0*uKnee);
    s=(s*s)/(4.0*uKnee+1e-4);
    float contrib=max(s,br-uThreshold)/max(br,1e-4);
    return float4(c*contrib,1.0);
}
)HLSL";

// Separable 9-tap Gaussian blur.  cbuffer b0: { texel.xy, horizontal }.
// Run as paired H/V passes (ping-pong) for an approximate 2D Gaussian.
static const char* kBloomBlurHLSL = R"HLSL(
Texture2D    uImage : register(t0);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float uTexelX; float uTexelY; int uHoriz; float _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target {
    static const float w[5]={0.227027,0.1945946,0.1216216,0.054054,0.016216};
    float2 d=(uHoriz==1)?float2(uTexelX,0):float2(0,uTexelY);
    float3 r=uImage.Sample(uSamp,i.uv).rgb*w[0];
    [unroll] for(int k=1;k<5;++k){
        r+=uImage.Sample(uSamp,i.uv+d*k).rgb*w[k];
        r+=uImage.Sample(uSamp,i.uv-d*k).rgb*w[k];
    }
    return float4(r,1.0);
}
)HLSL";

// ─── 2D UI canvas HLSL ──────────────────────────────────────────────────────
// Generates a screen-space quad from SV_VertexID (0-3, TRIANGLESTRIP).
// cbuffer layout: rect(16) + color(16) + uvRect(16) + viewport(8) + mode(4) +
// pad(4) = 64 bytes.  uUVRect = {u0, v0, u1, v1} into the font atlas (glyph
// quads); uMode: 0 = solid color, 1 = font-atlas glyph (alpha from the atlas R
// channel).  Mirrors kUIVS/kUIFS on the GL backend.
static const char* kUIHLSL = R"HLSL(
cbuffer UICB : register(b0) {
    float4 uRect;      // xy = top-left in pixels, zw = size in pixels
    float4 uColor;     // rgba
    float4 uUVRect;    // glyph atlas UVs: xy = min, zw = max
    float2 uViewport;  // w, h in pixels
    float  uMode;      // 0 = solid quad, 1 = font-atlas glyph
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
    o.clip = float4(sp.x / uViewport.x * 2.0f - 1.0f,
                    1.0f - sp.y / uViewport.y * 2.0f,
                    0.0f, 1.0f);
    o.uv = lerp(uUVRect.xy, uUVRect.zw, uv);
    return o;
}
float4 UIPSMain(UIOut i) : SV_TARGET
{
    if (uMode > 0.5f)
        return float4(uColor.rgb, uColor.a * uFontAtlas.Sample(uSamp, i.uv).r);
    return uColor;
}
)HLSL";

// ─── Ray-traced GI (software BVH) HLSL ──────────────────────────────────────
// D3D11 port of the GL-4.3 compute GI (kGi* in OpenGLRenderer.cpp), which in
// turn mirrors the Metal reference. SSBOs → StructuredBuffers, image store →
// RWTexture2D, floatBitsToInt → asint(). All five stages compile as SM 5.0.

// World-space G-buffer pre-pass (position + normal MRT). CRITICAL: rendered
// with the SAME extraction/camera as the scene pass (Metal lesson 5846efc) or
// the screen-space mask misaligns and shadows swim with camera rotation.
// Reuses the PerObject cbuffer layout so the shared uploadObject helper feeds it.
static const char* kGiGBufHLSL = R"HLSL(
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
static const char* kGiTraversalHLSL = R"HLSL(
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
static const char* kGiShadowCSHLSL = R"HLSL(
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

// One HARD occlusion ray toward local light i. Returns visibility (1 = lit).
// Kept scalar + called with literal indices below: FXC/SM5.0 cannot use a
// dynamically indexed vector component as an l-value, and the runtime trip
// count (uLocalExtra.x) blocks the unroll that would make the index literal.
float giLocalVis(int i, float3 P, float3 N, int localCount)
{
    if (i >= localCount) return 1.0;
    float3 toL   = uLocalPosRange[i].xyz - P;
    float  distL = length(toL);
    if (distL <= 0.05) return 1.0;              // on top of the light → lit
    if (distL >= uLocalPosRange[i].w) return 1.0; // outside the attenuation radius → no contribution
    float3 dirL = toL / distL;
    if (dot(N, dirL) <= 0.0) return 0.0;
    return giSceneAnyHit(P + N * 0.05, dirL, 0.02, max(distL - 0.1, 0.02)) ? 0.0 : 1.0;
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
    int localCount = clamp(int(uLocalExtra.x), 0, 4);
    float4 localVis;
    localVis.x = giLocalVis(0, pv.xyz, N, localCount);
    localVis.y = giLocalVis(1, pv.xyz, N, localCount);
    localVis.z = giLocalVis(2, pv.xyz, N, localCount);
    localVis.w = giLocalVis(3, pv.xyz, N, localCount);
    uOutLocal[gid.xy] = localVis;
}
)HLSL";

// Temporal accumulation: reproject via last frame's viewProj; history carries
// world position (rgb) + shadow scalar (a). Tolerance deliberately TIGHT
// (Metal lesson 58ee312). D3D NDC → UV includes the y-flip (unlike GL).
static const char* kGiTemporalHLSL = R"HLSL(
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
static const char* kGiBlurHLSL = R"HLSL(
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
// D3D11 twist: typed UAV loads of RGBA16F/RG16F are an optional 11.3 cap, so
// the previous atlas values arrive as SRV COPIES (t5/t6, refreshed by
// CopyResource before the dispatch) instead of imageLoad on the UAV.
static const char* kGiProbeCSHLSL = R"HLSL(
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

static const int kOctSize = 8; // must match the host's kGiProbeOctSize

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
// an optional D3D11.3 cap, so the UAV is never read).
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

namespace
{
    // GPU mesh uploaded on first sight, mirroring the GL/Metal backends.
    struct GpuMesh
    {
        ComPtr<ID3D11Buffer>             vbuf;
        ComPtr<ID3D11Buffer>             ibuf;
        UINT                             indexCount = 0;
        ComPtr<ID3D11ShaderResourceView> texture; // base color, null = none
        HE::AABB                         localBounds;
    };

    // GPU resources for a skinned/skeletal mesh.
    // Three vertex buffers: interleaved pos+norm+uv (slot 0), bone IDs (slot 1), bone weights (slot 2).
    struct GpuSkeletalMesh
    {
        ComPtr<ID3D11Buffer>             vb;         // interleaved pos(12)+norm(12)+uv(8) = 32 bytes/vertex
        ComPtr<ID3D11Buffer>             boneIdVb;   // uint4 per vertex (16 bytes)
        ComPtr<ID3D11Buffer>             boneWgtVb;  // float4 per vertex (16 bytes)
        ComPtr<ID3D11Buffer>             ib;
        ComPtr<ID3D11ShaderResourceView> srv;        // albedo texture (may be null)
        int                              indexCount  = 0;
    };

    // Constant-buffer layouts must match the HLSL cbuffers exactly (16-byte rules).
    struct PerObjectCB
    {
        glm::mat4 mvp;
        glm::mat4 model;
        glm::vec4 color;   // rgb + hasTexture in .a
        glm::vec4 pbr;     // x=metallic, y=roughness, z=opacity
    };
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
        glm::vec4  sunDir;   // xyz = sun direction
        glm::vec4  fog;      // x=fogDensity, y=fogHeightFalloff
        glm::vec4  viewport; // x=W, y=H, z=ssaoEnabled
        glm::vec4  giParams;     // x = GI enabled (0/1), y = indirect intensity
        glm::vec4  giGridOrigin; // xyz = probe grid origin, w = spacing
        glm::vec4  giGridCounts; // xyz = probe counts, w = probesPerRow
    };

    // GL copy of the dominant-directional-light pick (glDominantDirectionalLight)
    // — the brightest directional light, NEVER the sky-dome sun (below the
    // horizon at night). Keep all backend copies in sync.
    static bool d3d11DominantDirectionalLight(const RenderWorld& rw,
                                              glm::vec3& towardOut, glm::vec3& colorIntensityOut)
    {
        const LightData* best = nullptr;
        for (const LightData& l : rw.lights)
            if (l.type == 0 && l.intensity > 0.0f && (!best || l.intensity > best->intensity))
                best = &l;
        if (!best || glm::dot(best->direction, best->direction) < 1e-8f)
        {
            towardOut         = glm::normalize(rw.sunDirection);
            colorIntensityOut = glm::vec3(0.0f);
            return false;
        }
        towardOut         = -glm::normalize(best->direction);
        colorIntensityOut = best->color * best->intensity;
        return true;
    }

    struct SkyCB {
        glm::mat4 invViewProj;
        glm::vec3 sunDir;    float timeOfDay;
        glm::vec3 sunColor;  float cloudCoverage;
        glm::vec3 wind;      float time;
        glm::vec3 auroraColor; float aurora;
        float milkyWay;      float flash; int hasMoonTex; float _pad;
    };

    struct SsaoRng {
        uint32_t s;
        float next() { s = s * 1664525u + 1013904223u; return float(s >> 8) * (1.0f / 16777216.0f); }
    };

    static std::vector<glm::vec3> BuildSSAOKernel(int n)
    {
        SsaoRng rng{ 0x9E3779B9u };
        std::vector<glm::vec3> k(n);
        for (int i = 0; i < n; ++i) {
            glm::vec3 s(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, rng.next());
            s = glm::normalize(s) * rng.next();
            float t = static_cast<float>(i) / static_cast<float>(n);
            s *= 0.1f + 0.9f * t * t;
            k[i] = s;
        }
        return k;
    }

    static std::vector<glm::vec3> BuildSSAONoise(int n)
    {
        SsaoRng rng{ 0x2545F491u };
        std::vector<glm::vec3> v(n);
        for (int i = 0; i < n; ++i)
            v[i] = glm::vec3(rng.next() * 2.0f - 1.0f, rng.next() * 2.0f - 1.0f, 0.0f);
        return v;
    }

    // Remaps the extractor's GL-convention light projection (depth -1..1) to D3D
    // clip space (depth 0..1). D3D NDC y is up; sampling flips V (top-left origin).
    const glm::mat4 kD3DClipFix(
        1.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.5f, 0.0f,
        0.0f, 0.0f, 0.5f, 1.0f);
}

struct D3D11RendererImpl
{
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGISwapChain>         swapchain;
    ComPtr<ID3D11RenderTargetView> rtv;
    ComPtr<ID3D11DepthStencilView> dsv;
    ComPtr<ID3D11Texture2D>        depthTex;
    bool vsync = true;
    int  width = 0, height = 0;

    // ── Scene pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11VertexShader>       vsInstanced; // A3: instanced geometry VS (reads t3 structured buffer)
    ComPtr<ID3D11Buffer>             instanceSB;  // A3: per-instance {mvp,model}, dynamic structured buffer
    ComPtr<ID3D11ShaderResourceView> instanceSRV; // A3: SRV over instanceSB, bound at VS t3
    static constexpr UINT k_maxInstances = 65536; // instance-buffer capacity (A3)
    static constexpr UINT k_instStride   = 128;   // bytes per instance = 2 × float4x4 (mvp, model)
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11InputLayout>    inputLayout;
    ComPtr<ID3D11Buffer>         perObjectCB;
    ComPtr<ID3D11Buffer>         perFrameCB;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11DepthStencilState> depthReadOnlyState; // transparent pass: test but no write
    ComPtr<ID3D11BlendState>        alphaBlendState;    // SRC_ALPHA / INV_SRC_ALPHA
    ComPtr<ID3D11RasterizerState>   rasterState;
    ComPtr<ID3D11ShaderResourceView> dummyTexture; // 1x1 white, for untextured meshes

    // ── A4: node-graph material shaders ──────────────────────────────────────
    // Graph materials (Material-Node editor) render through per-material VS/PS the engine
    // builds at draw time from MaterialShaderLibrary HLSL (SPIRV-Cross). Unlike D3D12/Vulkan
    // there is NO PSO / pipeline object: blend + depth + render-target format are separate
    // D3D11 states set at draw time, so a graph material's VS/PS/InputLayout are identical
    // for opaque/transparent/HDR — the draw path simply inherits the pass's blend + depth.
    // All of this is inert (m_matReady stays false) when HE_HAVE_SHADERC is off, so behaviour
    // equals today's built-in PBR path. Canonical SPIRV-Cross HLSL register mapping
    // (shader_model=50, binding→register, verified for D3D12):
    //   b0 HeLighting(PS) | b1 U(VS) | b3 HeParams(PS) | b8/b9 HeLighting/HeParams(WPO VS)
    //   t2 heTex0, t4..t7 heTexP0..3 (+ SamplerState s2, s4..s7, linear-wrap).
    HE::MaterialShaderLibrary m_matShaderLib; // unguarded member (like Vulkan/D3D12)
    struct MatShaders {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader>  ps;
        ComPtr<ID3D11InputLayout>  il;
    };
    std::unordered_map<uint64_t, MatShaders> m_materialShaders; // key = hash ^ transparentbit
    ComPtr<ID3D11Buffer>       m_matLightCB;  // HeLighting (full Lighting struct) — b0 PS / b8 WPO VS, filled once/frame
    ComPtr<ID3D11Buffer>       m_matObjCB;    // U (176 B)         — b1 VS,          filled per draw
    ComPtr<ID3D11Buffer>       m_matParamCB;  // HeParams (256 B)  — b3 PS / b9 WPO VS, filled per draw
    ComPtr<ID3D11SamplerState> m_matSampler;  // linear-wrap, bound at s2 + s4..s7
    bool m_matReady      = false; // true once createMaterialResources() succeeded
    bool m_matHlslLogged = false; // one-time dump of generated HLSL for HW verify
    // createMaterialResources() + getOrBuildMaterialShaders() are defined inline below.

    // ── Shadow map ──────────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>       depthVS;    // depth-only pass
    ComPtr<ID3D11Texture2D>          shadowTex;
    ComPtr<ID3D11DepthStencilView>   shadowDSV;
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    int shadowSize = 2048;

    // ── Viewport offscreen render target ────────────────────────────────────
    ComPtr<ID3D11Texture2D>          viewportTex;
    ComPtr<ID3D11RenderTargetView>   viewportRTV;
    ComPtr<ID3D11ShaderResourceView> viewportSRV;
    ComPtr<ID3D11Texture2D>          viewportDepth;
    ComPtr<ID3D11DepthStencilView>   viewportDSV;
    uint32_t viewportW    = 0;
    uint32_t viewportH    = 0;
    uint32_t viewportReqW = 0;
    uint32_t viewportReqH = 0;

    // ── HDR scene color (RGBA16F) — geometry renders here ───────────────────
    ComPtr<ID3D11Texture2D>          hdrTex;
    ComPtr<ID3D11RenderTargetView>   hdrRTV;
    ComPtr<ID3D11ShaderResourceView> hdrSRV;

    // ── Bloom ping-pong (RGBA16F, half-res) ──────────────────────────────────
    ComPtr<ID3D11Texture2D>          bloomTex[2];
    ComPtr<ID3D11RenderTargetView>   bloomRTV[2];
    ComPtr<ID3D11ShaderResourceView> bloomSRV[2];

    // ── LDR intermediate (RGBA8) — tonemap output / FXAA input ──────────────
    ComPtr<ID3D11Texture2D>          ldrTex;
    ComPtr<ID3D11RenderTargetView>   ldrRTV;
    ComPtr<ID3D11ShaderResourceView> ldrSRV;

    // ── PostFX shaders & state ────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>      fsVS;
    ComPtr<ID3D11PixelShader>       tonemapPS;
    ComPtr<ID3D11PixelShader>       fxaaPS;
    ComPtr<ID3D11PixelShader>       bloomBrightPS;
    ComPtr<ID3D11PixelShader>       bloomBlurPS;
    ComPtr<ID3D11SamplerState>      linearSampler;
    ComPtr<ID3D11DepthStencilState> noDepthDSS;
    ComPtr<ID3D11RasterizerState>   fsRastState;
    ComPtr<ID3D11Buffer>            postFxCB;
    bool postFxReady     = false;
    float exposure       = 1.0f;

    // ── Sky pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>       skyVS;
    ComPtr<ID3D11PixelShader>        skyPS;
    ComPtr<ID3D11Buffer>             skyCB;
    ComPtr<ID3D11Texture2D>          moonTex2D;
    ComPtr<ID3D11ShaderResourceView> moonSRV;
    ComPtr<ID3D11Texture3D>          noiseTex3D;
    ComPtr<ID3D11ShaderResourceView> noiseSRV;
    ComPtr<ID3D11SamplerState>       skyNoiseSampler;
    bool skyReady = false;
    // ── Debug line pipeline ───────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>  debugVS;
    ComPtr<ID3D11PixelShader>   debugPS;
    ComPtr<ID3D11Buffer>        debugVB;
    ComPtr<ID3D11Buffer>        debugCB;
    ComPtr<ID3D11InputLayout>   debugIL;
    bool debugReady = false;
    std::vector<DebugLine> m_debugLines;
    float m_wallTime = 0.0f;
    float bloomStrength  = 0.25f;
    float bloomThreshold = 1.0f;
    float bloomKnee      = 0.1f;
    bool  bloomEnabled   = true;
    bool  fxaaEnabled    = true;

    // ── SSAO pipeline ──────────────────────────────────────────────────────
    // Position prepass
    ComPtr<ID3D11VertexShader>       ssaoPosVS;
    ComPtr<ID3D11PixelShader>        ssaoPosPS;
    ComPtr<ID3D11Buffer>             ssaoPosPerObjCB;   // { mat4 posMVP; mat4 posModelView; }
    // SSAO passes
    ComPtr<ID3D11PixelShader>        ssaoPS;
    ComPtr<ID3D11PixelShader>        ssaoBlurPS;
    ComPtr<ID3D11Buffer>             ssaoCB;            // SSAOCB (kernel + params)
    ComPtr<ID3D11Buffer>             ssaoBlurCB;        // BlurCB { texelX, texelY, pad }
    // Render targets
    ComPtr<ID3D11Texture2D>          ssaoPosTex;        // RGBA16F view-space positions
    ComPtr<ID3D11RenderTargetView>   ssaoPosRTV;
    ComPtr<ID3D11ShaderResourceView> ssaoPosSRV;
    ComPtr<ID3D11Texture2D>          ssaoPosDepth;      // separate depth for position prepass
    ComPtr<ID3D11DepthStencilView>   ssaoPosDepthDSV;
    ComPtr<ID3D11Texture2D>          ssaoTex;           // R8 AO output
    ComPtr<ID3D11RenderTargetView>   ssaoRTV;
    ComPtr<ID3D11ShaderResourceView> ssaoSRV;
    ComPtr<ID3D11Texture2D>          ssaoBlurTex;       // R8 blurred AO
    ComPtr<ID3D11RenderTargetView>   ssaoBlurRTV;
    ComPtr<ID3D11ShaderResourceView> ssaoBlurSRV;
    // Resources
    ComPtr<ID3D11Texture2D>          ssaoNoiseTex;      // 4x4 RGBA32F rotation noise
    ComPtr<ID3D11ShaderResourceView> ssaoNoiseSRV;
    ComPtr<ID3D11Texture2D>          whiteTex;          // 1x1 white, AO fallback when disabled
    ComPtr<ID3D11ShaderResourceView> whiteSRV;
    ComPtr<ID3D11SamplerState>       pointSampler;      // POINT + WRAP for SSAO noise + pos
    // Settings
    float ssaoRadius    = 0.5f;
    float ssaoBias      = 0.025f;
    float ssaoIntensity = 1.5f;
    bool  ssaoEnabled   = true;
    int   ssaoMethod    = 0;
    bool  ssaoReady     = false;
    int   ssaoW         = 0;
    int   ssaoH         = 0;

    // ── Skinned mesh pipeline ─────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader> skinnedVS;
    ComPtr<ID3D11InputLayout>  skinnedLayout;
    ComPtr<ID3D11Buffer>       bonesCB;
    std::unordered_map<HE::UUID, GpuSkeletalMesh> skeletalMeshCache;

    // ── UI canvas pipeline ────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>      uiVS;
    ComPtr<ID3D11PixelShader>       uiPS;
    ComPtr<ID3D11Buffer>            uiCB;       // 64 bytes: rect(16)+color(16)+uvRect(16)+viewport(8)+mode(4)+pad(4)
    ComPtr<ID3D11BlendState>        uiBlend;    // alpha blend
    ComPtr<ID3D11DepthStencilState> uiDepth;    // depth test off
    ComPtr<ID3D11SamplerState>      uiSampler;  // linear + clamp, for the font atlas
    // R8 font atlases uploaded lazily from UIFontCache (key 0 = shared default
    // font). Atlas bitmaps are immutable once baked, so a one-time upload per
    // key is safe; failed bakes are NOT cached so a late-baking font still lands.
    struct UIFontAtlas { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; };
    std::unordered_map<uint32_t, UIFontAtlas> uiFontAtlases;

    // The atlas SRV for a font key, uploaded on first use. Falls back to the 1x1
    // white dummy (glyphs render as solid boxes) so the pass never binds null.
    ID3D11ShaderResourceView* uiFontAtlasSRV(uint32_t key)
    {
        if (auto it = uiFontAtlases.find(key); it != uiFontAtlases.end())
            return it->second.srv.Get();
        const HE::BakedUIFont* f = (key == 0) ? &HE::sharedUIFont() : HE::UIFontCache::find(key);
        if (!f || !f->ok || f->pixels.empty())
            return dummyTexture.Get();

        D3D11_TEXTURE2D_DESC td{};
        td.Width  = static_cast<UINT>(f->atlasW);
        td.Height = static_cast<UINT>(f->atlasH);
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage     = D3D11_USAGE_IMMUTABLE;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA srd{};
        srd.pSysMem     = f->pixels.data();
        srd.SysMemPitch = static_cast<UINT>(f->atlasW); // R8 = 1 byte/texel

        UIFontAtlas a;
        if (FAILED(device->CreateTexture2D(&td, &srd, &a.tex)) ||
            FAILED(device->CreateShaderResourceView(a.tex.Get(), nullptr, &a.srv)))
            return dummyTexture.Get();
        ID3D11ShaderResourceView* raw = a.srv.Get();
        uiFontAtlases.emplace(key, std::move(a));
        return raw;
    }

    // ── Profiler GPU timing (whole-frame) ─────────────────────────────────────
    // One D3D11_QUERY_TIMESTAMP pair inside a TIMESTAMP_DISJOINT per frame, kept
    // in a small ring so a slot is only read back kGpuTimerRing frames after it
    // was issued — GetData(flags=0) at that age never blocks in practice, and a
    // not-yet-ready slot is dropped rather than stalling the pipeline. Queries
    // are only issued while the profiler is recording / live (never on the hot
    // path otherwise, mirroring the GL backend).
    static constexpr int kGpuTimerRing = 4;
    struct GpuTimerSlot
    {
        ComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
        bool pending = false; // issued, result not consumed yet
    };
    GpuTimerSlot gpuSlots[kGpuTimerRing];
    uint64_t gpuFrameIdx     = 0;
    int      gpuCurSlot      = -1;
    bool     gpuTimerInit    = false;
    bool     gpuTimingActive = false;
    bool     gpuWasActive    = false;
    bool     gpuDetailed     = false;
    IRenderer::FrameGpuStats lastGpuStats;
    // CPU counters merged into GetFrameGpuStats (scene draws only, like GL:
    // instanced batches count per instance drawn, tris scaled accordingly).
    struct FrameCounters { uint32_t draws = 0, tris = 0, visible = 0, total = 0; };
    FrameCounters counters;

    void gpuTimerReap(GpuTimerSlot& slot, bool block)
    {
        if (!slot.pending) return;
        if (block) context->Flush(); // make sure the queries can complete
        auto fetch = [&](ID3D11Query* q, void* out, UINT size) -> bool
        {
            HRESULT hr = context->GetData(q, out, size, 0);
            while (block && hr == S_FALSE)
                hr = context->GetData(q, out, size, 0);
            return hr == S_OK;
        };
        D3D11_QUERY_DATA_TIMESTAMP_DISJOINT dj{};
        UINT64 t0 = 0, t1 = 0;
        if (!fetch(slot.disjoint.Get(), &dj, sizeof(dj)) ||
            !fetch(slot.tsStart.Get(),  &t0, sizeof(t0)) ||
            !fetch(slot.tsEnd.Get(),    &t1, sizeof(t1)))
        {
            slot.pending = false; // slot is about to be reused — drop the sample
            return;
        }
        slot.pending = false;
        // Disjoint frames (clock change / power event) yield garbage deltas —
        // keep the previous reading rather than publishing one.
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) return;
        lastGpuStats.gpuFrameMs    = static_cast<double>(t1 - t0) * 1000.0
                                   / static_cast<double>(dj.Frequency);
        lastGpuStats.passes.clear(); // whole-frame timing: no per-pass breakdown
        lastGpuStats.gpuTimingMode = "whole-frame";
    }

    void gpuTimerBeginFrame()
    {
        // Latch the profiler decision once per frame so Begin/EndFrame agree
        // (a mid-frame toggle can never unbalance a Begin/End pair).
        EngineProfiler& prof = EngineProfiler::instance();
        const bool rec  = prof.isRecording();
        const bool live = prof.liveEnabled();
        gpuTimingActive = device && (rec || live);
        // Same-frame reap (one Flush + spin) for detailed / single-frame capture:
        // the profiler reads that frame's stats immediately, so the async ring
        // would attribute a different frame's GPU time to it (mirrors GL's glFinish).
        gpuDetailed = gpuTimingActive && rec
                   && (prof.detailedGpuCapture() || prof.isSingleFrameCapture());
        const bool freshActivation = gpuTimingActive && !gpuWasActive;
        gpuWasActive = gpuTimingActive;
        gpuCurSlot   = -1;
        if (!gpuTimingActive) return;

        if (!gpuTimerInit)
        {
            const D3D11_QUERY_DESC dq{ D3D11_QUERY_TIMESTAMP_DISJOINT, 0 };
            const D3D11_QUERY_DESC tq{ D3D11_QUERY_TIMESTAMP, 0 };
            bool ok = true;
            for (GpuTimerSlot& s : gpuSlots)
                ok = ok && SUCCEEDED(device->CreateQuery(&dq, &s.disjoint))
                        && SUCCEEDED(device->CreateQuery(&tq, &s.tsStart))
                        && SUCCEEDED(device->CreateQuery(&tq, &s.tsEnd));
            if (!ok)
            {
                for (GpuTimerSlot& s : gpuSlots) s = GpuTimerSlot{};
                gpuTimingActive = false; // GetFrameGpuStats keeps gpuFrameMs = -1
                return;
            }
            gpuTimerInit = true;
        }
        // On (re)activation, drop stale in-flight slots so the profiler shows
        // "no data yet" (gpuFrameMs = -1) instead of cross-session numbers.
        if (freshActivation)
        {
            for (GpuTimerSlot& s : gpuSlots) s.pending = false;
            lastGpuStats = IRenderer::FrameGpuStats{};
        }

        const int idx = static_cast<int>(gpuFrameIdx % kGpuTimerRing);
        GpuTimerSlot& slot = gpuSlots[idx];
        gpuTimerReap(slot, /*block=*/false); // issued kGpuTimerRing frames ago
        gpuCurSlot = idx;
        context->Begin(slot.disjoint.Get());
        context->End(slot.tsStart.Get()); // timestamps have no Begin, only End
    }

    void gpuTimerEndFrame()
    {
        if (!gpuTimingActive || gpuCurSlot < 0) { ++gpuFrameIdx; return; }
        GpuTimerSlot& slot = gpuSlots[gpuCurSlot];
        context->End(slot.tsEnd.Get());
        context->End(slot.disjoint.Get());
        slot.pending = true;
        if (gpuDetailed)
            gpuTimerReap(slot, /*block=*/true);
        gpuCurSlot = -1;
        ++gpuFrameIdx;
    }

    void gpuTimerShutdown()
    {
        for (GpuTimerSlot& s : gpuSlots) s = GpuTimerSlot{};
        gpuTimerInit = false;
        gpuWasActive = false;
        lastGpuStats = IRenderer::FrameGpuStats{};
    }

    void createHDRTargets(uint32_t w, uint32_t h)
    {
        hdrRTV.Reset(); hdrSRV.Reset(); hdrTex.Reset();
        bloomRTV[0].Reset(); bloomSRV[0].Reset(); bloomTex[0].Reset();
        bloomRTV[1].Reset(); bloomSRV[1].Reset(); bloomTex[1].Reset();
        ldrRTV.Reset(); ldrSRV.Reset(); ldrTex.Reset();

        auto makeRT = [&](DXGI_FORMAT fmt, uint32_t tw, uint32_t th,
                          ComPtr<ID3D11Texture2D>& t,
                          ComPtr<ID3D11RenderTargetView>& rtv,
                          ComPtr<ID3D11ShaderResourceView>& srv) -> bool
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = tw; td.Height = th;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) return false;
            device->CreateRenderTargetView(t.Get(), nullptr, &rtv);
            device->CreateShaderResourceView(t.Get(), nullptr, &srv);
            return rtv && srv;
        };

        makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, w, h, hdrTex, hdrRTV, hdrSRV);
        const uint32_t bw = std::max(1u, w / 2), bh = std::max(1u, h / 2);
        for (int i = 0; i < 2; ++i)
            makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, bw, bh, bloomTex[i], bloomRTV[i], bloomSRV[i]);
        makeRT(DXGI_FORMAT_R8G8B8A8_UNORM, w, h, ldrTex, ldrRTV, ldrSRV);
        createSSAOTargets((int)w, (int)h);
    }

    bool createSSAOPipeline()
    {
        // Compile position prepass VS+PS
        {
            ComPtr<ID3DBlob> vsBlob, psBlob, err;
            if (FAILED(D3DCompile(kSSAOPosHLSL, strlen(kSSAOPosHLSL), nullptr, nullptr, nullptr,
                                  "VSPos", "vs_5_0", 0, 0, &vsBlob, &err))) {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                return false;
            }
            if (FAILED(D3DCompile(kSSAOPosHLSL, strlen(kSSAOPosHLSL), nullptr, nullptr, nullptr,
                                  "PSPos", "ps_5_0", 0, 0, &psBlob, &err))) {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                return false;
            }
            device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &ssaoPosVS);
            device->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ssaoPosPS);
            // Per-object CB for position prepass: { mat4 posMVP; mat4 posModelView; }
            D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 128; cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &ssaoPosPerObjCB);
        }
        // Compile SSAO main PS
        {
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3DCompile(kSSAOHLSL, strlen(kSSAOHLSL), nullptr, nullptr, nullptr,
                                  "SSAOMain", "ps_5_0", 0, 0, &blob, &err))) {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                return false;
            }
            device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ssaoPS);
        }
        // Compile SSAO blur PS
        {
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3DCompile(kSSAOBlurHLSL, strlen(kSSAOBlurHLSL), nullptr, nullptr, nullptr,
                                  "SSAOBlurMain", "ps_5_0", 0, 0, &blob, &err))) {
                if (err) OutputDebugStringA((char*)err->GetBufferPointer());
                return false;
            }
            device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &ssaoBlurPS);
        }
        // SSAO CB: { float4x4 proj; float4 noiseScale; float4 params; float4 kernel[32]; }
        // = 64 + 16 + 16 + 32*16 = 608 bytes, must be multiple of 16 -> 608 OK
        {
            D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 608; cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &ssaoCB);
        }
        // Blur CB: { float2 texel; float2 pad; }
        {
            D3D11_BUFFER_DESC cbd{}; cbd.ByteWidth = 16; cbd.Usage = D3D11_USAGE_DYNAMIC;
            cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&cbd, nullptr, &ssaoBlurCB);
        }
        // Point sampler with WRAP (for noise tiling)
        {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&sd, &pointSampler);
        }
        // 4x4 rotation noise texture (RGBA32F, WRAP)
        {
            std::vector<glm::vec3> noiseData = BuildSSAONoise(16);
            // Expand to RGBA32F
            std::vector<float> rgba(16 * 4);
            for (int i = 0; i < 16; ++i) {
                rgba[i*4+0] = noiseData[i].x;
                rgba[i*4+1] = noiseData[i].y;
                rgba[i*4+2] = noiseData[i].z;
                rgba[i*4+3] = 0.0f;
            }
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 4; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = rgba.data(); init.SysMemPitch = 4 * 4 * sizeof(float);
            device->CreateTexture2D(&td, &init, &ssaoNoiseTex);
            device->CreateShaderResourceView(ssaoNoiseTex.Get(), nullptr, &ssaoNoiseSRV);
        }
        // 1x1 white texture (AO fallback when SSAO disabled)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            uint8_t white = 255;
            D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = &white; init.SysMemPitch = 1;
            device->CreateTexture2D(&td, &init, &whiteTex);
            device->CreateShaderResourceView(whiteTex.Get(), nullptr, &whiteSRV);
        }
        ssaoReady = ssaoPosVS && ssaoPosPS && ssaoPS && ssaoBlurPS && ssaoCB && ssaoBlurCB
                    && pointSampler && ssaoNoiseSRV && whiteSRV && ssaoPosPerObjCB;
        return ssaoReady;
    }

    void createSSAOTargets(int w, int h)
    {
        ssaoPosRTV.Reset(); ssaoPosSRV.Reset(); ssaoPosTex.Reset();
        ssaoPosDepthDSV.Reset(); ssaoPosDepth.Reset();
        ssaoRTV.Reset(); ssaoSRV.Reset(); ssaoTex.Reset();
        ssaoBlurRTV.Reset(); ssaoBlurSRV.Reset(); ssaoBlurTex.Reset();

        auto makeRT = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& t,
                          ComPtr<ID3D11RenderTargetView>& rtv,
                          ComPtr<ID3D11ShaderResourceView>& srv) -> bool {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w; td.Height = (UINT)h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) return false;
            device->CreateRenderTargetView(t.Get(), nullptr, &rtv);
            device->CreateShaderResourceView(t.Get(), nullptr, &srv);
            return rtv && srv;
        };
        makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, ssaoPosTex, ssaoPosRTV, ssaoPosSRV);
        makeRT(DXGI_FORMAT_R8_UNORM,           ssaoTex,    ssaoRTV,    ssaoSRV);
        makeRT(DXGI_FORMAT_R8_UNORM,           ssaoBlurTex, ssaoBlurRTV, ssaoBlurSRV);

        // Depth buffer for position prepass
        {
            D3D11_TEXTURE2D_DESC dd{};
            dd.Width = (UINT)w; dd.Height = (UINT)h;
            dd.MipLevels = dd.ArraySize = 1;
            dd.Format = DXGI_FORMAT_D16_UNORM; dd.SampleDesc.Count = 1;
            dd.Usage = D3D11_USAGE_DEFAULT;
            dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
            device->CreateTexture2D(&dd, nullptr, &ssaoPosDepth);
            device->CreateDepthStencilView(ssaoPosDepth.Get(), nullptr, &ssaoPosDepthDSV);
        }
        ssaoW = w; ssaoH = h;
    }

    // Returns the SRV that the scene shader should bind as t2 (AO texture).
    ID3D11ShaderResourceView* runSSAO(ID3D11DeviceContext* ctx,
                                      const std::vector<const DrawCall*>& opaqueDCs,
                                      const glm::mat4& viewProj, const glm::mat4& view,
                                      const glm::mat4& proj,
                                      int w, int h,
                                      const std::function<const GpuMesh*(HE::UUID)>& resolveMeshFn,
                                      const GpuMesh& fallbackMesh,
                                      ID3D11InputLayout* il,
                                      ID3D11DepthStencilState* depthSt,
                                      ID3D11RasterizerState* rasterSt)
    {
        if (!ssaoReady || !ssaoPosRTV || !ssaoRTV || !ssaoBlurRTV) return whiteSRV.Get();
        if (ssaoW != w || ssaoH != h) createSSAOTargets(w, h);

        const UINT stride = 8 * sizeof(float), off = 0;
        D3D11_VIEWPORT vp{}; vp.Width = float(w); vp.Height = float(h); vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        // ── Pass 1: Position prepass ──────────────────────────────────────────
        {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(2, 1, &nullSrv);
            ctx->OMSetRenderTargets(1, ssaoPosRTV.GetAddressOf(), ssaoPosDepthDSV.Get());
            float clear[4] = {0,0,0,0};
            ctx->ClearRenderTargetView(ssaoPosRTV.Get(), clear);
            ctx->ClearDepthStencilView(ssaoPosDepthDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->IASetInputLayout(il);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(ssaoPosVS.Get(), nullptr, 0);
            ctx->PSSetShader(ssaoPosPS.Get(), nullptr, 0);
            ctx->OMSetDepthStencilState(depthSt, 0);
            ctx->RSSetState(rasterSt);
            ctx->VSSetConstantBuffers(0, 1, ssaoPosPerObjCB.GetAddressOf());

            for (const DrawCall* dc : opaqueDCs) {
                const GpuMesh* mesh = resolveMeshFn(dc->meshAssetId);
                const GpuMesh& m = mesh ? *mesh : fallbackMesh;
                if (!m.vbuf || !m.ibuf) continue;
                ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &off);
                ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);

                auto drawWithTransform = [&](const glm::mat4& modelMat) {
                    struct { glm::mat4 mvp, modelView; } pcb;
                    pcb.mvp       = viewProj * modelMat;
                    pcb.modelView = view     * modelMat;
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(ssaoPosPerObjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, &pcb, sizeof(pcb));
                        ctx->Unmap(ssaoPosPerObjCB.Get(), 0);
                    }
                    ctx->DrawIndexed(m.indexCount, 0, 0);
                };

                if (!dc->instanceTransforms.empty())
                    for (const glm::mat4& t : dc->instanceTransforms) drawWithTransform(t);
                else
                    drawWithTransform(dc->transform);
            }
        }

        // Unbind posRTV so it can be read as SRV
        { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }

        // ── Pass 2: SSAO ──────────────────────────────────────────────────────
        {
            ctx->OMSetRenderTargets(1, ssaoRTV.GetAddressOf(), nullptr);
            float clear[4] = {1,1,1,1};
            ctx->ClearRenderTargetView(ssaoRTV.Get(), clear);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(fsVS.Get(), nullptr, 0);
            ctx->PSSetShader(ssaoPS.Get(), nullptr, 0);
            ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
            ctx->RSSetState(fsRastState.Get());
            ctx->PSSetSamplers(0, 1, pointSampler.GetAddressOf());

            // Build and upload SSAO CB
            struct SSAOCBData {
                glm::mat4  proj;         // 64 bytes
                glm::vec4  noiseScale;   // 16 bytes
                glm::vec4  params;       // 16 bytes
                glm::vec4  kernel[32];   // 512 bytes = 608 total
            } cb{};
            cb.proj       = proj;
            cb.noiseScale = glm::vec4(float(w) / 4.0f, float(h) / 4.0f, 0, 0);
            cb.params     = glm::vec4(ssaoRadius, ssaoBias, ssaoIntensity, float(ssaoMethod));
            std::vector<glm::vec3> kernel = BuildSSAOKernel(32);
            for (int i = 0; i < 32; ++i) cb.kernel[i] = glm::vec4(kernel[i], 0);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(ssaoCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &cb, sizeof(cb));
                ctx->Unmap(ssaoCB.Get(), 0);
            }
            ctx->PSSetConstantBuffers(0, 1, ssaoCB.GetAddressOf());
            ID3D11ShaderResourceView* srvs[2] = { ssaoPosSRV.Get(), ssaoNoiseSRV.Get() };
            ctx->PSSetShaderResources(0, 2, srvs);
            ctx->Draw(3, 0);
            { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }
            ID3D11ShaderResourceView* nullSrvs[2] = {};
            ctx->PSSetShaderResources(0, 2, nullSrvs);
        }

        // ── Pass 3: Blur ──────────────────────────────────────────────────────
        {
            ctx->OMSetRenderTargets(1, ssaoBlurRTV.GetAddressOf(), nullptr);
            ctx->PSSetShader(ssaoBlurPS.Get(), nullptr, 0);
            // Upload blur texel size
            struct { glm::vec2 texel; glm::vec2 pad; } blurCb{};
            blurCb.texel = glm::vec2(1.0f / float(w), 1.0f / float(h));
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(ssaoBlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                std::memcpy(mapped.pData, &blurCb, sizeof(blurCb));
                ctx->Unmap(ssaoBlurCB.Get(), 0);
            }
            ctx->PSSetConstantBuffers(0, 1, ssaoBlurCB.GetAddressOf());
            ID3D11ShaderResourceView* srv = ssaoSRV.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(3, 0);
            { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSrv);
        }

        return ssaoBlurSRV.Get();
    }

    // ─── Ray-traced GI (software BVH) — D3D11 port of the GL-4.3 compute GI ──
    // CPU-built per-mesh BLASes (HE::GiBvh, unit-tested in test_gi_bvh.cpp)
    // concatenated into structured buffers + a flat per-frame instance array;
    // compute shadow rays + DDGI probe gather, temporal + blur as fullscreen
    // pixel passes. Mirrors OpenGLRenderer's kGi* stages 1:1.
    struct GiBlasRange
    {
        int32_t nodeOffset = 0, nodeCount = 0;
        int32_t triOffset  = 0, triCount  = 0;
        bool    valid      = false;
    };
    struct GiInstanceGpu // must match the HLSL GiInst layout (raw structured buffer)
    {
        glm::mat4 invTransform;
        glm::vec4 baseColor;
        int32_t   nodeOffset = 0, triOffset = 0, pad0 = 0, pad1 = 0;
    };
    static constexpr float kGiProbeSpacing     = 4.0f;
    static constexpr int   kGiMaxProbesPerAxis = 10;
    static constexpr int   kGiProbeOctSize     = 8;

    bool  giSupported          = true;  // FL 11.0 guarantees CS 5.0; compile failure clears it
    bool  giEnabled            = false;
    bool  giPipelinesBuilt     = false;
    float giIndirectIntensity  = 1.0f;
    float giLightRadius        = 0.5f;  // degrees, shadow-ray cone
    int   giProbeBudgetPerFrame = 256;

    ComPtr<ID3D11VertexShader>  giGBufVS;
    ComPtr<ID3D11PixelShader>   giGBufPS;
    ComPtr<ID3D11ComputeShader> giShadowCS;
    ComPtr<ID3D11ComputeShader> giProbeCS;
    ComPtr<ID3D11PixelShader>   giTemporalPS;
    ComPtr<ID3D11PixelShader>   giBlurPS;
    ComPtr<ID3D11Buffer>        giShadowCB, giCountCB, giTemporalCB, giBlurCB, giProbeCB;
    ComPtr<ID3D11SamplerState>  giLinearClamp;

    std::unordered_map<HE::UUID, GiBlasRange> giBlasCache;
    std::vector<HE::GiBvhNode>     giNodesCpu;
    std::vector<HE::GiBvhTriangle> giTrisCpu;
    std::vector<GiInstanceGpu>     giInstancesCpu;
    bool giBlasDirty     = false;
    int  giInstanceCount = 0;
    ComPtr<ID3D11Buffer>             giNodeSB, giTriSB, giInstanceSB;
    ComPtr<ID3D11ShaderResourceView> giNodeSRV, giTriSRV, giInstanceSRV;

    int giShadowW = 0, giShadowH = 0;
    ComPtr<ID3D11Texture2D> giGBufPosTex, giGBufNormTex, giGBufDepth, giRawTex,
                            giHistTex[2], giResultTex;
    ComPtr<ID3D11RenderTargetView>    giGBufPosRTV, giGBufNormRTV, giHistRTV[2], giResultRTV;
    ComPtr<ID3D11DepthStencilView>    giGBufDSV;
    ComPtr<ID3D11ShaderResourceView>  giGBufPosSRV, giGBufNormSRV, giRawSRV,
                                      giHistSRV[2], giResultSRV;
    ComPtr<ID3D11UnorderedAccessView> giRawUAV;
    ComPtr<ID3D11Texture2D>           giLocalMaskTex; // RGBA16F per-pixel local-light visibility
    ComPtr<ID3D11ShaderResourceView>  giLocalMaskSRV;
    ComPtr<ID3D11UnorderedAccessView> giLocalMaskUAV;
    int       giHistIdx     = 0;
    bool      giHistValid   = false;
    glm::mat4 giPrevViewProj{ 1.0f };
    float     giFrameSeed   = 0.0f;

    glm::vec3  giGridOrigin{ 0.0f };
    glm::ivec3 giGridCounts{ 0 };
    int  giProbeCount = 0, giProbesPerRow = 0, giProbeCursor = 0;
    bool giProbeGridBuilt = false;
    ComPtr<ID3D11Texture2D>           giIrrTex, giVisTex, giIrrPrevTex, giVisPrevTex;
    ComPtr<ID3D11ShaderResourceView>  giIrrSRV, giVisSRV, giIrrPrevSRV, giVisPrevSRV;
    ComPtr<ID3D11UnorderedAccessView> giIrrUAV, giVisUAV;

    GiBlasRange buildGiBlas(ContentManager* cm, const HE::UUID& meshId)
    {
        GiBlasRange range;
        if (!cm) return range;
        const StaticMeshAsset* asset = cm->getStaticMesh(meshId);
        if (!asset || asset->indices.empty()) return range;

        // Same two layouts resolveMesh uploads: cooked = interleaved 8-float
        // (position at offset 0), loose = tightly packed 3-float positions.
        HE::GiBvh bvh;
        if (asset->cooked && !asset->interleaved.empty())
            bvh = HE::buildGiBvh(asset->interleaved.data(), asset->vertexCount, 8,
                                 asset->indices.data(), asset->indices.size());
        else if (!asset->vertices.empty())
            bvh = HE::buildGiBvh(asset->vertices.data(), asset->vertices.size() / 3, 3,
                                 asset->indices.data(), asset->indices.size());
        if (!bvh.valid()) return range;

        range.nodeOffset = static_cast<int32_t>(giNodesCpu.size());
        range.nodeCount  = static_cast<int32_t>(bvh.nodes.size());
        range.triOffset  = static_cast<int32_t>(giTrisCpu.size());
        range.triCount   = static_cast<int32_t>(bvh.triangles.size());
        range.valid      = true;
        giNodesCpu.insert(giNodesCpu.end(), bvh.nodes.begin(), bvh.nodes.end());
        giTrisCpu.insert(giTrisCpu.end(), bvh.triangles.begin(), bvh.triangles.end());
        giBlasDirty = true;
        return range;
    }

    void updateGiAccel(ContentManager* cm, const RenderWorld& rw)
    {
        giInstanceCount = 0;
        if (!giEnabled || !giSupported) return;

        // Same caster filter as the shadow pass: castsShadow only, UNCULLED —
        // rays go in arbitrary directions, an off-screen caster still occludes.
        giInstancesCpu.clear();
        auto resolveRange = [&](const HE::UUID& id) -> GiBlasRange
        {
            auto it = giBlasCache.find(id);
            if (it == giBlasCache.end())
                it = giBlasCache.emplace(id, buildGiBlas(cm, id)).first;
            return it->second;
        };
        for (const RenderObject& obj : rw.objects)
        {
            if (!obj.castsShadow) continue;
            // Default-cube fallback — entities without a resolvable mesh RENDER
            // as the default cube, so they must occlude as one too.
            GiBlasRange range = resolveRange(obj.meshAssetId);
            if (!range.valid) range = resolveRange(HE::kDefaultCubeMeshId);
            if (!range.valid) continue;
            GiInstanceGpu inst;
            inst.invTransform = glm::inverse(obj.transform);
            inst.baseColor    = glm::vec4(obj.baseColor, 1.0f);
            inst.nodeOffset   = range.nodeOffset;
            inst.triOffset    = range.triOffset;
            giInstancesCpu.push_back(inst);
        }
        giInstanceCount = static_cast<int>(giInstancesCpu.size());
        if (giInstanceCount == 0) return;

        auto makeSB = [&](const void* data, UINT count, UINT strideBytes,
                          ComPtr<ID3D11Buffer>& buf, ComPtr<ID3D11ShaderResourceView>& srv)
        {
            buf.Reset(); srv.Reset();
            if (count == 0) return;
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth           = count * strideBytes;
            bd.Usage               = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
            bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            bd.StructureByteStride = strideBytes;
            D3D11_SUBRESOURCE_DATA init{}; init.pSysMem = data;
            if (FAILED(device->CreateBuffer(&bd, &init, &buf))) return;
            D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format              = DXGI_FORMAT_UNKNOWN; // required for structured SRVs
            sd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
            sd.Buffer.FirstElement = 0;
            sd.Buffer.NumElements  = count;
            device->CreateShaderResourceView(buf.Get(), &sd, &srv);
        };
        // Nodes/tris only when a new BLAS was appended, instances every frame
        // (transforms move; counts are small → IMMUTABLE re-create is fine).
        if (giBlasDirty)
        {
            makeSB(giNodesCpu.data(), static_cast<UINT>(giNodesCpu.size()),
                   sizeof(HE::GiBvhNode), giNodeSB, giNodeSRV);
            makeSB(giTrisCpu.data(), static_cast<UINT>(giTrisCpu.size()),
                   sizeof(HE::GiBvhTriangle), giTriSB, giTriSRV);
            giBlasDirty = false;
        }
        makeSB(giInstancesCpu.data(), static_cast<UINT>(giInstancesCpu.size()),
               sizeof(GiInstanceGpu), giInstanceSB, giInstanceSRV);
        if (!giNodeSRV || !giTriSRV || !giInstanceSRV) giInstanceCount = 0;
    }

    void destroyGiAccel()
    {
        giNodeSB.Reset(); giTriSB.Reset(); giInstanceSB.Reset();
        giNodeSRV.Reset(); giTriSRV.Reset(); giInstanceSRV.Reset();
        giBlasCache.clear();
        giNodesCpu.clear();
        giTrisCpu.clear();
        giInstancesCpu.clear();
        giInstanceCount = 0;
        giBlasDirty     = false;
    }

    // Lazily builds the GI pipelines on the first GI-active frame. A compile
    // failure logs + disables GI for the session (blind-port safety), exactly
    // like the GL port.
    void createGiPipelines()
    {
        if (giPipelinesBuilt) return;
        giPipelinesBuilt = true; // one attempt per session, success or not

        UINT flags = 0;
#if defined(_DEBUG)
        flags |= D3DCOMPILE_DEBUG;
#endif
        auto compile = [&](const std::string& src, const char* entry, const char* profile,
                           ComPtr<ID3DBlob>& blob) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(src.c_str(), src.size(), "gi", nullptr, nullptr,
                                  entry, profile, flags, 0, &blob, &err)))
            {
                Logger::Log(Logger::LogLevel::Error,
                    (std::string("D3D11Renderer: GI shader compile failed (") + entry + "): "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown")).c_str());
                return false;
            }
            return true;
        };
        auto makeCB = [&](UINT bytes, ComPtr<ID3D11Buffer>& cb) -> bool
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = (bytes + 15u) & ~15u;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &cb));
        };

        bool ok = true;
        ComPtr<ID3DBlob> b;
        if (ok && (ok = compile(kGiGBufHLSL, "GiGBufVS", "vs_5_0", b)))
            ok = SUCCEEDED(device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giGBufVS));
        if (ok && (ok = compile(kGiGBufHLSL, "GiGBufPS", "ps_5_0", b)))
            ok = SUCCEEDED(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giGBufPS));
        if (ok && (ok = compile(std::string(kGiTraversalHLSL) + kGiShadowCSHLSL, "GiShadowCS", "cs_5_0", b)))
            ok = SUCCEEDED(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giShadowCS));
        if (ok && (ok = compile(std::string(kGiTraversalHLSL) + kGiProbeCSHLSL, "GiProbeCS", "cs_5_0", b)))
            ok = SUCCEEDED(device->CreateComputeShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giProbeCS));
        if (ok && (ok = compile(kGiTemporalHLSL, "main", "ps_5_0", b)))
            ok = SUCCEEDED(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giTemporalPS));
        if (ok && (ok = compile(kGiBlurHLSL, "main", "ps_5_0", b)))
            ok = SUCCEEDED(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giBlurPS));

        ok = ok && makeCB(7 * 16, giShadowCB) // sunDirRadius + frame + localPosRange[4] + localExtra
                && makeCB(16, giCountCB)
                && makeCB(sizeof(glm::mat4) + 16, giTemporalCB)
                && makeCB(16, giBlurCB)
                && makeCB(6 * 16 + 3 * 8 * 16, giProbeCB);
        if (ok)
        {
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            ok = SUCCEEDED(device->CreateSamplerState(&sd, &giLinearClamp));
        }

        if (!ok)
        {
            Logger::Log(Logger::LogLevel::Error,
                        "D3D11Renderer: GI pipeline build failed — GI disabled");
            giGBufVS.Reset(); giGBufPS.Reset(); giShadowCS.Reset(); giProbeCS.Reset();
            giTemporalPS.Reset(); giBlurPS.Reset();
            giSupported = false;
            return;
        }
        Logger::Log(Logger::LogLevel::Info,
                    "D3D11Renderer: GI pipelines built (compute ray tracing active)");
    }

    void ensureGiShadowTargets(int w, int h)
    {
        w = std::max(1, w); h = std::max(1, h);
        if (giGBufPosTex && w == giShadowW && h == giShadowH) return;
        giShadowW = w; giShadowH = h;
        giHistValid = false; // fresh targets → no usable history

        auto makeTex = [&](DXGI_FORMAT fmt, UINT bind,
                           ComPtr<ID3D11Texture2D>& t,
                           ComPtr<ID3D11RenderTargetView>* rtv,
                           ComPtr<ID3D11ShaderResourceView>* srv,
                           ComPtr<ID3D11UnorderedAccessView>* uav) -> bool
        {
            t.Reset();
            if (rtv) rtv->Reset();
            if (srv) srv->Reset();
            if (uav) uav->Reset();
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w; td.Height = (UINT)h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) return false;
            if (rtv && FAILED(device->CreateRenderTargetView(t.Get(), nullptr, rtv->GetAddressOf()))) return false;
            if (srv && FAILED(device->CreateShaderResourceView(t.Get(), nullptr, srv->GetAddressOf()))) return false;
            if (uav && FAILED(device->CreateUnorderedAccessView(t.Get(), nullptr, uav->GetAddressOf()))) return false;
            return true;
        };

        bool ok = makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                          giGBufPosTex, &giGBufPosRTV, &giGBufPosSRV, nullptr)
               && makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                          giGBufNormTex, &giGBufNormRTV, &giGBufNormSRV, nullptr)
               && makeTex(DXGI_FORMAT_R16_FLOAT,
                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                          giRawTex, nullptr, &giRawSRV, &giRawUAV)
               // Per-pixel local (point/spot) light visibility (1 channel per
               // light, first 4). Deterministic hard rays → no temporal/blur;
               // the scene shader samples it directly at t7.
               && makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                          giLocalMaskTex, nullptr, &giLocalMaskSRV, &giLocalMaskUAV)
               && makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                          giHistTex[0], &giHistRTV[0], &giHistSRV[0], nullptr)
               && makeTex(DXGI_FORMAT_R16G16B16A16_FLOAT,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                          giHistTex[1], &giHistRTV[1], &giHistSRV[1], nullptr)
               && makeTex(DXGI_FORMAT_R16_FLOAT,
                          D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE,
                          giResultTex, &giResultRTV, &giResultSRV, nullptr);
        // Depth buffer for the G-buffer prepass.
        giGBufDSV.Reset(); giGBufDepth.Reset();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = (UINT)w; dd.Height = (UINT)h;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D16_UNORM; dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        ok = ok && SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &giGBufDepth))
                && SUCCEEDED(device->CreateDepthStencilView(giGBufDepth.Get(), nullptr, &giGBufDSV));
        if (!ok)
        {
            giGBufPosTex.Reset();
            giShadowW = giShadowH = 0;
        }
    }

    // One-shot probe-grid fit over the scene AABB (worldBounds are refreshed
    // from the real mesh bounds in DrawScene before this runs).
    void ensureGiProbeGrid(const RenderWorld& rw)
    {
        if (giProbeGridBuilt) return;
        if (rw.objects.empty()) return;

        HE::AABB sceneBox;
        for (const RenderObject& obj : rw.objects)
            if (obj.worldBounds.isValid())
                sceneBox.expand(obj.worldBounds);
        if (!sceneBox.isValid()) return;

        const glm::vec3 padded = sceneBox.extents() + glm::vec3(kGiProbeSpacing);
        giGridCounts = glm::ivec3(
            std::clamp(static_cast<int>(std::ceil(padded.x * 2.0f / kGiProbeSpacing)) + 1, 2, kGiMaxProbesPerAxis),
            std::clamp(static_cast<int>(std::ceil(padded.y * 2.0f / kGiProbeSpacing)) + 1, 2, kGiMaxProbesPerAxis),
            std::clamp(static_cast<int>(std::ceil(padded.z * 2.0f / kGiProbeSpacing)) + 1, 2, kGiMaxProbesPerAxis));
        const glm::vec3 gridSpan = glm::vec3(giGridCounts - 1) * kGiProbeSpacing;
        giGridOrigin   = sceneBox.center() - gridSpan * 0.5f;
        giProbeCount   = giGridCounts.x * giGridCounts.y * giGridCounts.z;
        giProbesPerRow = std::min(giProbeCount, 32);
        giProbeCursor  = 0;
        giProbeGridBuilt = true;
        Logger::Log(Logger::LogLevel::Info,
                    ("D3D11Renderer: GI probe grid " + std::to_string(giGridCounts.x) + "x"
                     + std::to_string(giGridCounts.y) + "x" + std::to_string(giGridCounts.z)
                     + " (" + std::to_string(giProbeCount) + " probes)").c_str());
    }

    void ensureGiProbeAtlas()
    {
        if (giIrrTex || giProbeCount <= 0) return;
        const int rows = (giProbeCount + giProbesPerRow - 1) / giProbesPerRow;
        const int w = giProbesPerRow * kGiProbeOctSize;
        const int h = rows * kGiProbeOctSize;

        // Zero-initialised: the probe kernel EMA-blends against the previous
        // value, so undefined contents would poison the first update round.
        auto makeAtlas = [&](DXGI_FORMAT fmt, UINT texelBytes, UINT bind,
                             ComPtr<ID3D11Texture2D>& t,
                             ComPtr<ID3D11ShaderResourceView>& srv,
                             ComPtr<ID3D11UnorderedAccessView>* uav) -> bool
        {
            std::vector<uint8_t> zeros(static_cast<size_t>(w) * h * texelBytes, 0);
            D3D11_TEXTURE2D_DESC td{};
            td.Width = (UINT)w; td.Height = (UINT)h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = bind;
            D3D11_SUBRESOURCE_DATA init{};
            init.pSysMem = zeros.data();
            init.SysMemPitch = (UINT)w * texelBytes;
            if (FAILED(device->CreateTexture2D(&td, &init, &t))) return false;
            if (FAILED(device->CreateShaderResourceView(t.Get(), nullptr, &srv))) return false;
            if (uav && FAILED(device->CreateUnorderedAccessView(t.Get(), nullptr, uav->GetAddressOf()))) return false;
            return true;
        };
        const bool ok =
               makeAtlas(DXGI_FORMAT_R16G16B16A16_FLOAT, 8,
                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                         giIrrTex, giIrrSRV, &giIrrUAV)
            && makeAtlas(DXGI_FORMAT_R16G16_FLOAT, 4,
                         D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_SHADER_RESOURCE,
                         giVisTex, giVisSRV, &giVisUAV)
            // SRV-only copies of the previous frame (typed UAV loads of these
            // formats are an optional 11.3 cap → the kernel reads SRVs instead).
            && makeAtlas(DXGI_FORMAT_R16G16B16A16_FLOAT, 8, D3D11_BIND_SHADER_RESOURCE,
                         giIrrPrevTex, giIrrPrevSRV, nullptr)
            && makeAtlas(DXGI_FORMAT_R16G16_FLOAT, 4, D3D11_BIND_SHADER_RESOURCE,
                         giVisPrevTex, giVisPrevSRV, nullptr);
        if (!ok)
        {
            giIrrTex.Reset(); giVisTex.Reset(); giIrrPrevTex.Reset(); giVisPrevTex.Reset();
            giIrrSRV.Reset(); giVisSRV.Reset(); giIrrPrevSRV.Reset(); giVisPrevSRV.Reset();
            giIrrUAV.Reset(); giVisUAV.Reset();
        }
    }

    void destroyGiTargets()
    {
        giGBufPosTex.Reset(); giGBufNormTex.Reset(); giGBufDepth.Reset(); giRawTex.Reset();
        giGBufPosRTV.Reset(); giGBufNormRTV.Reset(); giGBufDSV.Reset();
        giGBufPosSRV.Reset(); giGBufNormSRV.Reset(); giRawSRV.Reset(); giRawUAV.Reset();
        giLocalMaskTex.Reset(); giLocalMaskSRV.Reset(); giLocalMaskUAV.Reset();
        for (int i = 0; i < 2; ++i)
        { giHistTex[i].Reset(); giHistRTV[i].Reset(); giHistSRV[i].Reset(); }
        giResultTex.Reset(); giResultRTV.Reset(); giResultSRV.Reset();
        giShadowW = giShadowH = 0;
        giHistValid = false;
        giIrrTex.Reset(); giVisTex.Reset(); giIrrPrevTex.Reset(); giVisPrevTex.Reset();
        giIrrSRV.Reset(); giVisSRV.Reset(); giIrrPrevSRV.Reset(); giVisPrevSRV.Reset();
        giIrrUAV.Reset(); giVisUAV.Reset();
        giProbeGridBuilt = false;
        giProbeCount = 0;
        giProbeCursor = 0;
    }

    // The 4-stage shadow-mask pipeline (G-buffer → compute rays → temporal →
    // blur). Returns the SRV the scene shader binds at t4 (null on failure).
    ID3D11ShaderResourceView* runGiShadow(ID3D11DeviceContext* ctx,
                                          const std::vector<const DrawCall*>& opaqueDCs,
                                          const glm::mat4& viewProj, int w, int h,
                                          const RenderWorld& rw,
                                          const std::function<const GpuMesh*(HE::UUID)>& resolveMeshFn,
                                          const GpuMesh& fallbackMesh,
                                          ID3D11InputLayout* il,
                                          ID3D11DepthStencilState* depthSt,
                                          ID3D11RasterizerState* rasterSt)
    {
        createGiPipelines();
        if (!giGBufVS || !giShadowCS || !giTemporalPS || !giBlurPS) return nullptr;
        ensureGiShadowTargets(w, h);
        if (!giGBufPosTex) return nullptr;

        const UINT stride = 8 * sizeof(float), off = 0;
        D3D11_VIEWPORT vp{}; vp.Width = float(giShadowW); vp.Height = float(giShadowH); vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        // ── 1. World-space G-buffer (position + normal MRT, half-res). Same
        // draw set + camera as the scene pass (the aspect/misalign lesson).
        {
            ID3D11ShaderResourceView* nullSrvs[4] = {};
            ctx->PSSetShaderResources(4, 4, nullSrvs); // t4-t7 may still hold last frame's GI (t7 = local mask, next bound as UAV)
            ID3D11RenderTargetView* rtvs[2] = { giGBufPosRTV.Get(), giGBufNormRTV.Get() };
            ctx->OMSetRenderTargets(2, rtvs, giGBufDSV.Get());
            const float clear[4] = { 0, 0, 0, 0 }; // a = 0 → background
            ctx->ClearRenderTargetView(giGBufPosRTV.Get(), clear);
            ctx->ClearRenderTargetView(giGBufNormRTV.Get(), clear);
            ctx->ClearDepthStencilView(giGBufDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->IASetInputLayout(il);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(giGBufVS.Get(), nullptr, 0);
            ctx->PSSetShader(giGBufPS.Get(), nullptr, 0);
            ctx->OMSetDepthStencilState(depthSt, 0);
            ctx->RSSetState(rasterSt);
            ctx->VSSetConstantBuffers(0, 1, perObjectCB.GetAddressOf());

            for (const DrawCall* dc : opaqueDCs)
            {
                if (!dc->contributesAO) continue; // precip/particles don't shade the mask
                const GpuMesh* mesh = resolveMeshFn(dc->meshAssetId);
                const GpuMesh& m = mesh ? *mesh : fallbackMesh;
                if (!m.vbuf || !m.ibuf) continue;
                ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &off);
                ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
                auto drawOne = [&](const glm::mat4& t)
                {
                    PerObjectCB o{};
                    o.mvp = viewProj * t; o.model = t;
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(perObjectCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
                    {
                        std::memcpy(mapped.pData, &o, sizeof(o));
                        ctx->Unmap(perObjectCB.Get(), 0);
                    }
                    ctx->DrawIndexed(m.indexCount, 0, 0);
                };
                if (!dc->instanceTransforms.empty())
                    for (const glm::mat4& t : dc->instanceTransforms) drawOne(t);
                else
                    drawOne(dc->transform);
            }
            ID3D11RenderTargetView* nulls[2] = {};
            ctx->OMSetRenderTargets(2, nulls, nullptr);
        }

        // ── 2. Shadow rays (compute, 1 cone-jittered ray/pixel vs the BVH) ──
        {
            glm::vec3 towardLight, lightColorIntensity;
            d3d11DominantDirectionalLight(rw, towardLight, lightColorIntensity);
            giFrameSeed += 1.0f;
            struct { glm::vec4 sunDirRadius, frame, localPosRange[4], localExtra; } scb{};
            scb.sunDirRadius = glm::vec4(towardLight, glm::radians(giLightRadius));
            scb.frame        = glm::vec4(giFrameSeed, float(giShadowW), float(giShadowH), 0.0f);
            // First 4 local (point/spot) lights of the same 8-light window the
            // scene shader iterates — PSMain counts non-directional lights in
            // the SAME order to index the mask channels, so count every
            // type != 0 light exactly like its loop does, fill the first 4.
            {
                int localCount = 0;
                const int windowCount = std::min(static_cast<int>(rw.lights.size()), 8);
                for (int li = 0; li < windowCount; ++li)
                {
                    const LightData& l = rw.lights[li];
                    if (l.type == 0) continue;
                    if (localCount < 4)
                        scb.localPosRange[localCount] = glm::vec4(l.position, std::max(l.range, 1e-4f));
                    ++localCount;
                }
                scb.localExtra = glm::vec4(float(std::min(localCount, 4)), 0.0f, 0.0f, 0.0f);
            }
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(giShadowCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            { std::memcpy(mapped.pData, &scb, sizeof(scb)); ctx->Unmap(giShadowCB.Get(), 0); }
            glm::ivec4 cnt(giInstanceCount, 0, 0, 0);
            if (SUCCEEDED(ctx->Map(giCountCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            { std::memcpy(mapped.pData, &cnt, sizeof(cnt)); ctx->Unmap(giCountCB.Get(), 0); }

            ctx->CSSetShader(giShadowCS.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[5] = { giGBufPosSRV.Get(), giGBufNormSRV.Get(),
                                                  giNodeSRV.Get(), giTriSRV.Get(), giInstanceSRV.Get() };
            ctx->CSSetShaderResources(0, 5, srvs);
            ID3D11Buffer* cbs[2] = { giShadowCB.Get(), giCountCB.Get() };
            ctx->CSSetConstantBuffers(0, 2, cbs);
            ID3D11UnorderedAccessView* uavs[2] = { giRawUAV.Get(), giLocalMaskUAV.Get() };
            ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
            ctx->Dispatch((UINT)((giShadowW + 7) / 8), (UINT)((giShadowH + 7) / 8), 1);
            ID3D11UnorderedAccessView* nullUavs[2] = {};
            ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
            ID3D11ShaderResourceView* nullSrvs[5] = {};
            ctx->CSSetShaderResources(0, 5, nullSrvs);
            ctx->CSSetShader(nullptr, nullptr, 0);
        }

        // ── 3. Temporal accumulation (fullscreen, ping-pong history) ────────
        const int curIdx = giHistIdx, prevIdx = 1 - curIdx;
        {
            ctx->OMSetRenderTargets(1, giHistRTV[curIdx].GetAddressOf(), nullptr);
            ctx->IASetInputLayout(nullptr);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(fsVS.Get(), nullptr, 0);
            ctx->PSSetShader(giTemporalPS.Get(), nullptr, 0);
            ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
            ctx->RSSetState(fsRastState.Get());
            ctx->PSSetSamplers(0, 1, pointSampler.GetAddressOf());
            struct { glm::mat4 prevViewProj; glm::vec4 params; } tcb{};
            tcb.prevViewProj = giPrevViewProj;
            tcb.params = glm::vec4(giHistValid ? 0.9f : 0.0f,
                                   float(giShadowW), float(giShadowH), 0.0f);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(giTemporalCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            { std::memcpy(mapped.pData, &tcb, sizeof(tcb)); ctx->Unmap(giTemporalCB.Get(), 0); }
            ctx->PSSetConstantBuffers(0, 1, giTemporalCB.GetAddressOf());
            ID3D11ShaderResourceView* srvs[3] = { giGBufPosSRV.Get(), giRawSRV.Get(),
                                                  giHistSRV[prevIdx].Get() };
            ctx->PSSetShaderResources(0, 3, srvs);
            ctx->Draw(3, 0);
            ID3D11RenderTargetView* n = nullptr;
            ctx->OMSetRenderTargets(1, &n, nullptr);
            ID3D11ShaderResourceView* nullSrvs[3] = {};
            ctx->PSSetShaderResources(0, 3, nullSrvs);
        }
        giHistValid    = true;
        giHistIdx      = prevIdx;
        giPrevViewProj = viewProj; // for NEXT frame's reprojection

        // ── 4. Spatial blur → the mask the scene shader samples ─────────────
        {
            ctx->OMSetRenderTargets(1, giResultRTV.GetAddressOf(), nullptr);
            ctx->PSSetShader(giBlurPS.Get(), nullptr, 0);
            glm::vec4 texel(1.0f / float(giShadowW), 1.0f / float(giShadowH), 0.0f, 0.0f);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(giBlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            { std::memcpy(mapped.pData, &texel, sizeof(texel)); ctx->Unmap(giBlurCB.Get(), 0); }
            ctx->PSSetConstantBuffers(0, 1, giBlurCB.GetAddressOf());
            ID3D11ShaderResourceView* srv = giHistSRV[curIdx].Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->Draw(3, 0);
            ID3D11RenderTargetView* n = nullptr;
            ctx->OMSetRenderTargets(1, &n, nullptr);
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(0, 1, &nullSrv);
        }
        return giResultSRV.Get();
    }

    void dispatchGiProbeUpdate(ID3D11DeviceContext* ctx, const RenderWorld& rw)
    {
        if (!giProbeCS || giInstanceCount == 0) return;
        ensureGiProbeGrid(rw);
        if (!giProbeGridBuilt) return;
        ensureGiProbeAtlas();
        if (!giIrrUAV || !giVisUAV) return;

        const int budget = std::min(giProbeBudgetPerFrame > 0 ? giProbeBudgetPerFrame : 1,
                                    giProbeCount);

        // Previous-frame values travel as SRV copies (no typed UAV loads on
        // baseline 11.0); texels outside this batch keep their values in the
        // canonical atlases since the kernel never writes them.
        ctx->CopyResource(giIrrPrevTex.Get(), giIrrTex.Get());
        ctx->CopyResource(giVisPrevTex.Get(), giVisTex.Get());

        struct GiProbeCBData
        {
            glm::vec4 gridOrigin, gridCounts, rayParams, sunDirRadius, sunColor, skyAmbient;
            glm::vec4 lightPosRange[8], lightColorType[8], lightDirCos[8];
        } pcb{};
        pcb.gridOrigin = glm::vec4(giGridOrigin, kGiProbeSpacing);
        pcb.gridCounts = glm::vec4(glm::vec3(giGridCounts), float(giProbesPerRow));
        const float maxDist = glm::length(glm::vec3(giGridCounts) * kGiProbeSpacing) + kGiProbeSpacing;
        pcb.rayParams = glm::vec4(maxDist, 0.92f, float(giProbeCursor), float(budget));
        glm::vec3 towardLight, lightColorIntensity;
        d3d11DominantDirectionalLight(rw, towardLight, lightColorIntensity);
        int lightCount = 0;
        for (const LightData& l : rw.lights)
        {
            if (lightCount >= 8) break;
            if ((l.type != 1 && l.type != 2) || l.intensity <= 0.0f) continue;
            pcb.lightPosRange[lightCount]  = glm::vec4(l.position, std::max(l.range, 1e-4f));
            pcb.lightColorType[lightCount] = glm::vec4(l.color * l.intensity, float(l.type));
            pcb.lightDirCos[lightCount]    = glm::vec4(l.direction, l.spotAngleCos);
            ++lightCount;
        }
        pcb.sunDirRadius = glm::vec4(towardLight, float(lightCount));
        pcb.sunColor     = glm::vec4(lightColorIntensity, 0.0f);
        pcb.skyAmbient   = glm::vec4(rw.ambient, 0.0f);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(giProbeCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        { std::memcpy(mapped.pData, &pcb, sizeof(pcb)); ctx->Unmap(giProbeCB.Get(), 0); }
        glm::ivec4 cnt(giInstanceCount, 0, 0, 0);
        if (SUCCEEDED(ctx->Map(giCountCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        { std::memcpy(mapped.pData, &cnt, sizeof(cnt)); ctx->Unmap(giCountCB.Get(), 0); }

        ctx->CSSetShader(giProbeCS.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[7] = { nullptr, nullptr,
                                              giNodeSRV.Get(), giTriSRV.Get(), giInstanceSRV.Get(),
                                              giIrrPrevSRV.Get(), giVisPrevSRV.Get() };
        ctx->CSSetShaderResources(0, 7, srvs);
        ID3D11Buffer* cbs[2] = { giProbeCB.Get(), giCountCB.Get() };
        ctx->CSSetConstantBuffers(0, 2, cbs);
        ID3D11UnorderedAccessView* uavs[2] = { giIrrUAV.Get(), giVisUAV.Get() };
        ctx->CSSetUnorderedAccessViews(0, 2, uavs, nullptr);
        ctx->Dispatch((UINT)budget, 1, 1);
        ID3D11UnorderedAccessView* nullUavs[2] = {};
        ctx->CSSetUnorderedAccessViews(0, 2, nullUavs, nullptr);
        ID3D11ShaderResourceView* nullSrvs[7] = {};
        ctx->CSSetShaderResources(0, 7, nullSrvs);
        ctx->CSSetShader(nullptr, nullptr, 0);

        giProbeCursor = (giProbeCursor + budget) % giProbeCount;
    }

    bool createPostFX()
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
                    (std::string("D3D11 PostFX '") + entry + "' failed: "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsB, tmB, fxB, brB, blB;
        if (!compile(kFSTriangleVS,   "main", "vs_5_0", vsB)) return false;
        if (!compile(kTonemapHLSL,    "main", "ps_5_0", tmB)) return false;
        if (!compile(kFxaaHLSL,       "main", "ps_5_0", fxB)) return false;
        if (!compile(kBloomBrightHLSL,"main", "ps_5_0", brB)) return false;
        if (!compile(kBloomBlurHLSL,  "main", "ps_5_0", blB)) return false;

        device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &fsVS);
        device->CreatePixelShader (tmB->GetBufferPointer(), tmB->GetBufferSize(), nullptr, &tonemapPS);
        device->CreatePixelShader (fxB->GetBufferPointer(), fxB->GetBufferSize(), nullptr, &fxaaPS);
        device->CreatePixelShader (brB->GetBufferPointer(), brB->GetBufferSize(), nullptr, &bloomBrightPS);
        device->CreatePixelShader (blB->GetBufferPointer(), blB->GetBufferSize(), nullptr, &bloomBlurPS);

        { D3D11_SAMPLER_DESC sd{};
          sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
          sd.MaxLOD = D3D11_FLOAT32_MAX;
          device->CreateSamplerState(&sd, &linearSampler); }

        { D3D11_DEPTH_STENCIL_DESC ds{};
          ds.DepthEnable = FALSE;
          device->CreateDepthStencilState(&ds, &noDepthDSS); }

        { D3D11_RASTERIZER_DESC rd{};
          rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE;
          device->CreateRasterizerState(&rd, &fsRastState); }

        { D3D11_BUFFER_DESC bd{};
          bd.ByteWidth = 16u; bd.Usage = D3D11_USAGE_DYNAMIC;
          bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
          device->CreateBuffer(&bd, nullptr, &postFxCB); }

        postFxReady = fsVS && tonemapPS && fxaaPS && bloomBrightPS && bloomBlurPS
                   && linearSampler && noDepthDSS && fsRastState && postFxCB;
        return postFxReady;
    }

    void updatePostFxCB(const float (&data)[4])
    {
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(postFxCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, data, 16); context->Unmap(postFxCB.Get(), 0); }
    }

    // Bright-pass + 10-pass ping-pong blur.  Returns the SRV of the bloom result
    // (bloomTex[0]) or dummyTexture if bloom resources are missing.
    ID3D11ShaderResourceView* runBloom(uint32_t bw, uint32_t bh)
    {
        if (!bloomBrightPS || !bloomBlurPS || !bloomTex[0]) return dummyTexture.Get();
        auto* ctx = context.Get();

        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(fsVS.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
        ctx->RSSetState(fsRastState.Get());
        ctx->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ctx->VSSetConstantBuffers(0, 1, postFxCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, postFxCB.GetAddressOf());
        D3D11_VIEWPORT bvp{}; bvp.Width = float(bw); bvp.Height = float(bh); bvp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &bvp);

        // Bright pass: hdrSRV → bloomTex[0]
        { const float cb[4] = { bloomThreshold, bloomKnee, 0, 0 };
          updatePostFxCB(cb);
          ctx->OMSetRenderTargets(1, bloomRTV[0].GetAddressOf(), nullptr);
          ctx->PSSetShader(bloomBrightPS.Get(), nullptr, 0);
          ID3D11ShaderResourceView* s = hdrSRV.Get();
          ctx->PSSetShaderResources(0, 1, &s);
          ctx->Draw(3, 0); }
        { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }

        // 10 ping-pong Gaussian blur passes (5H + 5V); result lands in bloomTex[0].
        ctx->PSSetShader(bloomBlurPS.Get(), nullptr, 0);
        const float tw = 1.0f / float(bw), th = 1.0f / float(bh);
        bool horiz = true;
        for (int p = 0; p < 10; ++p)
        {
            const int dst = horiz ? 1 : 0, src = horiz ? 0 : 1;
            const float cb[4] = { tw, th, horiz ? 1.0f : 0.0f, 0.0f };
            updatePostFxCB(cb);
            ctx->OMSetRenderTargets(1, bloomRTV[dst].GetAddressOf(), nullptr);
            ID3D11ShaderResourceView* s = bloomSRV[src].Get();
            ctx->PSSetShaderResources(0, 1, &s);
            ctx->Draw(3, 0);
            { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }
            horiz = !horiz;
        }
        return bloomSRV[0].Get();
    }

    void createViewportRT(uint32_t w, uint32_t h)
    {
        viewportRTV.Reset(); viewportSRV.Reset(); viewportTex.Reset();
        viewportDSV.Reset(); viewportDepth.Reset();

        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h;
        td.MipLevels = td.ArraySize = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage    = D3D11_USAGE_DEFAULT;
        td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &viewportTex))) return;
        device->CreateRenderTargetView(viewportTex.Get(), nullptr, &viewportRTV);
        device->CreateShaderResourceView(viewportTex.Get(), nullptr, &viewportSRV);

        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = w; dd.Height = h;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage    = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &viewportDepth))) return;
        device->CreateDepthStencilView(viewportDepth.Get(), nullptr, &viewportDSV);

        viewportW = w;
        viewportH = h;
        createHDRTargets(w, h);
    }

    GpuMesh cube;

    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    FrustumCuller   m_culler;
    RenderSorter    m_sorter;
    RenderGraph     m_renderGraph;
    CommandBuffer   m_cmds;
    std::vector<uint8_t>  m_visible;
    std::vector<uint32_t> m_sortedIndices;
    std::unordered_map<HE::UUID, GpuMesh> meshCache;

    // ── MaterialComponent override + hot-reload (A2) ─────────────────────────
    // Override-material base-color textures cached by material UUID (parallel to the mesh's
    // baked texture): a draw's dc.materialAssetId, when its material is loaded, wins over the
    // mesh's baked texture — mirrors GL/Metal. srv==null caches the "loaded, no texture" result
    // (flat) so it isn't re-resolved every frame. Editor edits push UUIDs to the pending lists,
    // drained at DrawScene top; dropping the ComPtr is GPU-safe (the D3D11 runtime defers the
    // release until the GPU is done), so no manual retire is needed unlike D3D12/Vulkan.
    struct MaterialTex { ComPtr<ID3D11Texture2D> tex; ComPtr<ID3D11ShaderResourceView> srv; };
    std::unordered_map<HE::UUID, MaterialTex> materialTexCache;
    std::vector<HE::UUID> pendingMatInval;
    std::vector<HE::UUID> pendingMeshInval;

    void createRTV()
    {
        ComPtr<ID3D11Texture2D> bb;
        swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(bb.GetAddressOf()));
        device->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    }

    void createDepth(int w, int h)
    {
        dsv.Reset();
        depthTex.Reset();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width            = static_cast<UINT>(w);
        dd.Height           = static_cast<UINT>(h);
        dd.MipLevels        = 1;
        dd.ArraySize        = 1;
        dd.Format           = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage            = D3D11_USAGE_DEFAULT;
        dd.BindFlags        = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &depthTex))) return;
        device->CreateDepthStencilView(depthTex.Get(), nullptr, &dsv);
    }

    bool createPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsBlob, psBlob, err;
        const std::string sceneSource = std::string(kSkyFuncHLSL) + kSceneHLSL;
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "VSMain", "vs_5_0", flags, 0, &vsBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D11Renderer: VS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &psBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D11Renderer: PS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &vs);
        device->CreatePixelShader (psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &ps);

        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        device->CreateInputLayout(layout, 3, vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), &inputLayout);

        // Depth-only vertex shader for the shadow pass.
        ComPtr<ID3DBlob> dvsBlob;
        if (SUCCEEDED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                                 "VSDepth", "vs_5_0", flags, 0, &dvsBlob, &err)))
            device->CreateVertexShader(dvsBlob->GetBufferPointer(), dvsBlob->GetBufferSize(), nullptr, &depthVS);

        // Instanced geometry VS (A3) + the per-instance {mvp,model} structured buffer
        // it reads at t3 (dynamic, refilled per instanced batch via MAP_WRITE_DISCARD).
        ComPtr<ID3DBlob> ivsBlob;
        if (SUCCEEDED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                                 "VSMainInstanced", "vs_5_0", flags, 0, &ivsBlob, &err)))
        {
            device->CreateVertexShader(ivsBlob->GetBufferPointer(), ivsBlob->GetBufferSize(), nullptr, &vsInstanced);
            D3D11_BUFFER_DESC ibd{};
            ibd.ByteWidth           = k_maxInstances * k_instStride;
            ibd.Usage               = D3D11_USAGE_DYNAMIC;
            ibd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
            ibd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
            ibd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
            ibd.StructureByteStride = k_instStride;
            if (SUCCEEDED(device->CreateBuffer(&ibd, nullptr, &instanceSB)))
            {
                D3D11_SHADER_RESOURCE_VIEW_DESC isd{};
                isd.Format              = DXGI_FORMAT_UNKNOWN; // required for a structured-buffer SRV
                isd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
                isd.Buffer.FirstElement = 0;
                isd.Buffer.NumElements  = k_maxInstances;
                device->CreateShaderResourceView(instanceSB.Get(), &isd, &instanceSRV);
            }
        }
        else
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D11Renderer: VSMainInstanced compile "
                "failed: ") + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
        }

        // Shadow map: R32_TYPELESS so it can be both a depth target and an SRV.
        {
            D3D11_TEXTURE2D_DESC sd{};
            sd.Width = sd.Height = static_cast<UINT>(shadowSize);
            sd.MipLevels = 1; sd.ArraySize = 1;
            sd.Format = DXGI_FORMAT_R32_TYPELESS;
            sd.SampleDesc.Count = 1;
            sd.Usage = D3D11_USAGE_DEFAULT;
            sd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
            if (SUCCEEDED(device->CreateTexture2D(&sd, nullptr, &shadowTex)))
            {
                D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
                dvd.Format        = DXGI_FORMAT_D32_FLOAT;
                dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                device->CreateDepthStencilView(shadowTex.Get(), &dvd, &shadowDSV);

                D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
                svd.Format              = DXGI_FORMAT_R32_FLOAT;
                svd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
                svd.Texture2D.MipLevels = 1;
                device->CreateShaderResourceView(shadowTex.Get(), &svd, &shadowSRV);
            }
        }

        auto makeCB = [&](UINT bytes, ComPtr<ID3D11Buffer>& out)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = (bytes + 15u) & ~15u; // 16-byte multiple
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&bd, nullptr, &out);
        };
        makeCB(sizeof(PerObjectCB), perObjectCB);
        makeCB(sizeof(PerFrameCB),  perFrameCB);

        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sd, &sampler);

        D3D11_DEPTH_STENCIL_DESC dsd{};
        dsd.DepthEnable    = TRUE;
        dsd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
        dsd.DepthFunc      = D3D11_COMPARISON_LESS;
        device->CreateDepthStencilState(&dsd, &depthState);

        { D3D11_DEPTH_STENCIL_DESC ro = dsd;
          ro.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
          device->CreateDepthStencilState(&ro, &depthReadOnlyState); }

        { D3D11_BLEND_DESC bd{};
          auto& rt = bd.RenderTarget[0];
          rt.BlendEnable    = TRUE;
          rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
          rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
          rt.BlendOp        = D3D11_BLEND_OP_ADD;
          rt.SrcBlendAlpha  = D3D11_BLEND_ONE;
          rt.DestBlendAlpha = D3D11_BLEND_ZERO;
          rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
          rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
          device->CreateBlendState(&bd, &alphaBlendState); }

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode = D3D11_FILL_SOLID;
        rd.CullMode = D3D11_CULL_NONE; // meshes aren't guaranteed a consistent winding
        rd.DepthClipEnable = TRUE;
        device->CreateRasterizerState(&rd, &rasterState);

        // 1×1 white fallback texture so the sampler always has something bound.
        {
            const uint32_t white = 0xFFFFFFFFu;
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 1; td.MipLevels = td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = &white; srd.SysMemPitch = 4;
            ComPtr<ID3D11Texture2D> tex;
            if (SUCCEEDED(device->CreateTexture2D(&td, &srd, &tex)))
                device->CreateShaderResourceView(tex.Get(), nullptr, &dummyTexture);
        }
        createPostFX();
        createSSAOPipeline();
        createSkyPipeline();
        createDebugLinePipeline();
        createSkinnedPipeline();
        createUIPipeline();
        createMaterialResources(); // A4: node-graph material CBs + sampler (no-op w/o HE_HAVE_SHADERC)
        return vs && ps && inputLayout && perObjectCB && perFrameCB && sampler;
    }

    void uploadBuffers(GpuMesh& mesh, const std::vector<float>& interleaved,
                       const std::vector<uint32_t>& indices)
    {
        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = static_cast<UINT>(interleaved.size() * sizeof(float));
        vbd.Usage     = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit{}; vinit.pSysMem = interleaved.data();
        device->CreateBuffer(&vbd, &vinit, &mesh.vbuf);

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        ibd.Usage     = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA iinit{}; iinit.pSysMem = indices.data();
        device->CreateBuffer(&ibd, &iinit, &mesh.ibuf);

        mesh.indexCount = static_cast<UINT>(indices.size());
    }

    void createCube()
    {
        // pos3 + normal3 + uv2, matching the shared interleaved layout.
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
        uploadBuffers(cube, verts, indices);
        cube.localBounds.expand({ -0.5f, -0.5f, -0.5f });
        cube.localBounds.expand({  0.5f,  0.5f,  0.5f });
    }

    // Create an immutable base-color texture + SRV from a cooked TextureAsset — RGBA8
    // or a block format (BC7/BC3) — with its full pre-baked mip chain (one immutable
    // subresource per level). Returns a null SRV when the asset is unusable or this
    // device can't sample the shipped format (→ flat). Shared by every base-color
    // upload site (static/skeletal mesh, override material). Block formats need no
    // runtime mip generation; the cook baked every level.
    ComPtr<ID3D11ShaderResourceView> createAlbedoSRV(const TextureAsset* tex)
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
            return srv;

        DXGI_FORMAT fmt; bool isBlock; UINT blockBytes = 16;
        switch (tex->format)
        {
        case TextureFormat::RGBA8: fmt = DXGI_FORMAT_R8G8B8A8_UNORM; isBlock = false; break;
        case TextureFormat::BC7:   fmt = DXGI_FORMAT_BC7_UNORM;      isBlock = true;  break;
        case TextureFormat::BC3:   fmt = DXGI_FORMAT_BC3_UNORM;      isBlock = true;  break;
        default: return srv; // ASTC / unknown → D3D can't sample it
        }
        // BC is core on FL11, but stay defensive: skip if the driver can't sample it.
        if (isBlock)
        {
            UINT sup = 0;
            if (FAILED(device->CheckFormatSupport(fmt, &sup)) ||
                !(sup & D3D11_FORMAT_SUPPORT_TEXTURE2D))
                return srv;
        }

        const UINT mips = tex->mipLevels > 0 ? tex->mipLevels : 1;
        // One immutable subresource per mip (level 0 first). Row pitch: block formats
        // are blocks-per-row × 16 B; RGBA8 is width × 4 B.
        std::vector<D3D11_SUBRESOURCE_DATA> srd(mips);
        size_t off = 0; UINT lw = static_cast<UINT>(tex->width), lh = static_cast<UINT>(tex->height);
        for (UINT l = 0; l < mips; ++l)
        {
            const UINT rowPitch = isBlock ? ((lw + 3) / 4) * blockBytes : lw * 4;
            const size_t bytes  = isBlock ? static_cast<size_t>((lw + 3) / 4) * ((lh + 3) / 4) * blockBytes
                                          : static_cast<size_t>(lw) * lh * 4;
            if (off + bytes > tex->data.size()) return {}; // truncated payload
            srd[l].pSysMem          = tex->data.data() + off;
            srd[l].SysMemPitch      = rowPitch;
            srd[l].SysMemSlicePitch = 0;
            off += bytes; lw = lw > 1 ? (lw >> 1) : 1; lh = lh > 1 ? (lh >> 1) : 1;
        }

        D3D11_TEXTURE2D_DESC td{};
        td.Width = static_cast<UINT>(tex->width); td.Height = static_cast<UINT>(tex->height);
        td.MipLevels = mips; td.ArraySize = 1;
        td.Format = fmt; td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> t;
        if (SUCCEEDED(device->CreateTexture2D(&td, srd.data(), &t)))
            device->CreateShaderResourceView(t.Get(), nullptr, &srv);
        return srv;
    }

    // Resolve an OVERRIDE material's base-color texture (dc.materialAssetId), cached by material
    // UUID. Returns true iff the material asset is loaded (outSrv is its SRV, or null when the
    // override material has no texture → flat, NOT the baked texture — exactly like GL); false
    // only while the material asset isn't loaded (retry next frame). Mirrors GL's
    // ResolveMaterialTexture: getMaterial + cache even the no-texture result.
    bool resolveMaterialOverride(const HE::UUID& materialId, ContentManager* cm,
                                 ID3D11ShaderResourceView*& outSrv)
    {
        outSrv = nullptr;
        if (materialId == HE::UUID{} || !cm) return false;
        if (auto it = materialTexCache.find(materialId); it != materialTexCache.end())
        { outSrv = it->second.srv.Get(); return true; }
        const MaterialAsset* mat = cm->getMaterial(materialId);
        if (!mat) return false; // not loaded yet — retry next frame without caching
        MaterialTex entry;
        const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
        const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
        // RGBA8 + cooked BC7/BC3 with the pre-baked mip chain (skips a block format
        // this device can't sample).
        entry.srv = createAlbedoSRV(cm->resolveTextureRef(texId0, texPath0));
        outSrv = entry.srv.Get(); // null when the override material has no usable texture
        materialTexCache.emplace(materialId, std::move(entry));
        return true;
    }

    // ── A4: node-graph material resources ────────────────────────────────────
    // Three dynamic constant buffers (HeLighting 64 B, U 176 B, HeParams 256 B) filled via
    // Map(WRITE_DISCARD) exactly like the built-in perObject/perFrame CBs, plus a linear-wrap
    // sampler for heTex0 + heTexP0..3. No PSO/root-sig — D3D11 sets shaders/CBs/SRVs/samplers
    // individually. No-op (m_matReady stays false) when HE_HAVE_SHADERC is off.
    void createMaterialResources()
    {
#if defined(HE_HAVE_SHADERC)
        auto makeCB = [&](UINT bytes, ComPtr<ID3D11Buffer>& out) -> bool {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = (bytes + 15u) & ~15u; // 16-byte multiple (64/176/256 already aligned)
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &out));
        };
        // Create all three unconditionally (no short-circuit), then AND the results.
        // Sized to the FULL Lighting struct — this was 64 (the v1 sun-only block)
        // while the fill memcpy'd sizeof(Lighting), overflowing the mapped
        // allocation ever since the v2 8-light window landed.
        const bool cbLight = makeCB(sizeof(HE::MaterialShaderLibrary::Lighting), m_matLightCB);
        const bool cbObj   = makeCB(176, m_matObjCB);   // U
        const bool cbParam = makeCB(256, m_matParamCB); // HeParams
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        const bool sampOk = SUCCEEDED(device->CreateSamplerState(&sd, &m_matSampler));
        m_matReady = cbLight && cbObj && cbParam && sampOk;
        Logger::Log(m_matReady ? Logger::LogLevel::Info : Logger::LogLevel::Error,
            m_matReady ? "D3D11Renderer: A4 material resources created"
                       : "D3D11Renderer: A4 material resource allocation failed");
#endif
    }

    // Build (or fetch from cache) the per-material VS + PS + input layout from the
    // MaterialShaderLibrary HLSL. Cached by hash^transparentbit for signature parity with
    // the D3D12/Vulkan getOrBuild* (the transparent bit is redundant on D3D11 — the shader
    // objects don't bake blend/depth — but kept so the cache key matches the other backends).
    // Returns nullptr (and caches the miss so it never retries per-draw) on any failure.
    MatShaders* getOrBuildMaterialShaders(uint64_t hash, const std::string& frag,
                                          const std::string& vertBody, bool transparent)
    {
#if defined(HE_HAVE_SHADERC)
        const uint64_t key = hash ^ (transparent ? 0xD1B54A32D192ED03ULL : 0ULL);
        if (auto it = m_materialShaders.find(key); it != m_materialShaders.end())
            return it->second.vs ? &it->second : nullptr; // null vs == cached miss

        using Backend = HE::MaterialShaderLibrary::Backend;
        const HE::MaterialShaderLibrary::Compiled& vc = vertBody.empty()
            ? m_matShaderLib.standardVertex(Backend::HLSL)
            : m_matShaderLib.customVertex(std::hash<std::string>{}(vertBody), vertBody, Backend::HLSL);
        const HE::MaterialShaderLibrary::Compiled& fc = m_matShaderLib.fragment(hash, frag, Backend::HLSL);
        if (!vc.ok || !fc.ok || vc.source.empty() || fc.source.empty())
        {
            Logger::Log(Logger::LogLevel::Warning, "D3D11Renderer: A4 material shader cross-compile failed");
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }
        if (!m_matHlslLogged)
        {
            m_matHlslLogged = true;
            Logger::Log(Logger::LogLevel::Info, (std::string("D3D11 A4 material VS HLSL:\n") + vc.source).c_str());
            Logger::Log(Logger::LogLevel::Info, (std::string("D3D11 A4 material PS HLSL:\n") + fc.source).c_str());
        }

        UINT cflags = 0;
#ifdef _DEBUG
        cflags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        // SPIRV-Cross emits the GLSL-sourced entry point as `main` (not VSMain/PSMain).
        ComPtr<ID3DBlob> vsb, psb, cerr;
        if (FAILED(D3DCompile(vc.source.c_str(), vc.source.size(), "matVS", nullptr, nullptr,
                              "main", "vs_5_0", cflags, 0, &vsb, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Warning, (std::string("D3D11Renderer: A4 material VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }
        if (FAILED(D3DCompile(fc.source.c_str(), fc.source.size(), "matPS", nullptr, nullptr,
                              "main", "ps_5_0", cflags, 0, &psb, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Warning, (std::string("D3D11Renderer: A4 material PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }

        MatShaders sh;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &sh.vs)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &sh.ps)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11Renderer: A4 material shader-object creation failed");
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }

        // IMPORTANT: SPIRV-Cross names GLSL vertex inputs by location as TEXCOORD{location}
        // (no remap_vertex_attributes registered in ShaderCompiler.cpp), so the material input
        // layout uses TEXCOORD0/1/2 — NOT the scene's POSITION/NORMAL/TEXCOORD. Same interleaved
        // 32-B pos/normal/uv vertex buffer the scene meshes use.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 2, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device->CreateInputLayout(layout, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &sh.il)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11Renderer: A4 material input layout creation failed");
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }
        return &m_materialShaders.emplace(key, std::move(sh)).first->second;
#else
        (void)hash; (void)frag; (void)vertBody; (void)transparent;
        return nullptr;
#endif
    }

    // Drain the editor's material/mesh hot-reload requests at DrawScene top. Dropping the ComPtr
    // is GPU-safe (the D3D11 runtime keeps the resource alive until pending GPU work finishes),
    // so the entry can be erased immediately; the mesh/material re-resolves next frame.
    void processPendingInvalidations()
    {
        for (const HE::UUID& id : pendingMatInval)
            materialTexCache.erase(id);
        pendingMatInval.clear();
        for (const HE::UUID& id : pendingMeshInval)
        {
            meshCache.erase(id);
            skeletalMeshCache.erase(id);
            // GI BLAS ranges live in CONCATENATED buffers — no splice, so an
            // edited mesh drops the whole cache and it rebuilds lazily (same
            // policy as the GL port's InvalidateMesh).
            if (giBlasCache.count(id))
                destroyGiAccel();
        }
        pendingMeshInval.clear();
    }

    const GpuMesh* resolveMesh(const HE::UUID& assetId, ContentManager* cm)
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
        uploadBuffers(mesh, interleaved, asset->indices);

        // Baked UUID (packed builds) with editor-path fallback (loose content).
        if (const MaterialAsset* mat = cm->resolveMaterialRef(asset->materialId, asset->materialPath))
        {
            const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
            const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
            // RGBA8 + cooked BC7/BC3 with the pre-baked mip chain.
            mesh.texture = createAlbedoSRV(cm->resolveTextureRef(texId0, texPath0));
        }
        return &meshCache.emplace(assetId, mesh).first->second;
    }

    bool createSkyPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* src, size_t srcLen, const char* entry, const char* profile,
                           ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(src, srcLen, entry, nullptr, nullptr,
                                  entry, profile, flags, 0, &out, &err)))
            {
                Logger::Log(Logger::LogLevel::Error,
                    (std::string("D3D11 sky '") + entry + "': " +
                     (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsB, psB;
        const std::string skyPS_src = std::string(kSkyFuncHLSL) + kSkyPSHLSL;
        if (!compile(kSkyVSHLSL, std::strlen(kSkyVSHLSL), "VSSky", "vs_5_0", vsB)) return false;
        if (!compile(skyPS_src.c_str(), skyPS_src.size(), "PSSky", "ps_5_0", psB)) return false;
        device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &skyVS);
        device->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &skyPS);
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth = (sizeof(SkyCB) + 15u) & ~15u;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &skyCB);

        // Procedural 3D noise volume the sky's starFbm3/worleyFbm sample (clouds) —
        // built once on the CPU. RG16 (R=value noise, G=Worley billows) + LINEAR +
        // WRAP so it tiles seamlessly.
        // Release: full 256³ tile so sky fBm octaves don't visibly repeat.
        // Debug: 64³ (64× fewer voxels) so the CPU bake takes < 1s instead of many
        // minutes without SIMD optimisation in MSVC Debug mode.
#ifdef NDEBUG
        constexpr int kNoiseN = 256;
#else
        constexpr int kNoiseN = 64;
#endif
        const std::vector<uint16_t> noise = BuildSkyNoise3D(kNoiseN);
        D3D11_TEXTURE3D_DESC nd{};
        nd.Width     = kNoiseN;
        nd.Height    = kNoiseN;
        nd.Depth     = kNoiseN;
        nd.MipLevels = 1;
        nd.Format    = DXGI_FORMAT_R16G16_UNORM;
        nd.Usage     = D3D11_USAGE_IMMUTABLE;
        nd.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA nsd{};
        nsd.pSysMem          = noise.data();
        nsd.SysMemPitch      = static_cast<UINT>(kNoiseN) * 4u;            // RG16 = 4 bytes/texel
        nsd.SysMemSlicePitch = static_cast<UINT>(kNoiseN) * kNoiseN * 4u;
        device->CreateTexture3D(&nd, &nsd, &noiseTex3D);
        if (noiseTex3D)
        {
            D3D11_SHADER_RESOURCE_VIEW_DESC sv{};
            sv.Format                    = DXGI_FORMAT_R16G16_UNORM;
            sv.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE3D;
            sv.Texture3D.MostDetailedMip = 0;
            sv.Texture3D.MipLevels       = 1;
            device->CreateShaderResourceView(noiseTex3D.Get(), &sv, &noiseSRV);
        }
        { D3D11_SAMPLER_DESC nsamp{};
          nsamp.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
          nsamp.AddressU = nsamp.AddressV = nsamp.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
          nsamp.MaxLOD   = D3D11_FLOAT32_MAX;
          device->CreateSamplerState(&nsamp, &skyNoiseSampler); }

        skyReady = skyVS && skyPS && skyCB && noiseSRV && skyNoiseSampler;
        return skyReady;
    }

    bool createDebugLinePipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsB, psB, err;
        if (FAILED(D3DCompile(kDebugLineHLSL, std::strlen(kDebugLineHLSL),
                              "dbgline", nullptr, nullptr, "VSLine", "vs_5_0", flags, 0, &vsB, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11 DebugLine VS compile failed");
            return false;
        }
        if (FAILED(D3DCompile(kDebugLineHLSL, std::strlen(kDebugLineHLSL),
                              "dbgline", nullptr, nullptr, "PSLine", "ps_5_0", flags, 0, &psB, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11 DebugLine PS compile failed");
            return false;
        }
        device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &debugVS);
        device->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &debugPS);
        const D3D11_INPUT_ELEMENT_DESC debugLayout[] = {
            {"POSITION",0,DXGI_FORMAT_R32G32B32_FLOAT,0, 0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",   0,DXGI_FORMAT_R32G32B32_FLOAT,0,12,D3D11_INPUT_PER_VERTEX_DATA,0},
        };
        device->CreateInputLayout(debugLayout, 2, vsB->GetBufferPointer(), vsB->GetBufferSize(), &debugIL);
        D3D11_BUFFER_DESC cbd{};
        cbd.ByteWidth = 64; cbd.Usage = D3D11_USAGE_DYNAMIC;
        cbd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&cbd, nullptr, &debugCB);
        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = 4096 * 6 * sizeof(float);
        vbd.Usage = D3D11_USAGE_DYNAMIC; vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        vbd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&vbd, nullptr, &debugVB);
        debugReady = debugVS && debugPS && debugIL && debugCB && debugVB;
        return debugReady;
    }

    void drawSky(ID3D11DeviceContext* ctx, const glm::mat4& invVP,
                 const glm::vec3& sunDir, const IRenderer::EnvironmentSettings& env)
    {
        if (!skyReady) return;
        if (!env.skyEnabled) return; // no Sky entity → leave the cleared background
        SkyCB cb{};
        cb.invViewProj = invVP;
        cb.sunDir      = sunDir; cb.timeOfDay = env.timeOfDay;
        cb.sunColor    = env.sunColor; cb.cloudCoverage = env.cloudCoverage;
        // Cloud drift: world-units/sec. The 0.025 factor matches the OpenGL reference
        // (windSpeed * 0.025) — without it the clouds scroll ~40× too fast.
        const float windScale = env.windSpeed * 0.025f;
        cb.wind = glm::vec3(
            std::sin(glm::radians(env.windDirection)) * windScale,
            0.0f,
            std::cos(glm::radians(env.windDirection)) * windScale);
        cb.time = m_wallTime;
        cb.auroraColor = env.auroraColor; cb.aurora = env.auroraIntensity;
        cb.milkyWay = env.milkyWayIntensity; cb.flash = env.flash;
        cb.hasMoonTex = moonSRV ? 1 : 0;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(skyCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, &cb, sizeof(cb)); ctx->Unmap(skyCB.Get(), 0); }
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(skyVS.Get(), nullptr, 0);
        ctx->PSSetShader(skyPS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, skyCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, skyCB.GetAddressOf());
        ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
        ctx->RSSetState(fsRastState.Get());
        ctx->PSSetSamplers(0, 1, linearSampler.GetAddressOf());
        ID3D11ShaderResourceView* moonSrv = moonSRV ? moonSRV.Get() : nullptr;
        ctx->PSSetShaderResources(0, 1, &moonSrv);
        ctx->PSSetShaderResources(1, 1, noiseSRV.GetAddressOf());
        ctx->PSSetSamplers(1, 1, skyNoiseSampler.GetAddressOf());
        ctx->Draw(3, 0);
        // Unbind textures and restore scene state
        ID3D11ShaderResourceView* nullSrv = nullptr;
        ctx->PSSetShaderResources(0, 1, &nullSrv);
        ctx->PSSetShaderResources(1, 1, &nullSrv);
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        ctx->RSSetState(rasterState.Get());
        ctx->PSSetSamplers(0, 1, sampler.GetAddressOf());
    }

    void drawDebugLines(ID3D11DeviceContext* ctx, const glm::mat4& viewProj,
                        const std::vector<DebugLine>& lines)
    {
        if (!debugReady || lines.empty()) return;
        std::vector<float> verts;
        verts.reserve(lines.size() * 12);
        for (const DebugLine& l : lines) {
            verts.insert(verts.end(), {l.start.x,l.start.y,l.start.z,l.color.r,l.color.g,l.color.b});
            verts.insert(verts.end(), {l.end.x,  l.end.y,  l.end.z,  l.color.r,l.color.g,l.color.b});
        }
        const UINT needed = static_cast<UINT>(verts.size() * sizeof(float));
        D3D11_BUFFER_DESC existDesc{};
        if (debugVB) debugVB->GetDesc(&existDesc);
        if (needed > existDesc.ByteWidth) {
            debugVB.Reset();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (needed + 0xFFF) & ~0xFFFu;
            bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&bd, nullptr, &debugVB);
        }
        if (!debugVB) return;
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(debugVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, verts.data(), verts.size()*sizeof(float)); ctx->Unmap(debugVB.Get(), 0); }
        if (SUCCEEDED(ctx->Map(debugCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, glm::value_ptr(viewProj), 64); ctx->Unmap(debugCB.Get(), 0); }
        const UINT stride = 6 * sizeof(float), offset = 0;
        ctx->IASetInputLayout(debugIL.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINELIST);
        ctx->IASetVertexBuffers(0, 1, debugVB.GetAddressOf(), &stride, &offset);
        ctx->VSSetShader(debugVS.Get(), nullptr, 0);
        ctx->PSSetShader(debugPS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, debugCB.GetAddressOf());
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        ctx->RSSetState(rasterState.Get());
        ctx->Draw(static_cast<UINT>(lines.size() * 2), 0);
        // Restore scene state
        ctx->IASetInputLayout(inputLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, perObjectCB.GetAddressOf());
        ctx->VSSetConstantBuffers(1, 1, perFrameCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, perObjectCB.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, perFrameCB.GetAddressOf());
    }

    // ── Skinned mesh pipeline ─────────────────────────────────────────────────
    bool createSkinnedPipeline()
    {
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsBlob, err;
        if (FAILED(D3DCompile(kSkinnedHLSL, std::strlen(kSkinnedHLSL), "skinned",
                              nullptr, nullptr, "VSMainSkinned", "vs_5_0", flags, 0, &vsBlob, &err)))
        {
            const char* msg = err ? static_cast<const char*>(err->GetBufferPointer()) : "unknown";
            Logger::Log(Logger::LogLevel::Error,
                        (std::string("D3D11: skinned VS compile: ") + msg).c_str());
            return false;
        }
        device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                   nullptr, &skinnedVS);

        // Input layout: slot0 = interleaved(pos+norm+uv), slot1 = boneIds, slot2 = boneWgt
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  1,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        device->CreateInputLayout(layout, 5,
                                  vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(),
                                  &skinnedLayout);

        // Bone CB: 128 × mat4 = 8192 bytes, dynamic for per-draw upload
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 8192u;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        device->CreateBuffer(&bd, nullptr, &bonesCB);

        return skinnedVS && skinnedLayout && bonesCB;
    }

    void createUIPipeline()
    {
        auto& dev = *device.Get();
        UINT flags = D3DCOMPILE_ENABLE_STRICTNESS;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsBlob, psBlob, err;
        if (FAILED(D3DCompile(kUIHLSL, strlen(kUIHLSL), nullptr, nullptr, nullptr,
                              "UIVSMain", "vs_5_0", flags, 0, &vsBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11: UI VS compile failed");
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return;
        }
        if (FAILED(D3DCompile(kUIHLSL, strlen(kUIHLSL), nullptr, nullptr, nullptr,
                              "UIPSMain", "ps_5_0", flags, 0, &psBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, "D3D11: UI PS compile failed");
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return;
        }
        dev.CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &uiVS);
        dev.CreatePixelShader (psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &uiPS);

        // cbuffer: rect(16) + color(16) + uvRect(16) + viewport(8) + mode(4) + pad(4) = 64 bytes
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 64u;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev.CreateBuffer(&bd, nullptr, &uiCB);

        // Atlas sampler: linear + clamp so glyph edges never wrap-bleed into
        // neighbouring atlas cells.
        D3D11_SAMPLER_DESC sd{};
        sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sd.MaxLOD   = D3D11_FLOAT32_MAX;
        dev.CreateSamplerState(&sd, &uiSampler);

        D3D11_BLEND_DESC bd2{};
        bd2.RenderTarget[0].BlendEnable            = TRUE;
        bd2.RenderTarget[0].SrcBlend              = D3D11_BLEND_SRC_ALPHA;
        bd2.RenderTarget[0].DestBlend             = D3D11_BLEND_INV_SRC_ALPHA;
        bd2.RenderTarget[0].BlendOp               = D3D11_BLEND_OP_ADD;
        bd2.RenderTarget[0].SrcBlendAlpha         = D3D11_BLEND_ONE;
        bd2.RenderTarget[0].DestBlendAlpha        = D3D11_BLEND_ZERO;
        bd2.RenderTarget[0].BlendOpAlpha          = D3D11_BLEND_OP_ADD;
        bd2.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        dev.CreateBlendState(&bd2, &uiBlend);

        D3D11_DEPTH_STENCIL_DESC dd{};
        dd.DepthEnable    = FALSE;
        dd.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        dd.StencilEnable  = FALSE;
        dev.CreateDepthStencilState(&dd, &uiDepth);
    }

    void renderUIPass(ID3D11DeviceContext* ctx, int width, int height)
    {
        if (!uiVS || m_renderWorld.uiObjects.empty()) return;

        ctx->VSSetShader(uiVS.Get(), nullptr, 0);
        ctx->PSSetShader(uiPS.Get(), nullptr, 0);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        ctx->VSSetConstantBuffers(0, 1, uiCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, uiCB.GetAddressOf());

        float blendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ctx->OMSetBlendState(uiBlend.Get(), blendFactor, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(uiDepth.Get(), 0);

        // Font atlas on t0 (uFontAtlas); glyphs sample it, solid quads ignore it.
        ctx->PSSetSamplers(0, 1, uiSampler.GetAddressOf());
        ID3D11ShaderResourceView* atlas = uiFontAtlasSRV(0);
        ctx->PSSetShaderResources(0, 1, &atlas);
        uint32_t boundAtlasKey = 0;

        struct UICBData { glm::vec4 rect; glm::vec4 color; glm::vec4 uvRect; glm::vec2 viewport; float mode; float pad; };
        for (const UIRenderObject& obj : m_renderWorld.uiObjects)
        {
            // A glyph quad may use an imported font's atlas — bind it on t0.
            if (obj.type == 2 && obj.fontAtlasKey != boundAtlasKey)
            {
                atlas = uiFontAtlasSRV(obj.fontAtlasKey);
                ctx->PSSetShaderResources(0, 1, &atlas);
                boundAtlasKey = obj.fontAtlasKey;
            }
            UICBData cb;
            cb.rect     = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
            cb.color    = obj.color;
            cb.uvRect   = glm::vec4(obj.uvMin.x, obj.uvMin.y, obj.uvMax.x, obj.uvMax.y);
            cb.viewport = glm::vec2(float(width), float(height));
            cb.mode     = obj.type == 2 ? 1.0f : 0.0f;
            cb.pad      = 0.0f;
            D3D11_MAPPED_SUBRESOURCE mr{};
            if (SUCCEEDED(ctx->Map(uiCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr)))
            {
                std::memcpy(mr.pData, &cb, sizeof(cb));
                ctx->Unmap(uiCB.Get(), 0);
            }
            ctx->Draw(4, 0);
        }

        // Restore: no atlas SRV, opaque blend, depth on
        { ID3D11ShaderResourceView* nullSrv = nullptr; ctx->PSSetShaderResources(0, 1, &nullSrv); }
        ctx->OMSetBlendState(nullptr, blendFactor, 0xFFFFFFFF);
        ctx->OMSetDepthStencilState(nullptr, 0);
    }

    // Upload and cache GPU resources for a SkeletalMeshAsset.
    const GpuSkeletalMesh* resolveSkeletalMesh(const HE::UUID& assetId, ContentManager* cm)
    {
        if (assetId == HE::UUID{} || !cm) return nullptr;
        if (auto it = skeletalMeshCache.find(assetId); it != skeletalMeshCache.end())
            return &it->second;

        const SkeletalMeshAsset* asset = cm->getSkeletalMesh(assetId);
        if (!asset || asset->vertices.empty() || asset->indices.empty()) return nullptr;

        const size_t vertexCount = asset->vertices.size() / 3;

        // Interleaved pos(12) + norm(12) + uv(8) = 32 bytes per vertex
        std::vector<float> interleaved;
        interleaved.reserve(vertexCount * 8);
        for (size_t v = 0; v < vertexCount; ++v)
        {
            interleaved.insert(interleaved.end(),
                { asset->vertices[v*3+0], asset->vertices[v*3+1], asset->vertices[v*3+2] });
            if (v*3+2 < asset->normals.size())
                interleaved.insert(interleaved.end(),
                    { asset->normals[v*3+0], asset->normals[v*3+1], asset->normals[v*3+2] });
            else
                interleaved.insert(interleaved.end(), { 0.0f, 0.0f, 0.0f });
            if (v*2+1 < asset->uvs.size())
                interleaved.insert(interleaved.end(), { asset->uvs[v*2+0], asset->uvs[v*2+1] });
            else
                interleaved.insert(interleaved.end(), { 0.0f, 0.0f });
        }

        // Bone IDs per vertex (uint32 × 4), zero-padded
        std::vector<uint32_t> boneIds(vertexCount * 4, 0u);
        if (!asset->boneIDs.empty())
            std::copy_n(asset->boneIDs.begin(),
                        std::min(asset->boneIDs.size(), vertexCount * 4), boneIds.begin());

        // Bone weights per vertex (float × 4), default 100% joint 0
        std::vector<float> boneWgts(vertexCount * 4, 0.0f);
        for (size_t v = 0; v < vertexCount; ++v) boneWgts[v*4] = 1.0f;
        if (!asset->boneWeights.empty())
            std::copy_n(asset->boneWeights.begin(),
                        std::min(asset->boneWeights.size(), vertexCount * 4), boneWgts.begin());

        auto makeVB = [&](const void* data, UINT bytes) -> ComPtr<ID3D11Buffer>
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth  = bytes;
            bd.Usage      = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags  = D3D11_BIND_VERTEX_BUFFER;
            D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = data;
            ComPtr<ID3D11Buffer> buf;
            device->CreateBuffer(&bd, &sd, &buf);
            return buf;
        };

        GpuSkeletalMesh mesh;
        mesh.indexCount = static_cast<int>(asset->indices.size());
        mesh.vb       = makeVB(interleaved.data(), static_cast<UINT>(interleaved.size() * sizeof(float)));
        mesh.boneIdVb = makeVB(boneIds.data(),    static_cast<UINT>(boneIds.size()  * sizeof(uint32_t)));
        mesh.boneWgtVb= makeVB(boneWgts.data(),   static_cast<UINT>(boneWgts.size() * sizeof(float)));

        // Index buffer
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = static_cast<UINT>(asset->indices.size() * sizeof(uint32_t));
            bd.Usage     = D3D11_USAGE_IMMUTABLE;
            bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
            D3D11_SUBRESOURCE_DATA sd{}; sd.pSysMem = asset->indices.data();
            device->CreateBuffer(&bd, &sd, &mesh.ib);
        }

        // Try to load albedo texture — same pattern as resolveMesh()
        // (baked UUID for packed builds, editor path as loose fallback).
        if (const MaterialAsset* mat = cm->resolveMaterialRef(asset->materialId, asset->materialPath))
        {
            const HE::UUID    texId0   = mat->textureIds.empty()   ? HE::UUID{}    : mat->textureIds[0];
            const std::string texPath0 = mat->texturePaths.empty() ? std::string{} : mat->texturePaths[0];
            // RGBA8 + cooked BC7/BC3 with the pre-baked mip chain.
            mesh.srv = createAlbedoSRV(cm->resolveTextureRef(texId0, texPath0));
        }

        return &skeletalMeshCache.emplace(assetId, std::move(mesh)).first->second;
    }
};

D3D11Renderer::D3D11Renderer()  : m_impl(new D3D11RendererImpl{}) {}
D3D11Renderer::~D3D11Renderer() { delete m_impl; }

void D3D11Renderer::Initialize(HE::Window* window)
{
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: initializing");
    SDL_PropertiesID props = SDL_GetWindowProperties(window->GetNativeWindow());
    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
        throw std::runtime_error("D3D11Renderer: could not get HWND");

    m_impl->width  = window->GetWidth();
    m_impl->height = window->GetHeight();

    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = m_impl->width;
    scd.BufferDesc.Height                  = m_impl->height;
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL fl;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION,
        &scd, &m_impl->swapchain,
        &m_impl->device, &fl, &m_impl->context);
    if (FAILED(hr))
        throw std::runtime_error("D3D11Renderer: D3D11CreateDeviceAndSwapChain failed");

    m_impl->createRTV();
    m_impl->createDepth(m_impl->width, m_impl->height);
    if (!m_impl->createPipeline())
        Logger::Log(Logger::LogLevel::Error, "D3D11Renderer: scene pipeline creation failed — only clear will work");
    m_impl->createCube();
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: initialized successfully");
}

void D3D11Renderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "D3D11Renderer: shutdown");
    m_impl->meshCache.clear();
    m_impl->materialTexCache.clear(); // override-material textures (ComPtr auto-release)
    m_impl->pendingMatInval.clear();
    m_impl->pendingMeshInval.clear();
    // A4: node-graph material resources (m_matShaderLib.clear() is header-inline → safe
    // unguarded; the shader/CB/sampler ComPtrs auto-release).
    m_impl->m_matReady = false;
    m_impl->m_matHlslLogged = false;
    m_impl->m_materialShaders.clear();
    m_impl->m_matShaderLib.clear();
    m_impl->m_matLightCB.Reset();
    m_impl->m_matObjCB.Reset();
    m_impl->m_matParamCB.Reset();
    m_impl->m_matSampler.Reset();
    m_impl->uiFontAtlases.clear();
    m_impl->uiSampler.Reset();
    m_impl->gpuTimerShutdown();
    m_impl->skyVS.Reset(); m_impl->skyPS.Reset(); m_impl->skyCB.Reset();
    m_impl->moonSRV.Reset(); m_impl->moonTex2D.Reset();
    m_impl->noiseSRV.Reset(); m_impl->noiseTex3D.Reset(); m_impl->skyNoiseSampler.Reset();
    m_impl->debugVS.Reset(); m_impl->debugPS.Reset(); m_impl->debugVB.Reset();
    m_impl->debugCB.Reset(); m_impl->debugIL.Reset();
    // GI resources (accel buffers, targets, atlases, pipelines).
    m_impl->destroyGiAccel();
    m_impl->destroyGiTargets();
    m_impl->giGBufVS.Reset(); m_impl->giGBufPS.Reset();
    m_impl->giShadowCS.Reset(); m_impl->giProbeCS.Reset();
    m_impl->giTemporalPS.Reset(); m_impl->giBlurPS.Reset();
    m_impl->giShadowCB.Reset(); m_impl->giCountCB.Reset(); m_impl->giTemporalCB.Reset();
    m_impl->giBlurCB.Reset(); m_impl->giProbeCB.Reset();
    m_impl->giLinearClamp.Reset();
    m_impl->rtv.Reset();
    m_impl->dsv.Reset();
    m_impl->depthTex.Reset();
    m_impl->swapchain.Reset();
    m_impl->context.Reset();
    m_impl->device.Reset();
}

void D3D11Renderer::DrawScene(int width, int height)
{
    if (!m_world || !m_impl->vs || width <= 0 || height <= 0) return;
    auto& p = *m_impl;

    // Drop caches for materials/meshes edited since last frame; they re-resolve this frame.
    p.processPendingInvalidations();

    // Feed time-of-day so the extractor recomputes the sun/moon direction (otherwise the
    // sky never responds to the time slider). Mirrors OpenGL/Metal.
    p.m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                              m_environment.sunColor, m_environment.sunIntensity,
                              m_environment.moonColor, m_environment.moonIntensity,
                              m_environment.cloudCoverage);
    p.m_extractor.setContentManager(m_contentManager);
    p.m_extractor.extract(*m_world, p.m_renderWorld,
                          static_cast<float>(width) / static_cast<float>(height),
                          &m_editorCamera);

    // Sky is independent of scene geometry — always draw it here so it renders
    // even when objects/sortedIndices are empty (early returns below).
    {
        ID3D11DeviceContext* skyCtx = p.context.Get();
        const glm::mat4 skyVP = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;
        p.drawSky(skyCtx, glm::inverse(skyVP), p.m_renderWorld.sunDirection, m_environment);
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

    // GI acceleration structures: refresh the BLAS cache + per-frame instance
    // array right after extraction (UNCULLED — off-screen casters still occlude),
    // mirroring GL's UpdateGiAccel placement. No-op when GI is off.
    p.updateGiAccel(m_contentManager, p.m_renderWorld);

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
    p.counters.total   = static_cast<uint32_t>(p.m_renderWorld.objects.size());
    p.counters.visible = static_cast<uint32_t>(p.m_sortedIndices.size());
    if (p.m_sortedIndices.empty()) return;

    if (p.m_renderGraph.empty())
    {
        p.m_renderGraph.addPass(std::make_unique<ShadowPass>());
        p.m_renderGraph.addPass(std::make_unique<GeometryPass>());
    }

    const glm::mat4 viewProj  = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;
    const glm::mat4 camView   = p.m_renderWorld.camera.view;
    const glm::mat4 camProj   = p.m_renderWorld.camera.projection;
    const bool      shadows   = p.m_renderWorld.shadow.enabled && p.shadowDSV && p.depthVS;
    const glm::mat4 lightClip = kD3DClipFix * p.m_renderWorld.shadow.viewProj;

    ID3D11DeviceContext* ctx = p.context.Get();
    ctx->IASetInputLayout(p.inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
    ctx->RSSetState(p.rasterState.Get());
    ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());

    // ── Per-frame constants (camera + up to 8 lights) ───────────────────────
    // A lambda because the GI/SSAO decision is only known inside the backbuffer
    // pass — the CB is refilled there with the final giActive/aoActive flags.
    auto fillPerFrame = [&](bool giActive, bool aoActive)
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
        f.viewport = glm::vec4(float(width), float(height), aoActive ? 1.0f : 0.0f, 0.0f);
        f.giParams     = glm::vec4(giActive ? 1.0f : 0.0f, p.giIndirectIntensity, 0.0f, 0.0f);
        f.giGridOrigin = glm::vec4(p.giGridOrigin, D3D11RendererImpl::kGiProbeSpacing);
        f.giGridCounts = glm::vec4(glm::vec3(p.giGridCounts), float(p.giProbesPerRow));
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(p.perFrameCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &f, sizeof(f));
            ctx->Unmap(p.perFrameCB.Get(), 0);
        }
        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
    };
    fillPerFrame(false, p.ssaoEnabled && p.ssaoReady);

#if defined(HE_HAVE_SHADERC)
    // A4: fill the shared HeLighting CB — identical for every graph-material draw this
    // frame (bound at b0 PS + b8 WPO VS in the material draw path). A lambda because
    // giParams.z is only known after the GI passes ran (refilled in the backbuffer
    // branch). Now fills the FULL v2 light window from the dominant directional light
    // (was sun-only sky values before — graph materials never saw point/spot lights
    // on D3D11 and stayed sun-lit at night).
    auto fillMatLight = [&](bool giActive)
    {
        if (!(p.m_matReady && p.m_matLightCB)) return;
        HE::MaterialShaderLibrary::Lighting lit{};
        glm::vec3 matSunDir, matSunColor;
        d3d11DominantDirectionalLight(p.m_renderWorld, matSunDir, matSunColor);
        lit.sunDir[0] = matSunDir.x;
        lit.sunDir[1] = matSunDir.y;
        lit.sunDir[2] = matSunDir.z;
        // Engine seconds for the node graph's Time input (HE_SKY_TIME pins it for deterministic
        // headless captures, mirroring the sky clock + GL/D3D12/Vulkan exactly).
        static const char* s_timeOv = std::getenv("HE_SKY_TIME");
        lit.sunDir[3] = (s_timeOv && *s_timeOv)
            ? static_cast<float>(std::atof(s_timeOv))
            : static_cast<float>(SDL_GetTicks()) / 1000.0f;
        lit.sunColor[0] = matSunColor.r; lit.sunColor[1] = matSunColor.g; lit.sunColor[2] = matSunColor.b;
        lit.ambient[0] = p.m_renderWorld.ambient.r;
        lit.ambient[1] = p.m_renderWorld.ambient.g;
        lit.ambient[2] = p.m_renderWorld.ambient.b;
        lit.camPos[0] = p.m_renderWorld.camera.position.x;
        lit.camPos[1] = p.m_renderWorld.camera.position.y;
        lit.camPos[2] = p.m_renderWorld.camera.position.z;
        // Full light window for heLitP() — same first-8 order as the built-in
        // shaders (keep the backend copies of this fill in sync).
        {
            const int lc = std::min(static_cast<int>(p.m_renderWorld.lights.size()), 8);
            for (int li = 0; li < lc; ++li)
            {
                const LightData& ld = p.m_renderWorld.lights[li];
                lit.lightPos[li][0] = ld.position.x;  lit.lightPos[li][1] = ld.position.y;
                lit.lightPos[li][2] = ld.position.z;  lit.lightPos[li][3] = static_cast<float>(ld.type);
                lit.lightDir[li][0] = ld.direction.x; lit.lightDir[li][1] = ld.direction.y;
                lit.lightDir[li][2] = ld.direction.z; lit.lightDir[li][3] = ld.spotAngleCos;
                lit.lightColor[li][0] = ld.color.r;   lit.lightColor[li][1] = ld.color.g;
                lit.lightColor[li][2] = ld.color.b;   lit.lightColor[li][3] = ld.intensity;
                lit.lightParams[li][0] = ld.range;
            }
            lit.counts[0] = static_cast<float>(lc);
        }
        lit.giParams[0] = static_cast<float>(width);
        lit.giParams[1] = static_cast<float>(height);
        lit.giParams[2] = giActive ? 1.0f : 0.0f;
        // csmSplits stays 0 — D3D11 has a single shadow map, no cascade array,
        // so the preamble's heCsmShadow() fallback is inert here.
        D3D11_MAPPED_SUBRESOURCE lm{};
        if (SUCCEEDED(ctx->Map(p.m_matLightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lm)))
        {
            std::memcpy(lm.pData, &lit, sizeof(lit));
            ctx->Unmap(p.m_matLightCB.Get(), 0);
        }
    };
    fillMatLight(false);
#endif

    const UINT stride = 8 * sizeof(float);
    const UINT offset = 0;

    auto uploadObject = [&](const glm::mat4& mvp, const glm::mat4& model,
                            const glm::vec3& baseColor, float hasTex,
                            float metallic, float roughness, float opacity = 1.0f)
    {
        PerObjectCB o{};
        o.mvp   = mvp; o.model = model;
        o.color = glm::vec4(baseColor, hasTex);
        o.pbr   = glm::vec4(metallic, roughness, opacity, 0.0f);
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(ctx->Map(p.perObjectCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        {
            std::memcpy(mapped.pData, &o, sizeof(o));
            ctx->Unmap(p.perObjectCB.Get(), 0);
        }
        ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
    };

    p.m_renderGraph.execute(p.m_renderWorld, p.m_sortedIndices,
        [&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
    {
        // ── Shadow pass: depth from the light's POV into the shadow map ──────
        if (io.output.id == kShadowMapTarget)
        {
            if (!shadows) return;
            // Save the active render target so we can restore it after the shadow pass.
            ComPtr<ID3D11RenderTargetView> savedRTV;
            ComPtr<ID3D11DepthStencilView> savedDSV;
            ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

            // Unbind the shadow SRV (t1) so it can be bound as a depth target.
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(1, 1, &nullSrv);
            ID3D11RenderTargetView* noRTV = nullptr;
            ctx->OMSetRenderTargets(1, &noRTV, p.shadowDSV.Get());
            ctx->ClearDepthStencilView(p.shadowDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            ctx->VSSetShader(p.depthVS.Get(), nullptr, 0);
            ctx->PSSetShader(nullptr, nullptr, 0);
            D3D11_VIEWPORT svp{}; svp.Width = svp.Height = static_cast<float>(p.shadowSize); svp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &svp);
            for (const DrawCall& dc : cmds.drawCalls())
            {
                const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
                const GpuMesh& m    = mesh ? *mesh : p.cube;
                if (!m.vbuf || !m.ibuf) continue;
                uploadObject(lightClip * dc.transform, dc.transform,
                             dc.baseColor, 0.0f, dc.metallic, dc.roughness);
                ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
                ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
                ctx->DrawIndexed(m.indexCount, 0, 0);
            }
            // Restore saved target + viewport + scene shaders.
            ID3D11RenderTargetView* restoreRTV = savedRTV.Get();
            ctx->OMSetRenderTargets(1, &restoreRTV, savedDSV.Get());
            D3D11_VIEWPORT vp{}; vp.Width = static_cast<float>(width); vp.Height = static_cast<float>(height); vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
            ctx->VSSetShader(p.vs.Get(), nullptr, 0);
            ctx->PSSetShader(p.ps.Get(), nullptr, 0);
            return;
        }

        if (io.output.id != kBackbufferTarget) return;

        // ── SSAO prepass (position -> AO -> blur) ────────────────────────────
        // Collect opaque/transparent DCs early (needed for position prepass AND main scene)
        std::vector<const DrawCall*> opaqueDCs_, transparentDCs_;
        for (const DrawCall& dc : cmds.drawCalls())
            (dc.opacity < 0.999f ? transparentDCs_ : opaqueDCs_).push_back(&dc);

        // ── Ray-traced GI (software BVH): shadow mask + probe update, BEFORE
        // SSAO — when GI shades, SSAO is skipped entirely (probe indirect
        // replaces AO, the ray mask replaces the shadow-map lookup).
        ID3D11ShaderResourceView* giShadowSRV = nullptr;
        bool giShadingActive = false;
        if (p.giEnabled && p.giSupported && p.giInstanceCount > 0)
        {
            ComPtr<ID3D11RenderTargetView> savedRTV;
            ComPtr<ID3D11DepthStencilView> savedDSV;
            ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

            giShadowSRV = p.runGiShadow(ctx, opaqueDCs_, viewProj,
                std::max(1, width / 2), std::max(1, height / 2), p.m_renderWorld,
                [&](HE::UUID id) -> const GpuMesh* { return p.resolveMesh(id, m_contentManager); },
                p.cube, p.inputLayout.Get(), p.depthState.Get(), p.rasterState.Get());
            if (giShadowSRV)
                p.dispatchGiProbeUpdate(ctx, p.m_renderWorld);
            giShadingActive = giShadowSRV && p.giIrrSRV && p.giVisSRV && p.giProbeGridBuilt;

            ID3D11RenderTargetView* restRTV = savedRTV.Get();
            ctx->OMSetRenderTargets(1, &restRTV, savedDSV.Get());
            D3D11_VIEWPORT vp{}; vp.Width = float(width); vp.Height = float(height); vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
        }

        ID3D11ShaderResourceView* aoSRV = p.whiteSRV.Get(); // default: unoccluded
        if (!giShadingActive && p.ssaoEnabled && p.ssaoReady) {
            // Save and restore render target around SSAO passes
            ComPtr<ID3D11RenderTargetView> savedRTV;
            ComPtr<ID3D11DepthStencilView> savedDSV;
            ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

            aoSRV = p.runSSAO(ctx, opaqueDCs_, viewProj, camView, camProj, width, height,
                [&](HE::UUID id) -> const GpuMesh* { return p.resolveMesh(id, m_contentManager); },
                p.cube, p.inputLayout.Get(), p.depthState.Get(), p.rasterState.Get());

            // Restore the scene render target and viewport
            ID3D11RenderTargetView* restRTV = savedRTV.Get();
            ctx->OMSetRenderTargets(1, &restRTV, savedDSV.Get());
            D3D11_VIEWPORT vp{}; vp.Width = float(width); vp.Height = float(height); vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
        }

        // Re-bind scene shaders after SSAO (SSAO pass changes shaders/samplers)
        ctx->IASetInputLayout(p.inputLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(p.vs.Get(), nullptr, 0);
        ctx->PSSetShader(p.ps.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
        ctx->RSSetState(p.rasterState.Get());
        ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());
        ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
        // Shadow SRV on t1
        ID3D11ShaderResourceView* shadowSrv_ = shadows ? p.shadowSRV.Get() : nullptr;
        ctx->PSSetShaderResources(1, 1, &shadowSrv_);
        // AO SRV on t2, point sampler on s1
        ctx->PSSetSamplers(1, 1, p.pointSampler.GetAddressOf());
        ctx->PSSetShaderResources(2, 1, &aoSRV);
        // GI mask + probe atlases on t4/t5/t6, linear-clamp sampler on s2
        // (white fallbacks keep the SRVs valid when GI is off — the shader
        // additionally gates on uGIParams.x). Refill both per-frame CBs with
        // the final GI decision (they were filled before the passes ran).
        {
            ID3D11ShaderResourceView* giLocalSrv =
                (giShadingActive && p.giLocalMaskSRV) ? p.giLocalMaskSRV.Get() : p.whiteSRV.Get();
            ID3D11ShaderResourceView* giSrvs[4] = {
                giShadingActive ? giShadowSRV       : p.whiteSRV.Get(),
                giShadingActive ? p.giIrrSRV.Get()  : p.whiteSRV.Get(),
                giShadingActive ? p.giVisSRV.Get()  : p.whiteSRV.Get(),
                giLocalSrv };
            ctx->PSSetShaderResources(4, 4, giSrvs);
            if (p.giLinearClamp)
                ctx->PSSetSamplers(2, 1, p.giLinearClamp.GetAddressOf());
            fillPerFrame(giShadingActive,
                         !giShadingActive && p.ssaoEnabled && p.ssaoReady && aoSRV != p.whiteSRV.Get());
#if defined(HE_HAVE_SHADERC)
            fillMatLight(giShadingActive);
            // heLitP GI masks for graph materials: sun mask on t10, per-light
            // local mask on t11 (the REAL mask when GI ran this frame).
            // Samplers s10/s11 = linear clamp.
            ID3D11ShaderResourceView* matMasks[2] = {
                giShadingActive ? giShadowSRV : p.whiteSRV.Get(), giLocalSrv };
            ctx->PSSetShaderResources(10, 2, matMasks);
            if (p.giLinearClamp)
            {
                ID3D11SamplerState* matSamps[2] = { p.giLinearClamp.Get(), p.giLinearClamp.Get() };
                ctx->PSSetSamplers(10, 2, matSamps);
            }
#endif
        }

        const glm::vec3 camPos = p.m_renderWorld.camera.position;

        // Reuse already-collected opaque/transparent DC lists from the SSAO prepass above.
        std::vector<const DrawCall*>& opaqueDCs = opaqueDCs_;
        std::vector<const DrawCall*>& transparentDCs = transparentDCs_;

        // Sort transparent back-to-front by distance.
        std::sort(transparentDCs.begin(), transparentDCs.end(),
            [&](const DrawCall* a, const DrawCall* b) {
                const glm::vec3 pa = glm::vec3(a->transform[3]);
                const glm::vec3 pb = glm::vec3(b->transform[3]);
                return glm::length(pa - camPos) > glm::length(pb - camPos);
            });

        // A3: real instancing applies to the opaque pass only; the transparent pass
        // reuses drawDC with a blend state + per-instance depth sort, so it keeps the
        // per-instance loop (allowInstancing is set false before that pass).
        bool allowInstancing = true;
        auto drawDC = [&](const DrawCall& dc) {
            const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            const GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.vbuf || !m.ibuf) return;

#if defined(HE_HAVE_SHADERC)
            // A4: node-graph material? Render through per-material VS/PS built from the
            // MaterialShaderLibrary HLSL, bypassing the built-in Blinn-Phong path entirely, then
            // RESTORE the scene state so subsequent built-in draws are unaffected. Falls through
            // unchanged when the material has no graph shader OR resources are down. Blend + depth
            // are NOT touched: the enclosing pass already binds the correct state for this DC's
            // opacity class (opaque: none + depthState; transparent: alphaBlend + depthReadOnly),
            // which is exactly what an opaque / transparent graph material wants.
            if (p.m_matReady && m_contentManager)
            {
                uint64_t matHash = 0; std::string matFrag, matVertBody;
                if (p.m_matShaderLib.resolveShaders(*m_contentManager, dc.materialAssetId,
                                                    matHash, matFrag, matVertBody))
                {
                    const bool matTransp = dc.opacity < 0.999f;
                    D3D11RendererImpl::MatShaders* sh =
                        p.getOrBuildMaterialShaders(matHash, matFrag, matVertBody, matTransp);
                    if (sh && sh->vs && sh->ps && sh->il)
                    {
                        // heTex0 = the material's base texture, matching the built-in selection +
                        // hasTex flag: an override material's texture wins (A2), else the mesh's
                        // baked texture (A1), else the white default. heTexP0..3 = white default
                        // this increment (real graph project textures are an A4 follow-up).
                        ID3D11ShaderResourceView* heTex0 = nullptr;
                        bool matTextured = false;
                        ID3D11ShaderResourceView* ovr = nullptr;
                        if (p.resolveMaterialOverride(dc.materialAssetId, m_contentManager, ovr))
                        {
                            heTex0 = ovr;                 // override wins (null → flat)
                            matTextured = (ovr != nullptr);
                        }
                        else if (m.texture)
                        {
                            heTex0 = m.texture.Get();     // baked mesh texture (A1)
                            matTextured = true;
                        }
                        if (!heTex0) heTex0 = p.dummyTexture.Get(); // white default → not textured

                        // Per-entity HeParams override wins over the material's shared params.
                        const MaterialAsset* ma = m_contentManager->getMaterial(dc.materialAssetId);
                        const std::vector<float>* params =
                            !dc.paramOverride.empty() ? &dc.paramOverride
                            : (ma && !ma->shaderParamData.empty() ? &ma->shaderParamData : nullptr);

                        // ── Bind material pipeline state ──────────────────────────────────
                        ctx->VSSetShader(sh->vs.Get(), nullptr, 0);
                        ctx->PSSetShader(sh->ps.Get(), nullptr, 0);
                        ctx->IASetInputLayout(sh->il.Get());
                        ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
                        ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
                        // HeLighting (b0 PS, b8 WPO VS) — same CB, filled once per frame.
                        ctx->PSSetConstantBuffers(0, 1, p.m_matLightCB.GetAddressOf());
                        ctx->VSSetConstantBuffers(8, 1, p.m_matLightCB.GetAddressOf());
                        // heTex0 (t2 PS) + heTexP0..3 (t4..t7 PS, white default) + linear-wrap
                        // samplers (s2 + s4..s7). t3 is intentionally unused by the mesh path.
                        ctx->PSSetShaderResources(2, 1, &heTex0);
                        ID3D11ShaderResourceView* whiteP[4] = {
                            p.dummyTexture.Get(), p.dummyTexture.Get(),
                            p.dummyTexture.Get(), p.dummyTexture.Get() };
                        ctx->PSSetShaderResources(4, 4, whiteP);
                        ID3D11SamplerState* matSamp = p.m_matSampler.Get();
                        ctx->PSSetSamplers(2, 1, &matSamp);
                        ID3D11SamplerState* matSamp4[4] = { matSamp, matSamp, matSamp, matSamp };
                        ctx->PSSetSamplers(4, 4, matSamp4);

                        auto drawMatInstance = [&](const glm::mat4& model) {
                            // std140 U block (176 B) at b1 VS.
                            struct MatU { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };
                            static_assert(sizeof(MatU) == 176, "material U block must be std140 176 B");
                            MatU u;
                            u.mvp   = viewProj * model;
                            u.model = model;
                            u.color = glm::vec4(dc.baseColor, 1.0f);
                            u.flags = glm::vec4(matTextured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                            u.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity, 0.0f);
                            D3D11_MAPPED_SUBRESOURCE mu{};
                            if (SUCCEEDED(ctx->Map(p.m_matObjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mu)))
                            {
                                std::memcpy(mu.pData, &u, sizeof(u));
                                ctx->Unmap(p.m_matObjCB.Get(), 0);
                            }
                            // HeParams (16 vec4 = 64 floats = 256 B) at b3 PS / b9 WPO VS, zero-padded.
                            float padded[64] = { 0.0f };
                            if (params)
                                std::memcpy(padded, params->data(),
                                            std::min(params->size(), size_t(64)) * sizeof(float));
                            D3D11_MAPPED_SUBRESOURCE mp{};
                            if (SUCCEEDED(ctx->Map(p.m_matParamCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
                            {
                                std::memcpy(mp.pData, padded, sizeof(padded));
                                ctx->Unmap(p.m_matParamCB.Get(), 0);
                            }
                            ctx->VSSetConstantBuffers(1, 1, p.m_matObjCB.GetAddressOf());   // b1 U (VS)
                            ctx->PSSetConstantBuffers(3, 1, p.m_matParamCB.GetAddressOf()); // b3 HeParams (PS)
                            ctx->VSSetConstantBuffers(9, 1, p.m_matParamCB.GetAddressOf()); // b9 HeParams (WPO VS)
                            ctx->DrawIndexed(m.indexCount, 0, 0);
                            ++p.counters.draws;
                            p.counters.tris += m.indexCount / 3;
                        };
                        // Instanced graph materials draw each instance via the material path (this
                        // increment does NOT combine graph materials with A3 GPU instancing).
                        if (!dc.instanceTransforms.empty())
                            for (const glm::mat4& t : dc.instanceTransforms) drawMatInstance(t);
                        else
                            drawMatInstance(dc.transform);

                        // ── CRITICAL: restore scene state for subsequent built-in draws ───
                        // The material path clobbered: VS/PS/IL, VS b1 (was perFrameCB, overwritten
                        // by U), PS b0 (was perObjectCB, overwritten by HeLighting), PS t2 (was
                        // aoSRV, overwritten by heTex0), and — since the GI port — PS t4..t6 +
                        // s2, which the built-in scene shader now reads (GI mask + probe
                        // atlases + linear-clamp sampler). VS b0 / PS b0 (perObject) and t0
                        // (albedo) are re-bound per draw by the built-in path, but PS b0 is
                        // restored here too since HeLighting overwrote it.
                        ctx->VSSetShader(p.vs.Get(), nullptr, 0);
                        ctx->PSSetShader(p.ps.Get(), nullptr, 0);
                        ctx->IASetInputLayout(p.inputLayout.Get());
                        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
                        ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
                        ctx->PSSetShaderResources(2, 1, &aoSRV); // t2 = AO (unoccluded white when off)
                        {
                            ID3D11ShaderResourceView* giSrvs[4] = {
                                giShadingActive ? giShadowSRV      : p.whiteSRV.Get(),
                                giShadingActive ? p.giIrrSRV.Get() : p.whiteSRV.Get(),
                                giShadingActive ? p.giVisSRV.Get() : p.whiteSRV.Get(),
                                (giShadingActive && p.giLocalMaskSRV) ? p.giLocalMaskSRV.Get()
                                                                      : p.whiteSRV.Get() };
                            ctx->PSSetShaderResources(4, 4, giSrvs);
                            if (p.giLinearClamp)
                                ctx->PSSetSamplers(2, 1, p.giLinearClamp.GetAddressOf());
                        }
                        return;
                    }
                }
            }
#endif
            // Base color: an explicit MaterialComponent override (dc.materialAssetId), once its
            // material is loaded, fully replaces the mesh's baked texture — even to flat.
            ID3D11ShaderResourceView* albedo = m.texture.Get(); // baked (may be null)
            ID3D11ShaderResourceView* ovr = nullptr;
            if (p.resolveMaterialOverride(dc.materialAssetId, m_contentManager, ovr))
                albedo = ovr; // override replaces the baked texture (null = flat)
            const float hasTex = albedo ? 1.0f : 0.0f;
            ID3D11ShaderResourceView* srv = albedo ? albedo : p.dummyTexture.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
            if (!dc.instanceTransforms.empty())
            {
                static_assert(D3D11RendererImpl::k_instStride == 2 * sizeof(glm::mat4),
                              "instance stride must be mvp+model");
                const UINT count = static_cast<UINT>(dc.instanceTransforms.size());
                const bool fits = allowInstancing && p.vsInstanced && p.instanceSRV
                                  && count <= p.k_maxInstances;
                if (fits)
                {
                    // A3: upload every instance's {mvp,model} to the structured buffer …
                    D3D11_MAPPED_SUBRESOURCE im{};
                    if (SUCCEEDED(ctx->Map(p.instanceSB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &im)))
                    {
                        auto* dst = static_cast<uint8_t*>(im.pData);
                        for (UINT k = 0; k < count; ++k)
                        {
                            const glm::mat4& t = dc.instanceTransforms[k];
                            const glm::mat4 xf[2] = { viewProj * t, t }; // mvp, model (column-major)
                            std::memcpy(dst + static_cast<size_t>(k) * p.k_instStride, xf, sizeof(xf));
                        }
                        ctx->Unmap(p.instanceSB.Get(), 0);
                    }
                    // … one PerObject CB (batch-constant colour/pbr; the instanced VS reads
                    // mvp/model from t3) … then ONE instanced draw.
                    uploadObject(glm::mat4(1.0f), glm::mat4(1.0f), dc.baseColor, hasTex,
                                 dc.metallic, dc.roughness, dc.opacity);
                    ctx->VSSetShader(p.vsInstanced.Get(), nullptr, 0);
                    ctx->VSSetShaderResources(3, 1, p.instanceSRV.GetAddressOf());
                    ctx->DrawIndexedInstanced(m.indexCount, count, 0, 0, 0);
                    // Restore the non-instanced VS and unbind t3 before the next draw/Map.
                    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* nullSRV = nullptr;
                    ctx->VSSetShaderResources(3, 1, &nullSRV);
                    ++p.counters.draws;
                    p.counters.tris += (m.indexCount / 3) * count;
                }
                else
                {
                    for (const glm::mat4& t : dc.instanceTransforms) { // fallback: transparent / ring full
                        uploadObject(viewProj * t, t, dc.baseColor, hasTex,
                                     dc.metallic, dc.roughness, dc.opacity);
                        ctx->DrawIndexed(m.indexCount, 0, 0);
                        ++p.counters.draws;
                        p.counters.tris += m.indexCount / 3;
                    }
                }
            }
            else {
                uploadObject(viewProj * dc.transform, dc.transform,
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity);
                ctx->DrawIndexed(m.indexCount, 0, 0);
                ++p.counters.draws;
                p.counters.tris += m.indexCount / 3;
            }
        };

        for (const DrawCall* dc : opaqueDCs) drawDC(*dc);

        // ── Skinned mesh pass ─────────────────────────────────────────────────
        // Shares PSMain (lighting + shadow + AO) already bound above.
        // Only the VS and input layout change; the rest of the pipeline is kept.
        if (p.skinnedVS && !cmds.skinnedDrawCalls().empty())
        {
            ctx->VSSetShader(p.skinnedVS.Get(), nullptr, 0);
            ctx->IASetInputLayout(p.skinnedLayout.Get());
            ctx->VSSetConstantBuffers(2, 1, p.bonesCB.GetAddressOf());

            constexpr int kMaxBones = 128;
            std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));

            for (const SkinnedDrawCall& dc : cmds.skinnedDrawCalls())
            {
                const GpuSkeletalMesh* sm = p.resolveSkeletalMesh(dc.meshAssetId, m_contentManager);
                if (!sm || !sm->vb || !sm->ib) continue;

                // Base color: MaterialComponent override wins over the baked texture (see drawDC).
                ID3D11ShaderResourceView* albedo = sm->srv.Get(); // baked (may be null)
                ID3D11ShaderResourceView* ovr = nullptr;
                if (p.resolveMaterialOverride(dc.materialAssetId, m_contentManager, ovr))
                    albedo = ovr;

                // Upload bone matrices to b2
                std::fill(boneScratch.begin(), boneScratch.end(), glm::mat4(1.0f));
                const int n = std::min(static_cast<int>(dc.boneMatrices.size()), kMaxBones);
                if (n > 0) std::copy_n(dc.boneMatrices.begin(), n, boneScratch.begin());
                {
                    D3D11_MAPPED_SUBRESOURCE mr{};
                    if (SUCCEEDED(ctx->Map(p.bonesCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr)))
                    {
                        std::memcpy(mr.pData, boneScratch.data(), kMaxBones * sizeof(glm::mat4));
                        ctx->Unmap(p.bonesCB.Get(), 0);
                    }
                }

                // Per-object CB (reuse the uploadObject lambda in scope)
                const float hasTex = albedo ? 1.0f : 0.0f;
                uploadObject(viewProj * dc.transform, dc.transform,
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity);

                // Bind three vertex buffer slots
                const UINT strides[3] = { 32u, 16u, 16u };
                const UINT offs[3]    = { 0u, 0u, 0u };
                ID3D11Buffer* vbs[3] = { sm->vb.Get(), sm->boneIdVb.Get(), sm->boneWgtVb.Get() };
                ctx->IASetVertexBuffers(0, 3, vbs, strides, offs);
                ctx->IASetIndexBuffer(sm->ib.Get(), DXGI_FORMAT_R32_UINT, 0);

                ID3D11ShaderResourceView* albedoSrv = albedo ? albedo : p.dummyTexture.Get();
                ctx->PSSetShaderResources(0, 1, &albedoSrv);

                ctx->DrawIndexed(static_cast<UINT>(sm->indexCount), 0, 0);
                ++p.counters.draws;
                p.counters.tris += static_cast<uint32_t>(sm->indexCount / 3);
            }

            // Restore scene VS + layout for the transparent pass
            ctx->VSSetShader(p.vs.Get(), nullptr, 0);
            ctx->IASetInputLayout(p.inputLayout.Get());
        }

        if (!transparentDCs.empty()) {
            allowInstancing = false; // transparent batches keep the per-instance loop (blend + depth sort)
            ctx->OMSetBlendState(p.alphaBlendState.Get(), nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(p.depthReadOnlyState.Get(), 0);
            for (const DrawCall* dc : transparentDCs) drawDC(*dc);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
        }
        // Debug lines on top of geometry, before post-process
        if (!p.m_debugLines.empty())
            p.drawDebugLines(ctx, viewProj, p.m_debugLines);
        // Unbind AO SRV before leaving
        { ID3D11ShaderResourceView* nullAO = nullptr; ctx->PSSetShaderResources(2, 1, &nullAO); }
    });
}

void D3D11Renderer::Render()
{
    auto& p = *m_impl;
    p.m_wallTime = static_cast<float>(SDL_GetTicks()) * 0.001f;
    p.counters = D3D11RendererImpl::FrameCounters{};
    p.gpuTimerBeginFrame();
    const float bgColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    // Recreate the viewport RT if the editor requested a different size.
    if (p.viewportReqW > 0 && p.viewportReqH > 0 &&
        (p.viewportReqW != p.viewportW || p.viewportReqH != p.viewportH))
        p.createViewportRT(p.viewportReqW, p.viewportReqH);

    const bool useViewport = p.viewportRTV && p.viewportDSV;

    if (useViewport)
    {
        D3D11_VIEWPORT vvp{};
        vvp.Width    = static_cast<float>(p.viewportW);
        vvp.Height   = static_cast<float>(p.viewportH);
        vvp.MaxDepth = 1.0f;

        // When PostFX is available, render geometry into the RGBA16F HDR target;
        // otherwise fall back to the RGBA8 viewport target directly.
        const bool useHDR = p.postFxReady && p.hdrRTV && p.ldrRTV && p.viewportRTV;
        ID3D11RenderTargetView* sceneRTV = useHDR ? p.hdrRTV.Get() : p.viewportRTV.Get();

        p.context->OMSetRenderTargets(1, &sceneRTV, p.viewportDSV.Get());
        p.context->ClearRenderTargetView(sceneRTV, bgColor);
        p.context->ClearDepthStencilView(p.viewportDSV.Get(),
                                         D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        p.context->RSSetViewports(1, &vvp);
        DrawScene(static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));

        if (useHDR)
        {
            // Unbind the HDR RT before using it as an SRV.
            { ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

            // Bloom bright-pass + ping-pong blur → bloomTex[0] (or dummyTexture if disabled).
            const uint32_t bw = std::max(1u, p.viewportW / 2);
            const uint32_t bh = std::max(1u, p.viewportH / 2);
            ID3D11ShaderResourceView* bloomResult =
                p.bloomEnabled ? p.runBloom(bw, bh) : p.dummyTexture.Get();

            // Restore full-res viewport for the tonemap and FXAA passes.
            p.context->RSSetViewports(1, &vvp);
            p.context->IASetInputLayout(nullptr);
            p.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            p.context->VSSetShader(p.fsVS.Get(), nullptr, 0);
            p.context->OMSetDepthStencilState(p.noDepthDSS.Get(), 0);
            p.context->RSSetState(p.fsRastState.Get());
            p.context->PSSetSamplers(0, 1, p.linearSampler.GetAddressOf());
            p.context->VSSetConstantBuffers(0, 1, p.postFxCB.GetAddressOf());
            p.context->PSSetConstantBuffers(0, 1, p.postFxCB.GetAddressOf());

            // Tonemap: (hdrSRV, bloomSRV) → ldrRTV.
            { const float cb[4] = { p.exposure,
                                    p.bloomEnabled ? p.bloomStrength : 0.0f, 0, 0 };
              p.updatePostFxCB(cb);
              p.context->OMSetRenderTargets(1, p.ldrRTV.GetAddressOf(), nullptr);
              p.context->PSSetShader(p.tonemapPS.Get(), nullptr, 0);
              ID3D11ShaderResourceView* srvs[2] = { p.hdrSRV.Get(), bloomResult };
              p.context->PSSetShaderResources(0, 2, srvs);
              p.context->Draw(3, 0);
              ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

            // FXAA: ldrSRV → viewportRTV (final output sampled by ImGui).
            { const float cb[4] = { 1.0f / float(p.viewportW),
                                    1.0f / float(p.viewportH), 0, 0 };
              p.updatePostFxCB(cb);
              p.context->OMSetRenderTargets(1, p.viewportRTV.GetAddressOf(), nullptr);
              p.context->PSSetShader(p.fxaaPS.Get(), nullptr, 0);
              ID3D11ShaderResourceView* srv = p.ldrSRV.Get();
              p.context->PSSetShaderResources(0, 1, &srv);
              p.context->Draw(3, 0);
              ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

            // Clear stale bindings, restore scene pipeline state for any future draws.
            { ID3D11ShaderResourceView* nulls[2] = {}; p.context->PSSetShaderResources(0, 2, nulls); }
            p.context->OMSetDepthStencilState(p.depthState.Get(), 0);
            p.context->RSSetState(p.rasterState.Get());
            p.context->PSSetSamplers(0, 1, p.sampler.GetAddressOf());
        }

        // UI canvas pass: draw onto the final composited viewport target (after tonemap/FXAA).
        p.context->OMSetRenderTargets(1, p.viewportRTV.GetAddressOf(), nullptr);
        p.context->RSSetViewports(1, &vvp);
        p.renderUIPass(p.context.Get(), static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));
        { ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

        // ImGui overlay → swapchain RT (clear first so it's a clean dark bg).
        p.context->OMSetRenderTargets(1, p.rtv.GetAddressOf(), nullptr);
        p.context->ClearRenderTargetView(p.rtv.Get(), bgColor);
    }
    else
    {
        // No viewport target requested — render scene directly to the swapchain.
        p.context->OMSetRenderTargets(1, p.rtv.GetAddressOf(), p.dsv.Get());
        p.context->ClearRenderTargetView(p.rtv.Get(), bgColor);
        if (p.dsv)
            p.context->ClearDepthStencilView(p.dsv.Get(),
                                             D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(p.width);
        vp.Height   = static_cast<float>(p.height);
        vp.MaxDepth = 1.0f;
        p.context->RSSetViewports(1, &vp);
        DrawScene(p.width, p.height);
        // UI canvas pass: swapchain RT + scene viewport already bound.
        p.renderUIPass(p.context.Get(), p.width, p.height);
    }

    if (m_overlayCallback) m_overlayCallback(nullptr);
    p.gpuTimerEndFrame();
    p.swapchain->Present(p.vsync ? 1 : 0, 0);
}

IRenderer::Capabilities D3D11Renderer::GetCapabilities() const
{
    Capabilities c{};
    c.supportsShadows        = true;
    c.supportsPostProcessing = m_impl->postFxReady;
    c.supportsHDR            = false;
    // Software ray-traced DDGI via CS 5.0 (FL 11.0 baseline) — same CPU-BVH
    // path as GL 4.3/Vulkan; cleared if the GI shaders fail to compile.
    c.supportsGlobalIllumination = m_impl->giSupported;
    return c;
}

void D3D11Renderer::SetGISettings(const GISettings& s)
{
    auto& p = *m_impl;
    p.giEnabled             = s.enabled && p.giSupported;
    p.giIndirectIntensity   = std::max(0.0f, s.indirectIntensity);
    p.giLightRadius         = std::clamp(s.lightRadius, 0.0f, 10.0f);
    p.giProbeBudgetPerFrame = std::clamp(s.probeBudgetPerFrame, 1, 4096);
}

void D3D11Renderer::SetViewportSize(uint32_t width, uint32_t height)
{
    m_impl->viewportReqW = width;
    m_impl->viewportReqH = height;
}

void* D3D11Renderer::GetViewportTexture()
{
    return m_impl->viewportSRV.Get();
}

bool D3D11Renderer::CaptureViewport(std::vector<uint8_t>& rgba, uint32_t& outW, uint32_t& outH)
{
    auto& p = *m_impl;
    if (!p.viewportTex || p.viewportW == 0 || p.viewportH == 0) return false;

    D3D11_TEXTURE2D_DESC desc{};
    p.viewportTex->GetDesc(&desc);
    desc.Usage          = D3D11_USAGE_STAGING;
    desc.BindFlags      = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags      = 0;

    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(p.device->CreateTexture2D(&desc, nullptr, &staging))) return false;
    p.context->CopyResource(staging.Get(), p.viewportTex.Get());

    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(p.context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;

    outW = p.viewportW;
    outH = p.viewportH;
    rgba.resize(static_cast<size_t>(outW) * outH * 4);
    const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
    for (uint32_t y = 0; y < outH; ++y)
        std::memcpy(rgba.data() + y * outW * 4, src + y * mapped.RowPitch, outW * 4);

    p.context->Unmap(staging.Get(), 0);
    return true;
}

void D3D11Renderer::SetVSync(bool enabled)
{
    Logger::Log(Logger::LogLevel::Info, enabled ? "D3D11Renderer: VSync enabled" : "D3D11Renderer: VSync disabled");
    m_impl->vsync = enabled;
}

void* D3D11Renderer::GetDevice()  const { return m_impl->device.Get(); }
void* D3D11Renderer::GetContext() const { return m_impl->context.Get(); }

void* D3D11Renderer::CreateImGuiTexture(const void* rgba8Pixels, int width, int height)
{
	D3D11_TEXTURE2D_DESC desc{};
	desc.Width            = static_cast<UINT>(width);
	desc.Height           = static_cast<UINT>(height);
	desc.MipLevels        = 1;
	desc.ArraySize        = 1;
	desc.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count = 1;
	desc.Usage            = D3D11_USAGE_DEFAULT;
	desc.BindFlags        = D3D11_BIND_SHADER_RESOURCE;

	D3D11_SUBRESOURCE_DATA initData{};
	initData.pSysMem     = rgba8Pixels;
	initData.SysMemPitch = static_cast<UINT>(width * 4);

	ComPtr<ID3D11Texture2D> tex;
	if (FAILED(m_impl->device->CreateTexture2D(&desc, &initData, &tex)))
		return nullptr;

	D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc{};
	srvDesc.Format              = DXGI_FORMAT_R8G8B8A8_UNORM;
	srvDesc.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
	srvDesc.Texture2D.MipLevels = 1;

	ID3D11ShaderResourceView* srv = nullptr;
	if (FAILED(m_impl->device->CreateShaderResourceView(tex.Get(), &srvDesc, &srv)))
		return nullptr;

	return srv;
}

void D3D11Renderer::DestroyImGuiTexture(void* handle)
{
	if (!handle) return;
	static_cast<ID3D11ShaderResourceView*>(handle)->Release();
}

void D3D11Renderer::SetDebugLines(const std::vector<DebugLine>& lines)
{
    m_impl->m_debugLines = lines;
}

void D3D11Renderer::SetSSAOSettings(const SSAOSettings& s)
{
    m_impl->ssaoEnabled   = s.enabled;
    m_impl->ssaoRadius    = s.radius;
    m_impl->ssaoIntensity = s.intensity;
    m_impl->ssaoMethod    = s.method;
}

void D3D11Renderer::SetBloomSettings(const BloomSettings& s)
{
    // Same field mapping as GL: threshold feeds the bright pass, intensity is
    // the tonemap's bloom add-back weight. The soft-knee stays at its default.
    m_impl->bloomEnabled   = s.enabled;
    m_impl->bloomThreshold = s.threshold;
    m_impl->bloomStrength  = s.intensity;
}

void D3D11Renderer::InvalidateMaterial(const HE::UUID& materialId)
{
    // Deferred to the next DrawScene (same thread), where the cache is safe to touch.
    if (m_impl && materialId != HE::UUID{})
        m_impl->pendingMatInval.push_back(materialId);
}

void D3D11Renderer::InvalidateMesh(const HE::UUID& meshId)
{
    if (m_impl && meshId != HE::UUID{})
        m_impl->pendingMeshInval.push_back(meshId);
}

IRenderer::FrameGpuStats D3D11Renderer::GetFrameGpuStats() const
{
    // GPU time comes from the newest reaped timestamp slot (1–N frames late;
    // -1 before the first reap / while timing is inactive). CPU counters are
    // this frame's.
    FrameGpuStats s = m_impl->lastGpuStats;
    s.drawCalls      = m_impl->counters.draws;
    s.triangles      = m_impl->counters.tris;
    s.visibleObjects = m_impl->counters.visible;
    s.totalObjects   = m_impl->counters.total;
    return s;
}

void D3D11Renderer::SetMoonTexture(const void* rgba8Pixels, int width, int height)
{
    auto& p = *m_impl;
    p.moonSRV.Reset(); p.moonTex2D.Reset();
    if (!rgba8Pixels || width <= 0 || height <= 0 || !p.device) return;
    D3D11_TEXTURE2D_DESC td{};
    td.Width = static_cast<UINT>(width); td.Height = static_cast<UINT>(height);
    td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = rgba8Pixels; srd.SysMemPitch = static_cast<UINT>(width*4);
    if (FAILED(p.device->CreateTexture2D(&td, &srd, &p.moonTex2D))) return;
    p.device->CreateShaderResourceView(p.moonTex2D.Get(), nullptr, &p.moonSRV);
}
