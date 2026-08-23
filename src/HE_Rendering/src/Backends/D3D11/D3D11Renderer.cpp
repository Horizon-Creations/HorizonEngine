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
// ── Cross-backend renderer helpers (audit 1a) ────────────────────────────────
// Each of these replaced a private copy that every backend carried; the copies
// were byte-identical by contract (the GPU reads the packed bytes positionally),
// so the shared versions are what keeps GL == Metal == Vulkan == D3D.
#include <HorizonRendering/SkyNoise3D.h>      // CPU sky/cloud noise volume bake
#include <HorizonRendering/SsaoKernel.h>      // SSAO sample kernel + rotation noise
#include <HorizonRendering/SkyFrameParams.h>  // HE::BuildSkyFrameParams (folds in the cloud wind vector)
#include <HorizonRendering/LightPacking.h>    // GPU light window + shadow-mask lights
#include <HorizonRendering/ClipSpace.h>       // GL depth (-1..1) → D3D depth (0..1)
#include <HorizonRendering/RenderConstants.h> // shadow-map size, GPU timer ring depth
#include <HorizonRendering/PreviewFraming.h>  // shared thumbnail/preview camera + framing constants
#include <material/PreviewMesh.h>             // procedural sphere/cube/plane for material tiles
#include "Backends/D3D_Shared/HlslSources.h"  // HLSL byte-identical to the D3D12 backend
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
#include <fstream>   // HE_*_PREVIEW_DUMP PPM writer (mirrors the GL reference)
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;

// [blind] added D3D11 sky+IBL+debuglines parity

// ─── Shader sources ─────────────────────────────────────────────────────────
// Everything that is byte-identical to the D3D12 backend — the sky colour
// function, the sky VS, debug lines, the skinned VS, SSAO/HBAO/GTAO, the
// fullscreen-triangle VS + tonemap, and all five GI stages — now lives in
// Backends/D3D_Shared/HlslSources.h. That header also spells out why the HLSL
// that is still embedded below is NOT shared (different resource bindings, a
// feature the other backend has and this one does not). Pulled in unqualified
// so the compile sites further down read exactly as they did before.
using namespace HE::hlsl;

// ─── Sky background pass HLSL ───────────────────────────────────────────────
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

static const int GI_PROBE_OCT = 8; // must match the host's kGIProbeOctSize

// DDGI probe sampling — trilinear over the 8 surrounding probes × soft
// backface × Chebyshev visibility. Direct port of the GL/Metal version.
float3 sampleDDGIIrradiance(float3 P, float3 N)
{
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
    // Fresnel (Schlick, roughness-aware — same term as heLitP, ssr-plan P4).
    float  NdV = saturate(dot(N, V));
    float3 fresnelSpec = F0
        + (max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), F0) - F0) * pow(1.0f - NdV, 5.0f);
    float3 ambSpec = skyColor(Rrough, uSunDir.xyz) * fresnelSpec;
    float ao = (uViewport.z > 0.5f) ? uAO.SampleLevel(uAOSampler, i.clip.xy / uViewport.xy, 0).r : 1.0f;
    // GI replaces the AO-gated IBL diffuse with probe-grid indirect (spec IBL
    // stays in both branches) — mirrors the GL/Metal gi.enabled branch.
    float3 result;
    if (uGIParams.x > 0.5f)
        result = sampleDDGIIrradiance(i.worldPos, N) * base * kd * uGIParams.y
               + ambSpec * (1.0f - 0.6f * rough);
    else
        result = ao * (ambDiff * 0.35f + ambSpec * (1.0f - 0.6f * rough));

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
               ? uGIShadow.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0).r
               : shadowFactor(i.worldPos, N, L);
        }
        else
        {
            // Local (point/spot) lights: ray-traced hard shadows when GI is
            // active — one visibility channel per light (first 4), written by
            // the shadow kernel from unjittered secondary rays (previously
            // local lights had no shadowing at all).
            if (uGIParams.x > 0.5f && giLocalIdx < 4)
                sh = uGILocal.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0)[giLocalIdx];
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

// ─── PostProcess HLSL ───────────────────────────────────────────────────────
// kFSTriangleVS + kTonemapHLSL moved to D3D_Shared/HlslSources.h (byte-identical
// to D3D12's). FXAA and the two bloom passes stay here: the D3D12 copies declare
// an extra `Texture2D _dummy : register(t1)` for their root signature.

// AA = Off (docs/anti-aliasing-plan.md). The resolve pass is what fills the
// viewport target, so it always draws — with this passthrough instead of FXAA.
static const char* kAABlitHLSL = R"HLSL(
Texture2D    uScene : register(t0);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float2 uRcpFrame; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
float4 main(In i) : SV_Target {
    return float4(uScene.Sample(uSamp, i.uv).rgb, 1);
}
)HLSL";

// SMAA-style spatial AA (docs/anti-aliasing-plan.md, A1) — the HLSL twin of the
// GL kSmaaFS / Metal smaaFragment. Find the span the pixel's boundary belongs
// to, classify both ends, derive the coverage analytically from the position
// inside the span, blend perpendicular. Orthogonal (L/Z/U) patterns only — no
// AreaTex, hence no diagonals and no corner rounding. Keep in step with the GL
// version: the three are meant to produce the same image.
static const char* kSmaaHLSL = R"HLSL(
Texture2D    uScene : register(t0);
SamplerState uSamp  : register(s0);
cbuffer CB : register(b0) { float2 uRcpFrame; float2 _pad; };
struct In { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
static const float kEdgeMin     = 1.0/24.0;
static const float kEdgeRel     = 1.0/8.0;
// 8 single-texel steps, then double steps: 32 texels of reach. Reach decides
// whether shallow edges get antialiased at all — see the GL twin.
static const int   kFineSteps   = 8;
static const int   kSearchIters = 20;
float luma(float3 c) { return dot(c, float3(0.299,0.587,0.114)); }
float lumaAt(float2 uv) { return luma(uScene.Sample(uSamp, uv).rgb); }
float smaaSearch(float2 uv, float2 along, float2 across, float thr,
                 out bool ended, out float outerLuma)
{
    ended = false; outerLuma = 0.0;
    float dist = 0.0;
    for (int i = 0; i < kSearchIters; ++i)
    {
        float st = (i < kFineSteps) ? 1.0 : 2.0;
        dist += st;
        float2 p = uv + along * dist;
        float  a = lumaAt(p);
        float  b = lumaAt(p + across);
        if (abs(a - b) < thr) { ended = true; outerLuma = a; return dist - st; }
    }
    return dist;
}
float smaaCover(float t, float y1, float y2, bool split)
{
    float f = split ? ((t < 0.5) ? lerp(y1, 0.0, t * 2.0) : lerp(0.0, y2, (t - 0.5) * 2.0))
                    : lerp(y1, y2, t);
    return max(0.0, -f);
}
float smaaWeight(float2 uv, float2 along, float2 across, float lumaP, float lumaO, float thr)
{
    bool  e1, e2;
    float o1, o2;
    float d1 = smaaSearch(uv, -along, across, thr, e1, o1);
    float d2 = smaaSearch(uv,  along, across, thr, e2, o2);
    float len = d1 + d2 + 1.0;
    float t   = (d1 + 0.5) / len;
    float y1  = e1 ? (abs(o1 - lumaO) < abs(o1 - lumaP) ? -0.5 : 0.5) : 0.0;
    float y2  = e2 ? (abs(o2 - lumaO) < abs(o2 - lumaP) ? -0.5 : 0.5) : 0.0;
    bool  split = (e1 && e2 && y1 == y2);
    // Quadrature across the pixel, not one sample at its centre — see the GL twin.
    float dt = 0.25 / len;
    return 0.5 * (smaaCover(clamp(t - dt, 0.0, 1.0), y1, y2, split)
                + smaaCover(clamp(t + dt, 0.0, 1.0), y1, y2, split));
}
float4 main(In i) : SV_Target {
    float2 rcp = uRcpFrame;
    float3 C  = uScene.Sample(uSamp, i.uv).rgb;
    float  lC = luma(C);
    float  lW = lumaAt(i.uv + float2(-rcp.x, 0.0));
    float  lE = lumaAt(i.uv + float2( rcp.x, 0.0));
    float  lN = lumaAt(i.uv + float2(0.0, -rcp.y));
    float  lS = lumaAt(i.uv + float2(0.0,  rcp.y));
    float lMax = max(lC, max(max(lW, lE), max(lN, lS)));
    float lMin = min(lC, min(min(lW, lE), min(lN, lS)));
    float thr  = max(kEdgeMin, lMax * kEdgeRel);
    if (lMax - lMin < thr) return float4(C, 1);
    float edgeH = abs(lN - 2.0 * lC + lS);
    float edgeV = abs(lW - 2.0 * lC + lE);
    float wA = 0.0, wB = 0.0;
    float2 offA, offB;
    if (edgeH >= edgeV)
    {
        offA = float2(0.0, -rcp.y); offB = float2(0.0, rcp.y);
        if (abs(lC - lN) >= thr) wA = smaaWeight(i.uv, float2(rcp.x, 0.0), offA, lC, lN, thr);
        if (abs(lC - lS) >= thr) wB = smaaWeight(i.uv, float2(rcp.x, 0.0), offB, lC, lS, thr);
    }
    else
    {
        offA = float2(-rcp.x, 0.0); offB = float2(rcp.x, 0.0);
        if (abs(lC - lW) >= thr) wA = smaaWeight(i.uv, float2(0.0, rcp.y), offA, lC, lW, thr);
        if (abs(lC - lE) >= thr) wB = smaaWeight(i.uv, float2(0.0, rcp.y), offB, lC, lE, thr);
    }
    float sum = wA + wB;
    if (sum > 1.0) { wA /= sum; wB /= sum; sum = 1.0; }
    float3 outC = C * (1.0 - sum)
                + wA * uScene.Sample(uSamp, i.uv + offA).rgb
                + wB * uScene.Sample(uSamp, i.uv + offB).rgb;
    return float4(outC, 1);
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

// ─── Particle-preview billboards (RenderParticleThumbnail) ───────────────────
// Port of OpenGLRenderer's kParticlePreviewVS/FS. Camera-facing quads, one
// INSTANCE per already-simulated particle; the six corner vertices come from
// SV_VertexID (GL uses gl_VertexID the same way) so there is no per-vertex
// buffer at all — the only vertex stream is the per-instance one, byte-identical
// to GL's 8-float stride: pos3 + size1 + color3 + alpha1, read here as two
// float4s. This shader only draws; simulation lives in ParticleSystem::stepPool.
static const char* kParticlePreviewHLSL = R"HLSL(
cbuffer ParticlePreviewCB : register(b0)
{
    float4x4 uViewProj;
    float4   uCamRight; // xyz = camera right (view row 0)
    float4   uCamUp;    // xyz = camera up    (view row 1)
    float4   uFlags;    // x = hasTexture (0/1)
};

Texture2D    uTex     : register(t0);
SamplerState uTexSamp : register(s0);

struct VSIn
{
    float4 posSize    : POSITION; // xyz = world position, w = billboard size
    float4 colorAlpha : COLOR;    // rgb = tint, a = alpha
    uint   vid        : SV_VertexID;
};

struct VSOut
{
    float4 pos   : SV_POSITION;
    float3 color : COLOR0;
    float  alpha : COLOR1;
    float2 uv    : TEXCOORD0;
};

VSOut VSMain(VSIn i)
{
    // Two triangles, same winding order as the GL kCorners[] table. Expressed as
    // arithmetic on the vertex index rather than a static array so FXC never has
    // to emit a dynamically indexed constant table for six literals.
    uint   v  = i.vid % 6u;
    uint   c  = (v == 0u || v == 3u) ? 0u : ((v == 1u) ? 1u : ((v == 2u || v == 4u) ? 2u : 3u));
    float2 corner = float2((c == 1u || c == 2u) ? 1.0 : -1.0,
                           (c == 2u || c == 3u) ? 1.0 : -1.0);

    float3 worldPos = i.posSize.xyz
                    + (uCamRight.xyz * corner.x + uCamUp.xyz * corner.y) * (i.posSize.w * 0.5);
    VSOut o;
    o.pos   = mul(uViewProj, float4(worldPos, 1.0));
    o.uv    = corner * 0.5 + 0.5;
    o.color = i.colorAlpha.rgb;
    o.alpha = i.colorAlpha.a;
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    bool   hasTex = uFlags.x > 0.5;
    float4 texc   = hasTex ? uTex.Sample(uTexSamp, i.uv) : float4(1.0, 1.0, 1.0, 1.0);
    // No texture -> soft circular sprite instead of a flat square, so a bare
    // particle system still reads as "particles" rather than "confetti".
    float  shape  = hasTex ? texc.a : smoothstep(1.0, 0.0, length(i.uv * 2.0 - 1.0));
    return float4(i.color * texc.rgb, i.alpha * shape);
}
)HLSL";

// ─── Skeletal-mesh preview (RenderSkeletalPreview) ───────────────────────────
// Port of OpenGLRenderer's kSkelPreviewVS/FS. Deliberately NOT kSkinnedHLSL +
// the scene PS: that pair needs perFrameCB, the shadow map, the AO/GI SRVs and
// the sampler set a frame binds — none of which exist when a preview is asked
// for BEFORE the first Render(). This one is self-contained (b0 + b2 + t0/s0),
// so the skinning actually runs out of frame instead of degrading to bind pose.
//
// b0 is byte-identical to HE::hlsl::kMeshPreviewHLSL's MeshPreviewCB, so the
// C++ side fills the SAME MeshPreviewCB struct and binds the SAME meshPvCB for
// both the skinned and unskinned preview draws. b2 matches kSkinnedHLSL's
// BonesCB, so the existing 8192-byte bonesCB is reused unchanged.
//
// The PS mirrors GL's kSkelPreviewFS exactly and NOT kMeshPreviewHLSL's PSMain:
// the skeletal one has no specular lobe, ambient 0.35 (not 0.32) and a 0.65
// unlit-diffuse factor (not 0.68). Two shaders that look interchangeable but
// are not — sharing one would shift every skeletal preview off GL.
static const char* kSkelPreviewHLSL = R"HLSL(
cbuffer MeshPreviewCB : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4   uColor;     // rgb = base colour
    float4   uCamPos;    // unused here (no specular), kept so b0 matches kMeshPreviewHLSL
    float4   uPbr;       // z = hasTexture (GL's uHasTex); x/y unused here
    float4   uSun;       // xyz points TOWARD the light, w > 0 arms it
    float4   uSunColor;  // rgb
    float4   uAmbient;   // rgb
};

cbuffer BonesCB : register(b2)
{
    float4x4 uBoneMatrices[128];
};

Texture2D    uTex     : register(t0);
SamplerState uTexSamp : register(s0);

struct SkinIn
{
    float3 pos     : POSITION;
    float3 normal  : NORMAL;
    float2 uv      : TEXCOORD0;
    uint4  boneIds : BLENDINDICES;
    float4 boneWgt : BLENDWEIGHT;
};

struct VSOut
{
    float4 pos    : SV_POSITION;
    float3 normal : NORMAL;
    float2 uv     : TEXCOORD0;
};

VSOut VSMainSkinned(SkinIn i)
{
    float4x4 skin = i.boneWgt.x * uBoneMatrices[i.boneIds.x]
                  + i.boneWgt.y * uBoneMatrices[i.boneIds.y]
                  + i.boneWgt.z * uBoneMatrices[i.boneIds.z]
                  + i.boneWgt.w * uBoneMatrices[i.boneIds.w];
    float4 sp = mul(skin, float4(i.pos, 1.0));
    VSOut o;
    o.normal = mul((float3x3)uModel, mul((float3x3)skin, i.normal));
    o.uv     = i.uv;
    o.pos    = mul(uMVP, sp);
    return o;
}

