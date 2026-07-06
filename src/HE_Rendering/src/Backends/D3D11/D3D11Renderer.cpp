#include "Backends/D3D11/D3D11Renderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/RenderWorld.h>
#include <Renderer/UIRenderObject.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/FrustumCuller.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
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
};

Texture2D    uTexture   : register(t0);
Texture2D    uShadowMap : register(t1);
Texture2D    uAO        : register(t2);
SamplerState uSampler   : register(s0);
SamplerState uAOSampler : register(s1);

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
    float3 result  = ao * (ambDiff * 0.35f + ambSpec * (1.0f - 0.6f * rough));

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
        float  sh = (type == 0) ? shadowFactor(i.worldPos, N, L) : 1.0;
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
// cbuffer layout: rect(16) + color(16) + viewport(8) + pad(8) = 48 bytes.
static const char* kUIHLSL = R"HLSL(
cbuffer UICB : register(b0) {
    float4 uRect;      // xy = top-left in pixels, zw = size in pixels
    float4 uColor;     // rgba
    float2 uViewport;  // w, h in pixels
    float2 _upad;
};
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
    o.uv = uv;
    return o;
}
float4 UIPSMain(UIOut i) : SV_TARGET { return uColor; }
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
    };

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
    ComPtr<ID3D11Buffer>            uiCB;       // 48 bytes: rect(16)+color(16)+viewport(8)+pad(8)
    ComPtr<ID3D11BlendState>        uiBlend;    // alpha blend
    ComPtr<ID3D11DepthStencilState> uiDepth;    // depth test off

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
            if (const TextureAsset* tex = cm->resolveTextureRef(texId0, texPath0);
                tex && !tex->data.empty() && tex->channels == 4 && tex->format == TextureFormat::RGBA8)
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = tex->width; td.Height = tex->height;
                td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA srd{};
                srd.pSysMem = tex->data.data();
                srd.SysMemPitch = tex->width * 4;
                ComPtr<ID3D11Texture2D> t;
                if (SUCCEEDED(device->CreateTexture2D(&td, &srd, &t)))
                    device->CreateShaderResourceView(t.Get(), nullptr, &mesh.texture);
            }
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

        // cbuffer: float4 rect(16) + float4 color(16) + float2 viewport(8) + float2 pad(8) = 48 bytes
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 48u;
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        dev.CreateBuffer(&bd, nullptr, &uiCB);

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

        struct UICBData { glm::vec4 rect; glm::vec4 color; glm::vec2 viewport; glm::vec2 pad; };
        for (const UIRenderObject& obj : m_renderWorld.uiObjects)
        {
            if (obj.type == 2) continue; // font-atlas glyph quads: GL/Metal only for now
            UICBData cb;
            cb.rect     = glm::vec4(obj.position.x, obj.position.y, obj.size.x, obj.size.y);
            cb.color    = obj.color;
            cb.viewport = glm::vec2(float(width), float(height));
            cb.pad      = glm::vec2(0.0f, 0.0f);
            D3D11_MAPPED_SUBRESOURCE mr{};
            if (SUCCEEDED(ctx->Map(uiCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr)))
            {
                std::memcpy(mr.pData, &cb, sizeof(cb));
                ctx->Unmap(uiCB.Get(), 0);
            }
            ctx->Draw(4, 0);
        }

        // Restore: opaque blend, depth on
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
            if (const TextureAsset* tex = cm->resolveTextureRef(texId0, texPath0);
                tex && !tex->data.empty() && tex->channels == 4 && tex->format == TextureFormat::RGBA8)
            {
                D3D11_TEXTURE2D_DESC td{};
                td.Width = tex->width; td.Height = tex->height;
                td.MipLevels = 1; td.ArraySize = 1;
                td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
                td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                D3D11_SUBRESOURCE_DATA srd{};
                srd.pSysMem = tex->data.data();
                srd.SysMemPitch = tex->width * 4;
                ComPtr<ID3D11Texture2D> t;
                if (SUCCEEDED(device->CreateTexture2D(&td, &srd, &t)))
                    device->CreateShaderResourceView(t.Get(), nullptr, &mesh.srv);
            }
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
    m_impl->skyVS.Reset(); m_impl->skyPS.Reset(); m_impl->skyCB.Reset();
    m_impl->moonSRV.Reset(); m_impl->moonTex2D.Reset();
    m_impl->noiseSRV.Reset(); m_impl->noiseTex3D.Reset(); m_impl->skyNoiseSampler.Reset();
    m_impl->debugVS.Reset(); m_impl->debugPS.Reset(); m_impl->debugVB.Reset();
    m_impl->debugCB.Reset(); m_impl->debugIL.Reset();
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

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
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
        f.viewport = glm::vec4(float(width), float(height), (p.ssaoEnabled && p.ssaoReady) ? 1.0f : 0.0f, 0.0f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(p.perFrameCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &f, sizeof(f));
            ctx->Unmap(p.perFrameCB.Get(), 0);
        }
        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
    }

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

        ID3D11ShaderResourceView* aoSRV = p.whiteSRV.Get(); // default: unoccluded
        if (p.ssaoEnabled && p.ssaoReady) {
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

        auto drawDC = [&](const DrawCall& dc) {
            const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            const GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.vbuf || !m.ibuf) return;
            ID3D11ShaderResourceView* srv = m.texture ? m.texture.Get() : p.dummyTexture.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
            const float hasTex = m.texture ? 1.0f : 0.0f;
            if (!dc.instanceTransforms.empty())
                for (const glm::mat4& t : dc.instanceTransforms) {
                    uploadObject(viewProj * t, t, dc.baseColor, hasTex,
                                 dc.metallic, dc.roughness, dc.opacity);
                    ctx->DrawIndexed(m.indexCount, 0, 0);
                }
            else {
                uploadObject(viewProj * dc.transform, dc.transform,
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity);
                ctx->DrawIndexed(m.indexCount, 0, 0);
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
                const float hasTex = sm->srv ? 1.0f : 0.0f;
                uploadObject(viewProj * dc.transform, dc.transform,
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity);

                // Bind three vertex buffer slots
                const UINT strides[3] = { 32u, 16u, 16u };
                const UINT offs[3]    = { 0u, 0u, 0u };
                ID3D11Buffer* vbs[3] = { sm->vb.Get(), sm->boneIdVb.Get(), sm->boneWgtVb.Get() };
                ctx->IASetVertexBuffers(0, 3, vbs, strides, offs);
                ctx->IASetIndexBuffer(sm->ib.Get(), DXGI_FORMAT_R32_UINT, 0);

                ID3D11ShaderResourceView* albedoSrv = sm->srv ? sm->srv.Get() : p.dummyTexture.Get();
                ctx->PSSetShaderResources(0, 1, &albedoSrv);

                ctx->DrawIndexed(static_cast<UINT>(sm->indexCount), 0, 0);
            }

            // Restore scene VS + layout for the transparent pass
            ctx->VSSetShader(p.vs.Get(), nullptr, 0);
            ctx->IASetInputLayout(p.inputLayout.Get());
        }

        if (!transparentDCs.empty()) {
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
    const float bgColor[4] = { 0.18f, 0.18f, 0.20f, 1.0f };

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
    p.swapchain->Present(p.vsync ? 1 : 0, 0);
}

IRenderer::Capabilities D3D11Renderer::GetCapabilities() const { return { true, m_impl->postFxReady, false }; }

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