float4 PSMain(VSOut i) : SV_TARGET
{
    bool   lit = uSun.w > 0.0;
    float3 L   = lit ? normalize(uSun.xyz) : normalize(float3(0.45, 0.75, 0.55));
    float3 lc  = lit ? uSunColor.rgb : float3(1.0, 1.0, 1.0);
    float3 amb = lit ? uAmbient.rgb  : float3(0.35, 0.35, 0.35);

    float3 N    = normalize(i.normal);
    float  diff = max(dot(N, L), 0.0);
    float3 albedo = (uPbr.z > 0.5) ? uTex.Sample(uTexSamp, i.uv).rgb * uColor.rgb
                                   : uColor.rgb;
    return float4(albedo * (amb + lc * (lit ? diff : 0.65 * diff)), 1.0);
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
        // BIND-POSE bounds, computed from the stored vertex positions exactly like
        // GpuMesh's (the loose-asset branch of resolveMesh). Without it there is
        // nothing to frame a skeletal preview or thumbnail on — the orbit camera
        // takes its centre and its distance scale from these. A posed mesh can of
        // course leave this box; the camera is deliberately NOT re-fitted per pose,
        // or the subject would drift while an animation scrubs (GL does the same).
        HE::AABB                         localBounds;
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

    struct SkyCB {
        glm::mat4 invViewProj;
        glm::vec3 sunDir;    float timeOfDay;
        glm::vec3 sunColor;  float cloudCoverage;
        glm::vec3 wind;      float time;
        glm::vec3 auroraColor; float aurora;
        float milkyWay;      float flash; int hasMoonTex; float _pad;
    };
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

    // ── Multi-window (P1d) ──────────────────────────────────────────────────
    // One DXGI swapchain + RTV per secondary window, keyed by SDL_Window* like the
    // GL/Vulkan/Metal backends. No depth buffer: RenderWindow() only clears and
    // presents, and Vulkan's secondary render pass has a single attachment too
    // (VulkanRenderer.cpp:1238-1247).
    //
    // Two declaration orders matter here, because members die in reverse order:
    //  * this map is declared AFTER `device`, so every secondary swapchain is
    //    released before the device is;
    //  * `swapchain` is declared before `rtv`, so the RTV — which holds a
    //    reference to back buffer 0 — is released before the swapchain it views.
    struct SecondaryWindow
    {
        ComPtr<IDXGISwapChain>         swapchain;
        ComPtr<ID3D11RenderTargetView> rtv;
    };
    std::unordered_map<SDL_Window*, SecondaryWindow> secondaryWindows;

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
    // equals today's built-in PBR path.
    //
    // ── Register map — THE CONTRACT, owned by MaterialShaderLibrary::fragment ──
    // SPIRV-Cross maps GLSL `binding = N` onto register(tN)/register(sN). SM 5.0
    // stops at s15 and the shared lighting preamble reaches binding 33, so the
    // library compiles the material fragment through he::shaderc::compileHlslPinned
    // with a table that keeps every TEXTURE at its binding number (t0..t127 is not
    // a constraint) and COMPACTS the six over-cap samplers into the free slots:
    //
    //   b0 HeLighting(PS) | b1 U(VS) | b3 HeParams(PS) | b8/b9 HeLighting/HeParams(WPO VS)
    //
    //   binding  name             SRV   sampler
    //      2     heTex0            t2  / s2
    //      4..7  heTexP0..3        t4..t7 / s4..s7
    //     10     heGIShadow       t10  / s10
    //     11     heGILocal        t11  / s11
    //     12     heCsm            t12  / s12   ** Texture2DArray **
    //     13     heLocalShadow    t13  / s13   ** Texture2DArray **
    //     15     heSkyEnv         t15  / s15   ** TextureCube **
    //     16     heAO             t16  / s0    (moved)
    //     17     heGIIrradiance   t17  / s1    (moved)
    //     18     heGIVisibility   t18  / s3    (moved)
    //     31     heSSRFwd         t31  / s8    (moved)
    //     32     heGIReflFwd      t32  / s9    (moved)
    //     33     heCloudShadow    t33  / s14   (moved)
    //
    // s0..s15 are therefore ALL consumed — the pinned material PS sits exactly on
    // the ps_5_0 sampler cap with zero headroom. A LANDSCAPE graph material adds
    // binding 14 (the weightmap) and so still cannot fit ps_5_0 at all; that half
    // of the old A4/X4509 gap is unchanged. Do not add a sampler here.
    //
    // Type matching is load-bearing: heCsm/heLocalShadow are Texture2DArray and
    // heSkyEnv is TextureCube in the generated HLSL. A plain Texture2D SRV in one
    // of those slots reads BLACK and only the debug layer says so, which is why
    // the typed 1x1 defaults below exist.
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
    ComPtr<ID3D11SamplerState> m_matSampler;  // linear-wrap, bound at s2 + s4..s7 (the graph textures)
    ComPtr<ID3D11SamplerState> m_matClamp;    // linear-CLAMP, every screen-space / shadow / env slot.
                                              // Deliberately NOT giLinearClamp: the thumbnail and
                                              // preview paths run before the first Render() and
                                              // before GI init, where giLinearClamp is still null.
    // TYPED 1x1 defaults for the preamble slots this backend has no real resource
    // for. Created ONCE here (not per draw) — see the type note in the register map.
    ComPtr<ID3D11ShaderResourceView> m_matWhiteArraySRV; // Texture2DArray, 1 layer, white
    ComPtr<ID3D11ShaderResourceView> m_matBlackCubeSRV;  // TextureCube, 1x1x6, transparent black
    ComPtr<ID3D11ShaderResourceView> m_matBlackSRV;      // Texture2D, 1x1, transparent black
    bool m_matReady      = false; // true once createMaterialResources() succeeded
    bool m_matHlslLogged = false; // one-time dump of generated HLSL for HW verify
    // createMaterialResources() + GetOrBuildMaterialShaders() are defined inline below.

    // ── Shadow map ──────────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>       depthVS;    // depth-only pass
    ComPtr<ID3D11Texture2D>          shadowTex;
    ComPtr<ID3D11DepthStencilView>   shadowDSV;
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    int shadowSize = HE::kShadowMapResolution;

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
    ComPtr<ID3D11PixelShader>       smaaPS;     // AA = SMAA
    ComPtr<ID3D11PixelShader>       aaBlitPS;   // AA = Off passthrough
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
    // Anti-aliasing method in force, already resolved against this backend's
    // capabilities (docs/anti-aliasing-plan.md). Off runs aaBlitPS instead of
    // fxaaPS — the pass itself always draws, it is what fills viewportRTV.
    HE::AAMethod aaMethod = HE::AAMethod::FXAA;

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

    // ── Profiler GPU timing (whole-frame + per-pass) ──────────────────────────
    // One D3D11_QUERY_TIMESTAMP pair inside a TIMESTAMP_DISJOINT per frame, kept
    // in a small ring so a slot is only read back HE::kGpuTimerRing frames after
    // it was issued — GetData(flags=0) at that age never blocks in practice, and
    // a not-yet-ready slot is dropped rather than stalling the pipeline. Queries
    // are only issued while the profiler is recording / live (never on the hot
    // path otherwise, mirroring the GL backend). The ring DEPTH is shared with
    // GL/Vulkan; the payload below is D3D11-specific (a DISJOINT query per slot,
    // which nobody else has — see gpuTimerReap's disjoint-frame rejection).
    //
    // P1e: the SAME slot now also carries the per-pass breakdown. D3D11 has no
    // GL_TIME_ELAPSED equivalent, so a pass costs TWO timestamp queries that
    // bracket it. They sit inside the SAME disjoint window as the whole-frame
    // pair and therefore share its Frequency AND its disjoint-frame rejection —
    // a second DISJOINT query would be redundant and could disagree with it.
    // The passes are strictly sequential siblings (no nesting, see
    // gpuTimerBeginPass), so their sum is exclusive and additive, which is what
    // the profiler panel promises for the "d3d11-timer" mode literal.
    struct GpuTimerSlot
    {
        ComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
        // Per-pass timestamp pairs. Created lazily on first use of an index and
        // then reused for the life of the slot (mirrors GL's growing query pool);
        // HE::kMaxTimedPasses caps them, extra passes are dropped, never written
        // out of range.
        ComPtr<ID3D11Query> passBegin[HE::kMaxTimedPasses];
        ComPtr<ID3D11Query> passEnd  [HE::kMaxTimedPasses];
        const char*         passName [HE::kMaxTimedPasses] = {};
        int                 passCount = 0;  // passes CLOSED during this use
        bool pending = false; // issued, result not consumed yet
    };
    GpuTimerSlot gpuSlots[HE::kGpuTimerRing];
    uint64_t gpuFrameIdx     = 0;
    int      gpuCurSlot      = -1;
    bool     gpuTimerInit    = false;
    bool     gpuTimingActive = false;
    bool     gpuWasActive    = false;
    bool     gpuDetailed     = false;
    bool     gpuPerPass      = false; // per-pass timestamps this frame (recording only)
    int      gpuPassOpen     = -1;    // index of the pass whose begin is issued (-1 = none)
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
        // Per-pass pairs, read BEFORE the slot is released: they were issued
        // between tsStart and tsEnd, so once those two are readable these are too.
        // A single unreadable pass query voids the whole breakdown (a partial one
        // would no longer sum to the frame) — the whole-frame number still stands.
        struct RawPass { const char* name; UINT64 a, b; };
        RawPass raw[HE::kMaxTimedPasses];
        int  rawCount = 0;
        bool passesOk = true;
        for (int i = 0; i < slot.passCount && i < HE::kMaxTimedPasses; ++i)
        {
            UINT64 a = 0, b = 0;
            if (!fetch(slot.passBegin[i].Get(), &a, sizeof(a)) ||
                !fetch(slot.passEnd[i].Get(),   &b, sizeof(b)))
            { passesOk = false; break; }
            raw[rawCount++] = { slot.passName[i], a, b };
        }
        slot.pending = false;
        // Disjoint frames (clock change / power event) yield garbage deltas —
        // keep the previous reading rather than publishing one. The per-pass
        // deltas ride the same clock, so this rejects them together.
        if (dj.Disjoint || dj.Frequency == 0 || t1 < t0) return;
        const double toMs = 1000.0 / static_cast<double>(dj.Frequency);
        lastGpuStats.gpuFrameMs = static_cast<double>(t1 - t0) * toMs;
        lastGpuStats.passes.clear();
        if (passesOk && rawCount > 0)
        {
            lastGpuStats.passes.reserve(static_cast<size_t>(rawCount));
            for (int i = 0; i < rawCount; ++i)
                lastGpuStats.passes.push_back({ raw[i].name,
                    (raw[i].b > raw[i].a) ? static_cast<double>(raw[i].b - raw[i].a) * toMs : 0.0,
                    /*approx=*/false }); // exact: a real timestamp pair, not an encoder span
        }
        // The mode reports what RAN: only claim per-pass when rows are present.
        lastGpuStats.gpuTimingMode = lastGpuStats.passes.empty() ? "whole-frame" : "d3d11-timer";
    }

    void gpuTimerBeginFrame()
    {
        // Latch the profiler decision once per frame so Begin/EndFrame agree
        // (a mid-frame toggle can never unbalance a Begin/End pair).
        EngineProfiler& prof = EngineProfiler::instance();
        const bool rec  = prof.isRecording();
        const bool live = prof.liveEnabled();
        gpuTimingActive = device && (rec || live);
        // Per-pass timestamps only while RECORDING (the live HUD shows the frame
        // total only) — same split as GL, so a live-HUD-only frame stays on the
        // cheap whole-frame path and reports "whole-frame".
        gpuPerPass  = gpuTimingActive && rec;
        // Same-frame reap (one Flush + spin) for detailed / single-frame capture:
        // the profiler reads that frame's stats immediately, so the async ring
        // would attribute a different frame's GPU time to it (mirrors GL's glFinish).
        gpuDetailed = gpuTimingActive && rec
                   && (prof.detailedGpuCapture() || prof.isSingleFrameCapture());
        const bool freshActivation = gpuTimingActive && !gpuWasActive;
        gpuWasActive = gpuTimingActive;
        gpuCurSlot   = -1;
        // Cleared BEFORE the early return: a mid-frame toggle-off would otherwise
        // strand the index and mute the next activation's first pass.
        gpuPassOpen  = -1;
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

        const int idx = static_cast<int>(gpuFrameIdx % HE::kGpuTimerRing);
        GpuTimerSlot& slot = gpuSlots[idx];
        gpuTimerReap(slot, /*block=*/false); // issued HE::kGpuTimerRing frames ago
        slot.passCount = 0;                  // recycle AFTER the reap consumed it
        gpuCurSlot = idx;
        context->Begin(slot.disjoint.Get());
        context->End(slot.tsStart.Get()); // timestamps have no Begin, only End
    }

    // Bracket one pass with a timestamp pair. Returns true iff a begin was issued
    // (the caller must then close it — use GpuPassScope, never a bare call).
    bool gpuTimerBeginPass(const char* name)
    {
        if (!gpuPerPass || gpuCurSlot < 0) return false;
        // No nesting: the rows must stay exclusive siblings or the sum silently
        // stops being additive. An inner Begin is a no-op, like GL's.
        if (gpuPassOpen >= 0) return false;
        GpuTimerSlot& slot = gpuSlots[gpuCurSlot];
        const int i = slot.passCount;
        if (i >= HE::kMaxTimedPasses) return false; // over cap → drop, never overflow
        if (!slot.passBegin[i] || !slot.passEnd[i])
        {
            const D3D11_QUERY_DESC tq{ D3D11_QUERY_TIMESTAMP, 0 };
            if (FAILED(device->CreateQuery(&tq, &slot.passBegin[i])) ||
                FAILED(device->CreateQuery(&tq, &slot.passEnd[i])))
            {
                slot.passBegin[i].Reset();
                slot.passEnd[i].Reset();
                return false; // no query pair → this pass simply goes untimed
            }
        }
        slot.passName[i] = name; // static literal, owned by the caller
        gpuPassOpen      = i;
        context->End(slot.passBegin[i].Get());
        return true;
    }

    void gpuTimerEndPass()
    {
        if (gpuPassOpen < 0 || gpuCurSlot < 0) return;
        GpuTimerSlot& slot = gpuSlots[gpuCurSlot];
        context->End(slot.passEnd[gpuPassOpen].Get());
        // Committed only now, so a pass that never closed can never be reaped.
        slot.passCount = gpuPassOpen + 1;
        gpuPassOpen    = -1;
    }

    // RAII pass timer, shaped like OpenGLRenderer::GpuPassScope: an early return
    // can never leave a begin without its end. end() closes it early so two
    // SIBLING passes can share one C++ scope without nesting (and without
    // re-indenting the region); it is idempotent, so the destructor cannot
    // double-end.
    struct GpuPassScope
    {
        D3D11RendererImpl* r;
        bool               active;
        // The Begin runs in the constructor BODY, not the init list: only a
        // function body is a complete-class context for the enclosing impl.
        GpuPassScope(D3D11RendererImpl* r_, const char* name)
            : r(r_), active(false) { active = r->gpuTimerBeginPass(name); }
        void end() { if (active) { r->gpuTimerEndPass(); active = false; } }
        ~GpuPassScope() { end(); }
        GpuPassScope(const GpuPassScope&)            = delete;
        GpuPassScope& operator=(const GpuPassScope&) = delete;
    };

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
        gpuPerPass   = false;
        gpuPassOpen  = -1;
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
            std::vector<glm::vec3> noiseData = HE::BuildSSAONoise(HE::kSsaoNoiseCount);
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
            std::vector<glm::vec3> kernel = HE::BuildSSAOKernel(HE::kSsaoKernelSize);
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
    struct GIBlasRange
    {
        int32_t nodeOffset = 0, nodeCount = 0;
        int32_t triOffset  = 0, triCount  = 0;
        bool    valid      = false;
    };
    struct GIInstanceGpu // must match the HLSL GiInst layout (raw structured buffer)
    {
        glm::mat4 invTransform;
        glm::vec4 baseColor;
        int32_t   nodeOffset = 0, triOffset = 0, pad0 = 0, pad1 = 0;
    };
    static constexpr float kGIProbeSpacing     = 4.0f;
    static constexpr int   kGIMaxProbesPerAxis = 10;
    static constexpr int   kGIProbeOctSize     = 8;

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

    std::unordered_map<HE::UUID, GIBlasRange> giBlasCache;
    std::vector<HE::GiBvhNode>     giNodesCpu;
    std::vector<HE::GiBvhTriangle> giTrisCpu;
    std::vector<GIInstanceGpu>     giInstancesCpu;
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

    GIBlasRange BuildGIBlas(ContentManager* cm, const HE::UUID& meshId)
    {
        GIBlasRange range;
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
        auto resolveRange = [&](const HE::UUID& id) -> GIBlasRange
        {
            auto it = giBlasCache.find(id);
            if (it == giBlasCache.end())
                it = giBlasCache.emplace(id, BuildGIBlas(cm, id)).first;
            return it->second;
        };
        for (const RenderObject& obj : rw.objects)
        {
            if (!obj.castsShadow) continue;
            // Default-cube fallback — entities without a resolvable mesh RENDER
            // as the default cube, so they must occlude as one too.
            GIBlasRange range = resolveRange(obj.meshAssetId);
            if (!range.valid) range = resolveRange(HE::kDefaultCubeMeshId);
            if (!range.valid) continue;
            GIInstanceGpu inst;
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
               sizeof(GIInstanceGpu), giInstanceSB, giInstanceSRV);
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

    // Builds the GI pipelines. Called eagerly from Initialize (so the compile cost
    // and any failure land before the first frame and before GetCapabilities() is
    // read); the call in runGiShadow remains as an idempotent safety net. A compile
    // failure logs + disables GI for the session (blind-port safety), like GL.
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
                HE_LOG_ERROR(RHI, "%s",
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
            HE_LOG_ERROR(RHI, "%s",
                        "D3D11Renderer: GI pipeline build failed — GI disabled");
            giGBufVS.Reset(); giGBufPS.Reset(); giShadowCS.Reset(); giProbeCS.Reset();
            giTemporalPS.Reset(); giBlurPS.Reset();
            giSupported = false;
            return;
        }
        HE_LOG_INFO(RHI, "%s",
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

        const glm::vec3 padded = sceneBox.extents() + glm::vec3(kGIProbeSpacing);
        giGridCounts = glm::ivec3(
            std::clamp(static_cast<int>(std::ceil(padded.x * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
            std::clamp(static_cast<int>(std::ceil(padded.y * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis),
            std::clamp(static_cast<int>(std::ceil(padded.z * 2.0f / kGIProbeSpacing)) + 1, 2, kGIMaxProbesPerAxis));
        const glm::vec3 gridSpan = glm::vec3(giGridCounts - 1) * kGIProbeSpacing;
        giGridOrigin   = sceneBox.center() - gridSpan * 0.5f;
        giProbeCount   = giGridCounts.x * giGridCounts.y * giGridCounts.z;
        giProbesPerRow = std::min(giProbeCount, 32);
        giProbeCursor  = 0;
        giProbeGridBuilt = true;
        HE_LOG_INFO(RHI, "%s",
                    ("D3D11Renderer: GI probe grid " + std::to_string(giGridCounts.x) + "x"
                     + std::to_string(giGridCounts.y) + "x" + std::to_string(giGridCounts.z)
                     + " (" + std::to_string(giProbeCount) + " probes)").c_str());
    }

    void ensureGiProbeAtlas()
    {
        if (giIrrTex || giProbeCount <= 0) return;
        const int rows = (giProbeCount + giProbesPerRow - 1) / giProbesPerRow;
        const int w = giProbesPerRow * kGIProbeOctSize;
        const int h = rows * kGIProbeOctSize;

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
            rw.dominantDirectionalLight(towardLight, lightColorIntensity);
            giFrameSeed += 1.0f;
            struct { glm::vec4 sunDirRadius, frame, localPosRange[4], localExtra; } scb{};
            scb.sunDirRadius = glm::vec4(towardLight, glm::radians(giLightRadius));
            scb.frame        = glm::vec4(giFrameSeed, float(giShadowW), float(giShadowH), 0.0f);
            // First 4 local (point/spot) lights of the same 8-light window the
            // scene shader iterates — PSMain counts non-directional lights in
            // the SAME order to index the mask channels, so count every
            // type != 0 light exactly like its loop does, fill the first 4.
            {
                static_assert(HE::kMaxMaskedLocalLights == 4, "scb.localPosRange holds 4 lights");
                const HE::PackedLocalShadowLights local = HE::BuildMaskedLocalLights(rw);
                for (int i = 0; i < HE::kMaxMaskedLocalLights; ++i)
                    scb.localPosRange[i] = local.posRange[i];
                scb.localExtra = glm::vec4(float(local.count), 0.0f, 0.0f, 0.0f);
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
        pcb.gridOrigin = glm::vec4(giGridOrigin, kGIProbeSpacing);
        pcb.gridCounts = glm::vec4(glm::vec3(giGridCounts), float(giProbesPerRow));
        const float maxDist = glm::length(glm::vec3(giGridCounts) * kGIProbeSpacing) + kGIProbeSpacing;
        pcb.rayParams = glm::vec4(maxDist, 0.92f, float(giProbeCursor), float(budget));
        glm::vec3 towardLight, lightColorIntensity;
        rw.dominantDirectionalLight(towardLight, lightColorIntensity);
        static_assert(HE::kMaxLightWindow == 8, "pcb light arrays hold 8 entries");
        const HE::PackedLightArray lights = HE::BuildPackedLightArray(rw);
        for (int i = 0; i < HE::kMaxLightWindow; ++i)
        {
            pcb.lightPosRange[i]  = lights.posRange[i];
            pcb.lightColorType[i] = lights.colorType[i];
            pcb.lightDirCos[i]    = lights.dirCos[i];
        }
        pcb.sunDirRadius = glm::vec4(towardLight, float(lights.count));
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
                HE_LOG_ERROR(RHI, "%s",
                    (std::string("D3D11 PostFX '") + entry + "' failed: "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsB, tmB, fxB, brB, blB, btB, smB;
        if (!compile(kFSTriangleVS,   "main", "vs_5_0", vsB)) return false;
        if (!compile(kTonemapHLSL,    "main", "ps_5_0", tmB)) return false;
        if (!compile(kFxaaHLSL,       "main", "ps_5_0", fxB)) return false;
        if (!compile(kSmaaHLSL,       "main", "ps_5_0", smB)) return false;
        if (!compile(kAABlitHLSL,     "main", "ps_5_0", btB)) return false;
        if (!compile(kBloomBrightHLSL,"main", "ps_5_0", brB)) return false;
        if (!compile(kBloomBlurHLSL,  "main", "ps_5_0", blB)) return false;

        device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &fsVS);
        device->CreatePixelShader (tmB->GetBufferPointer(), tmB->GetBufferSize(), nullptr, &tonemapPS);
        device->CreatePixelShader (fxB->GetBufferPointer(), fxB->GetBufferSize(), nullptr, &fxaaPS);
        device->CreatePixelShader (smB->GetBufferPointer(), smB->GetBufferSize(), nullptr, &smaaPS);
        device->CreatePixelShader (btB->GetBufferPointer(), btB->GetBufferSize(), nullptr, &aaBlitPS);
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

        postFxReady = fsVS && tonemapPS && fxaaPS && smaaPS && aaBlitPS && bloomBrightPS && bloomBlurPS
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
            HE_LOG_ERROR(RHI, "%s", (std::string("D3D11Renderer: VS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &psBlob, &err)))
        {
            HE_LOG_ERROR(RHI, "%s", (std::string("D3D11Renderer: PS compile failed: ")
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
            HE_LOG_ERROR(RHI, "%s", (std::string("D3D11Renderer: VSMainInstanced compile "
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
        // GI up front rather than on the first GI draw. Two reasons: the shader
        // compile is the expensive part and belongs in init, not in a frame; and
        // a compile failure here clears giSupported BEFORE GetCapabilities() is
        // first read, so the editor's GI toggle reflects reality from the start.
        // Idempotent (giPipelinesBuilt), so the lazy call in runGiShadow is a no-op.
        createGiPipelines();
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
        // Linear CLAMP for the screen-space / shadow / environment slots: a wrapping
        // sampler on a gl_FragCoord-derived UV folds the far edge back over the near
        // one at the screen border.
        D3D11_SAMPLER_DESC cd{};
        cd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        cd.AddressU = cd.AddressV = cd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        cd.MaxLOD   = D3D11_FLOAT32_MAX;
        const bool clampOk = SUCCEEDED(device->CreateSamplerState(&cd, &m_matClamp));

        // ── Typed 1x1 defaults ────────────────────────────────────────────────
        // CreateShaderResourceView(tex, nullptr, ...) infers TEXTURE2D for an
        // ArraySize == 1 resource, so the array view MUST be described explicitly
        // or heCsm/heLocalShadow get a Texture2D in a Texture2DArray slot — which
        // D3D11 answers with black and a debug-layer message, and nothing else.
        auto makeSrv = [&](const D3D11_TEXTURE2D_DESC& td, const D3D11_SUBRESOURCE_DATA* init,
                           const D3D11_SHADER_RESOURCE_VIEW_DESC& vd,
                           ComPtr<ID3D11ShaderResourceView>& out) -> bool {
            ComPtr<ID3D11Texture2D> tex;
            return SUCCEEDED(device->CreateTexture2D(&td, init, &tex))
                && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), &vd, &out));
        };
        const uint32_t kWhite = 0xFFFFFFFFu; // opaque white
        const uint32_t kBlack = 0x00000000u; // rgb 0, a 0

        // heCsm (t12) + heLocalShadow (t13): this backend has ONE shadow map, not a
        // cascade array and not a 16-layer local atlas, so both get this. WHITE =
        // stored depth 1.0 = "nothing in front of the light", the value that makes
        // the PCF loops resolve to fully lit if they ever ran. They do not: the
        // preamble gates them on csmSplits.w and lightParams[i].y, both left 0 by
        // fillMatLight, so this is belt-and-braces.
        bool defOk = true;
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = &kWhite; srd.SysMemPitch = 4;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
            vd.Texture2DArray.MipLevels = 1;
            vd.Texture2DArray.ArraySize = 1;
            defOk = makeSrv(td, &srd, vd, m_matWhiteArraySRV) && defOk;
        }
        // heSkyEnv (t15): no prefiltered environment cube on this backend — the sky
        // is raymarched per pixel in its own pass, there is nothing to hand a
        // material. BLACK, and heLight.fog.z stays 0 so the branch never runs. If
        // the gate were ever flipped on by mistake, black contributes nothing
        // (the term folds away) where white would blow the image out.
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 6;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            td.MiscFlags = D3D11_RESOURCE_MISC_TEXTURECUBE;
            D3D11_SUBRESOURCE_DATA srd[6];
            for (auto& s : srd) { s.pSysMem = &kBlack; s.SysMemPitch = 4; s.SysMemSlicePitch = 4; }
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURECUBE;
            vd.TextureCube.MipLevels = 1;
            defOk = makeSrv(td, srd, vd, m_matBlackCubeSRV) && defOk;
        }
        // heSSRFwd (t31) + heGIReflFwd (t32): no screen-space trace and no
        // ray-traced reflection pass on this backend. rgb = radiance, a =
        // confidence, and the preamble mixes by `a` — so (0,0,0,0) is the value
        // that folds the term away even with the ssr.x / giRefl.z gates on. White
        // here would paint a full-strength white reflection over every surface.
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = td.Height = 1; td.MipLevels = 1; td.ArraySize = 1;
            td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA srd{}; srd.pSysMem = &kBlack; srd.SysMemPitch = 4;
            D3D11_SHADER_RESOURCE_VIEW_DESC vd{};
            vd.Format        = DXGI_FORMAT_R8G8B8A8_UNORM;
            vd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
            vd.Texture2D.MipLevels = 1;
            defOk = makeSrv(td, &srd, vd, m_matBlackSRV) && defOk;
        }
        m_matReady = cbLight && cbObj && cbParam && sampOk && clampOk && defOk;
        Logger::LogTo(HE::Log::Cat::RHI, m_matReady ? Logger::LogLevel::Info : Logger::LogLevel::Error,
            m_matReady ? "D3D11Renderer: A4 material resources created"
                       : "D3D11Renderer: A4 material resource allocation failed");
#endif
    }

#if defined(HE_HAVE_SHADERC)
    // ── The shared lighting preamble's texture + sampler window ──────────────
    // EVERY lit graph material's PS declares all of these (they live in
    // kLightingPreamble, not in the node graph), so every one of them has to be
    // bound or the shader samples an unbound slot. ONE implementation, called by
    // the scene draw path AND by the preview/thumbnail path, so the two cannot
    // drift; the per-material heTex0 (t2) + heTexP0..3 (t4..t7) stay with the
    // caller since only it knows the material.
    //
    // Pass nullptr for anything the caller has no real resource for and the typed
    // default is used. The preview path passes nullptr for all five: it renders
    // with its own studio light, outside the frame's GI/SSAO entirely.
    //
    // NOTE the sampler call is PSSetSamplers(0, 16) — one shot for the whole file.
    // The pinned register map consumes every ps_5_0 sampler slot, so a partial
    // update would just leave a stale sampler from the previous pass in the gap.
    void bindMaterialPreamble(ID3D11DeviceContext* c,
                              ID3D11ShaderResourceView* aoSrv       = nullptr,
                              ID3D11ShaderResourceView* giSunMask   = nullptr,
                              ID3D11ShaderResourceView* giLocalMask = nullptr,
                              ID3D11ShaderResourceView* giIrr       = nullptr,
                              ID3D11ShaderResourceView* giVis       = nullptr)
    {
        // whiteSRV is 1x1 R8_UNORM white. Every slot it defaults below reads .r
        // only — the two screen-space masks, AO and the cloud transmittance — and
        // for all four 1.0 is the fold-away value: unoccluded, fully visible, fully
        // transmitting. Same texture the built-in scene path uses for the same
        // inputs. Do NOT reuse it for a slot that reads .rgb: an R8 view returns
        // (1, 0, 0, 1), i.e. red, not white.
        ID3D11ShaderResourceView* const white = whiteSRV ? whiteSRV.Get() : dummyTexture.Get();

        // t10..t18 in one call. t14 is the landscape weightmap — no non-landscape
        // graph material declares it, but it sits inside the range, so it gets the
        // white 2D dummy rather than a hole.
        ID3D11ShaderResourceView* srvs[9] = {
            giSunMask   ? giSunMask   : white,  // t10 heGIShadow      (gate giParams.z)
            giLocalMask ? giLocalMask : white,  // t11 heGILocal       (gate giParams.z)
            m_matWhiteArraySRV.Get(),           // t12 heCsm           ** ARRAY **
            m_matWhiteArraySRV.Get(),           // t13 heLocalShadow   ** ARRAY **
            dummyTexture.Get(),                 // t14 landscape weightmap (not declared here)
            m_matBlackCubeSRV.Get(),            // t15 heSkyEnv        ** CUBE **
            aoSrv       ? aoSrv       : white,  // t16 heAO            (gate fog.w)
            // t17/t18 read .rgb and .rg (probe radiance / depth moments), so their
            // default is the RGBA8 dummy, not the R8 `white` above. No value here
            // is a MEANINGFUL probe field — giProbe.y gates them off whenever the
            // atlases are absent; this is only about handing the slot something
            // that is neither null nor accidentally red.
            giIrr       ? giIrr       : dummyTexture.Get(), // t17 heGIIrradiance (gate giProbe.y)
            giVis       ? giVis       : dummyTexture.Get(), // t18 heGIVisibility (gate giProbe.y)
        };
        c->PSSetShaderResources(10, 9, srvs);

        // t31..t33: none of the three exists on this backend.
        ID3D11ShaderResourceView* fwd[3] = {
            m_matBlackSRV.Get(),                // t31 heSSRFwd        (gate ssr.x)
            m_matBlackSRV.Get(),                // t32 heGIReflFwd     (gate giRefl.z)
            white,                              // t33 heCloudShadow   (gate cloudShadowB.x)
                                                //     white = transmittance 1 = no cloud shadow
        };
        c->PSSetShaderResources(31, 3, fwd);

        ID3D11SamplerState* wrap  = m_matSampler.Get(); // the graph textures
        ID3D11SamplerState* clamp = m_matClamp.Get();   // everything else
        ID3D11SamplerState* samps[16] = {
            clamp,  // s0  heAO           (moved from s16)
            clamp,  // s1  heGIIrradiance (moved from s17)
            wrap,   // s2  heTex0
            clamp,  // s3  heGIVisibility (moved from s18)
            wrap,   // s4  heTexP0
            wrap,   // s5  heTexP1
            wrap,   // s6  heTexP2
            wrap,   // s7  heTexP3
            clamp,  // s8  heSSRFwd       (moved from s31)
            clamp,  // s9  heGIReflFwd    (moved from s32)
            clamp,  // s10 heGIShadow
            clamp,  // s11 heGILocal
            clamp,  // s12 heCsm
            clamp,  // s13 heLocalShadow
            clamp,  // s14 heCloudShadow  (moved from s33)
            clamp,  // s15 heSkyEnv
        };
        c->PSSetSamplers(0, 16, samps);
    }
#endif // HE_HAVE_SHADERC

    // Build (or fetch from cache) the per-material VS + PS + input layout from the
    // MaterialShaderLibrary HLSL. Cached by hash^transparentbit for signature parity with
    // the D3D12/Vulkan GetOrBuild* (the transparent bit is redundant on D3D11 — the shader
    // objects don't bake blend/depth — but kept so the cache key matches the other backends).
    // Returns nullptr (and caches the miss so it never retries per-draw) on any failure.
    MatShaders* GetOrBuildMaterialShaders(uint64_t hash, const std::string& frag,
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
            HE_LOG_WARN(RHI, "%s", "D3D11Renderer: A4 material shader cross-compile failed");
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }
        if (!m_matHlslLogged)
        {
            m_matHlslLogged = true;
            HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material VS HLSL:\n") + vc.source).c_str());
            HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material PS HLSL:\n") + fc.source).c_str());
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
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: A4 material VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }
        if (FAILED(D3DCompile(fc.source.c_str(), fc.source.size(), "matPS", nullptr, nullptr,
                              "main", "ps_5_0", cflags, 0, &psb, &cerr)))
        {
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: A4 material PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            m_materialShaders.emplace(key, MatShaders{});
            return nullptr;
        }

        MatShaders sh;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &sh.vs)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &sh.ps)))
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: A4 material shader-object creation failed");
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
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: A4 material input layout creation failed");
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
                HE_LOG_ERROR(RHI, "%s",
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
        const std::vector<uint16_t> noise = HE::BuildSkyNoise3D(kNoiseN);
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
            HE_LOG_ERROR(RHI, "%s", "D3D11 DebugLine VS compile failed");
            return false;
        }
        if (FAILED(D3DCompile(kDebugLineHLSL, std::strlen(kDebugLineHLSL),
                              "dbgline", nullptr, nullptr, "PSLine", "ps_5_0", flags, 0, &psB, &err)))
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11 DebugLine PS compile failed");
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
        // Translate the environment through the SHARED sky-constants builder
        // instead of hand-assigning fields (which is how D3D11 previously ended up
        // with +cos where GL/Metal have -cos, drifting the clouds 180° the wrong
        // way). SkyCB is a small subset of SkyFrameParams, so read the named
        // fields out — NOT a memcpy: the layouts differ.
        HE::SkyFrameInputs skyIn;
        skyIn.invViewProj    = invVP;
        skyIn.sunDir         = sunDir;
        skyIn.time           = m_wallTime;
        skyIn.hasMoonTexture = moonSRV ? true : false; // ComPtr → contextual bool
        const HE::SkyFrameParams sp = HE::BuildSkyFrameParams(env, skyIn);
        SkyCB cb{};
        cb.invViewProj = sp.invViewProj;
        cb.sunDir      = glm::vec3(sp.sunDir);   cb.timeOfDay     = sp.params.x;
        cb.sunColor    = glm::vec3(sp.sunColor); cb.cloudCoverage = sp.params.y;
        cb.wind        = glm::vec3(sp.wind);     cb.time          = sp.params.z;
        cb.auroraColor = glm::vec3(sp.auroraColor); cb.aurora     = sp.params.w;
        cb.milkyWay    = sp.auroraColor.w;       cb.flash         = sp.wind.w;
        cb.hasMoonTex  = sp.sunDir.w > 0.5f ? 1 : 0;   // sunDir.w is the 0/1 has-moon flag
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
            HE_LOG_ERROR(RHI, "%s",
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
            HE_LOG_ERROR(RHI, "%s", "D3D11: UI VS compile failed");
            if (err) OutputDebugStringA(static_cast<const char*>(err->GetBufferPointer()));
            return;
        }
        if (FAILED(D3DCompile(kUIHLSL, strlen(kUIHLSL), nullptr, nullptr, nullptr,
                              "UIPSMain", "ps_5_0", flags, 0, &psBlob, &err)))
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11: UI PS compile failed");
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
        // Bind-pose bounds from the stored positions — the same call the loose
        // static-mesh path makes. SkeletalMeshAsset ships no baked boundsMin/Max
        // (unlike StaticMeshAsset's cooked branch), so there is only this one path.
        mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
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

    // ─── Content-Browser thumbnails ──────────────────────────────────────────
    // Everything below is reachable BEFORE the first Render(): the editor asks for
    // tiles while the content browser first populates, and viewportTex/hdrTex/
    // ldrTex/bloomTex[] are only created inside Render(). So this path owns a
    // PRIVATE target and touches no Render()-created resource, and it sets every
    // piece of pipeline state it depends on rather than inheriting a frame's — the
    // immediate context's IA topology defaults to UNDEFINED (a silently dropped
    // draw) and its rasterizer default to CULL_BACK (half a silhouette missing).

    ComPtr<ID3D11Texture2D>        thumbTex;      // R8G8B8A8_UNORM (never _SRGB)
    ComPtr<ID3D11RenderTargetView> thumbRTV;
    ComPtr<ID3D11Texture2D>        thumbDepthTex; // D32_FLOAT
    ComPtr<ID3D11DepthStencilView> thumbDSV;
    ComPtr<ID3D11Texture2D>        thumbStaging;  // CPU-readable copy target
    int thumbSize = 0;

    // Mesh/material tile pipeline (HE::hlsl::kMeshPreviewHLSL).
    ComPtr<ID3D11VertexShader> meshPvVS;
    ComPtr<ID3D11PixelShader>  meshPvPS;
    ComPtr<ID3D11InputLayout>  meshPvIL;
    ComPtr<ID3D11Buffer>       meshPvCB;
    bool meshPvTried = false; // one-shot: never re-attempt a failed compile per tile

    // The material tile's / material preview's procedural primitive
    // (HE::buildPreviewMesh: 0 sphere, 1 cube, 2 plane). ONE shared pair of
    // buffers rebuilt when the requested shape changes, exactly like GL's single
    // m_previewVAO + m_previewShape — the thumbnail path always wants shape 0 and
    // rebuilds if the Material Editor left a cube behind.
    ComPtr<ID3D11Buffer> previewSphereVB;
    ComPtr<ID3D11Buffer> previewSphereIB;
    UINT                 previewSphereIdx   = 0;
    int                  previewShapeBuilt  = -1; // -1 = nothing built yet

    // Particle tile pipeline (kParticlePreviewHLSL).
    ComPtr<ID3D11VertexShader> particlePvVS;
    ComPtr<ID3D11PixelShader>  particlePvPS;
    ComPtr<ID3D11InputLayout>  particlePvIL;
    ComPtr<ID3D11Buffer>       particlePvCB;
    ComPtr<ID3D11Buffer>       particlePvInstVB; // dynamic, grown on demand
    UINT                       particlePvInstCap = 0;
    ComPtr<ID3D11BlendState>   particlePvBlend;  // SRC_ALPHA/INV_SRC_ALPHA on ALL FOUR channels
    bool particlePvTried = false;

    // Must match kMeshPreviewHLSL's cbuffer at b0, in order (224 B).
    struct MeshPreviewCB
    {
        glm::mat4 mvp;
        glm::mat4 model;
        glm::vec4 color;
        glm::vec4 camPos;
        glm::vec4 pbr;      // x=metallic y=roughness z=hasTexture w=0
        glm::vec4 sun;      // w = 0 → the shader's fixed studio light (GL parity)
        glm::vec4 sunColor;
        glm::vec4 ambient;
    };
    static_assert(sizeof(MeshPreviewCB) == 224, "MeshPreviewCB must match kMeshPreviewHLSL b0");

    // Must match kParticlePreviewHLSL's cbuffer at b0 (112 B).
    struct ParticlePreviewCB
    {
        glm::mat4 viewProj;
        glm::vec4 camRight;
        glm::vec4 camUp;
        glm::vec4 flags; // x = hasTexture
    };
    static_assert(sizeof(ParticlePreviewCB) == 112, "ParticlePreviewCB must match kParticlePreviewHLSL b0");

    // Lazily (re)create the shared thumbnail target at S×S. Its own texture, not
    // the viewport's: a tile rendered into the viewport target would replace what
    // the scene view is showing — and on the first run the viewport target does
    // not exist yet at all.
    bool ensureThumbnailTarget(int S)
    {
        if (thumbRTV && thumbDSV && thumbStaging && thumbSize == S) return true;
        thumbSize = 0;
        thumbRTV.Reset(); thumbTex.Reset();
        thumbDSV.Reset(); thumbDepthTex.Reset();
        thumbStaging.Reset();
        if (!device || S <= 0) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width = td.Height = static_cast<UINT>(S);
        td.MipLevels = td.ArraySize = 1;
        td.Format             = DXGI_FORMAT_R8G8B8A8_UNORM; // never _SRGB: the caller wants raw bytes
        td.SampleDesc.Count   = 1;
        td.Usage              = D3D11_USAGE_DEFAULT;
        td.BindFlags          = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &thumbTex))) return false;
        if (FAILED(device->CreateRenderTargetView(thumbTex.Get(), nullptr, &thumbRTV))) return false;

        D3D11_TEXTURE2D_DESC dd = td;
        dd.Format    = DXGI_FORMAT_D32_FLOAT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &thumbDepthTex))) return false;
        if (FAILED(device->CreateDepthStencilView(thumbDepthTex.Get(), nullptr, &thumbDSV))) return false;

        // Cached staging copy: the content browser asks for many tiles in a row and
        // a per-tile CreateTexture2D is pure overhead at a fixed size.
        D3D11_TEXTURE2D_DESC sd = td;
        sd.Usage          = D3D11_USAGE_STAGING;
        sd.BindFlags      = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &thumbStaging))) return false;

        thumbSize = S;
        return true;
    }

    // Bind + clear the thumbnail target. Clear colour is FULLY TRANSPARENT — the
    // Content Browser composites the tile over its own backdrop.
    void beginThumbnailPass(int S)
    {
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetRenderTargets(1, thumbRTV.GetAddressOf(), thumbDSV.Get());
        context->ClearRenderTargetView(thumbRTV.Get(), clear);
        context->ClearDepthStencilView(thumbDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{};
        vp.Width = vp.Height = static_cast<float>(S);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
        context->RSSetState(rasterState.Get()); // CULL_NONE — GL disables culling here
    }

    // Unbind everything the tile bound. A texture still bound as an RTV reads back
    // black, and a stale SRV on t0 trips the debug layer on the next pass.
    void endThumbnailPass()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        context->PSSetShaderResources(0, 1, &nullSrv);
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
        context->OMSetDepthStencilState(nullptr, 0);
    }

    // Read the tile back as tightly packed, TOP-DOWN RGBA8. D3D11 render targets
    // are already top-down (row 0 = top), so unlike GL there is NO flip here — but
    // the mapped RowPitch is not necessarily S*4, so the rows are de-striped.
    bool readThumbnailTarget(int S, std::vector<uint8_t>& outRgba8)
    {
        if (!thumbTex || !thumbStaging) return false;
        context->CopyResource(thumbStaging.Get(), thumbTex.Get());

        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (FAILED(context->Map(thumbStaging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
        const size_t rowBytes = static_cast<size_t>(S) * 4;
        outRgba8.assign(rowBytes * static_cast<size_t>(S), 0);
        const uint8_t* src = static_cast<const uint8_t*>(mapped.pData);
        for (int y = 0; y < S; ++y)
            std::memcpy(outRgba8.data() + static_cast<size_t>(y) * rowBytes,
                        src + static_cast<size_t>(y) * mapped.RowPitch, rowBytes);
        context->Unmap(thumbStaging.Get(), 0);
        return true;
    }

    // Compile the small pos/normal/uv preview program once (shared with D3D12 via
    // HlslSources.h, and behaviourally with GL's kMeshPreview* program).
    bool ensureMeshPreviewPipeline()
    {
        if (meshPvVS && meshPvPS && meshPvIL && meshPvCB) return true;
        if (meshPvTried) return false;
        meshPvTried = true;
        if (!device) return false;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(kMeshPreviewHLSL, strlen(kMeshPreviewHLSL), "meshPreview",
                                  nullptr, nullptr, entry, profile, flags, 0, &out, &err)))
            {
                HE_LOG_ERROR(RHI, "%s",
                    (std::string("D3D11 mesh-preview '") + entry + "' failed: "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsb, psb;
        if (!compile("VSMain", "vs_5_0", vsb)) return false;
        if (!compile("PSMain", "ps_5_0", psb)) return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &meshPvVS)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &meshPvPS)))
            return false;

        // Same interleaved 32-byte pos/normal/uv vertex the scene meshes and
        // HE::buildPreviewMesh both produce.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",   0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT,    0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device->CreateInputLayout(layout, 3, vsb->GetBufferPointer(), vsb->GetBufferSize(), &meshPvIL)))
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(MeshPreviewCB);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &meshPvCB))) return false;
        return true;
    }

    // Upload the preview primitive for `shape`, reusing the cached buffers when the
    // shape is unchanged. Geometry comes from HE::buildPreviewMesh, shared with the
    // GL and Metal paths so the three backends can never draw different spheres.
    // IMMUTABLE buffers, hence the full recreate on a shape change — the primitive
    // switches only when the Material Editor's shape combo moves, not per frame.
    bool ensurePreviewShape(int shape)
    {
        shape = std::clamp(shape, 0, 2);
        if (previewSphereVB && previewSphereIB && previewSphereIdx && previewShapeBuilt == shape)
            return true;
        previewSphereVB.Reset();
        previewSphereIB.Reset();
        previewSphereIdx  = 0;
        previewShapeBuilt = -1;

        std::vector<float>    verts;
        std::vector<uint32_t> idx;
        HE::buildPreviewMesh(shape, verts, idx);
        if (verts.empty() || idx.empty()) return false;

        D3D11_BUFFER_DESC vbd{};
        vbd.ByteWidth = static_cast<UINT>(verts.size() * sizeof(float));
        vbd.Usage     = D3D11_USAGE_IMMUTABLE;
        vbd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        D3D11_SUBRESOURCE_DATA vinit{}; vinit.pSysMem = verts.data();
        if (FAILED(device->CreateBuffer(&vbd, &vinit, &previewSphereVB))) return false;

        D3D11_BUFFER_DESC ibd{};
        ibd.ByteWidth = static_cast<UINT>(idx.size() * sizeof(uint32_t));
        ibd.Usage     = D3D11_USAGE_IMMUTABLE;
        ibd.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA iinit{}; iinit.pSysMem = idx.data();
        if (FAILED(device->CreateBuffer(&ibd, &iinit, &previewSphereIB))) return false;

        previewSphereIdx  = static_cast<UINT>(idx.size());
        previewShapeBuilt = shape;
        return true;
    }

    // Draw one pos/normal/uv mesh into whatever target is bound, orbit-framed on
    // the caller's bounds. Port of OpenGLRenderer::DrawMeshPreviewGeometry; the
    // caller owns target, viewport and clear.
    void drawMeshPreviewGeometry(ID3D11Buffer* vb, ID3D11Buffer* ib, UINT indexCount,
                                 ID3D11ShaderResourceView* texture,
                                 const glm::vec3& center, float extent,
                                 const glm::vec3& baseColor, float metallic, float roughness,
                                 float yaw, float pitch, float dist,
                                 float fovDegrees = HE::kPreviewFovDegrees)
    {
        if (!vb || !ib || indexCount == 0) return;
        if (!ensureMeshPreviewPipeline()) return;

        // meshOrbit returns an OpenGL-convention projection (depth -1..1); D3D11
        // pre-multiplies exactly ONE fix-up. glm::perspective must NOT be called
        // here — GLM_FORCE_DEPTH_ZERO_TO_ONE is private to this target, so it
        // would remap the depth a second time.
        const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist,
                                                    1.0f, fovDegrees);
        const glm::mat4 model(1.0f);

        MeshPreviewCB cb{};
        cb.mvp      = HE::kD3DClipFix * cam.proj * cam.view * model;
        cb.model    = model;
        cb.color    = glm::vec4(baseColor, 1.0f);
        cb.camPos   = glm::vec4(cam.position, 1.0f);
        cb.pbr      = glm::vec4(metallic, roughness, texture ? 1.0f : 0.0f, 0.0f);
        cb.sun      = glm::vec4(0.0f); // w = 0 → fixed studio light, exactly like GL
        cb.sunColor = glm::vec4(1.0f);
        cb.ambient  = glm::vec4(0.32f, 0.32f, 0.32f, 0.0f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(meshPvCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, &cb, sizeof(cb)); context->Unmap(meshPvCB.Get(), 0); }

        const UINT stride = 32u, offset = 0u;
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(meshPvIL.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(meshPvVS.Get(), nullptr, 0);
        context->PSSetShader(meshPvPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, meshPvCB.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, meshPvCB.GetAddressOf());
        ID3D11ShaderResourceView* srv = texture ? texture : dummyTexture.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf()); // linear-wrap
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF); // PSMain writes alpha 1 where it draws
        context->OMSetDepthStencilState(depthState.Get(), 0); // LESS + depth write
        context->DrawIndexed(indexCount, 0, 0);
    }

    bool ensureParticlePreviewPipeline()
    {
        if (particlePvVS && particlePvPS && particlePvIL && particlePvCB && particlePvBlend) return true;
        if (particlePvTried) return false;
        particlePvTried = true;
        if (!device) return false;

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(kParticlePreviewHLSL, strlen(kParticlePreviewHLSL), "particlePreview",
                                  nullptr, nullptr, entry, profile, flags, 0, &out, &err)))
            {
                HE_LOG_ERROR(RHI, "%s",
                    (std::string("D3D11 particle-preview '") + entry + "' failed: "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsb, psb;
        if (!compile("VSMain", "vs_5_0", vsb)) return false;
        if (!compile("PSMain", "ps_5_0", psb)) return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &particlePvVS)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &particlePvPS)))
            return false;

        // Per-INSTANCE only (there is no per-vertex stream — the corners come from
        // SV_VertexID). 32 bytes, byte-identical to GL's instance buffer.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0,  0, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
            { "COLOR",    0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 16, D3D11_INPUT_PER_INSTANCE_DATA, 1 },
        };
        if (FAILED(device->CreateInputLayout(layout, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(), &particlePvIL)))
            return false;

        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = sizeof(ParticlePreviewCB);
        bd.Usage          = D3D11_USAGE_DYNAMIC;
        bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bd, nullptr, &particlePvCB))) return false;

        // NOT the scene's alphaBlendState: that one is SrcBlendAlpha=ONE /
        // DestBlendAlpha=ZERO, so the destination alpha would become the LAST
        // particle's alpha instead of accumulating. GL's glBlendFunc(SRC_ALPHA,
        // ONE_MINUS_SRC_ALPHA) applies to all four channels, and the tile's alpha
        // is what the Content Browser composites with — so match it exactly.
        D3D11_BLEND_DESC bld{};
        auto& rt = bld.RenderTarget[0];
        rt.BlendEnable    = TRUE;
        rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOp        = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha  = D3D11_BLEND_SRC_ALPHA;
        rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
        rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bld, &particlePvBlend))) return false;
        return true;
    }

    // Port of OpenGLRenderer::DrawParticlePreviewGeometry. The caller owns target,
    // viewport and clear.
    void drawParticlePreviewGeometry(ID3D11ShaderResourceView* texture,
                                     const std::vector<ParticlePreviewInstance>& particles,
                                     float yaw, float pitch, float dist)
    {
        if (particles.empty() || !ensureParticlePreviewPipeline()) return;

        // Orbit camera auto-framed on the LIVE particles' bounds (an emitter's
        // extent depends on velocity/gravity/lifetime, unlike a fixed mesh). Each
        // particle is a BILLBOARD of radius size*0.5, so the bounds must include
        // that radius — framing on the centres alone crops every quad by half its
        // width. The 0.1 floor on the extent is GL's, and it is load-bearing:
        // HE::meshOrbit only floors at 0.05, so a tight cloud of small sprites
        // would otherwise frame closer here than on OpenGL.
        glm::vec3 bmin(1e30f), bmax(-1e30f);
        for (const auto& q : particles)
        {
            const glm::vec3 r(q.size * 0.5f);
            bmin = glm::min(bmin, q.position - r);
            bmax = glm::max(bmax, q.position + r);
        }
        const bool      valid  = bmin.x <= bmax.x;
        const glm::vec3 center = valid ? (bmin + bmax) * 0.5f : glm::vec3(0.0f);
        const float     extent = valid ? std::max(glm::length(bmax - bmin) * 0.5f, 0.1f) : 1.0f;

        const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist);
        ParticlePreviewCB cb{};
        cb.viewProj = HE::kD3DClipFix * cam.proj * cam.view; // one fix-up, see drawMeshPreviewGeometry
        // Camera-facing basis for billboard expansion (right = view row 0, up = row 1).
        cb.camRight = glm::vec4(cam.view[0][0], cam.view[1][0], cam.view[2][0], 0.0f);
        cb.camUp    = glm::vec4(cam.view[0][1], cam.view[1][1], cam.view[2][1], 0.0f);
        cb.flags    = glm::vec4(texture ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
        D3D11_MAPPED_SUBRESOURCE mc{};
        if (SUCCEEDED(context->Map(particlePvCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mc)))
        { std::memcpy(mc.pData, &cb, sizeof(cb)); context->Unmap(particlePvCB.Get(), 0); }

        // Instance stream: pos3 + size1 + color3 + alpha1, same 8 floats as GL.
        const UINT count = static_cast<UINT>(particles.size());
        if (particlePvInstCap < count)
        {
            particlePvInstVB.Reset();
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = count * 32u;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_VERTEX_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &particlePvInstVB))) { particlePvInstCap = 0; return; }
            particlePvInstCap = count;
        }
        std::vector<float> inst;
        inst.reserve(static_cast<size_t>(count) * 8);
        for (const auto& q : particles)
            inst.insert(inst.end(), { q.position.x, q.position.y, q.position.z, q.size,
                                      q.color.x, q.color.y, q.color.z, q.alpha });
        D3D11_MAPPED_SUBRESOURCE mi{};
        if (FAILED(context->Map(particlePvInstVB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mi))) return;
        std::memcpy(mi.pData, inst.data(), inst.size() * sizeof(float));
        context->Unmap(particlePvInstVB.Get(), 0);

        const UINT stride = 32u, offset = 0u;
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(particlePvIL.Get());
        context->IASetVertexBuffers(0, 1, particlePvInstVB.GetAddressOf(), &stride, &offset);
        context->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(particlePvVS.Get(), nullptr, 0);
        context->PSSetShader(particlePvPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, particlePvCB.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, particlePvCB.GetAddressOf());
        ID3D11ShaderResourceView* srv = texture ? texture : dummyTexture.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(particlePvBlend.Get(), bf, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depthReadOnlyState.Get(), 0); // test on, write off (GL: glDepthMask(FALSE))
        context->DrawInstanced(6, count, 0, 0);
    }

    // ─── Interactive asset previews ──────────────────────────────────────────
    // Same "callable before the first Render()" contract as the thumbnails above
    // (EditorApplication calls RenderMaterialPreview long before r->Render()), but
    // three DIFFERENCES from a tile:
    //   • a persistent target PER PREVIEW, never the thumbnail's. One shared target
    //     would mean a Content-Browser tile — or a second open panel — silently
    //     replacing whatever the Material Editor is showing this frame.
    //   • the target is BIND_SHADER_RESOURCE as well as BIND_RENDER_TARGET, because
    //     the return value IS its SRV. The thumbnail target needs no SRV: it is only
    //     ever copied to staging and read back.
    //   • yaw/pitch/dist come from the caller (the panel's orbit), where a tile uses
    //     the fixed kThumb* three-quarter shot.

    struct PreviewTarget
    {
        ComPtr<ID3D11Texture2D>          tex;
        ComPtr<ID3D11RenderTargetView>   rtv;
        ComPtr<ID3D11ShaderResourceView> srv;      // ← THE ImGui handle, created once with the target
        ComPtr<ID3D11Texture2D>          depthTex;
        ComPtr<ID3D11DepthStencilView>   dsv;
        ComPtr<ID3D11Texture2D>          staging;  // HE_*_PREVIEW_DUMP only, allocated on first dump
        int w = 0, h = 0;
    };

    PreviewTarget matPreview;      // RenderMaterialPreview
    PreviewTarget skelPreview;     // RenderSkeletalPreview  (the only non-square one)
    PreviewTarget particlePreview; // RenderParticlePreview

    // Skinned preview pipeline (kSkelPreviewHLSL). Reuses meshPvCB at b0 (identical
    // layout) and the scene's bonesCB at b2 (identical 128×mat4 BonesCB).
    ComPtr<ID3D11VertexShader> skelPvVS;
    ComPtr<ID3D11PixelShader>  skelPvPS;
    ComPtr<ID3D11InputLayout>  skelPvIL;
    bool skelPvTried = false;

    // One-shot log for the graph-material fallback (see RenderMaterialPreview). The
    // Material Editor calls that entry point EVERY frame its panel is open, so a
    // per-call log would be a line per frame forever.
    bool matPreviewFallbackLogged = false;

    // Round a requested preview edge UP to a multiple of 64. The panels pass
    // ImGui::GetContentRegionAvail(), which moves by a pixel as the user drags a
    // splitter; without the bucket every such pixel would destroy and recreate the
    // target — and the SRV with it, which is a leak on the backends whose ImGui
    // descriptor supply is fixed and whose DestroyImGuiTexture is a no-op. D3D11
    // has no such heap, but all three backends bucket identically so a dumped
    // preview has the same dimensions everywhere.
    static int bucketPreview(int v) { return ((std::max(v, 1) + 63) / 64) * 64; }

    // Lazily (re)create a preview target at W×H. Only called with already-clamped,
    // already-bucketed dimensions.
    bool ensurePreviewTarget(PreviewTarget& t, int W, int H)
    {
        if (t.rtv && t.srv && t.dsv && t.w == W && t.h == H) return true;
        t = PreviewTarget{};
        if (!device || W <= 0 || H <= 0) return false;

        D3D11_TEXTURE2D_DESC td{};
        td.Width  = static_cast<UINT>(W);
        td.Height = static_cast<UINT>(H);
        td.MipLevels = td.ArraySize = 1;
        // Never _SRGB: ImGui composites this straight into an already-encoded UI,
        // exactly as the GL path's GL_RGBA8 texture does.
        td.Format           = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage            = D3D11_USAGE_DEFAULT;
        td.BindFlags        = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&td, nullptr, &t.tex)))                     return false;
        if (FAILED(device->CreateRenderTargetView(t.tex.Get(), nullptr, &t.rtv)))      return false;
        if (FAILED(device->CreateShaderResourceView(t.tex.Get(), nullptr, &t.srv)))    return false;

        D3D11_TEXTURE2D_DESC dd = td;
        dd.Format    = DXGI_FORMAT_D32_FLOAT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &t.depthTex)))                return false;
        if (FAILED(device->CreateDepthStencilView(t.depthTex.Get(), nullptr, &t.dsv))) return false;

        t.w = W;
        t.h = H;
        return true;
    }

    // Bind + clear a preview target. Clear colour is FULLY TRANSPARENT and depth
    // 1.0 with a LESS compare — the editor composites the result over its own
    // backdrop, so anything opaque here would show as a black card.
    void beginPreviewPass(const PreviewTarget& t)
    {
        const float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        ID3D11RenderTargetView* rtv = t.rtv.Get();
        context->OMSetRenderTargets(1, &rtv, t.dsv.Get());
        context->ClearRenderTargetView(t.rtv.Get(), clear);
        context->ClearDepthStencilView(t.dsv.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(t.w);
        vp.Height   = static_cast<float>(t.h);
        vp.MaxDepth = 1.0f;
        context->RSSetViewports(1, &vp);
        context->RSSetState(rasterState.Get()); // CULL_NONE — GL disables culling here
    }

    // Unbind everything. MANDATORY before returning the handle: a texture still
    // bound as an RTV cannot be sampled, so ImGui would draw nothing at all.
    void endPreviewPass()
    {
        context->OMSetRenderTargets(0, nullptr, nullptr);
        ID3D11ShaderResourceView* nullSrv = nullptr;
        context->PSSetShaderResources(0, 1, &nullSrv);
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
        context->OMSetDepthStencilState(nullptr, 0);
    }

    // Headless pixel witness: write the preview target to $envName as a binary PPM,
    // byte-for-byte the format OpenGLRenderer writes, so the two can be diffed
    // directly. Header "P6\n<W> <H>\n255\n" then W*H*3 tightly packed RGB, TOP-DOWN.
    // No flip: a D3D render target is already top-down (row 0 = top); GL's flip is a
    // GL-only correction. No channel swap either — the target is R8G8B8A8_UNORM, so
    // the bytes are already R,G,B,A and only the alpha is dropped.
    //
    // This is the ONLY pixel witness a preview has on this backend: the frame dump
    // cannot stand in for it, because the editor sets the ImGui overlay callback to
    // nullptr in the dump path and the frame therefore never contains a panel.
    void dumpPreviewTarget(PreviewTarget& t, const char* envName)
    {
        const char* path = std::getenv(envName);
        if (!path || !*path || !t.tex) return;

        if (!t.staging)
        {
            D3D11_TEXTURE2D_DESC sd{};
            t.tex->GetDesc(&sd);
            sd.Usage          = D3D11_USAGE_STAGING;
            sd.BindFlags      = 0;
            sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            sd.MiscFlags      = 0;
            if (FAILED(device->CreateTexture2D(&sd, nullptr, &t.staging))) return;
        }
        context->CopyResource(t.staging.Get(), t.tex.Get());

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(t.staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return;
        std::vector<uint8_t> rgb(static_cast<size_t>(t.w) * t.h * 3);
        const uint8_t* src = static_cast<const uint8_t*>(m.pData);
        for (int y = 0; y < t.h; ++y)
        {
            const uint8_t* row = src + static_cast<size_t>(y) * m.RowPitch;
            uint8_t*       out = rgb.data() + static_cast<size_t>(y) * t.w * 3;
            for (int x = 0; x < t.w; ++x)
            {
                out[x * 3 + 0] = row[x * 4 + 0];
                out[x * 3 + 1] = row[x * 4 + 1];
                out[x * 3 + 2] = row[x * 4 + 2];
            }
        }
        context->Unmap(t.staging.Get(), 0);

        if (std::ofstream f(path, std::ios::binary); f)
        {
            f << "P6\n" << t.w << " " << t.h << "\n255\n";
            f.write(reinterpret_cast<const char*>(rgb.data()), static_cast<std::streamsize>(rgb.size()));
        }
    }

    // Compile the skinned preview program once. Separate from createSkinnedPipeline:
    // that one pairs kSkinnedHLSL's VS with the SCENE pixel shader, which needs
    // perFrameCB + the shadow/AO/GI SRVs a frame binds and is therefore unusable
    // before the first Render().
    bool ensureSkelPreviewPipeline()
    {
        if (skelPvVS && skelPvPS && skelPvIL) return true;
        if (skelPvTried) return false;
        skelPvTried = true;
        if (!device || !bonesCB) return false;   // b2 comes from createSkinnedPipeline
        if (!ensureMeshPreviewPipeline()) return false; // b0 == meshPvCB

        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* entry, const char* profile, ComPtr<ID3DBlob>& out) -> bool
        {
            ComPtr<ID3DBlob> err;
            if (FAILED(D3DCompile(kSkelPreviewHLSL, strlen(kSkelPreviewHLSL), "skelPreview",
                                  nullptr, nullptr, entry, profile, flags, 0, &out, &err)))
            {
                HE_LOG_ERROR(RHI, "%s",
                    (std::string("D3D11 skeletal-preview '") + entry + "' failed: "
                     + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> vsb, psb;
        if (!compile("VSMainSkinned", "vs_5_0", vsb)) return false;
        if (!compile("PSMain",        "ps_5_0", psb)) return false;
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &skelPvVS)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &skelPvPS)))
            return false;

        // Same three-stream layout resolveSkeletalMesh uploads: slot 0 interleaved
        // pos/normal/uv, slot 1 bone IDs, slot 2 bone weights. Built from THIS VS's
        // blob rather than reusing skinnedLayout, so the two can never drift.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "POSITION",     0, DXGI_FORMAT_R32G32B32_FLOAT,    0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "NORMAL",       0, DXGI_FORMAT_R32G32B32_FLOAT,    0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD",     0, DXGI_FORMAT_R32G32_FLOAT,       0, 24, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDINDICES", 0, DXGI_FORMAT_R32G32B32A32_UINT,  1,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "BLENDWEIGHT",  0, DXGI_FORMAT_R32G32B32A32_FLOAT, 2,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device->CreateInputLayout(layout, 5, vsb->GetBufferPointer(), vsb->GetBufferSize(), &skelPvIL)))
            return false;
        return true;
    }

    // Draw a skeletal mesh, skinned by `boneMatrices` (bind pose when empty), with
    // the caller's already-built view-projection. Port of the draw half of
    // OpenGLRenderer::RenderSkeletalPreview; the caller owns target/viewport/clear.
    void drawSkelPreviewGeometry(const GpuSkeletalMesh& sm, const glm::mat4& viewProj,
                                 const std::vector<glm::mat4>& boneScratch)
    {
        // The bone streams are required, not optional: with slot 1/2 unbound the VS
        // reads zero weights, every skin matrix collapses to 0 and the mesh vanishes
        // into a point — which looks like "the preview is broken", not "upload failed".
        if (!sm.vb || !sm.ib || !sm.boneIdVb || !sm.boneWgtVb || sm.indexCount <= 0) return;
        if (!ensureSkelPreviewPipeline()) return;

        MeshPreviewCB cb{};
        cb.mvp      = viewProj;              // model is identity, so mvp == viewProj
        cb.model    = glm::mat4(1.0f);
        cb.color    = glm::vec4(0.75f, 0.75f, 0.75f, 1.0f); // GL's uColor for this preview
        cb.camPos   = glm::vec4(0.0f);       // unused by this PS (no specular lobe)
        cb.pbr      = glm::vec4(0.0f, 0.0f, sm.srv ? 1.0f : 0.0f, 0.0f); // z = hasTexture
        cb.sun      = glm::vec4(0.0f);       // w = 0 → the shader's fixed studio light (GL parity)
        cb.sunColor = glm::vec4(1.0f);
        cb.ambient  = glm::vec4(0.35f, 0.35f, 0.35f, 0.0f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(context->Map(meshPvCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, &cb, sizeof(cb)); context->Unmap(meshPvCB.Get(), 0); }
        if (SUCCEEDED(context->Map(bonesCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        { std::memcpy(m.pData, boneScratch.data(), boneScratch.size() * sizeof(glm::mat4)); context->Unmap(bonesCB.Get(), 0); }

        ID3D11Buffer* vbs[3]     = { sm.vb.Get(), sm.boneIdVb.Get(), sm.boneWgtVb.Get() };
        const UINT    strides[3] = { 32u, 16u, 16u };
        const UINT    offsets[3] = { 0u, 0u, 0u };
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(skelPvIL.Get());
        context->IASetVertexBuffers(0, 3, vbs, strides, offsets);
        context->IASetIndexBuffer(sm.ib.Get(), DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(skelPvVS.Get(), nullptr, 0);
        context->PSSetShader(skelPvPS.Get(), nullptr, 0);
        context->VSSetConstantBuffers(0, 1, meshPvCB.GetAddressOf());
        context->VSSetConstantBuffers(2, 1, bonesCB.GetAddressOf());
        context->PSSetConstantBuffers(0, 1, meshPvCB.GetAddressOf());
        ID3D11ShaderResourceView* srv = sm.srv ? sm.srv.Get() : dummyTexture.Get();
        context->PSSetShaderResources(0, 1, &srv);
        context->PSSetSamplers(0, 1, sampler.GetAddressOf());
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF); // PSMain writes alpha 1 where it draws
        context->OMSetDepthStencilState(depthState.Get(), 0); // LESS + depth write
        context->DrawIndexed(static_cast<UINT>(sm.indexCount), 0, 0);
    }

#if defined(HE_HAVE_SHADERC)
    // Draw one primitive (or a picked static mesh) shaded by a NODE-GRAPH material.
    // Port of OpenGLRenderer::DrawMaterialPreviewGeometry; the caller owns target,
    // viewport and clear, and has already resolved the graph shader sources.
    // Returns false when the material's HLSL will not build — which since the
    // register-pin table landed means a genuinely broken graph, not the blanket
    // ps_5_0 sampler-cap rejection that used to fail every material here.
    bool drawMaterialPreviewGeometry(const HE::UUID& materialId, uint64_t matHash,
                                     const std::string& matFrag, const std::string& matVertBody,
                                     float yaw, float pitch, float dist,
                                     int shape, const HE::UUID& meshId, ContentManager* cm)
    {
        MatShaders* sh = GetOrBuildMaterialShaders(matHash, matFrag, matVertBody, /*transparent=*/false);
        if (!sh || !sh->vs || !sh->ps || !sh->il) return false;

        // Geometry: a picked STATIC MESH wins over `shape`, auto-framed on its own
        // bounds so `dist` means the same thing for a teapot as for the unit sphere.
        // A mesh that is not resident falls back to the primitive (GL parity — the
        // caller never has to pre-check).
        const GpuMesh* gm = (meshId != HE::UUID{}) ? resolveMesh(meshId, cm) : nullptr;
        ID3D11Buffer* vb = nullptr; ID3D11Buffer* ib = nullptr; UINT indexCount = 0;
        glm::vec3 center(0.0f);
        float     extent = 1.0f; // what `dist` is measured in, so it frames alike
        if (gm && gm->vbuf && gm->ibuf && gm->indexCount > 0)
        {
            vb = gm->vbuf.Get(); ib = gm->ibuf.Get(); indexCount = gm->indexCount;
            center = HE::boundsCenter(gm->localBounds);
            extent = HE::boundsExtent(gm->localBounds);
        }
        else
        {
            if (!ensurePreviewShape(shape)) return false;
            vb = previewSphereVB.Get(); ib = previewSphereIB.Get(); indexCount = previewSphereIdx;
        }
        if (!vb || !ib || indexCount == 0) return false;

        // 32°, not the kPreviewFovDegrees 35° the built-in path uses — GL's graph
        // preview is a 32° shot and the editor's `dist` defaults are tuned to it.
        // ONE ClipSpace fix-up on meshOrbit's GL-convention projection; never
        // glm::perspective here (GLM_FORCE_DEPTH_ZERO_TO_ONE is private to this
        // target and would remap the depth a second time).
        const HE::PreviewCamera cam = HE::meshOrbit(center, extent, yaw, pitch, dist, 1.0f, 32.0f);
        const glm::mat4 model(1.0f);
        const glm::mat4 viewProj = HE::kD3DClipFix * cam.proj * cam.view;

        const MaterialAsset* ma = cm ? cm->getMaterial(materialId) : nullptr;

        // HeLighting (b0 PS / b8 WPO VS): the preview's OWN studio light, exactly
        // the numbers GL's DrawMaterialPreviewGeometry writes — NOT the frame's
        // fillMatLight, which does not run before the first Render() and would in
        // any case light the preview with the open level's sun.
        {
            HE::MaterialShaderLibrary::Lighting lit{};
            const glm::vec3 sd = glm::normalize(glm::vec3(0.45f, 0.75f, 0.55f));
            lit.sunDir[0] = sd.x; lit.sunDir[1] = sd.y; lit.sunDir[2] = sd.z;
            lit.sunDir[3] = 0.0f; // graph Time input — pinned at 0 so a dump is deterministic
            lit.sunColor[0] = lit.sunColor[1] = lit.sunColor[2] = 1.05f;
            lit.ambient[0]  = lit.ambient[1]  = lit.ambient[2]  = 0.28f;
            lit.camPos[0] = cam.position.x; lit.camPos[1] = cam.position.y; lit.camPos[2] = cam.position.z;
            // The studio sun as the single array light, so heLitP() previews shade.
            lit.lightPos[0][3]   = 0.0f; // directional
            lit.lightDir[0][0]   = -sd.x; lit.lightDir[0][1] = -sd.y; lit.lightDir[0][2] = -sd.z;
            lit.lightColor[0][0] = lit.lightColor[0][1] = lit.lightColor[0][2] = 1.05f;
            lit.lightColor[0][3] = 1.0f;
            lit.counts[0]        = 1.0f;
            // giParams stays zero: no GI masks are bound here, and heLitP() skips
            // the mask samples entirely when giParams.z is 0.
            D3D11_MAPPED_SUBRESOURCE lm{};
            if (SUCCEEDED(context->Map(m_matLightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lm)))
            { std::memcpy(lm.pData, &lit, sizeof(lit)); context->Unmap(m_matLightCB.Get(), 0); }
        }

        // U block (b1 VS, std140 176 B) — same struct the scene's drawDC writes.
        {
            struct MatU { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };
            static_assert(sizeof(MatU) == 176, "material U block must be std140 176 B");
            MatU u;
            u.mvp   = viewProj * model;
            u.model = model;
            u.color = glm::vec4(ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2])
                                   : glm::vec3(1.0f), 1.0f);
            u.flags = glm::vec4(0.0f); // GL's obj.flags — hasTex stays 0 in the preview
            u.pbr   = glm::vec4(ma ? ma->metallic  : 0.0f,
                                ma ? ma->roughness : 0.5f,
                                ma ? ma->opacity   : 1.0f, 0.0f);
            D3D11_MAPPED_SUBRESOURCE mu{};
            if (SUCCEEDED(context->Map(m_matObjCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mu)))
            { std::memcpy(mu.pData, &u, sizeof(u)); context->Unmap(m_matObjCB.Get(), 0); }
        }

        // HeParams (b3 PS / b9 WPO VS, 16 vec4, zero-padded).
        {
            float padded[64] = { 0.0f };
            if (ma && !ma->shaderParamData.empty())
                std::memcpy(padded, ma->shaderParamData.data(),
                            std::min(ma->shaderParamData.size(), size_t(64)) * sizeof(float));
            D3D11_MAPPED_SUBRESOURCE mp{};
            if (SUCCEEDED(context->Map(m_matParamCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mp)))
            { std::memcpy(mp.pData, padded, sizeof(padded)); context->Unmap(m_matParamCB.Get(), 0); }
        }

        const UINT stride = 32u, offset = 0u;
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->IASetInputLayout(sh->il.Get());
        context->IASetVertexBuffers(0, 1, &vb, &stride, &offset);
        context->IASetIndexBuffer(ib, DXGI_FORMAT_R32_UINT, 0);
        context->VSSetShader(sh->vs.Get(), nullptr, 0);
        context->PSSetShader(sh->ps.Get(), nullptr, 0);
        context->PSSetConstantBuffers(0, 1, m_matLightCB.GetAddressOf()); // b0 HeLighting
        context->VSSetConstantBuffers(8, 1, m_matLightCB.GetAddressOf()); // b8 HeLighting (WPO VS)
        context->VSSetConstantBuffers(1, 1, m_matObjCB.GetAddressOf());   // b1 U
        context->PSSetConstantBuffers(3, 1, m_matParamCB.GetAddressOf()); // b3 HeParams
        context->VSSetConstantBuffers(9, 1, m_matParamCB.GetAddressOf()); // b9 HeParams (WPO VS)
        // The shared lighting preamble's whole texture/sampler window (t10..t18,
        // t31..t33, s0..s15). All-default: this preview has no GI, no SSAO and no
        // frame state — it is lit by the studio Lighting block written above, whose
        // gates (giParams.z, fog.z/.w, giProbe.y, ssr.x, giRefl.z, cloudShadowB.x,
        // csmSplits.w) are all 0, so every one of those samples folds away. They
        // still have to be BOUND and correctly TYPED or the draw trips the debug
        // layer. Also sets s2 + s4..s7 for the graph textures below.
        bindMaterialPreamble(context.Get());
        // heTex0 (t2) + heTexP0..3 (t4..t7). GL binds the material's real graph
        // textures here via ResolveGraphTexture; D3D11 has NO graph-texture cache
        // yet (the same A4 gap the scene path carries — see the heTex0 TODO in
        // drawDC), so all five are the 1×1 white default and a graph that samples a
        // project texture previews flat. This is the one remaining reason a D3D11
        // material tile does not match the OpenGL one; closing that gap closes this.
        ID3D11ShaderResourceView* white5[5] = {
            dummyTexture.Get(), dummyTexture.Get(), dummyTexture.Get(),
            dummyTexture.Get(), dummyTexture.Get() };
        context->PSSetShaderResources(2, 1, &white5[0]);
        context->PSSetShaderResources(4, 4, &white5[1]);
        const float bf[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        context->OMSetBlendState(nullptr, bf, 0xFFFFFFFF);
        context->OMSetDepthStencilState(depthState.Get(), 0); // LESS + depth write
        context->DrawIndexed(indexCount, 0, 0);
        return true;
    }
#endif // HE_HAVE_SHADERC
};

D3D11Renderer::D3D11Renderer()  : m_impl(new D3D11RendererImpl{}) {}
D3D11Renderer::~D3D11Renderer() { delete m_impl; }

void D3D11Renderer::Initialize(HE::Window* window)
{
    HE_LOG_INFO(RHI, "%s", "D3D11Renderer: initializing");
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
        HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: scene pipeline creation failed — only clear will work");
    m_impl->createCube();
    HE_LOG_INFO(RHI, "%s", "D3D11Renderer: initialized successfully");
}

void D3D11Renderer::Shutdown()
{
    HE_LOG_INFO(RHI, "%s", "D3D11Renderer: shutdown");
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
    // Thumbnail + preview targets and their pipelines. Released HERE rather than
    // left to ~D3D11RendererImpl so they go before device.Reset() below, and so a
    // re-Initialize() rebuilds them against the NEW device instead of handing out a
    // stale SRV. The `*Tried` one-shot flags are cleared for the same reason.
    //
    // ONLY resources with a lazy ensure*() path belong in this block. Anything
    // built exclusively by createResources() (skinnedVS/skinnedLayout/bonesCB,
    // debugReady) must NOT be reset here: nothing would rebuild it, and the scene's
    // skinned pass and debug-line pass both gate on exactly those.
    m_impl->skeletalMeshCache.clear();
    m_impl->thumbTex.Reset();   m_impl->thumbRTV.Reset();
    m_impl->thumbDepthTex.Reset(); m_impl->thumbDSV.Reset();
    m_impl->thumbStaging.Reset(); m_impl->thumbSize = 0;
    m_impl->matPreview      = D3D11RendererImpl::PreviewTarget{};
    m_impl->skelPreview     = D3D11RendererImpl::PreviewTarget{};
    m_impl->particlePreview = D3D11RendererImpl::PreviewTarget{};
    m_impl->meshPvVS.Reset(); m_impl->meshPvPS.Reset();
    m_impl->meshPvIL.Reset(); m_impl->meshPvCB.Reset();
    m_impl->meshPvTried = false;
    m_impl->skelPvVS.Reset(); m_impl->skelPvPS.Reset(); m_impl->skelPvIL.Reset();
    m_impl->skelPvTried = false;
    m_impl->particlePvVS.Reset(); m_impl->particlePvPS.Reset();
    m_impl->particlePvIL.Reset(); m_impl->particlePvCB.Reset();
    m_impl->particlePvInstVB.Reset(); m_impl->particlePvInstCap = 0;
    m_impl->particlePvBlend.Reset(); m_impl->particlePvTried = false;
    m_impl->previewSphereVB.Reset(); m_impl->previewSphereIB.Reset();
    m_impl->previewSphereIdx = 0;    m_impl->previewShapeBuilt = -1;
    m_impl->matPreviewFallbackLogged = false;
    // GI resources (accel buffers, targets, atlases, pipelines).
    m_impl->destroyGiAccel();
    m_impl->destroyGiTargets();
    m_impl->giGBufVS.Reset(); m_impl->giGBufPS.Reset();
    m_impl->giShadowCS.Reset(); m_impl->giProbeCS.Reset();
    m_impl->giTemporalPS.Reset(); m_impl->giBlurPS.Reset();
    m_impl->giShadowCB.Reset(); m_impl->giCountCB.Reset(); m_impl->giTemporalCB.Reset();
    m_impl->giBlurCB.Reset(); m_impl->giProbeCB.Reset();
    m_impl->giLinearClamp.Reset();
    // Secondary-window swapchains + RTVs (P1d). Belt and braces: Application::Run
    // already DetachWindow()s every secondary window before calling Shutdown()
    // (Application.cpp:379-383), so this is normally empty. It is cleared HERE
    // rather than left to ~D3D11RendererImpl so the swapchains go before
    // device.Reset() below, and so a re-Initialize() starts from an empty map
    // instead of holding swapchains built against the old device.
    m_impl->secondaryWindows.clear();
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
    // Timed as its own row under GL's name; unlike GL (where the sky is a sibling
    // AFTER the opaque geometry) D3D11 draws it first, so it leads the breakdown.
    {
        D3D11RendererImpl::GpuPassScope _skyTimer(&p, "Sky+Clouds");
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
    const glm::mat4 lightClip = HE::kD3DClipFix * p.m_renderWorld.shadow.viewProj;

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
        f.giGridOrigin = glm::vec4(p.giGridOrigin, D3D11RendererImpl::kGIProbeSpacing);
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
        p.m_renderWorld.dominantDirectionalLight(matSunDir, matSunColor);
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
        // shaders. Shared fill (HE::FillMaterialLightWindow); D3D11 has no local
        // (point/spot) shadow atlas yet, so it passes false and lightParams[i].y
        // stays 0 = "casts no local shadow".
        HE::FillMaterialLightWindow(p.m_renderWorld, lit, /*localShadowsActive=*/false);
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
            D3D11RendererImpl::GpuPassScope _shadowTimer(&p, "Shadow"); // closes at this branch's return
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
        // Collect opaque/transparent DCs early (needed for position prepass AND main scene).
        // BEHAVIOUR CHANGE: the loop this replaced classified on dc.opacity alone, so a
        // particle fading out through its instance tint stayed in the OPAQUE pass;
        // RenderSorter multiplies in instanceTint.a like GL/Metal always did.
        std::vector<const DrawCall*> opaqueDCs_, transparentDCs_;
        RenderSorter::partitionByOpacity(cmds.drawCalls(), opaqueDCs_, transparentDCs_);

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
            // Timed only when it actually runs — an always-present 0 ms SSAO row
            // would claim the pass ran and cost nothing.
            D3D11RendererImpl::GpuPassScope _ssaoTimer(&p, "SSAO");
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

        // ── Opaque ───────────────────────────────────────────────────────────
        // Opened here, not at the draw loop, so the post-SSAO state rebind and
        // the per-frame CB refills below are attributed to the pass they serve —
        // otherwise that GPU work falls out of every row and the breakdown stops
        // summing to the frame. Mirrors GL, which begins "Opaque" at the same
        // point (right after the SSAO block, before the scene-state rebind).
        // Closed explicitly before the transparent pass: siblings, never nested.
        D3D11RendererImpl::GpuPassScope _opaqueTimer(&p, "Opaque");

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
            // The graph materials' HeLighting block. Their TEXTURE window (t10..t18,
            // t31..t33 + s0..s15) is no longer bound here: the built-in path clobbers
            // most of that range every draw, so it is bound per graph-material draw by
            // bindMaterialPreamble() inside drawDC instead of once per frame.
            fillMatLight(giShadingActive);
#endif
        }

        const glm::vec3 camPos = p.m_renderWorld.camera.position;

        // Reuse already-collected opaque/transparent DC lists from the SSAO prepass above.
        std::vector<const DrawCall*>& opaqueDCs = opaqueDCs_;
        std::vector<const DrawCall*>& transparentDCs = transparentDCs_;

        // Sort transparent back-to-front by distance.
        RenderSorter::sortBackToFront(transparentDCs, camPos);

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
                    // Transparent graph materials get a blend-on / depth-write-off
                    // shader variant. MUST use the same predicate the opaque/blended
                    // partition above used, or a draw lands in the blended pass with a
                    // depth-writing variant (hence RenderSorter::isTransparent, tint
                    // alpha included, not a bare dc.opacity test).
                    const bool matTransp = RenderSorter::isTransparent(dc);
                    D3D11RendererImpl::MatShaders* sh =
                        p.GetOrBuildMaterialShaders(matHash, matFrag, matVertBody, matTransp);
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
                        // The shared lighting preamble's whole texture/sampler window
                        // (t10..t18, t31..t33, s0..s15) — REAL resources where this
                        // backend has them, typed defaults where it does not:
                        //   heGIShadow/heGILocal  the frame's ray-traced masks (real when GI ran)
                        //   heAO                  the frame's SSAO result (real; white when off)
                        //   heGIIrradiance/-Vis   the DDGI probe atlases (real when GI ran)
                        //   heCsm/heLocalShadow   1x1 ARRAY default — this backend has a single
                        //                         shadow map, not a cascade/atlas array
                        //   heSkyEnv              1x1x6 CUBE default — no prefiltered env here
                        //   heSSRFwd/heGIReflFwd  black — no SSR, no GI reflections here
                        //   heCloudShadow         white — no cloud-shadow map here
                        // Also binds s2 + s4..s7 for the graph textures below.
                        p.bindMaterialPreamble(ctx, aoSRV,
                            giShadingActive ? giShadowSRV : nullptr,
                            (giShadingActive && p.giLocalMaskSRV) ? p.giLocalMaskSRV.Get() : nullptr,
                            giShadingActive ? p.giIrrSRV.Get() : nullptr,
                            giShadingActive ? p.giVisSRV.Get() : nullptr);
                        // heTex0 (t2 PS) + heTexP0..3 (t4..t7 PS, white default).
                        // t3 is intentionally unused by the mesh path.
                        ctx->PSSetShaderResources(2, 1, &heTex0);
                        ID3D11ShaderResourceView* whiteP[4] = {
                            p.dummyTexture.Get(), p.dummyTexture.Get(),
                            p.dummyTexture.Get(), p.dummyTexture.Get() };
                        ctx->PSSetShaderResources(4, 4, whiteP);

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
                        //
                        // Since the register-pin remap, bindMaterialPreamble() ALSO writes the
                        // full s0..s15 sampler window, which adds s0 (uSampler, linear-wrap
                        // albedo) and s1 (uAOSampler, POINT) to that clobber set — the built-in
                        // scene PS declares s0/s1/s2 (see its register block) and nothing else.
                        // Miss these and every built-in draw after a graph material filters its
                        // albedo and its AO through the material path's samplers instead, which
                        // is silent: no warning, just softly wrong pixels.
                        //
                        // The new SRV binds need no restore: they are all t10 and above, and no
                        // built-in pass on this backend reads past t7.
                        ctx->VSSetShader(p.vs.Get(), nullptr, 0);
                        ctx->PSSetShader(p.ps.Get(), nullptr, 0);
                        ctx->IASetInputLayout(p.inputLayout.Get());
                        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
                        ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
                        ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());      // s0 uSampler   (linear-wrap)
                        ctx->PSSetSamplers(1, 1, p.pointSampler.GetAddressOf()); // s1 uAOSampler (POINT)
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

        // ── Transparent ──────────────────────────────────────────────────────
        // Blended geometry + debug lines, exactly what GL's "Transparent" row
        // covers. Unconditional WITHIN the graph pass (like GL's), so nothing
        // blended being visible still yields the row — but note DrawScene returns
        // before the graph on an empty scene, and then Shadow/SSAO/Opaque/
        // Transparent are all absent for that frame, not zero.
        _opaqueTimer.end(); // sibling boundary
        D3D11RendererImpl::GpuPassScope _transparentTimer(&p, "Transparent");
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
            // Timed only when bloom actually runs (GL gates its row the same way).
            ID3D11ShaderResourceView* bloomResult = p.dummyTexture.Get();
            if (p.bloomEnabled)
            {
                D3D11RendererImpl::GpuPassScope _bloomTimer(&p, "Bloom");
                bloomResult = p.runBloom(bw, bh);
            }

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
            { D3D11RendererImpl::GpuPassScope _tonemapTimer(&p, "Tonemap");
              const float cb[4] = { p.exposure,
                                    p.bloomEnabled ? p.bloomStrength : 0.0f, 0, 0 };
              p.updatePostFxCB(cb);
              p.context->OMSetRenderTargets(1, p.ldrRTV.GetAddressOf(), nullptr);
              p.context->PSSetShader(p.tonemapPS.Get(), nullptr, 0);
              ID3D11ShaderResourceView* srvs[2] = { p.hdrSRV.Get(), bloomResult };
              p.context->PSSetShaderResources(0, 2, srvs);
              p.context->Draw(3, 0);
              ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

            // AA resolve: ldrSRV → viewportRTV (final output sampled by ImGui).
            // Always drawn — the method only picks the pixel shader.
            // The ROW is named off the method the same way GL names its own
            // (AA Resolve / SMAA / FXAA), so HE_DUMP_AA cannot make the row lie.
            { const bool aaOff  = (p.aaMethod == HE::AAMethod::Off);
              const bool aaSmaa = (p.aaMethod == HE::AAMethod::SMAA);
              D3D11RendererImpl::GpuPassScope _aaTimer(
                  &p, aaOff ? "AA Resolve" : (aaSmaa ? "SMAA" : "FXAA"));
              const float cb[4] = { 1.0f / float(p.viewportW),
                                    1.0f / float(p.viewportH), 0, 0 };
              p.updatePostFxCB(cb);
              p.context->OMSetRenderTargets(1, p.viewportRTV.GetAddressOf(), nullptr);
              p.context->PSSetShader(aaOff  ? p.aaBlitPS.Get()
                                   : aaSmaa ? p.smaaPS.Get()
                                            : p.fxaaPS.Get(), nullptr, 0);
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
        // Timed HERE, not inside renderUIPass: RenderWidgetThumbnail calls that
        // same function OUT of frame, which would open a pass in a slot no frame
        // owns and corrupt the breakdown.
        { D3D11RendererImpl::GpuPassScope _uiTimer(&p, "UI");
          p.renderUIPass(p.context.Get(), static_cast<int>(p.viewportW), static_cast<int>(p.viewportH)); }
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
        // Timed at the call site, see the viewport branch above.
        { D3D11RendererImpl::GpuPassScope _uiTimer(&p, "UI");
          p.renderUIPass(p.context.Get(), p.width, p.height); }
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
    HE_LOG_INFO(RHI, "%s", enabled ? "D3D11Renderer: VSync enabled" : "D3D11Renderer: VSync disabled");
    m_impl->vsync = enabled;
}

void* D3D11Renderer::GetDevice()  const { return m_impl->device.Get(); }
void* D3D11Renderer::GetContext() const { return m_impl->context.Get(); }

// ─── Multi-window support (P1d) ──────────────────────────────────────────────
// A secondary window gets its own DXGI swapchain + RTV; RenderWindow() clears it
// and presents it. There is deliberately NO scene rendering: GL, Vulkan and Metal
// all stop at clear+present as well (OpenGLRenderer.cpp:11898 still carries a
// literal "TODO: secondary-window draw calls"), so this is parity with the other
// backends rather than a half-finished port.
//
// Nothing throws out of these three. Application::createSecondaryWindow (and
// destroyWindow) run OUTSIDE the render loop's try/catch, so an exception here
// would take the process down — every failure logs and leaves the window
// unattached instead, after which RenderWindow()/DetachWindow() no-op on the map
// lookup. That is why GL's `throw` is not copied here.

void D3D11Renderer::AttachWindow(HE::Window* window)
{
    if (!window || !m_impl->device) return;
    SDL_Window* sdlWin = window->GetNativeWindow();
    if (!sdlWin) return;
    if (m_impl->secondaryWindows.count(sdlWin)) return; // already attached

    HWND hwnd = static_cast<HWND>(SDL_GetPointerProperty(
        SDL_GetWindowProperties(sdlWin), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (!hwnd)
    {
        HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: could not get HWND for secondary window");
        return;
    }

    // The primary swapchain came out of D3D11CreateDeviceAndSwapChain (Initialize()),
    // so this backend never kept an IDXGIFactory around. Walk to one:
    // device → IDXGIDevice → adapter → factory.
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_impl->device.As(&dxgiDevice)) || !dxgiDevice)
    {
        HE_LOG_ERROR(RHI, "%s",
                     "D3D11Renderer: QueryInterface(IDXGIDevice) failed — secondary window not attached");
        return;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(adapter.GetAddressOf())) || !adapter)
    {
        HE_LOG_ERROR(RHI, "%s",
                     "D3D11Renderer: IDXGIDevice::GetAdapter failed — secondary window not attached");
        return;
    }
    ComPtr<IDXGIFactory> factory;
    if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory),
                                  reinterpret_cast<void**>(factory.GetAddressOf()))) || !factory)
    {
        HE_LOG_ERROR(RHI, "%s",
                     "D3D11Renderer: could not obtain IDXGIFactory — secondary window not attached");
        return;
    }

    // Swapchain description: the same fields Initialize() passes for the primary,
    // only with this window's HWND and size.
    //
    // The size is the LOGICAL (points) size from GetWidth/GetHeight, NOT
    // SDL_GetWindowSizeInPixels the way Vulkan's secondary path does it:
    // Window.cpp:58-62 spells out that the D3D backends size from the logical
    // window while GL/Vulkan/Metal size from the pixel drawable, and both D3D
    // primaries follow that. Using the pixel size here would double the swapchain
    // on a HiDPI display.
    DXGI_SWAP_CHAIN_DESC scd{};
    scd.BufferCount                        = 1;
    scd.BufferDesc.Width                   = static_cast<UINT>(window->GetWidth());
    scd.BufferDesc.Height                  = static_cast<UINT>(window->GetHeight());
    scd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    scd.BufferDesc.RefreshRate.Numerator   = 60;
    scd.BufferDesc.RefreshRate.Denominator = 1;
    scd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    scd.OutputWindow                       = hwnd;
    scd.SampleDesc.Count                   = 1;
    scd.Windowed                           = TRUE;
    scd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    // Built into a local and only moved into the map once BOTH objects exist, so a
    // failure half-way through cannot leave a zombie entry that RenderWindow()
    // would then pick up (Vulkan's `m_extraWindows[sdlWin]` inserts first).
    D3D11RendererImpl::SecondaryWindow sw;
    if (FAILED(factory->CreateSwapChain(m_impl->device.Get(), &scd, sw.swapchain.GetAddressOf()))
        || !sw.swapchain)
    {
        HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: CreateSwapChain failed for secondary window");
        return;
    }

    // RTV over back buffer 0 — mirrors createRTV(), which cannot simply be called:
    // it reads the PRIMARY `swapchain` member and writes the PRIMARY `rtv` member.
    ComPtr<ID3D11Texture2D> bb;
    if (FAILED(sw.swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                                       reinterpret_cast<void**>(bb.GetAddressOf()))) || !bb)
    {
        HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: GetBuffer(0) failed for secondary window");
        return;
    }
    if (FAILED(m_impl->device->CreateRenderTargetView(bb.Get(), nullptr, sw.rtv.GetAddressOf()))
        || !sw.rtv)
    {
        HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: CreateRenderTargetView failed for secondary window");
        return;
    }

    // No depth buffer is created on purpose: a clear+present pass needs none.

    // Secondary-window RESIZE is not wired up at the Application level yet:
    // Window::PollEvents only tracks the PRIMARY window's size — every window event
    // it handles is gated on `event.window.windowID == SDL_GetWindowID(m_window)`
    // (Window.cpp:150-166) — so a secondary Window's GetWidth/GetHeight stay frozen
    // at their creation values and there is nothing here to react to.
    // Resizing the OS window therefore stretches the presented back buffer. Wiring
    // a real ResizeBuffers path needs an Application-level size event first.

    m_impl->secondaryWindows.emplace(sdlWin, std::move(sw));
    HE_LOG_INFO(RHI, "%s", "D3D11Renderer: secondary window attached");
}

void D3D11Renderer::DetachWindow(HE::Window* window)
{
    if (!window) return;
    auto it = m_impl->secondaryWindows.find(window->GetNativeWindow());
    if (it == m_impl->secondaryWindows.end()) return; // not attached

    // RTV first, then the swapchain it views into. Deliberately no ClearState() or
    // Flush() on the way out: that immediate context is the PRIMARY's, and
    // ClearState() would wipe the primary's bound pipeline state along with it.
    it->second.rtv.Reset();
    it->second.swapchain.Reset();
    m_impl->secondaryWindows.erase(it);
    HE_LOG_INFO(RHI, "%s", "D3D11Renderer: secondary window detached");
}

void D3D11Renderer::RenderWindow(HE::Window* window)
{
    if (!window || !m_impl->context) return;
    auto it = m_impl->secondaryWindows.find(window->GetNativeWindow());
    if (it == m_impl->secondaryWindows.end()) return; // not attached
    auto& sw = it->second;
    if (!sw.swapchain || !sw.rtv) return;

    // NOT black, on purpose. A black clear is pixel-identical to a window that was
    // never drawn into at all — on every backend — so a black secondary window
    // could not distinguish "this override ran" from "this override never ran",
    // and the feature would be unverifiable. This slate blue IS the witness that
    // AttachWindow/RenderWindow actually executed.
    const float clearColor[4] = { 0.16f, 0.22f, 0.34f, 1.0f };
    m_impl->context->OMSetRenderTargets(1, sw.rtv.GetAddressOf(), nullptr);
    m_impl->context->ClearRenderTargetView(sw.rtv.Get(), clearColor);
    // No RSSetViewports: ClearRenderTargetView ignores viewport and scissor and
    // fills the whole view, which is exactly what the witness colour needs.

    // The overlay callback is deliberately NOT injected here. That callback renders
    // ImGui::GetDrawData() — the MAIN viewport's draw data, sized by the PRIMARY
    // window's DisplaySize — so calling it here would paint a clipped duplicate of
    // the main editor UI into this window. The editor's detached panels are served
    // by ImGui's own multi-viewport backends instead (ImGuiConfigFlags_ViewportsEnable,
    // EditorApplication.cpp:616), which create and present their own per-viewport
    // swapchains and never route through this API.
    // Vulkan's secondary path reaches the same conclusion and drops the callback
    // for the same reason; the long note at VulkanRenderer.cpp:1372-1393 has the
    // measured detail (its secondary render pass has no depth attachment, so the
    // injected ImGui pipeline tripped renderpass-compatibility validation).

    // The renderer has to present: Window::SwapBuffers() is a no-op unless the API
    // is OpenGL (Window.cpp:178-183), so Application's post-RenderWindow
    // SwapBuffers() does nothing for D3D.
    // Note: with VSync on this blocks a second time per frame, on top of the
    // primary's Present in Render() — an open secondary window roughly halves the
    // frame rate. Reading m_impl->vsync is still the correct behaviour.
    sw.swapchain->Present(m_impl->vsync ? 1 : 0, 0);

    // Unbind before returning. The immediate context is shared with the primary
    // path, which ends Render() with the primary swapchain RTV bound; leaving a
    // secondary window's RTV bound is a real cross-window hazard.
    m_impl->context->OMSetRenderTargets(0, nullptr, nullptr);
}

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

void D3D11Renderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
    m_impl->aaMethod = IRenderer::ResolveAAMethod(s.method, GetCapabilities());
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

// ─── Material pipeline warm-up ──────────────────────────────────────────────
// Build each node-graph material's VS/PS NOW so the first draw doesn't stall on a
// synchronous SPIRV-Cross + D3DCompile inside the frame. Built-in-PBR materials
// resolve no graph shader and are skipped; cache hits are free.
//
// Unlike D3D12/Vulkan there is exactly ONE variant per material here: D3D11 has no
// PSO, so blend/depth/RT-format are separate states set at draw time and the
// opaque and transparent draws share the same shader objects. Hence no
// hdr × transparent matrix — one GetOrBuildMaterialShaders call per material.
void D3D11Renderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
#if defined(HE_HAVE_SHADERC)
    auto& p = *m_impl;
    // m_matReady gates the DRAW path, so warming without it would build shaders
    // that can never be used.
    if (!p.m_matReady || !m_contentManager) return;

    int built = 0;
    for (const HE::UUID& id : materialIds)
    {
        uint64_t hash = 0; std::string frag, vertBody;
        if (!p.m_matShaderLib.resolveShaders(*m_contentManager, id, hash, frag, vertBody))
            continue; // built-in PBR — nothing to build
        // GetOrBuildMaterialShaders keys on hash ^ (transparent ? magic : 0), so the
        // non-transparent variant's key IS the hash. Checked here (not just inside)
        // so an already-warm material isn't counted as newly built.
        if (p.m_materialShaders.count(hash)) continue;
        if (p.GetOrBuildMaterialShaders(hash, frag, vertBody, false)) ++built;
    }
    if (built > 0)
        HE_LOG_INFO(RHI, "%s",
            ("D3D11Renderer: warmed up " + std::to_string(built) + " material program(s)").c_str());
#else
    (void)materialIds;
#endif
}

// ─── Content-Browser asset thumbnails ───────────────────────────────────────
bool D3D11Renderer::RenderAssetThumbnail(ContentManager& cm, ThumbnailKind kind,
                                         const HE::UUID& assetId, uint32_t size,
                                         std::vector<uint8_t>& outRgba8)
{
    auto& p = *m_impl;
    const int S = HE::clampThumbnailSize(size);
    // Adopt the ContentManager: thumbnails are requested BEFORE the first Render(),
    // so SetContentManager may not have run yet and resolveMesh would find nothing.
    if (!m_contentManager) m_contentManager = &cm;
    if (assetId == HE::UUID{}) return false;
    if (!p.ensureThumbnailTarget(S)) return false;

    p.beginThumbnailPass(S);

    bool drew = false;
    if (kind == ThumbnailKind::StaticMesh)
    {
        if (const GpuMesh* mesh = p.resolveMesh(assetId, m_contentManager))
        {
            p.drawMeshPreviewGeometry(mesh->vbuf.Get(), mesh->ibuf.Get(), mesh->indexCount,
                                      mesh->texture.Get(),
                                      HE::boundsCenter(mesh->localBounds),
                                      HE::boundsExtent(mesh->localBounds),
                                      glm::vec3(HE::kMeshTileBaseColor),
                                      HE::kMeshTileMetallic, HE::kMeshTileRoughness,
                                      HE::kThumbYaw, HE::kThumbPitch, HE::kMeshFrameDist);
            drew = true;
        }
    }
    else if (kind == ThumbnailKind::Material)
    {
        // Graph material → its REAL shader; otherwise the built-in PBR scalars on
        // the same sphere, so every material asset still produces a tile. Mirrors
        // OpenGLRenderer::RenderAssetThumbnail exactly.
        //
        // This branch used to draw the fallback sphere unconditionally, because the
        // graph PS could not be built on D3D at all (the ps_5_0 sampler-register
        // cap). MaterialShaderLibrary now pins the registers and it builds, so the
        // honest tile is the real one.
#if defined(HE_HAVE_SHADERC)
        // kMatGraphDist, NOT kMatFallbackDist: drawMaterialPreviewGeometry shoots at
        // 32° and the fallback at 35°, and the two distances are picked so the
        // sphere fills the same ~90 % of the tile either way. Swapping them changes
        // the sphere's apparent size.
        if (p.m_matReady && m_contentManager)
        {
            uint64_t matHash = 0; std::string matFrag, matVertBody;
            if (p.m_matShaderLib.resolveShaders(*m_contentManager, assetId,
                                                matHash, matFrag, matVertBody))
                drew = p.drawMaterialPreviewGeometry(assetId, matHash, matFrag, matVertBody,
                                                     HE::kThumbYaw, HE::kThumbPitch,
                                                     HE::kMatGraphDist, 0 /*sphere*/,
                                                     HE::UUID{}, m_contentManager);
        }
#endif
        // Still the right tile for a built-in-PBR material (it has no graph at all),
        // and the safety net for a graph whose HLSL genuinely will not build.
        if (!drew && p.ensurePreviewShape(0 /*sphere*/))
        {
            const MaterialAsset* ma = m_contentManager->getMaterial(assetId);
            // Same base-texture selection as the scene's override path (getMaterial →
            // textureIds[0]/texturePaths[0] → SRV), mirroring GL's ResolveGraphTexture.
            ID3D11ShaderResourceView* baseTex = nullptr;
            p.resolveMaterialOverride(assetId, m_contentManager, baseTex);
            p.drawMeshPreviewGeometry(p.previewSphereVB.Get(), p.previewSphereIB.Get(),
                                      p.previewSphereIdx, baseTex,
                                      glm::vec3(0.0f), 1.0f,
                                      ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2])
                                         : glm::vec3(0.8f),
                                      ma ? ma->metallic  : 0.0f,
                                      ma ? ma->roughness : 0.5f,
                                      HE::kThumbYaw, HE::kThumbPitch, HE::kMatFallbackDist);
            drew = true;
        }
    }
    else if (kind == ThumbnailKind::SkeletalMesh)
    {
        // BIND POSE, drawn by the UNSKINNED program: the stored vertex positions
        // already are the bind pose, so no bone matrices and no skinning shader are
        // needed. Slot 0 of a GpuSkeletalMesh is the same 32-byte pos/normal/uv
        // vertex meshPvIL describes; the bone ID/weight streams in slots 1 and 2 are
        // simply not bound. Exactly what GL does with the extra VAO attributes.
        if (const GpuSkeletalMesh* mesh = p.resolveSkeletalMesh(assetId, m_contentManager))
        {
            p.drawMeshPreviewGeometry(mesh->vb.Get(), mesh->ib.Get(),
                                      static_cast<UINT>(mesh->indexCount),
                                      mesh->srv.Get(),
                                      HE::boundsCenter(mesh->localBounds),
                                      HE::boundsExtent(mesh->localBounds),
                                      glm::vec3(HE::kMeshTileBaseColor),
                                      HE::kMeshTileMetallic, HE::kMeshTileRoughness,
                                      HE::kThumbYaw, HE::kThumbPitch, HE::kMeshFrameDist);
            drew = true;
        }
    }

    if (drew) drew = p.readThumbnailTarget(S, outRgba8);
    p.endThumbnailPass();
    return drew;
}

// ─── UI widget thumbnails ───────────────────────────────────────────────────
bool D3D11Renderer::RenderWidgetThumbnail(const std::vector<UIRenderObject>& uiObjects,
                                          uint32_t size, std::vector<uint8_t>& outRgba8)
{
    auto& p = *m_impl;
    const int S = HE::clampThumbnailSize(size);
    if (uiObjects.empty() || !p.ensureThumbnailTarget(S)) return false;

    p.beginThumbnailPass(S);

    // renderUIPass draws m_renderWorld.uiObjects into whatever is bound, so the tile
    // borrows that list for one pass and puts the frame's own back. SWAPPING (not
    // copying) out is what keeps the scene's objects intact.
    std::vector<UIRenderObject> saved;
    saved.swap(p.m_renderWorld.uiObjects);
    p.m_renderWorld.uiObjects = uiObjects;
    p.renderUIPass(p.context.Get(), S, S);
    p.m_renderWorld.uiObjects.swap(saved);

    const bool ok = p.readThumbnailTarget(S, outRgba8);
    p.endThumbnailPass();
    return ok;
}

// ─── Particle-system thumbnails ─────────────────────────────────────────────
bool D3D11Renderer::RenderParticleThumbnail(ContentManager& cm, const HE::UUID& materialId,
                                            const std::vector<ParticlePreviewInstance>& particles,
                                            uint32_t size, std::vector<uint8_t>& outRgba8)
{
    auto& p = *m_impl;
    const int S = HE::clampThumbnailSize(size);
    if (!m_contentManager) m_contentManager = &cm;
    if (particles.empty() || !p.ensureParticlePreviewPipeline()) return false;
    if (!p.ensureThumbnailTarget(S)) return false;

    // The material's base texture, or null → the shader's soft circular sprite.
    // resolveMaterialOverride returns false only while the asset is still loading.
    ID3D11ShaderResourceView* tex = nullptr;
    if (!p.resolveMaterialOverride(materialId, m_contentManager, tex)) tex = nullptr;

    p.beginThumbnailPass(S);
    // Same fixed three-quarter framing as the mesh tiles, so a grid of assets
    // reads as one set.
    p.drawParticlePreviewGeometry(tex, particles, HE::kThumbYaw, HE::kThumbPitch, HE::kParticleDist);
    const bool ok = p.readThumbnailTarget(S, outRgba8);
    p.endThumbnailPass();
    return ok;
}

// ─── Interactive material preview ───────────────────────────────────────────
// The Material Editor's viewport. Same subject as the Content-Browser tile, but
// with the panel's orbit instead of the fixed three-quarter shot and a handle
// returned instead of pixels.
//
// The graph path below is attempted first and falls back to the SAME built-in-PBR
// sphere RenderAssetThumbnail's Material branch draws. That fallback is no longer
// the normal outcome: it used to fire for EVERY graph material because the PIXEL
// shader could not compile on D3D at all (the shared preamble reaches sampler
// binding 33, SPIRV-Cross mapped binding N to register(sN), and ps_5_0 caps at
// s15 — FXC answered X4509). MaterialShaderLibrary::fragment now compiles the
// fragment through he::shaderc::compileHlslPinned with a table that compacts the
// six over-cap samplers into the free slots, so the shader builds and this
// preview shades with the material's real graph.
//
// STILL OPEN, and the reason a tile here will not pixel-match OpenGL yet: D3D11
// has no graph-texture cache, so heTexP0..3 are white 1x1 defaults where GL binds
// the material's real project textures (ResolveGraphTexture). A graph that samples
// a texture previews flat.
//
// ALSO STILL OPEN: the pin table fills s0..s15 exactly, with zero headroom. A
// LANDSCAPE graph material additionally needs binding 14 (the weightmap) and so
// still does not fit ps_5_0 — that half of the old gap is untouched.
void* D3D11Renderer::RenderMaterialPreview(ContentManager& cm, const HE::UUID& materialId,
                                           uint32_t size, float yaw, float pitch, float dist,
                                           int shape, const HE::UUID& meshId)
{
    auto& p = *m_impl;
    if (!m_contentManager) m_contentManager = &cm;
    if (!p.device || !m_contentManager) return nullptr;
    // Clamp per the IRenderer contract FIRST, then bucket — a bucket applied to an
    // unclamped size could land above the contract's ceiling.
    const int S = D3D11RendererImpl::bucketPreview(HE::clampPreviewSize(size));

#if defined(HE_HAVE_SHADERC)
    // A built-in-PBR material has no node-graph program and OpenGL returns nullptr
    // for it BEFORE allocating a target — the editor then shows its placeholder,
    // and the Content-Browser tile path is what covers that case. Mirror exactly:
    // returning a sphere here where GL returns nothing would make the two backends
    // disagree about whether a preview exists at all.
    uint64_t matHash = 0; std::string matFrag, matVertBody;
    const bool hasGraph = p.m_matReady
        && p.m_matShaderLib.resolveShaders(*m_contentManager, materialId, matHash, matFrag, matVertBody);
#else
    const bool hasGraph = false;
#endif
    if (!hasGraph) return nullptr;

    if (!p.ensurePreviewTarget(p.matPreview, S, S)) return nullptr;
    p.beginPreviewPass(p.matPreview);

    bool drew = false;
#if defined(HE_HAVE_SHADERC)
    drew = p.drawMaterialPreviewGeometry(materialId, matHash, matFrag, matVertBody,
                                         yaw, pitch, dist, shape, meshId, m_contentManager);
#endif
    if (!drew)
    {
        // ── Fallback: built-in-PBR shading on the requested primitive, with the
        // CALLER's yaw/pitch/dist (a tile's fixed angles would defeat the panel's
        // orbit) und mit demselben 32°-Schuss wie der Graph-Pfad darüber, damit die
        // Kugel beim Wechsel zwischen den beiden Pfaden nicht in ihrer Größe
        // springt. D3D12 macht es genauso.
        //
        // Since the register-pin table landed this only fires for a graph whose
        // HLSL is genuinely broken (or a landscape graph, which still overruns the
        // ps_5_0 sampler cap) — not, as before, for every graph material.
        if (!p.matPreviewFallbackLogged)
        {
            p.matPreviewFallbackLogged = true;
            HE_LOG_WARN(RHI, "%s",
                "D3D11Renderer: node-graph material preview shader failed to build — "
                "falling back to the built-in PBR primitive. Logged once per session.");
        }
        // `meshId` still wins over `shape` here — the Material Editor's mesh picker
        // has to keep working while the graph shader is unavailable, and the harness
        // drives exactly this via HE_DUMP_PREVIEWMESH. Auto-framed on the mesh's own
        // bounds so `dist` means the same thing as for the unit primitive; a mesh
        // that is not resident falls back to the primitive, as everywhere else.
        const GpuMesh* gm = (meshId != HE::UUID{}) ? p.resolveMesh(meshId, m_contentManager) : nullptr;
        const bool     useMesh = gm && gm->vbuf && gm->ibuf && gm->indexCount > 0;
        if (useMesh || p.ensurePreviewShape(shape))
        {
            const MaterialAsset* ma = m_contentManager->getMaterial(materialId);
            ID3D11ShaderResourceView* baseTex = nullptr;
            p.resolveMaterialOverride(materialId, m_contentManager, baseTex);
            // The material's own texture wins; a mesh drawn without one keeps its
            // baked albedo rather than going flat.
            if (!baseTex && useMesh) baseTex = gm->texture.Get();
            p.drawMeshPreviewGeometry(useMesh ? gm->vbuf.Get() : p.previewSphereVB.Get(),
                                      useMesh ? gm->ibuf.Get() : p.previewSphereIB.Get(),
                                      useMesh ? gm->indexCount : p.previewSphereIdx,
                                      baseTex,
                                      useMesh ? HE::boundsCenter(gm->localBounds) : glm::vec3(0.0f),
                                      useMesh ? HE::boundsExtent(gm->localBounds) : 1.0f,
                                      ma ? glm::vec3(ma->baseColor[0], ma->baseColor[1], ma->baseColor[2])
                                         : glm::vec3(0.8f),
                                      ma ? ma->metallic  : 0.0f,
                                      ma ? ma->roughness : 0.5f,
                                      yaw, pitch, dist, 32.0f);
            drew = true;
        }
    }

    if (drew) p.dumpPreviewTarget(p.matPreview, "HE_PREVIEW_DUMP");
    p.endPreviewPass();
    // The cached SRV, created once with the target — never a per-call registration.
    return drew ? static_cast<void*>(p.matPreview.srv.Get()) : nullptr;
}

// ─── Interactive skeletal-mesh preview ──────────────────────────────────────
// The only preview here that is NOT square: the Skeletal Mesh Editor sizes its
// target to the pane so the mesh fills it edge to edge, and the projection takes
// its aspect from the same numbers.
void* D3D11Renderer::RenderSkeletalPreview(ContentManager& cm, const HE::UUID& meshId,
                                           const std::vector<glm::mat4>& boneMatrices,
                                           uint32_t width, uint32_t height,
                                           float yaw, float pitch, float dist,
                                           bool showSkeleton, glm::mat4* outViewProj)
{
    auto& p = *m_impl;
    if (!m_contentManager) m_contentManager = &cm;
    if (!p.device || !m_contentManager) return nullptr;

    const GpuSkeletalMesh* smesh = p.resolveSkeletalMesh(meshId, m_contentManager);
    if (!smesh || !smesh->vb || !smesh->ib || smesh->indexCount <= 0) return nullptr;
    if (!p.ensureSkelPreviewPipeline()) return nullptr;

    // Bucket the two axes INDEPENDENTLY, so ordinary splitter drag does not
    // recreate the target (and, on the descriptor-limited backends, leak one) for
    // every pixel of movement.
    //
    // Die Projektion nimmt trotzdem das ANGEFORDERTE Seitenverhältnis, nicht das
    // des gerundeten Ziels. `ImGui::Image` zeichnet die Textur auf die Größe des
    // Bereichs, skaliert die W×H-Textur also auf w×h — und zwar auf beiden Achsen
    // unterschiedlich stark, sobald die Rundung die Seitenverhältnisse trennt.
    // Eine Kugel bleibt im Endbild genau dann rund, wenn die Projektion mit w/h
    // gebaut wurde: das Rendering ist dann für den Bucket „vorverzerrt" und die
    // Skalierung hebt das exakt auf. Mit W/H wäre sie im Ziel rund und auf dem
    // Schirm oval. Vulkan und D3D12 machen es genauso.
    const int W = D3D11RendererImpl::bucketPreview(HE::clampSkeletalPreviewSize(width));
    const int H = D3D11RendererImpl::bucketPreview(HE::clampSkeletalPreviewSize(height));
    if (!p.ensurePreviewTarget(p.skelPreview, W, H)) return nullptr;

    // Orbit camera auto-framed on the BIND-POSE bounds (see GpuSkeletalMesh::
    // localBounds — deliberately not re-fitted per pose).
    const HE::PreviewCamera cam = HE::meshOrbit(HE::boundsCenter(smesh->localBounds),
                                                HE::boundsExtent(smesh->localBounds),
                                                yaw, pitch, dist,
                                                static_cast<float>(HE::clampSkeletalPreviewSize(width)) /
                                                static_cast<float>(HE::clampSkeletalPreviewSize(height)));
    // ONE ClipSpace fix-up on meshOrbit's GL-convention projection.
    const glm::mat4 viewProj = HE::kD3DClipFix * cam.proj * cam.view;
    // Handed out in THIS backend's clip convention (the fix-up included), because
    // that is the space a caller's own overlay has to be drawn in on D3D11 — the
    // debug-line path here expects exactly this matrix.
    if (outViewProj) *outViewProj = viewProj;

    // 128-entry identity scratch: an empty/short boneMatrices then yields the bind
    // pose, matching GL. kMaxBones is the BonesCB array length, not a guess.
    constexpr size_t kMaxBones = 128;
    std::vector<glm::mat4> boneScratch(kMaxBones, glm::mat4(1.0f));
    const size_t boneCount = std::min(boneMatrices.size(), kMaxBones);
    if (boneCount > 0) std::copy_n(boneMatrices.begin(), boneCount, boneScratch.begin());

    p.beginPreviewPass(p.skelPreview);
    p.drawSkelPreviewGeometry(*smesh, viewProj, boneScratch);

    // ── Bone overlay: joint markers (three axis-aligned crosses) + parent→child
    // segments, drawn through the EXISTING debug-line pipeline rather than a third
    // line shader. World joint transform = boneMatrix * inverse(inverseBindMatrix),
    // since a bone matrix is defined as globalJointXform * invBind
    // (composeBoneMatrices, AnimationEval.cpp).
    //
    // GL draws these at glLineWidth(2.0); D3D11 has no line-width state at all, so
    // the overlay is 1 px here. That is a real, unavoidable difference.
    if (showSkeleton)
    {
        if (const SkeletalMeshAsset* asset = m_contentManager->getSkeletalMesh(meshId))
        {
            std::vector<glm::vec3> jointWorld(asset->skeleton.size(), glm::vec3(0.0f));
            for (size_t i = 0; i < asset->skeleton.size(); ++i)
            {
                const glm::mat4 invBind = glm::make_mat4(asset->skeleton[i].inverseBindMatrix.data());
                const glm::mat4 world =
                    (i < boneScratch.size() ? boneScratch[i] : glm::mat4(1.0f)) * glm::inverse(invBind);
                jointWorld[i] = glm::vec3(world[3]);
            }

            const glm::vec3 jointColor(1.0f, 0.85f, 0.15f), boneColor(0.2f, 0.9f, 1.0f);
            const float markerSize =
                std::max(HE::boundsExtent(smesh->localBounds) * 0.015f, 0.005f);
            std::vector<DebugLine> lines;
            lines.reserve(asset->skeleton.size() * 4);
            for (size_t i = 0; i < asset->skeleton.size(); ++i)
            {
                const glm::vec3 j = jointWorld[i];
                lines.push_back({ j - glm::vec3(markerSize, 0, 0), j + glm::vec3(markerSize, 0, 0), jointColor });
                lines.push_back({ j - glm::vec3(0, markerSize, 0), j + glm::vec3(0, markerSize, 0), jointColor });
                lines.push_back({ j - glm::vec3(0, 0, markerSize), j + glm::vec3(0, 0, markerSize), jointColor });
                const int32_t parent = asset->skeleton[i].parent;
                if (parent >= 0 && static_cast<size_t>(parent) < jointWorld.size())
                    lines.push_back({ jointWorld[parent], j, boneColor });
            }
            // drawDebugLines grows its vertex buffer on demand, so a dense skeleton
            // is fine. It restores the SCENE's shaders/CBs on the way out, which is
            // inert here — endPreviewPass unbinds the target immediately after.
            p.drawDebugLines(p.context.Get(), viewProj, lines);
        }
    }

    p.dumpPreviewTarget(p.skelPreview, "HE_SKEL_PREVIEW_DUMP");
    p.endPreviewPass();
    return static_cast<void*>(p.skelPreview.srv.Get());
}

// ─── Interactive particle preview ───────────────────────────────────────────
// The Particle Graph Editor owns the simulation (ParticleSystem::stepPool, in
// HE_Scene) and hands over one fully-resolved instance per live particle; this
// only draws them. `meshId` is ignored, exactly as on OpenGL — the preview is
// always camera-facing billboards.
void* D3D11Renderer::RenderParticlePreview(ContentManager& cm, const HE::UUID& /*meshId*/,
                                           const HE::UUID& materialId,
                                           const std::vector<ParticlePreviewInstance>& particles,
                                           uint32_t size, float yaw, float pitch, float dist)
{
    auto& p = *m_impl;
    if (!m_contentManager) m_contentManager = &cm;
    if (!p.device) return nullptr;
    if (!p.ensureParticlePreviewPipeline()) return nullptr;

    const int S = D3D11RendererImpl::bucketPreview(HE::clampPreviewSize(size));
    if (!p.ensurePreviewTarget(p.particlePreview, S, S)) return nullptr;

    // The material's base texture, or null → the shader's soft circular sprite.
    ID3D11ShaderResourceView* tex = nullptr;
    if (!p.resolveMaterialOverride(materialId, m_contentManager, tex)) tex = nullptr;

    p.beginPreviewPass(p.particlePreview);
    // An EMPTY pool still returns a valid (fully transparent) handle rather than
    // nullptr — GL parity, and it is what keeps the panel from flickering to its
    // "unavailable" placeholder on the frames between bursts.
    p.drawParticlePreviewGeometry(tex, particles, yaw, pitch, dist);
    p.dumpPreviewTarget(p.particlePreview, "HE_PARTICLE_PREVIEW_DUMP");
    p.endPreviewPass();
    return static_cast<void*>(p.particlePreview.srv.Get());
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
