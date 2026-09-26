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
#include <HorizonRendering/MaterialScalars.h>
#include <HorizonRendering/RenderGraph.h>
#include <HorizonRendering/CommandBuffer.h>
#include <Math/AABB.h>
#include <Types/UUID.h>
#include <HorizonRendering/GiBvh.h>          // GI: CPU BLAS (shared with GL/Vulkan/Metal-SW)
#include <ContentManager/DefaultAssets.h>    // GI: default-cube occluder fallback
#include <material/MaterialShaderLibrary.h> // A4: shared cross-backend material shader layer (unguarded, like Vulkan/D3D12)
#include <MaterialGraph/MaterialGraph.h>     // kMatMaxGraphTextures (heTexP0..3)
// ── Cross-backend renderer helpers (audit 1a) ────────────────────────────────
// Each of these replaced a private copy that every backend carried; the copies
// were byte-identical by contract (the GPU reads the packed bytes positionally),
// so the shared versions are what keeps GL == Metal == Vulkan == D3D.
#include <HorizonRendering/SkyNoise3D.h>      // CPU sky/cloud noise volume bake
#include <HorizonRendering/SsaoKernel.h>      // SSAO sample kernel + rotation noise
#include <HorizonRendering/SkyFrameParams.h>  // HE::BuildSkyFrameParams (folds in the cloud wind vector)
#include <HorizonRendering/SkyShaderSource.h> // the GL sky, cross-compiled for the sky pass
#if defined(HE_HAVE_SHADERC)
#include "ShaderCompiler.h"                   // he::shaderc::compileHlslPinned (sky pass)
#endif
#include <HorizonRendering/LightPacking.h>    // GPU light window + shadow-mask lights
#include <HorizonRendering/ClipSpace.h>       // GL depth (-1..1) → D3D depth (0..1)
#include <HorizonRendering/WorldPreviewGrid.h> // RenderWorldPreview: grid, background, dump
#include <HorizonRendering/WorldPreviewFrame.h> // RenderWorldPreview: camera, snapshot, light
#include <HorizonRendering/RenderConstants.h> // shadow-map size, GPU timer ring depth
#include <HorizonRendering/TemporalAA.h>      // TAA jitter sequence + jittered matrix (shared with D3D12/Vulkan)
#include "Backends/D3D_Shared/HlslSources.h"  // HLSL byte-identical to the D3D12 backend
#include "Backends/D3D11/D3D11MaterialBindings.h" // A4: heLandscapeWeights t14/s0 per draw, shared with he_tests (Thema 57)
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

// ─── Shader sources ─────────────────────────────────────────────────────────
// Everything that is byte-identical to the D3D12 backend — the sky colour
// function, the sky VS, debug lines, the skinned VS, SSAO/HBAO/GTAO, the
// fullscreen-triangle VS + tonemap, and all five GI stages — now lives in
// Backends/D3D_Shared/HlslSources.h. That header also spells out why the HLSL
// that is still embedded below is NOT shared (different resource bindings, a
// feature the other backend has and this one does not). Pulled in unqualified
// so the compile sites further down read exactly as they did before.
using namespace HE::hlsl;

// ─── Sky background pass HLSL (FALLBACK) ────────────────────────────────────
// The sky pass normally runs the GL sky cross-compiled through he::shaderc
// (HorizonRendering/SkyShaderSource.h, see createSkyPipeline). This older,
// reduced sky — gradient atmosphere, 2D aurora, no nebula/cirrus/god rays — is
// only what a build without the cross-compiler, or a failed compile, draws.
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
    // x = metallic, y = roughness, z = opacity,
    // w = 1 when the object IGNORES shadows (MeshComponent::receivesShadow ==
    //     false). Inverted on purpose: every zero-initialised PerObjectCB fill
    //     (shadow pass, GI G-buffer, previews) then keeps shadowing.
    float4   uPBR;
};
cbuffer PerFrame : register(b1)
{
    float4   uCameraPos;        // xyz
    int4     uLightCount;       // x = count
    float4   uLightPos[8];      // xyz pos,  w type (0 dir / 1 point / 2 spot)
    float4   uLightDir[8];      // xyz dir,  w cos(spot half angle)
    float4   uLightColor[8];    // rgb,      w intensity
    float4   uLightParams[8];   // x range
    // Cascaded shadow maps (mirrors GL's uCascadeVP/uCascadeSplits and Metal's
    // SceneUniforms): per-cascade light view-proj in D3D clip (kD3DClipFix
    // pre-applied on the CPU), the planar view-space far distance of each
    // cascade (w = cascade count) and the camera forward the cascade pick
    // measures that distance along. uShadowBias = project ShadowSettings
    // receiver bias (x = slope-scaled factor, y = minimum).
    float4x4 uCascadeVP[3];
    float4   uCascadeSplits;
    float4   uCameraFwd;        // xyz = world forward, w = 1 / shadow map size
    float4   uShadowBias;
    int4     uShadowEnabled;    // x = 0/1, y = 1 → tint fragments by cascade (debug)
    float4   uSunDir;           // xyz = sun direction toward sky, w unused
    float4   uFog;              // x = fogDensity, y = fogHeightFalloff
    float4   uViewport;        // x=width, y=height, z=ssaoEnabled (0/1)
    float4   uGIParams;        // x = GI enabled (0/1), y = indirect intensity
    float4   uGIGridOrigin;    // xyz = probe grid origin, w = spacing
    float4   uGIGridCounts;    // xyz = probe counts, w = probesPerRow
    // Forward SSR (docs/ssr-cross-backend-plan.md C5): x = 1 → a trace ran this
    // frame and uSSRFwd holds it, y = intensity, z = max roughness, w = 0. The
    // same four lanes heLitP reads out of Lighting::ssr — must stay in step with
    // PerFrameCB, which is memcpy'd into this cbuffer whole.
    float4   uSSRParams;
    // Local (point/spot) shadow atlas (mirrors Metal's SceneUniforms::
    // localShadowVP / GL's uLocalShadowVP): one light view-proj per atlas layer
    // in D3D clip (kD3DClipFix pre-applied), 16 layers = spot 1 / point 6
    // cube faces. uLightParams[i].y is the light's base layer (-1 = casts no
    // local shadow). uLocalShadowParams.x = 1 / atlas size.
    float4x4 uLocalShadowVP[16];
    float4   uLocalShadowParams;
    // Clustered lighting (plan P7 on the forward path, HE::BuildClusterLights):
    // x/y/z = cluster grid dims (x == 0 → clustering off, the 8-light window
    // then carries point/spot lights too), w = gridZ / log(far / near).
    // uClusterCamFwd.xyz = camera forward the depth slice is measured along,
    // w = the grid's near plane. Appended last — the cbuffer is memcpy'd whole.
    float4   uClusterParams;
    float4   uClusterCamFwd;
};

Texture2D    uTexture   : register(t0);
// Directional CSM depth array — one slice per cascade, sampled through the
// dedicated point/clamp shadow sampler on s3 (a PCF tap must read ONE texel's
// depth, and the wrap sampler on s0 would pull the opposite edge in).
Texture2DArray uShadowMap : register(t1);
Texture2D    uAO        : register(t2);
Texture2D    uGIShadow  : register(t4); // half-res ray-traced sun-shadow mask
Texture2D    uGIIrr     : register(t5); // DDGI irradiance atlas (RGBA16F)
Texture2D    uGIVis     : register(t6); // DDGI visibility atlas (RG16F)
Texture2D    uGILocal   : register(t7); // half-res local-light visibility mask (1 channel per light, first 4)
// Half-res screen-space reflection trace (rgb = reflected radiance, a =
// confidence). t16 on purpose: the low SRV registers are all spoken for
// (t0..t7 here and in the graph materials, t8..t13 the pinned SSR pass block,
// t14/t15 the decals) and D3D11 has 128 SRV slots — only samplers (16) and
// constant buffers (14) are scarce, which is why this one shares uGISampler.
Texture2D    uSSRFwd    : register(t16);
// Local (point/spot) shadow atlas — 16-layer depth array, sampled through the
// same point/clamp sampler (s3) as the cascades. t17: next free register after
// the SSR result; the D3D12 scene shader uses the same one (shared contract).
Texture2DArray uLocalShadowMap : register(t17);
// Clustered lighting lists (HE::BuildClusterLights in LightPacking.h): 4 float4
// per light (posType / dirCos / colourIntensity / {range, atlas layer + 1,
// GI mask channel + 1, 0}), one {offset, count} per cluster, and the flat
// index list. t18..t20: the next free registers after the local atlas; the
// D3D12 scene shader uses the same ones (shared contract). Read only when
// uClusterParams.x > 0 — the buffers are bound whenever that is the case.
StructuredBuffer<float4> uClusterLights : register(t18);
StructuredBuffer<uint2>  uClusterGrid   : register(t19);
StructuredBuffer<uint>   uClusterIdx    : register(t20);
SamplerState uSampler   : register(s0);
SamplerState uAOSampler : register(s1);
SamplerState uGISampler : register(s2); // linear clamp (mask upsample + atlases)
SamplerState uShadowSampler : register(s3); // point clamp (CSM depth array)

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
// Instanced twin for a run of same-mesh shadow casters: one DrawIndexedInstanced
// per run. The CPU fills gInstances (t3) with {lightClip * model, model}, the
// same product VSDepth gets through uMVP, so the depth map does not change.
float4 VSDepthInstanced(VSIn i, uint iid : SV_InstanceID) : SV_POSITION
{
    return mul(gInstances[iid].mvp, float4(i.pos, 1.0));
}

// Cascaded shadows — the HLSL twin of Metal's shadowFactor() (same top-left
// texture origin and [0,1] clip depth) and GL's computeShadow(): pick the first
// cascade whose far distance covers the fragment (by PLANAR camera-forward
// distance, the same measure the extractor split the cascades with — euclidean
// distance would push screen-edge pixels into a too-coarse cascade), project
// into that cascade's light clip, normal-offset + slope-scaled bias scaled by
// cascade, 3×3 PCF over its slice of the depth array. outCascade returns the
// chosen index for the debug tint.
float shadowFactor(float3 worldPos, float3 N, float3 L, out int outCascade)
{
    outCascade = 0;
    if (uShadowEnabled.x == 0) return 1.0;
    float viewDist = dot(worldPos - uCameraPos.xyz, uCameraFwd.xyz);
    int count = int(uCascadeSplits.w);
    int c = (count > 0) ? count - 1 : 0;
    if      (count > 0 && viewDist < uCascadeSplits.x) c = 0;
    else if (count > 1 && viewDist < uCascadeSplits.y) c = 1;
    else if (count > 2 && viewDist < uCascadeSplits.z) c = 2;
    c = clamp(c, 0, 2);
    outCascade = c;

    // Normal-offset bias scaled by cascade — coarser (farther) cascades have
    // larger texels and need a bigger offset to avoid acne.
    float4 lp = mul(uCascadeVP[c], float4(worldPos + N * (0.06 * float(c + 1)), 1.0));
    float3 p  = lp.xyz / lp.w;                            // z already [0,1] (D3D clip)
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); // top-left origin
    float  texel = uCameraFwd.w;                          // 1 / shadow map size
    // Reject one texel inside the border so the 3×3 kernel never reads outside
    // this cascade (clamped neighbour texels → edge fringes).
    if (p.z > 1.0 || any(uv < texel) || any(uv > 1.0 - texel)) return 1.0;
    float ndl  = saturate(dot(N, L));
    float bias = clamp(uShadowBias.x * tan(acos(ndl)), uShadowBias.y, 0.02) * float(c + 1);
    float vis = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float cd = uShadowMap.Sample(uShadowSampler,
                                         float3(uv + float2(x, y) * texel, float(c))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    // No direct-light floor in shadow (GL/Metal agree): ambient + IBL already
    // provide the indirect minimum; a floor bleeds sun colour into shadow.
    return vis / 9.0;
}

// Point/spot shadow lookup in the local shadow atlas — the HLSL twin of Metal's
// localShadowFactor(). Spot lights project into their single perspective
// layer; point lights first pick the cube face from the fragment→light
// vector's major axis (faces stored as 6 consecutive array layers, +X −X +Y
// −Y +Z −Z), then project into that face's layer. Same 3×3 PCF and
// normal-offset bias family as the directional CSM above; the receiver bias
// is fixed (the project ShadowSettings pair tunes the cascades only, as on
// Metal/GL). Takes the light EXPLICITLY (position + type, decoded atlas base
// layer with -1 = none) so the 8-light window and the cluster lists share it
// — the twin of the preamble's heClusterShadow.
float localShadowFactor(float4 posType, int base, float3 worldPos, float3 N)
{
    // uLocalShadowParams.y = atlas rendered + bound this frame. First gate on
    // purpose: a zero-initialised PerFrame fill has lightParams.y = 0, which
    // would otherwise read as "layer 0" and sample an unbound atlas as black.
    if (uLocalShadowParams.y < 0.5) return 1.0;
    if (base < 0) return 1.0;
    int layer = base;
    if (int(posType.w) == 1) // point: major-axis cube-face pick
    {
        float3 d = worldPos - posType.xyz;
        float3 a = abs(d);
        int face;
        if      (a.x >= a.y && a.x >= a.z) face = (d.x > 0.0) ? 0 : 1;
        else if (a.y >= a.z)               face = (d.y > 0.0) ? 2 : 3;
        else                               face = (d.z > 0.0) ? 4 : 5;
        layer = base + face;
    }
    float3 toL = normalize(posType.xyz - worldPos);
    float  ndl = saturate(dot(N, toL));
    float4 lp = mul(uLocalShadowVP[layer], float4(worldPos + N * 0.02, 1.0));
    if (lp.w <= 0.0) return 1.0;                          // behind the light's near plane
    float3 p  = lp.xyz / lp.w;                            // z in [0,1] (D3D clip)
    float2 uv = float2(p.x * 0.5 + 0.5, 0.5 - p.y * 0.5); // top-left origin
    float  texel = uLocalShadowParams.x;                  // 1 / atlas size
    if (p.z > 1.0 || p.z < 0.0 || any(uv < texel) || any(uv > 1.0 - texel)) return 1.0;
    float bias = clamp(0.0015 * tan(acos(ndl)), 0.0006, 0.01);
    float vis = 0.0;
    [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float cd = uLocalShadowMap.Sample(uShadowSampler,
                                              float3(uv + float2(x, y) * texel, float(layer))).r;
            vis += (p.z - bias > cd) ? 0.0 : 1.0;
        }
    return vis / 9.0;
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

    // Flat-shade fallback for a scene without any light. With clustering on
    // the window holds directional lights only, so a night scene of point
    // lights has uLightCount.x == 0 and still must NOT land here — the CPU
    // zeroes uClusterParams.x when there is no light at all.
    if (uLightCount.x == 0 && uClusterParams.x < 0.5)
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
    // FORWARD reflection cascade (sky → SSR). SYNC: this is the heLitP twin in
    // MaterialShaderLibrary.cpp and the same stage GL's kUnlitFS, Metal's
    // fragmentMain and Vulkan's scene.frag carry — a graph material next to a
    // built-in one must mix the sources identically, or the same mirror changes
    // appearance with the material type. There is no ray-traced-GI-reflection
    // stage between the two here: D3D11 has none, and its gate would be a
    // constant zero. The trace carries no per-pixel roughness (the half-res
    // pre-pass has no material data), so the roughness fade lives HERE, with the
    // exact shading value. No `f` suffixes on the two constants below — the
    // cross-copy drift test in tests/test_culling.cpp matches this text.
    float3 envSpec = skyColor(Rrough, uSunDir.xyz);
    if (uSSRParams.x > 0.5)
    {
        float4 sr   = uSSRFwd.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0);
        float fade = 1.0 - smoothstep(uSSRParams.z * 0.7, uSSRParams.z, rough);
        envSpec = lerp(envSpec, sr.rgb, sr.a * uSSRParams.y * fade);
    }
    // Fresnel (Schlick, roughness-aware — same term as heLitP, ssr-plan P4).
    float  NdV = saturate(dot(N, V));
    float3 fresnelSpec = F0
        + (max(float3(1.0f - rough, 1.0f - rough, 1.0f - rough), F0) - F0) * pow(1.0f - NdV, 5.0f);
    float3 ambSpec = envSpec * fresnelSpec;
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
    int dbgCascade = 0; // cascade chosen by the directional shadow (debug tint)
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
        // (replaces the cascade array entirely), else the CSM lookup. An
        // if/else rather than a ternary so the `out` cascade index is written
        // only on the shadow-map branch (mirrors GL).
        float sh = 1.0;
        if (type == 0)
        {
            if (uGIParams.x > 0.5f) sh = uGIShadow.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0).r;
            else                    sh = shadowFactor(i.worldPos, N, L, dbgCascade);
        }
        else
        {
            // Local (point/spot) lights: shadow-mapped when the light casts
            // shadows (uLightParams.y = atlas base layer, set by the extractor).
            // When GI is active the ray-traced hard mask (first 4 local lights)
            // is combined in via min() — the map covers lights the mask can't.
            // Mirrors Metal/GL.
            sh = localShadowFactor(uLightPos[li], int(uLightParams[li].y), i.worldPos, N);
            if (uGIParams.x > 0.5f && giLocalIdx < 4)
                sh = min(sh, uGILocal.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0)[giLocalIdx]);
            giLocalIdx++;
        }
        // "Receives Shadow" off: the object is lit as if nothing occluded it.
        // After BOTH branches so it covers the shadow map and the GI masks alike.
        if (uPBR.w > 0.5f) sh = 1.0;
        result += BRDF(L, V, N, base, met, rough) * uLightColor[li].rgb * uLightColor[li].w * atten * sh;
    }
    // Clustered point/spot lights (plan P7 on the forward path): the fragment
    // picks its screen tile × log-depth slice and shades only that cluster's
    // list — the same attenuation, shadow and BRDF as the window loop above,
    // so HE_FORWARD_CLUSTER=0 (window carries everything) is a byte-for-byte
    // A/B of the first 8 lights. Mirrors heClusterLighting in the deferred
    // resolve (MaterialShaderLibrary.cpp), including the GI mask channel
    // carried in params.z.
    if (uClusterParams.x > 0.5)
    {
        float  nearZ = max(uClusterCamFwd.w, 1e-4);
        float  viewZ = max(dot(i.worldPos - uCameraPos.xyz, uClusterCamFwd.xyz), nearZ);
        int    gx = int(uClusterParams.x), gy = int(uClusterParams.y), gz = int(uClusterParams.z);
        float2 cuv = i.clip.xy / uViewport.xy; // top-left origin, like the CPU scatter
        int    cx = clamp(int(cuv.x * float(gx)), 0, gx - 1);
        int    cy = clamp(int(cuv.y * float(gy)), 0, gy - 1);
        int    cz = clamp(int(log(viewZ / nearZ) * uClusterParams.w), 0, gz - 1);
        uint2  cell = uClusterGrid[(cz * gy + cy) * gx + cx];
        [loop] for (uint k = 0; k < cell.y; ++k)
        {
            uint   ci      = uClusterIdx[cell.x + k] * 4u;
            float4 posType = uClusterLights[ci + 0u];
            float4 dirCos  = uClusterLights[ci + 1u];
            float4 colInt  = uClusterLights[ci + 2u];
            float4 params  = uClusterLights[ci + 3u];
            float3 d    = posType.xyz - i.worldPos;
            float  dist = max(length(d), 1e-4);
            float3 L    = d / dist;
            float  range = max(params.x, 1e-4);
            float  atten = saturate(1.0 - dist / range);
            atten *= atten;
            if (posType.w > 1.5) // spot cone
            {
                float c       = dot(-L, normalize(dirCos.xyz));
                float cosCone = dirCos.w;
                atten *= smoothstep(cosCone, lerp(cosCone, 1.0, 0.2), c);
            }
            if (atten <= 0.0) continue;
            // params.y = atlas base layer + 1 (0 = none), params.z = GI local
            // mask channel + 1 (0 = none; assigned with the window's exact
            // first-4 scan, so the light keeps the channel the mask rendered).
            float sh = localShadowFactor(posType, int(params.y) - 1, i.worldPos, N);
            int   mc = int(params.z) - 1;
            if (uGIParams.x > 0.5f && mc >= 0)
                sh = min(sh, uGILocal.SampleLevel(uGISampler, i.clip.xy / uViewport.xy, 0)[mc]);
            if (uPBR.w > 0.5f) sh = 1.0;
            result += BRDF(L, V, N, base, met, rough) * colInt.rgb * colInt.w * atten * sh;
        }
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
    // Debug: tint each fragment by its shadow cascade (red / green / blue /
    // yellow) so the cascade split placement is verifiable at a glance.
    // Mirrors GL and Metal (IRenderer::SetShadowDebug).
    if (uShadowEnabled.y != 0 && uShadowEnabled.x != 0)
    {
        const float3 tint[4] = { float3(1.0, 0.4, 0.4), float3(0.4, 1.0, 0.4),
                                 float3(0.4, 0.6, 1.0), float3(1.0, 1.0, 0.4) };
        result *= tint[min(dbgCascade, 3)];
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
    float4 uRotation;  // { angle(radians), pivotX, pivotY, unused }
};
Texture2D    uFontAtlas : register(t0);
SamplerState uSamp      : register(s0);
struct UIOut { float4 clip : SV_POSITION; float2 uv : TEXCOORD0; };
UIOut UIVSMain(uint vid : SV_VertexID)
{
    static const float2 c[4] = { float2(0,0), float2(1,0), float2(0,1), float2(1,1) };
    float2 uv = c[vid];
    float2 sp = uRect.xy + uv * uRect.zw;
    if (uRotation.x != 0.0f)
    {
        float sa = sin(uRotation.x), ca = cos(uRotation.x);
        float2 d = sp - uRotation.yz;
        sp = uRotation.yz + float2(d.x * ca - d.y * sa, d.x * sa + d.y * ca);
    }
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

    // The index range a DrawCall covers on the mesh it ends up drawing — the
    // D3D11 twin of GL's/Metal's DrawIndexRange. A whole-mesh draw (indexCount
    // 0 — every one-section mesh, every draw before sections existed) spans
    // the buffer; a section draw takes its own [offset, count), CLAMPED to the
    // buffer: the draw loops substitute the default cube when the real mesh is
    // not resident yet, and a range taken from the real asset must not read
    // past the cube's index buffer. `start` is in INDICES, which is what
    // DrawIndexed's StartIndexLocation takes (GL wants a byte pointer, Metal a
    // byte offset — this is the one place the three differ).
    struct D3D11IndexRange { UINT count; UINT start; };
    static inline D3D11IndexRange DrawIndexRange(const DrawCall& dc, UINT meshIndexCount)
    {
        if (dc.indexCount == 0) return { meshIndexCount, 0u };
        const UINT off = std::min<UINT>(dc.indexOffset, meshIndexCount);
        const UINT cnt = std::min<UINT>(dc.indexCount, meshIndexCount - off);
        return { cnt, off };
    }

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
        glm::mat4  cascadeVP[3];    // per-cascade light view-proj (D3D clip pre-applied)
        glm::vec4  cascadeSplits;   // xyz = planar view-space far distances, w = count
        glm::vec4  cameraFwd;       // xyz = world forward, w = 1 / shadow map size
        glm::vec4  shadowBias;      // x = slope-scaled factor, y = minimum
        glm::ivec4 shadowEnabled;   // x = 0/1, y = cascade debug tint
        glm::vec4  sunDir;   // xyz = sun direction
        glm::vec4  fog;      // x=fogDensity, y=fogHeightFalloff
        glm::vec4  viewport; // x=W, y=H, z=ssaoEnabled
        glm::vec4  giParams;     // x = GI enabled (0/1), y = indirect intensity
        glm::vec4  giGridOrigin; // xyz = probe grid origin, w = spacing
        glm::vec4  giGridCounts; // xyz = probe counts, w = probesPerRow
        glm::vec4  ssrParams;    // x = SSR gate, y = intensity, z = max roughness
        // Local (point/spot) shadow atlas: per-layer light view-proj (D3D clip
        // pre-applied), x = 1 / atlas size. Appended last — the cbuffer is
        // memcpy'd whole and the HLSL PerFrame block mirrors this order.
        glm::mat4  localShadowVP[16];
        glm::vec4  localShadowParams;
        // Clustered lighting: x/y/z = grid dims (0 = off), w = slice scale;
        // camFwd.xyz = depth axis, w = near (HE::ClusterLightBuild). Appended
        // last — the HLSL PerFrame block mirrors this order.
        glm::vec4  clusterParams;
        glm::vec4  clusterCamFwd;
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
    ComPtr<ID3D11ShaderResourceView> depthSRV; // C1: the same depth, readable (decals)
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
    // Clustered lighting (plan P7 on the forward path): three dynamic structured
    // buffers refilled per frame from HE::BuildClusterLights, bound at PS
    // t18/t19/t20 for the built-in scene shader. Sized once from the
    // LightPacking caps (256 lights × 4 float4, 3456 cells, 65536 indices).
    ComPtr<ID3D11Buffer>             clusterLightSB, clusterGridSB, clusterIdxSB;
    ComPtr<ID3D11ShaderResourceView> clusterLightSRV, clusterGridSRV, clusterIdxSRV;
    bool forwardClustered = true;  // HE_FORWARD_CLUSTER=0 → 8-light window A/B guard
    bool clusterCapWarned = false; // one log line when the caps drop lights
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
    // Compiled in regardless of HE_HAVE_SHADERC: without the cross-compiler the shaders
    // come from the pak's precompiled variants (MaterialShaderVariant), and a material
    // that has neither falls back to the built-in path. Canonical SPIRV-Cross HLSL register mapping
    // (shader_model=50, binding→register, verified for D3D12):
    //   b0 HeLighting(PS) | b1 U(VS) | b3 HeParams(PS) | b8/b9 HeLighting/HeParams(WPO VS)
    //   t2 heTex0, t4..t7 heTexP0..3 (+ SamplerState s2, s4..s7, linear-wrap).
    //   t14 heLandscapeWeights + SamplerState s0 (linear-CLAMP), per draw, see
    //   D3D11MaterialBindings.h — s0 is ALSO the built-in pass's albedo sampler
    //   and goes back to it after every material draw.
    HE::MaterialShaderLibrary m_matShaderLib; // unguarded member (like Vulkan/D3D12)
    struct MatShaders {
        ComPtr<ID3D11VertexShader> vs;
        ComPtr<ID3D11PixelShader>  ps;
        ComPtr<ID3D11InputLayout>  il;
    };
    std::unordered_map<uint64_t, MatShaders> m_materialShaders; // key = shader hash (opaque + blended share one entry)
    ComPtr<ID3D11Buffer>       m_matLightCB;  // HeLighting (full Lighting struct) — b0 PS / b8 WPO VS, filled once/frame
    ComPtr<ID3D11Buffer>       m_matObjCB;    // U (176 B)         — b1 VS,          filled per draw
    ComPtr<ID3D11Buffer>       m_matParamCB;  // HeParams (256 B)  — b3 PS / b9 WPO VS, filled per draw
    ComPtr<ID3D11SamplerState> m_matSampler;  // linear-wrap, bound at s2 + s4..s7
    ComPtr<ID3D11SamplerState> m_matWeightSampler; // linear-clamp, bound at s0 for the draw (heLandscapeWeights)
    bool m_matReady      = false; // true once createMaterialResources() succeeded
    bool m_matHlslLogged = false; // one-time dump of generated HLSL for HW verify
    // createMaterialResources() + GetOrBuildMaterialShaders() are defined inline below.

    // ── Screen-space decals (forward) ───────────────────────────────────────
    // docs/decals-cross-backend-plan.md §6b. D3D11 has no G-buffer, so a decal is
    // not composited into a base-colour attachment like on Metal/GL — it samples
    // the scene depth, rebuilds the world position, clips it against the projector
    // box and blends the SELF-SHADED result into the already-lit colour target.
    // Registers are pinned by the shader library into D3D11's bindable range:
    //   b13 HeDecal | t14/s14 heDecalTex | t15/s15 heGBDepth
    // (raw binding 23/19/22 would land past the 14 CB / 16 sampler slot limits).
    ComPtr<ID3D11VertexShader>       decalVS;
    ComPtr<ID3D11PixelShader>        decalPS;
    ComPtr<ID3D11Buffer>             decalCB;         // DecalUniforms, WRITE_DISCARD per decal
    ComPtr<ID3D11SamplerState>       decalTexSampler; // s14: linear wrap
    ComPtr<ID3D11SamplerState>       decalDepthSampler; // s15: POINT clamp — uv is texel-exact,
                                                        // and linear filtering of a depth format
                                                        // is optional hardware support
    ComPtr<ID3D11RasterizerState>    decalRast;       // cull FRONT: the camera may sit in the box
    ComPtr<ID3D11DepthStencilState>  decalNoDepth;    // no test, no write — the box decides
    ComPtr<ID3D11BlendState>         decalBlend;      // SrcAlpha/InvSrcAlpha, write mask RGB
    std::unordered_map<HE::UUID, ComPtr<ID3D11ShaderResourceView>> decalTexCache;
    bool decalReady  = false; // resources built
    bool decalFailed = false; // build failed once — never retried per frame

    // Node-graph project textures (MaterialAsset::graphTextureIds/Paths → heTexP0..3, the
    // Texture Sample nodes), keyed exactly like GL's ResolveGraphTexture: "hi:lo" for a
    // packed UUID, the path for a loose editor asset. Null is cached too (unloadable →
    // white default, no per-frame retry). InvalidateTexture drops the UUID key so an
    // edited texture re-uploads; a path-keyed loose asset is not hot-reloaded (same as GL).
    std::unordered_map<std::string, ComPtr<ID3D11ShaderResourceView>> graphTexCache;
    std::vector<HE::UUID> pendingTexInval;
    static std::string graphTexKey(const HE::UUID& id, const std::string& path)
    {
        return id != HE::UUID{} ? (std::to_string(id.hi) + ":" + std::to_string(id.lo)) : path;
    }

    // ── Cascaded shadow maps ────────────────────────────────────────────────
    // One R32 depth ARRAY, kCsmCascades slices (one per cascade), a DSV per
    // slice for the depth pass and one array SRV the scene shader picks a slice
    // from. Mirrors GL's GL_TEXTURE_2D_ARRAY / Metal's texture2d_array. The
    // cascade count MUST match the scene shader's uCascadeVP[3] and stay ≤
    // ShadowData::kMaxCascades; the extractor fits the project's count (1..3).
    static constexpr int kCsmCascades = 3;
    ComPtr<ID3D11VertexShader>       depthVS;    // depth-only pass
    ComPtr<ID3D11VertexShader>       depthVSInstanced; // same, one draw per same-mesh run (reads t3)
    ComPtr<ID3D11Texture2D>          shadowTex;
    ComPtr<ID3D11DepthStencilView>   shadowDSV[kCsmCascades];
    ComPtr<ID3D11ShaderResourceView> shadowSRV;
    ComPtr<ID3D11SamplerState>       shadowSampler;     // point clamp, s3 (PCF taps)
    ComPtr<ID3D11RasterizerState>    shadowRasterState; // depth bias for the caster pass
    int shadowSize = HE::kShadowMapResolution;
    // Project ShadowSettings (IRenderer::SetShadowSettings): distance /
    // cascade count / split lambda go to the extractor, the bias pair to the
    // scene shader, a resolution change re-creates the array at the top of
    // the next frame (shadowSizeDirty) — never from the setter, which may
    // land between passes.
    IRenderer::ShadowSettings shadowSettings;
    bool shadowSizeDirty = false;
    bool debugShadowCascades = false;
    // Per-cascade caster cull/sort scratch (NOT m_visible/m_sortedIndices —
    // those hold the camera cull the geometry pass consumes).
    std::vector<uint8_t>         shadowVisible;
    std::vector<uint32_t>        shadowSorted;
    RenderSorter::DepthBatchList shadowBatches;

    // ── Local (point/spot) shadow atlas ─────────────────────────────────────
    // Same depth-array pattern as the cascades, independent of the directional
    // light: ShadowData::kMaxLocalShadowLayers layers (spot = 1, point = 6
    // cube faces), a DSV per layer, one array SRV on t17. Fixed 1024² per
    // view like Metal/GL — not part of the project ShadowSettings.
    static constexpr int kLocalShadowLayers = ShadowData::kMaxLocalShadowLayers;
    static constexpr int kLocalShadowSize   = 1024;
    ComPtr<ID3D11Texture2D>          localShadowTex;
    ComPtr<ID3D11DepthStencilView>   localShadowDSV[kLocalShadowLayers];
    ComPtr<ID3D11ShaderResourceView> localShadowSRV;

    bool createLocalShadowArray()
    {
        localShadowSRV.Reset();
        for (auto& dsv : localShadowDSV) dsv.Reset();
        localShadowTex.Reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = static_cast<UINT>(kLocalShadowSize);
        sd.MipLevels = 1; sd.ArraySize = kLocalShadowLayers;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &localShadowTex)))
        {
            HE_LOG_ERROR(RHI, "D3D11Renderer: local shadow atlas %dx%dx%d creation failed",
                         kLocalShadowSize, kLocalShadowSize, kLocalShadowLayers);
            return false;
        }
        for (int v = 0; v < kLocalShadowLayers; ++v)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
            dvd.Format                         = DXGI_FORMAT_D32_FLOAT;
            dvd.ViewDimension                  = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dvd.Texture2DArray.FirstArraySlice = static_cast<UINT>(v);
            dvd.Texture2DArray.ArraySize       = 1;
            device->CreateDepthStencilView(localShadowTex.Get(), &dvd, &localShadowDSV[v]);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format                         = DXGI_FORMAT_R32_FLOAT;
        svd.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        svd.Texture2DArray.MipLevels       = 1;
        svd.Texture2DArray.FirstArraySlice = 0;
        svd.Texture2DArray.ArraySize       = kLocalShadowLayers;
        device->CreateShaderResourceView(localShadowTex.Get(), &svd, &localShadowSRV);
        return localShadowSRV != nullptr;
    }

    // (Re)creates the cascade depth array at `shadowSize` (initial build and
    // the resolution swap). R32_TYPELESS so the same texture is both a depth
    // target (D32_FLOAT DSV per slice) and an SRV (R32_FLOAT array view).
    // Returns false when the device refused it; the old views are dropped
    // first, so a failure leaves the shadows off (`shadows` gates on the SRV).
    bool createShadowArray()
    {
        shadowSRV.Reset();
        for (auto& dsv : shadowDSV) dsv.Reset();
        shadowTex.Reset();
        D3D11_TEXTURE2D_DESC sd{};
        sd.Width = sd.Height = static_cast<UINT>(shadowSize);
        sd.MipLevels = 1; sd.ArraySize = kCsmCascades;
        sd.Format = DXGI_FORMAT_R32_TYPELESS;
        sd.SampleDesc.Count = 1;
        sd.Usage = D3D11_USAGE_DEFAULT;
        sd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&sd, nullptr, &shadowTex)))
        {
            HE_LOG_ERROR(RHI, "D3D11Renderer: cascade shadow array %dx%dx%d creation failed",
                         shadowSize, shadowSize, kCsmCascades);
            return false;
        }
        for (int c = 0; c < kCsmCascades; ++c)
        {
            D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
            dvd.Format                         = DXGI_FORMAT_D32_FLOAT;
            dvd.ViewDimension                  = D3D11_DSV_DIMENSION_TEXTURE2DARRAY;
            dvd.Texture2DArray.FirstArraySlice = static_cast<UINT>(c);
            dvd.Texture2DArray.ArraySize       = 1;
            device->CreateDepthStencilView(shadowTex.Get(), &dvd, &shadowDSV[c]);
        }
        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format                         = DXGI_FORMAT_R32_FLOAT;
        svd.ViewDimension                  = D3D11_SRV_DIMENSION_TEXTURE2DARRAY;
        svd.Texture2DArray.MipLevels       = 1;
        svd.Texture2DArray.FirstArraySlice = 0;
        svd.Texture2DArray.ArraySize       = kCsmCascades;
        device->CreateShaderResourceView(shadowTex.Get(), &svd, &shadowSRV);
        return shadowSRV != nullptr;
    }

    // ── Viewport offscreen render target ────────────────────────────────────
    ComPtr<ID3D11Texture2D>          viewportTex;
    ComPtr<ID3D11RenderTargetView>   viewportRTV;
    ComPtr<ID3D11ShaderResourceView> viewportSRV;
    ComPtr<ID3D11Texture2D>          viewportDepth;
    ComPtr<ID3D11DepthStencilView>   viewportDSV;
    ComPtr<ID3D11ShaderResourceView> viewportDepthSRV; // C1, as depthSRV above
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
    // True when skyPS is the GL sky cross-compiled through he::shaderc (skyCB is
    // then a whole HE::SkyFrameParams); false on the kSkyPSHLSL fallback (SkyCB).
    bool skyFullModel = false;
    // ── Debug line pipeline ───────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>  debugVS;
    ComPtr<ID3D11PixelShader>   debugPS;
    ComPtr<ID3D11Buffer>        debugVB;
    ComPtr<ID3D11Buffer>        debugCB;
    ComPtr<ID3D11InputLayout>   debugIL;
    bool debugReady = false;
    std::vector<DebugLine> m_debugLines;

    // ── World preview (RenderWorldPreview) ───────────────────────────────
    // One target set per slot, as on GL/Metal: the asset tabs share slot 0,
    // each secondary Scene viewport has its own, because several of them draw
    // in the same frame and ImGui samples every one of them later. HDR colour
    // (the sky and a sun at 2.2 run past 1.0) resolved through the scene's
    // tonemap into the RGBA8 texture ImGui shows. The vertex side is the
    // scene's own (vs / skinnedVS); only the two small pixel shaders and the
    // light cbuffer are the preview's.
    struct WorldPreviewTarget
    {
        ComPtr<ID3D11Texture2D>          hdrTex, ldrTex, depthTex;
        ComPtr<ID3D11RenderTargetView>   hdrRTV, ldrRTV;
        ComPtr<ID3D11ShaderResourceView> hdrSRV, ldrSRV;
        ComPtr<ID3D11DepthStencilView>   dsv;
        int w = 0, h = 0;
    };
    WorldPreviewTarget worldPreview[IRenderer::kWorldPreviewSlots];
    ComPtr<ID3D11PixelShader> previewMeshPS, previewSkinnedPS;
    ComPtr<ID3D11Buffer>      previewLightCB;
    bool previewReady  = false;
    bool previewFailed = false; // compile failed once — not retried per call

    // ── Motion trails (RenderWorld::ribbonBatches) ────────────────────────
    // A ribbon needs neither a shader nor a pass of its own: it arrives as CPU
    // geometry in the ordinary cooked layout (pos3 + norm3 + uv2), so it goes
    // into a dynamic buffer pair and is then handed to drawDC as a perfectly
    // normal blended DrawCall — same PBR path, same graph-material shaders, same
    // sort (docs/rope-trail-plan.md §6.2). One pool slot per band; MAP_WRITE_DISCARD
    // does the versioning, so re-uploading a slot mid-frame is safe.
    std::vector<ComPtr<ID3D11Buffer>> ribbonVB;
    std::vector<ComPtr<ID3D11Buffer>> ribbonIB;
    // Fill `out` with the pool slot's buffers after uploading the band into it.
    // False = the buffers could not be created; the caller skips the band.
    bool uploadRibbon(ID3D11DeviceContext* ctx, size_t index,
                      const std::vector<float>& verts,
                      const std::vector<uint32_t>& indices, GpuMesh& out)
    {
        if (!ctx || verts.empty() || indices.empty()) return false;
        if (ribbonVB.size() <= index) { ribbonVB.resize(index + 1); ribbonIB.resize(index + 1); }

        auto ensure = [&](UINT needed, UINT bindFlag, ComPtr<ID3D11Buffer>& buf) -> bool {
            D3D11_BUFFER_DESC existing{};
            if (buf) buf->GetDesc(&existing);
            if (buf && existing.ByteWidth >= needed) return true;
            buf.Reset();
            D3D11_BUFFER_DESC bd{};
            // Round up so a trail that grows a point per frame does not reallocate
            // every frame while it fills up.
            bd.ByteWidth      = (needed + 0xFFFu) & ~0xFFFu;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = bindFlag;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&bd, nullptr, &buf);
            return buf != nullptr;
        };
        const UINT vbytes = static_cast<UINT>(verts.size()   * sizeof(float));
        const UINT ibytes = static_cast<UINT>(indices.size() * sizeof(uint32_t));
        if (!ensure(vbytes, D3D11_BIND_VERTEX_BUFFER, ribbonVB[index])) return false;
        if (!ensure(ibytes, D3D11_BIND_INDEX_BUFFER,  ribbonIB[index])) return false;

        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(ctx->Map(ribbonVB[index].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        std::memcpy(m.pData, verts.data(), vbytes);
        ctx->Unmap(ribbonVB[index].Get(), 0);
        if (FAILED(ctx->Map(ribbonIB[index].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return false;
        std::memcpy(m.pData, indices.data(), ibytes);
        ctx->Unmap(ribbonIB[index].Get(), 0);

        out = GpuMesh{};
        out.vbuf       = ribbonVB[index];
        out.ibuf       = ribbonIB[index];
        out.indexCount = static_cast<UINT>(indices.size());
        return true;
    }

    float m_wallTime = 0.0f;
    float bloomStrength  = 0.25f;
    float bloomThreshold = 1.0f;
    float bloomKnee      = 0.1f;
    bool  bloomEnabled   = true;
    // Anti-aliasing method in force, already resolved against this backend's
    // capabilities (docs/anti-aliasing-plan.md). Off runs aaBlitPS instead of
    // fxaaPS — the pass itself always draws, it is what fills viewportRTV.
    HE::AAMethod aaMethod = HE::AAMethod::FXAA;
    // Post-resolve sharpen of the temporal mode (AntiAliasingSettings::sharpness).
    float aaSharpness = 0.35f;

    // ── Temporal AA (docs/anti-aliasing-plan.md A2/A3) ───────────────────────
    // GL's RenderVelocity/RenderTaa, one for one: a positions-only velocity pass
    // over the opaque objects (depth-tested LESS_EQUAL against the scene depth,
    // no write), a resolve on the tonemapped LDR image into a ping-pong RGBA8
    // history, and the sharpen in the AA-resolve slot. All three shaders are
    // optional: a compile failure leaves them null, GetCapabilities then reports
    // no temporal AA and ResolveAAMethod falls back to SMAA.
    ComPtr<ID3D11VertexShader>       taaVelocityVS;
    ComPtr<ID3D11PixelShader>        taaVelocityPS, taaResolvePS, taaSharpenPS;
    ComPtr<ID3D11Buffer>             taaVelocityCB;     // HE::TaaVelocityConstants
    ComPtr<ID3D11DepthStencilState>  taaVelocityDSS;    // LESS_EQUAL, no depth write
    ComPtr<ID3D11Texture2D>          taaVelocityTex;    // RG16F, uvNow - uvPrev
    ComPtr<ID3D11RenderTargetView>   taaVelocityRTV;
    ComPtr<ID3D11ShaderResourceView> taaVelocitySRV;
    ComPtr<ID3D11Texture2D>          taaHistoryTex[2];  // RGBA8, ping-pong
    ComPtr<ID3D11RenderTargetView>   taaHistoryRTV[2];
    ComPtr<ID3D11ShaderResourceView> taaHistorySRV[2];
    uint32_t  taaW = 0, taaH = 0;
    int       taaHistoryCur   = 0;
    bool      taaHistoryValid = false;
    uint32_t  taaFrameIndex   = 0;
    glm::vec2 taaJitter{ 0.0f };
    // Set by the caller of DrawScene: true only when this frame runs the post
    // chain that resolves the jitter (DrawViewportFrame's HDR path). The
    // swapchain path has no LDR image and no AA slot — jittering it would shake
    // the image by a subpixel every frame with nothing to average it out.
    bool      taaFrame = false;
    glm::mat4 taaPrevViewProj{ 1.0f };
    std::unordered_map<uint32_t, glm::mat4> taaPrevTransforms, taaCurTransforms;

    bool taaReady() const
    {
        return taaVelocityVS && taaVelocityPS && taaResolvePS && taaSharpenPS
            && taaVelocityCB && taaVelocityDSS;
    }

    void destroyTaaTargets()
    {
        taaVelocityRTV.Reset(); taaVelocitySRV.Reset(); taaVelocityTex.Reset();
        for (int i = 0; i < 2; ++i)
        { taaHistoryRTV[i].Reset(); taaHistorySRV[i].Reset(); taaHistoryTex[i].Reset(); }
        taaW = taaH = 0;
        taaHistoryValid = false;
        taaPrevTransforms.clear();
        taaCurTransforms.clear();
    }

    // Targets follow the scene size. A fresh history is garbage, not history:
    // the first frame after a (re)create shows the current frame only.
    bool ensureTaaTargets(uint32_t w, uint32_t h)
    {
        w = std::max(1u, w); h = std::max(1u, h);
        if (taaHistoryTex[0] && taaW == w && taaH == h) return true;
        destroyTaaTargets();
        auto makeRT = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& t,
                          ComPtr<ID3D11RenderTargetView>& rtv,
                          ComPtr<ID3D11ShaderResourceView>& srv) -> bool
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = w; td.Height = h;
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt; td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&td, nullptr, &t))) return false;
            device->CreateRenderTargetView(t.Get(), nullptr, &rtv);
            device->CreateShaderResourceView(t.Get(), nullptr, &srv);
            return rtv && srv;
        };
        const bool ok = makeRT(DXGI_FORMAT_R16G16_FLOAT, taaVelocityTex, taaVelocityRTV, taaVelocitySRV)
                     && makeRT(DXGI_FORMAT_R8G8B8A8_UNORM, taaHistoryTex[0], taaHistoryRTV[0], taaHistorySRV[0])
                     && makeRT(DXGI_FORMAT_R8G8B8A8_UNORM, taaHistoryTex[1], taaHistoryRTV[1], taaHistorySRV[1]);
        if (!ok)
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: TAA targets could not be created");
            destroyTaaTargets();
            return false;
        }
        taaW = w; taaH = h;
        taaHistoryCur   = 0;
        taaHistoryValid = false;
        return true;
    }

    // Screen-space motion of the opaque geometry: for every visible object,
    // where its vertices are now vs. where they were last frame (camera AND
    // object motion). Runs right after the opaque + skinned draws, against the
    // depth they left in the bound DSV, so only visible surfaces report; sky,
    // skinned meshes and the blended tail keep the clear's zero, exactly as on
    // GL. `vpJit` must be the scene draws' own matrix — the LESS_EQUAL test only
    // holds if the velocity raster reproduces their depth, so the product is
    // formed in the same order (vpJit * transform). Restores the target, depth
    // state, shaders and b0 the scene pass had.
    void renderTaaVelocity(ID3D11DeviceContext* ctx, const glm::mat4& vpClean,
                           const glm::mat4& vpJit, ContentManager* cm)
    {
        if (!taaVelocityRTV) return;
        ComPtr<ID3D11RenderTargetView> savedRTV;
        ComPtr<ID3D11DepthStencilView> savedDSV;
        ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());
        if (!savedDSV) return;

        // Cleared to zero ("did not move") by DrawViewportFrame, before DrawScene
        // — which returns early on an empty scene, and the resolve must not
        // then read last frame's motion.
        ID3D11RenderTargetView* velRTV = taaVelocityRTV.Get();
        ctx->OMSetRenderTargets(1, &velRTV, savedDSV.Get());
        ctx->OMSetDepthStencilState(taaVelocityDSS.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
        ctx->IASetInputLayout(inputLayout.Get());
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(taaVelocityVS.Get(), nullptr, 0);
        ctx->PSSetShader(taaVelocityPS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, taaVelocityCB.GetAddressOf());

        const UINT stride = 8 * sizeof(float);
        const UINT offset = 0;
        taaCurTransforms.clear();
        for (const uint32_t idx : m_sortedIndices)
        {
            const RenderObject& obj = m_renderWorld.objects[idx];
            const GpuMesh* mesh = resolveMesh(obj.meshAssetId, cm);
            if (!mesh || !mesh->vbuf || !mesh->ibuf || mesh->indexCount == 0) continue;

            // An object seen for the first time reports no motion — its
            // "previous" position is where it is now.
            const auto it = taaPrevTransforms.find(obj.entityId);
            const glm::mat4 prevModel = (it != taaPrevTransforms.end()) ? it->second : obj.transform;
            taaCurTransforms[obj.entityId] = obj.transform;

            HE::TaaVelocityConstants c;
            c.mvpJitter = vpJit * obj.transform;
            c.mvpNow    = vpClean * obj.transform;
            c.mvpPrev   = taaPrevViewProj * prevModel;
            D3D11_MAPPED_SUBRESOURCE m{};
            if (FAILED(ctx->Map(taaVelocityCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) continue;
            std::memcpy(m.pData, &c, sizeof(c));
            ctx->Unmap(taaVelocityCB.Get(), 0);
            ctx->IASetVertexBuffers(0, 1, mesh->vbuf.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(mesh->ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(mesh->indexCount, 0, 0);
        }

        // Advance the history HERE, at the end of the one pass that consumed
        // it, so a frame is never compared against itself.
        taaPrevViewProj = vpClean;
        taaPrevTransforms.swap(taaCurTransforms);

        ID3D11RenderTargetView* rtv = savedRTV.Get();
        ctx->OMSetRenderTargets(1, &rtv, savedDSV.Get());
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(0, 1, perObjectCB.GetAddressOf());
    }

    // ── SSAO pipeline ──────────────────────────────────────────────────────
    // Position prepass
    ComPtr<ID3D11VertexShader>       ssaoPosVS;
    ComPtr<ID3D11VertexShader>       ssaoPosVSInstanced; // VSPosInstanced: {mvp, modelView} per instance at t3
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
    // ── Reflection MRT pre-pass (forward SSR, plan §3.2 way (a) / C2) ───────
    // Two extra attachments hung off the SSAO position pre-pass, so the trace
    // sees exactly the format it would read out of GB1/GBDepth in a deferred
    // path. Attachment 0 stays the view-space position the occlusion stage
    // already consumes — the shared pre-pass fragment writes the identical
    // vec4(vViewPos, 1.0) there, so pass 2 never notices which shader drew it.
    // Created lazily on the first SSR frame, torn down with the SSAO targets.
    ComPtr<ID3D11Texture2D>          reflAttrTex;  // RGBA16F: rg oct normal, b roughness (= 0)
    ComPtr<ID3D11RenderTargetView>   reflAttrRTV;
    ComPtr<ID3D11ShaderResourceView> reflAttrSRV;
    ComPtr<ID3D11Texture2D>          reflNdcTex;   // R32F: NDC depth, gbufferMain convention
    ComPtr<ID3D11RenderTargetView>   reflNdcRTV;
    ComPtr<ID3D11ShaderResourceView> reflNdcSRV;
    ComPtr<ID3D11VertexShader>       reflPrepassVS;
    ComPtr<ID3D11PixelShader>        reflPrepassPS;
    ComPtr<ID3D11InputLayout>        reflPrepassIL; // TEXCOORD0/1 — NOT the scene's POSITION/NORMAL
    ComPtr<ID3D11Buffer>             reflPrepassCB; // U { mat4 mvp, modelView, model } = 192 B
    bool reflPrepassReady  = false;
    bool reflPrepassFailed = false;
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

    // ── Screen-space reflections, forward path (plan checkpoint C) ──────────
    // The D3D11 twin of OpenGLRenderer::RenderForwardSSR and Metal's
    // EncodeForwardSSR, built from the SAME shared shaders (ssrTrace / ssrBlur),
    // pinned into D3D11's bindable register range by kSSRHlslPins:
    //   b12 HeSSRTrace/HeSSRBlur | t8/s8 colour-or-input | t9/s9 GB1
    //   t11/s11 depth | t12/s12 hist radiance | t13/s13 hist position
    // There is no composite pass (§2.2: no G-buffer here) and no wide/rough-mix
    // stage (the forward path has none on Metal or GL either, and the pre-pass
    // writes roughness = 0) — the scene shader samples ONE reflection texture.
    ComPtr<ID3D11PixelShader>        ssrTracePS;
    ComPtr<ID3D11PixelShader>        ssrBlurPS;
    ComPtr<ID3D11Buffer>             ssrTraceCB;   // SSRTraceUniforms (336 B) → b12
    ComPtr<ID3D11Buffer>             ssrBlurCB;    // SSRBlurUniforms  (16 B)  → b12
    ComPtr<ID3D11SamplerState>       ssrPointClamp;  // GB1/depth/history position
    ComPtr<ID3D11SamplerState>       ssrLinearClamp; // scene colour, blur input
    // Half-res ping-pong. hist[2] is the temporal pair the trace reads and
    // writes (radiance + receiver world position, two attachments in one draw);
    // ping/refl are the separable blur chain's two ends.
    ComPtr<ID3D11Texture2D>          ssrHistRadTex[2], ssrHistPosTex[2];
    ComPtr<ID3D11RenderTargetView>   ssrHistRadRTV[2], ssrHistPosRTV[2];
    ComPtr<ID3D11ShaderResourceView> ssrHistRadSRV[2], ssrHistPosSRV[2];
    ComPtr<ID3D11Texture2D>          ssrPingTex, ssrReflTex;
    ComPtr<ID3D11RenderTargetView>   ssrPingRTV, ssrReflRTV;
    ComPtr<ID3D11ShaderResourceView> ssrPingSRV, ssrReflSRV;
    // Full-res copy of the PREVIOUS frame's finished HDR colour — the forward
    // path's radiance source (one frame of content lag, Option A of
    // docs/ssr-plan.md §2). Lives with the HDR target so a resize can never
    // leave the two disagreeing.
    ComPtr<ID3D11Texture2D>          ssrColorHistTex;
    ComPtr<ID3D11ShaderResourceView> ssrColorHistSRV;
    bool      ssrColorHistValid = false;
    int       ssrW = 0, ssrH = 0;
    int       ssrHistIdx   = 0;
    bool      ssrHistValid = false;
    float     ssrFrameSeed = 0.0f;
    glm::mat4 ssrPrevViewProj{1.0f};
    bool  ssrReady        = false;
    bool  ssrFailed       = false;
    bool  ssrEnabled      = false;
    float ssrIntensity    = 1.0f;
    float ssrMaxRoughness = 0.6f;
    float ssrMaxDistance  = 30.0f;
    float ssrThickness    = 0.5f;
    int   ssrQuality      = 1;

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
    // Scissor is a RASTERIZER property in D3D11, so clipping needs a state of
    // its own (cull none like every other 2D pass, scissor on). Bound only while
    // a clipped run is being drawn; the pass restores whatever was set before.
    ComPtr<ID3D11RasterizerState>   uiScissorRast;
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
    // in a small ring so a slot is only read back HE::kGpuTimerRing frames after
    // it was issued — GetData(flags=0) at that age never blocks in practice, and
    // a not-yet-ready slot is dropped rather than stalling the pipeline. Queries
    // are only issued while the profiler is recording / live (never on the hot
    // path otherwise, mirroring the GL backend). The ring DEPTH is shared with
    // GL/Vulkan; the payload below is D3D11-specific (a DISJOINT query per slot,
    // which nobody else has — see gpuTimerReap's disjoint-frame rejection).
    struct GpuTimerSlot
    {
        ComPtr<ID3D11Query> disjoint, tsStart, tsEnd;
        bool pending = false; // issued, result not consumed yet
    };
    GpuTimerSlot gpuSlots[HE::kGpuTimerRing];
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

        const int idx = static_cast<int>(gpuFrameIdx % HE::kGpuTimerRing);
        GpuTimerSlot& slot = gpuSlots[idx];
        gpuTimerReap(slot, /*block=*/false); // issued HE::kGpuTimerRing frames ago
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

        // Forward SSR's radiance source: a full-res copy of the PREVIOUS frame's
        // hdrTex. Built here, with the target it copies, so the two can never
        // disagree about size or format after a resize — and the "valid" flag
        // drops with them, because the first frame at a new size has no history.
        ssrColorHistSRV.Reset(); ssrColorHistTex.Reset();
        ssrColorHistValid = false;
        if (hdrTex)
        {
            D3D11_TEXTURE2D_DESC td{};
            hdrTex->GetDesc(&td);
            td.BindFlags = D3D11_BIND_SHADER_RESOURCE; // CopyResource target, never an RTV
            if (SUCCEEDED(device->CreateTexture2D(&td, nullptr, &ssrColorHistTex)))
                device->CreateShaderResourceView(ssrColorHistTex.Get(), nullptr, &ssrColorHistSRV);
        }
        destroySSRTargets(); // half-res ping-pong follows the new size lazily
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
            // Instanced twin for GeometryPass batches. Optional: without it every
            // batch loops through VSPos, so a failure is logged, not returned.
            ComPtr<ID3DBlob> ivsBlob;
            if (SUCCEEDED(D3DCompile(kSSAOPosHLSL, strlen(kSSAOPosHLSL), nullptr, nullptr, nullptr,
                                     "VSPosInstanced", "vs_5_0", 0, 0, &ivsBlob, &err)))
                device->CreateVertexShader(ivsBlob->GetBufferPointer(), ivsBlob->GetBufferSize(),
                                           nullptr, &ssaoPosVSInstanced);
            else
                HE_LOG_ERROR(RHI, "%s", (std::string("D3D11Renderer: VSPosInstanced compile failed: ")
                    + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
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
        // The reflection attachments share this pre-pass's size and depth, so
        // they die with it and are rebuilt by the next SSR frame (C2).
        reflAttrRTV.Reset(); reflAttrSRV.Reset(); reflAttrTex.Reset();
        reflNdcRTV.Reset();  reflNdcSRV.Reset();  reflNdcTex.Reset();

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

    // The two extra pre-pass attachments (plan C2). Separate from
    // createSSAOTargets because a project that never switches SSR on must not
    // pay for them, and because they must be rebuilt whenever the SSAO pass is
    // resized — createSSAOTargets drops them, this puts them back.
    bool ensureReflTargets(int w, int h)
    {
        if (reflAttrSRV && reflNdcSRV) return true;
        reflAttrRTV.Reset(); reflAttrSRV.Reset(); reflAttrTex.Reset();
        reflNdcRTV.Reset();  reflNdcSRV.Reset();  reflNdcTex.Reset();
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
        // RGBA16F because the oct normal needs the precision (the trace decodes
        // it), R32F because the NDC depth is compared against ray-marched depths
        // — an 8-bit or half copy of either shows as reflections drifting.
        const bool ok = makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, reflAttrTex, reflAttrRTV, reflAttrSRV)
                      & makeRT(DXGI_FORMAT_R32_FLOAT,          reflNdcTex,  reflNdcRTV,  reflNdcSRV);
        if (!ok)
        {
            reflAttrRTV.Reset(); reflAttrSRV.Reset(); reflAttrTex.Reset();
            reflNdcRTV.Reset();  reflNdcSRV.Reset();  reflNdcTex.Reset();
            HE_LOG_WARN(RHI, "%s", "D3D11Renderer: reflection pre-pass targets failed");
        }
        return ok;
    }

    // Returns the SRV that the scene shader should bind as t2 (AO texture).
    // One instanced draw for a depth-only batch (a shadow caster run, an SSAO or
    // GI pre-pass batch): `count` {A, B} matrix pairs from fill(k, pair) go into
    // instanceSB — the scene pass's 128-byte buffer — and instVS reads them at
    // t3 by SV_InstanceID. Every caller writes the products its per-object loop
    // puts into a constant buffer, so the instanced frame is the loop's frame.
    // Afterwards restoreVS is bound again and t3 is unbound (instanceSB is
    // re-Mapped by the next batch). false = nothing drawn (no twin shader, no
    // buffer, too many instances, HE_DEPTH_INSTANCING=0): the caller loops.
    template <class Fill>
    bool drawDepthInstanced(ID3D11DeviceContext* ctx, ID3D11VertexShader* instVS,
                            ID3D11VertexShader* restoreVS, UINT indexCount, UINT startIndex,
                            UINT count, Fill&& fill)
    {
        static_assert(k_instStride == 2 * sizeof(glm::mat4), "instance stride must be two mat4");
        if (!instVS || !instanceSB || !instanceSRV || count < 2 || count > k_maxInstances
            || !RenderSorter::depthInstancingEnabled())
            return false;
        D3D11_MAPPED_SUBRESOURCE im{};
        if (FAILED(ctx->Map(instanceSB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &im))) return false;
        auto* dst = static_cast<uint8_t*>(im.pData);
        for (UINT k = 0; k < count; ++k)
        {
            glm::mat4 pair[2];
            fill(k, pair);
            std::memcpy(dst + static_cast<size_t>(k) * k_instStride, pair, sizeof(pair));
        }
        ctx->Unmap(instanceSB.Get(), 0);
        ctx->VSSetShader(instVS, nullptr, 0);
        ctx->VSSetShaderResources(3, 1, instanceSRV.GetAddressOf());
        ctx->DrawIndexedInstanced(indexCount, count, startIndex, 0, 0);
        ctx->VSSetShader(restoreVS, nullptr, 0);
        ID3D11ShaderResourceView* nullSRV = nullptr;
        ctx->VSSetShaderResources(3, 1, &nullSRV);
        return true;
    }

    ID3D11ShaderResourceView* runSSAO(ID3D11DeviceContext* ctx,
                                      const std::vector<const DrawCall*>& opaqueDCs,
                                      const glm::mat4& viewProj, const glm::mat4& view,
                                      const glm::mat4& proj,
                                      int w, int h,
                                      const std::function<const GpuMesh*(HE::UUID)>& resolveMeshFn,
                                      const GpuMesh& fallbackMesh,
                                      ID3D11InputLayout* il,
                                      ID3D11DepthStencilState* depthSt,
                                      ID3D11RasterizerState* rasterSt,
                                      bool reflMrt = false, bool aoWanted = true)
    {
        // `reflMrt` and `aoWanted` are SEPARATE gates (plan C2, the same shape
        // GL and Vulkan grew). SSR needs pass 1 on frames where SSAO is off or
        // the GI probes replaced it, and it needs it with three attachments
        // instead of one; the occlusion and blur stages then simply do not run,
        // and the caller keeps its white AO fallback.
        if (!ssaoReady || !ssaoPosRTV || !ssaoRTV || !ssaoBlurRTV) return whiteSRV.Get();
        if (ssaoW != w || ssaoH != h) createSSAOTargets(w, h);
        if (reflMrt && !(reflPrepassReady && ensureReflTargets(w, h))) reflMrt = false;
        if (!reflMrt && !aoWanted) return whiteSRV.Get();

        const UINT stride = 8 * sizeof(float), off = 0;
        D3D11_VIEWPORT vp{}; vp.Width = float(w); vp.Height = float(h); vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        // ── Pass 1: Position prepass ──────────────────────────────────────────
        {
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(2, 1, &nullSrv);
            float clear[4] = {0,0,0,0};
            if (reflMrt)
            {
                // Attachment 0 is still ssaoPosRTV and the shared pre-pass
                // fragment writes exactly what ssao_pos.frag writes into it, so
                // pass 2 below cannot tell which shader ran.
                ID3D11RenderTargetView* rtvs[3] = { ssaoPosRTV.Get(), reflAttrRTV.Get(),
                                                    reflNdcRTV.Get() };
                ctx->OMSetRenderTargets(3, rtvs, ssaoPosDepthDSV.Get());
                for (ID3D11RenderTargetView* r : rtvs) ctx->ClearRenderTargetView(r, clear);
            }
            else
            {
                ctx->OMSetRenderTargets(1, ssaoPosRTV.GetAddressOf(), ssaoPosDepthDSV.Get());
                ctx->ClearRenderTargetView(ssaoPosRTV.Get(), clear);
            }
            ctx->ClearDepthStencilView(ssaoPosDepthDSV.Get(), D3D11_CLEAR_DEPTH, 1.0f, 0);
            // The shared pre-pass VS is cross-compiled GLSL, so its inputs are
            // TEXCOORD0/1 and it needs its own input layout — the same split the
            // graph-material path makes for the same reason.
            ctx->IASetInputLayout(reflMrt ? reflPrepassIL.Get() : il);
            ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ctx->VSSetShader(reflMrt ? reflPrepassVS.Get() : ssaoPosVS.Get(), nullptr, 0);
            ctx->PSSetShader(reflMrt ? reflPrepassPS.Get() : ssaoPosPS.Get(), nullptr, 0);
            ctx->OMSetDepthStencilState(depthSt, 0);
            ctx->RSSetState(rasterSt);
            ID3D11Buffer* posCB = reflMrt ? reflPrepassCB.Get() : ssaoPosPerObjCB.Get();
            // b1: the pre-pass's U block (binding 1, unpinned). VS b1 is the
            // scene's perFrameCB, re-bound by the scene setup block after SSAO.
            if (reflMrt) ctx->VSSetConstantBuffers(1, 1, &posCB);
            else         ctx->VSSetConstantBuffers(0, 1, &posCB);

            for (const DrawCall* dc : opaqueDCs) {
                const GpuMesh* mesh = resolveMeshFn(dc->meshAssetId);
                const GpuMesh& m = mesh ? *mesh : fallbackMesh;
                if (!m.vbuf || !m.ibuf) continue;
                ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &off);
                ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
                // Section draw → its own slice of the index buffer; whole-mesh
                // draw → all of it. The list carries one DC per slot, so the
                // slices together cover the mesh exactly once.
                const D3D11IndexRange range = DrawIndexRange(*dc, m.indexCount);

                auto drawWithTransform = [&](const glm::mat4& modelMat) {
                    // Three matrices for the MRT pre-pass (it also needs `model`
                    // for the world normal), two for the position-only one; the
                    // first two are the same bytes in both layouts.
                    struct { glm::mat4 mvp, modelView, model; } pcb;
                    pcb.mvp       = viewProj * modelMat;
                    pcb.modelView = view     * modelMat;
                    pcb.model     = modelMat;
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    if (SUCCEEDED(ctx->Map(posCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
                        std::memcpy(mapped.pData, &pcb,
                                    reflMrt ? sizeof(pcb) : sizeof(glm::mat4) * 2);
                        ctx->Unmap(posCB, 0);
                    }
                    ctx->DrawIndexed(range.count, range.start, 0);
                };

                // A GeometryPass batch (instanceTransforms non-empty ⇔ run > 1) is
                // one instanced draw on the position-only path, with the loop's
                // {mvp, modelView} per instance. The MRT reflection pre-pass
                // keeps the loop: its VS is the library's cross-compiled GLSL,
                // whose instanced variant reads the model as vertex attributes,
                // not from a structured buffer.
                const std::vector<glm::mat4>& inst = dc->instanceTransforms;
                if (!reflMrt && drawDepthInstanced(ctx, ssaoPosVSInstanced.Get(), ssaoPosVS.Get(),
                        range.count, range.start, static_cast<UINT>(inst.size()),
                        [&](UINT k, glm::mat4* pair) {
                            pair[0] = viewProj * inst[k];
                            pair[1] = view     * inst[k];
                        }))
                {
                    static bool loggedOnce = false; // the runtime witness on Windows
                    if (!loggedOnce)
                    {
                        loggedOnce = true;
                        HE_LOG_INFO(RHI, "D3D11Renderer: SSAO pre-pass instanced (first batch: %u instances)",
                                    static_cast<unsigned>(inst.size()));
                    }
                    continue;
                }
                if (!inst.empty())
                    for (const glm::mat4& t : inst) drawWithTransform(t);
                else
                    drawWithTransform(dc->transform);
            }
        }

        // Unbind the pre-pass RTVs so they can be read as SRVs. One null at slot
        // 0 clears slots 1..7 too, so this covers the MRT case as well.
        { ID3D11RenderTargetView* n = nullptr; ctx->OMSetRenderTargets(1, &n, nullptr); }

        // SSR-only frame: the occlusion and blur stages have nothing to do, and
        // the caller's `aoSRV != whiteSRV` check must keep reporting "no AO".
        if (!aoWanted) return whiteSRV.Get();

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

    // ─── Reflection pre-pass pipeline (forward SSR, plan C2) ────────────────
    // One shader for every backend (MaterialShaderLibrary reflPrepassVertex /
    // reflPrepassFragment) so the encoding the trace decodes cannot drift.
    // Built lazily on the first SSR frame; a failure is remembered and SSR then
    // simply never runs.
    bool EnsureReflPrepassPipeline()
    {
#if !defined(HE_HAVE_SHADERC)
        return false;
#else
        if (reflPrepassReady)  return true;
        if (reflPrepassFailed) return false;
        reflPrepassFailed = true; // cleared only on full success below

        using Backend = HE::MaterialShaderLibrary::Backend;
        const HE::MaterialShaderLibrary::Compiled& vc = m_matShaderLib.reflPrepassVertex(Backend::HLSL);
        const HE::MaterialShaderLibrary::Compiled& fc = m_matShaderLib.reflPrepassFragment(Backend::HLSL);
        if (!vc.ok || !fc.ok || vc.source.empty() || fc.source.empty())
        {
            HE_LOG_WARN(RHI, "%s", "D3D11Renderer: reflection pre-pass cross-compile failed");
            return false;
        }
        ComPtr<ID3DBlob> vsb, psb, err;
        // SPIRV-Cross emits the GLSL-sourced entry point as `main`.
        if (FAILED(D3DCompile(vc.source.c_str(), vc.source.size(), "reflPrepassVS", nullptr, nullptr,
                              "main", "vs_5_0", 0, 0, &vsb, &err)))
        {
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: refl pre-pass VS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(fc.source.c_str(), fc.source.size(), "reflPrepassPS", nullptr, nullptr,
                              "main", "ps_5_0", 0, 0, &psb, &err)))
        {
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: refl pre-pass PS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &reflPrepassVS)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &reflPrepassPS)))
            return false;
        // TEXCOORD0/1, not POSITION/NORMAL: the cross-compiler names GLSL vertex
        // inputs by location (see GetOrBuildMaterialShaders). Same interleaved
        // 32-byte pos/normal/uv buffer the scene meshes use; the UV is unread.
        const D3D11_INPUT_ELEMENT_DESC layout[] = {
            { "TEXCOORD", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0,  0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
            { "TEXCOORD", 1, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        };
        if (FAILED(device->CreateInputLayout(layout, 2, vsb->GetBufferPointer(), vsb->GetBufferSize(),
                                             &reflPrepassIL)))
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: refl pre-pass input layout creation failed");
            return false;
        }
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = (static_cast<UINT>(sizeof(HE::MaterialShaderLibrary::ReflPrepassUniforms)) + 15u) & ~15u;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &reflPrepassCB))) return false;
        }
        reflPrepassFailed = false;
        reflPrepassReady  = true;
        return true;
#endif
    }

    // ─── Forward screen-space reflections (plan C1/C3/C4) ───────────────────
    bool EnsureSSRPipelines()
    {
#if !defined(HE_HAVE_SHADERC)
        return false;
#else
        if (ssrReady)  return true;
        if (ssrFailed) return false;
        ssrFailed = true;
        if (!fsVS) return false; // the fullscreen VS lives with the PostFX resources

        using Backend = HE::MaterialShaderLibrary::Backend;
        const HE::MaterialShaderLibrary::Compiled& tc = m_matShaderLib.ssrTrace(Backend::HLSL);
        const HE::MaterialShaderLibrary::Compiled& bc = m_matShaderLib.ssrBlur(Backend::HLSL);
        if (!tc.ok || !bc.ok || tc.source.empty() || bc.source.empty())
        {
            HE_LOG_WARN(RHI, "%s", "D3D11Renderer: SSR shader cross-compile failed");
            return false;
        }
        auto makePS = [&](const std::string& src, const char* name,
                          ComPtr<ID3D11PixelShader>& out) -> bool {
            ComPtr<ID3DBlob> blob, err;
            if (FAILED(D3DCompile(src.c_str(), src.size(), name, nullptr, nullptr,
                                  "main", "ps_5_0", 0, 0, &blob, &err)))
            {
                HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: SSR ") + name
                    + " compile failed: "
                    + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
                return false;
            }
            return SUCCEEDED(device->CreatePixelShader(blob->GetBufferPointer(),
                                                       blob->GetBufferSize(), nullptr, &out));
        };
        if (!makePS(tc.source, "ssrTracePS", ssrTracePS)) return false;
        if (!makePS(bc.source, "ssrBlurPS",  ssrBlurPS))  return false;

        auto makeCB = [&](size_t bytes, ComPtr<ID3D11Buffer>& out) -> bool {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = (static_cast<UINT>(bytes) + 15u) & ~15u;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            return SUCCEEDED(device->CreateBuffer(&bd, nullptr, &out));
        };
        if (!makeCB(sizeof(HE::MaterialShaderLibrary::SSRTraceUniforms), ssrTraceCB)) return false;
        if (!makeCB(sizeof(HE::MaterialShaderLibrary::SSRBlurUniforms),  ssrBlurCB))  return false;

        // CLAMP, not the SSAO noise sampler's WRAP: a ray that walks off the
        // screen must fetch the edge texel, not the opposite edge — wrapping
        // turns an off-screen miss into a confident hit somewhere else entirely.
        {
            D3D11_SAMPLER_DESC sd{};
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            sd.MaxLOD = D3D11_FLOAT32_MAX;
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
            if (FAILED(device->CreateSamplerState(&sd, &ssrPointClamp))) return false;
            sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            if (FAILED(device->CreateSamplerState(&sd, &ssrLinearClamp))) return false;
        }
        ssrFailed = false;
        ssrReady  = true;
        HE_LOG_INFO(RHI, "%s", "D3D11Renderer: screen-space reflection pipeline created");
        return true;
#endif
    }

    void destroySSRTargets()
    {
        for (int i = 0; i < 2; ++i)
        {
            ssrHistRadRTV[i].Reset(); ssrHistRadSRV[i].Reset(); ssrHistRadTex[i].Reset();
            ssrHistPosRTV[i].Reset(); ssrHistPosSRV[i].Reset(); ssrHistPosTex[i].Reset();
        }
        ssrPingRTV.Reset(); ssrPingSRV.Reset(); ssrPingTex.Reset();
        ssrReflRTV.Reset(); ssrReflSRV.Reset(); ssrReflTex.Reset();
        ssrW = ssrH = 0;
        ssrHistValid = false;
    }

    // Half-resolution RGBA16F ping-pong, the same set Metal's EnsureSSRTarget
    // and GL's EnsureSSRTargets build. NOT the colour history — that one lives
    // with the HDR target and outlives a resize of these.
    bool createSSRTargets(int w, int h)
    {
        if (ssrHistRadRTV[0] && w == ssrW && h == ssrH) return true;
        destroySSRTargets();
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
        bool ok = true;
        for (int i = 0; i < 2; ++i)
        {
            ok &= makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, ssrHistRadTex[i], ssrHistRadRTV[i], ssrHistRadSRV[i]);
            ok &= makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, ssrHistPosTex[i], ssrHistPosRTV[i], ssrHistPosSRV[i]);
        }
        ok &= makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, ssrPingTex, ssrPingRTV, ssrPingSRV);
        ok &= makeRT(DXGI_FORMAT_R16G16B16A16_FLOAT, ssrReflTex, ssrReflRTV, ssrReflSRV);
        if (!ok)
        {
            destroySSRTargets();
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: SSR target creation failed");
            return false;
        }
        ssrW = w; ssrH = h;
        // Frame 1 has no history: the pair is undefined until the trace has
        // written it once, so the blend factor stays 0 until then.
        ssrHistIdx   = 0;
        ssrHistValid = false;
        return true;
    }

    // Full-res copy of the finished HDR frame (opaque + sky + transparency),
    // taken at the end of the scene pass. NEXT frame's trace reprojects its hits
    // into it — the one frame of content lag is the accepted forward trade.
    void captureSSRColorHistory()
    {
        if (!ssrColorHistTex || !hdrTex) return;
        context->CopyResource(ssrColorHistTex.Get(), hdrTex.Get());
        ssrColorHistValid = true;
    }

    // Trace + blur chain. Returns the SRV the scene shader samples as uSSRFwd,
    // or null when SSR could not run this frame. The caller has already put the
    // scene render target aside; this leaves nothing bound.
    ID3D11ShaderResourceView* RenderForwardSSR(ID3D11DeviceContext* ctx, int pw, int ph,
                                               const glm::mat4& viewProj, const glm::mat4& view)
    {
#if !defined(HE_HAVE_SHADERC)
        (void)ctx; (void)pw; (void)ph; (void)viewProj; (void)view;
        return nullptr;
#else
        if (!ssrReady || !reflAttrSRV || !reflNdcSRV) return nullptr;
        if (!ssrColorHistSRV || !ssrColorHistValid)   return nullptr; // frame 1 seeds the copy
        const int tw = std::max(1, pw / 2), th = std::max(1, ph / 2);
        if (!createSSRTargets(tw, th)) return nullptr;

        const glm::vec3 camFwd = -glm::normalize(glm::vec3(glm::inverse(view)[2]));
        const int curIdx  = ssrHistIdx;
        const int prevIdx = 1 - curIdx;

        D3D11_VIEWPORT vp{}; vp.Width = float(tw); vp.Height = float(th); vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(fsVS.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(noDepthDSS.Get(), 0);
        ctx->RSSetState(fsRastState.Get());
        // The trace blends against its own history INSIDE the shader; a blend
        // state left over from the transparent pass would blend it twice.
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

        // ── 1. Trace (MRT: blended radiance + receiver position) ────────────
        {
            const bool temporal = ssrQuality >= 2;
            float hist = 0.0f;
            if (temporal && ssrHistValid)
            {
                // Camera-motion damping, the same measure Metal and GL take: a
                // moved view means the un-reprojected reflection CONTENT is stale.
                float delta = 0.0f;
                for (int c = 0; c < 4; ++c)
                    for (int r = 0; r < 4; ++r)
                        delta = std::max(delta, std::abs(viewProj[c][r] - ssrPrevViewProj[c][r]));
                hist = delta > 1e-5f ? 0.55f : 0.85f;
            }
            ssrFrameSeed += 1.0f;

            HE::MaterialShaderLibrary::SSRTraceUniforms tu;
            const glm::mat4 ivp = glm::inverse(viewProj);
            std::memcpy(tu.viewProj,     glm::value_ptr(viewProj),        16 * sizeof(float));
            std::memcpy(tu.invViewProj,  glm::value_ptr(ivp),             16 * sizeof(float));
            std::memcpy(tu.prevViewProj, glm::value_ptr(ssrPrevViewProj), 16 * sizeof(float));
            tu.cfg2[0] = ssrFrameSeed;
            tu.cfg2[1] = hist;
            tu.cfg2[2] = 1.0f; // forward: colour from the previous frame's copy
            tu.cfg2[3] = temporal ? 1.0f : 0.0f;
            tu.camPos[0] = m_renderWorld.camera.position.x;
            tu.camPos[1] = m_renderWorld.camera.position.y;
            tu.camPos[2] = m_renderWorld.camera.position.z;
            tu.camFwd[0] = camFwd.x; tu.camFwd[1] = camFwd.y; tu.camFwd[2] = camFwd.z;
            tu.cfg[0] = ssrMaxDistance;
            tu.cfg[1] = ssrThickness;
            tu.cfg[2] = ssrMaxRoughness;
            tu.cfg[3] = static_cast<float>(ssrQuality <= 0 ? 16 : ssrQuality == 1 ? 32 : 64);
            // D3D conventions, identical to Metal's and to what the decal pass
            // already derives (docs/ssr-cross-backend-plan.md §1.4):
            // SV_Position.y counts from the TOP while NDC y points up → sign -1;
            // depth is 0..1 in both the texture and NDC → scale 1, bias 0.
            tu.conv[0] = -1.0f;
            tu.conv[1] =  1.0f;
            tu.conv[2] =  0.0f;
            tu.conv[3] =  0.1f;
            tu.vp[0] = static_cast<float>(tw);
            tu.vp[1] = static_cast<float>(th);
            D3D11_MAPPED_SUBRESOURCE md{};
            if (SUCCEEDED(ctx->Map(ssrTraceCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &md)))
            {
                std::memcpy(md.pData, &tu, sizeof(tu));
                ctx->Unmap(ssrTraceCB.Get(), 0);
            }
            // Render targets FIRST, resources second — the previous frame's
            // history pair is bound as SRVs here and as RTVs next frame, and
            // D3D11 resolves that overlap by silently unbinding one of them.
            ID3D11RenderTargetView* rtvs[2] = { ssrHistRadRTV[curIdx].Get(),
                                                ssrHistPosRTV[curIdx].Get() };
            ctx->OMSetRenderTargets(2, rtvs, nullptr);
            ctx->PSSetShader(ssrTracePS.Get(), nullptr, 0);
            ctx->PSSetConstantBuffers(12, 1, ssrTraceCB.GetAddressOf()); // b12 HeSSRTrace
            ID3D11ShaderResourceView* srvs[6] = {
                ssrColorHistSRV.Get(),      // t8  heSceneColor (prev frame HDR)
                reflAttrSRV.Get(),          // t9  heGB1
                nullptr,                    // t10 unused by the trace
                reflNdcSRV.Get(),           // t11 heGBDepth
                ssrHistRadSRV[prevIdx].Get(),// t12 heSSRHistRad
                ssrHistPosSRV[prevIdx].Get()};//t13 heSSRHistPos
            ctx->PSSetShaderResources(8, 6, srvs);
            // The radiance history is point-sampled from the temporal tier
            // upward; at quality 0 the very same texture is what the scene
            // shader upsamples to full screen, and POINT would show there as
            // half-res stair-steps in the mirror. GB1, the depth and the
            // receiver positions are ALWAYS point — a lerped oct normal or a
            // lerped NDC depth decodes to nonsense at every silhouette.
            ID3D11SamplerState* histSamp = ssrQuality >= 2 ? ssrPointClamp.Get()
                                                           : ssrLinearClamp.Get();
            ID3D11SamplerState* samps[6] = {
                ssrLinearClamp.Get(), ssrPointClamp.Get(), ssrPointClamp.Get(),
                ssrPointClamp.Get(),  histSamp,            ssrPointClamp.Get() };
            ctx->PSSetSamplers(8, 6, samps);
            ctx->Draw(3, 0);
            ++counters.draws;

            ID3D11RenderTargetView* nulls[2] = { nullptr, nullptr };
            ctx->OMSetRenderTargets(2, nulls, nullptr);
            ssrHistIdx      = prevIdx;
            ssrHistValid    = true;
            ssrPrevViewProj = viewProj;
        }

        ID3D11ShaderResourceView* result = ssrHistRadSRV[curIdx].Get();

        // ── 2. Blur chain — the same tier policy as Metal's forward variant.
        // No wide / roughness-mix stage: the forward path has none there either
        // (that shader serves the GI reflections), and the pre-pass carries no
        // roughness, so there would be nothing to lerp against.
        if (ssrQuality >= 1)
        {
            ctx->PSSetShader(ssrBlurPS.Get(), nullptr, 0);
            ctx->PSSetConstantBuffers(12, 1, ssrBlurCB.GetAddressOf()); // b12 HeSSRBlur
            ctx->PSSetSamplers(8, 1, ssrLinearClamp.GetAddressOf());    // s8 heSSRIn
            auto blurPass = [&](ID3D11ShaderResourceView* src, ID3D11RenderTargetView* dst,
                                float dx, float dy)
            {
                HE::MaterialShaderLibrary::SSRBlurUniforms bu;
                bu.dir[0] = dx;
                bu.dir[1] = dy;
                bu.dir[2] = 1.0f / static_cast<float>(tw);
                bu.dir[3] = 1.0f / static_cast<float>(th);
                D3D11_MAPPED_SUBRESOURCE bm{};
                if (SUCCEEDED(ctx->Map(ssrBlurCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &bm)))
                {
                    std::memcpy(bm.pData, &bu, sizeof(bu));
                    ctx->Unmap(ssrBlurCB.Get(), 0);
                }
                // Target on, THEN the source — the chain reads what it wrote one
                // step earlier, so the order is what keeps the ping-pong legal.
                ctx->OMSetRenderTargets(1, &dst, nullptr);
                ctx->PSSetShaderResources(8, 1, &src);
                ctx->Draw(3, 0);
                ++counters.draws;
                ID3D11RenderTargetView* n = nullptr;
                ctx->OMSetRenderTargets(1, &n, nullptr);
                ID3D11ShaderResourceView* ns = nullptr;
                ctx->PSSetShaderResources(8, 1, &ns);
            };
            const float sx = 1.0f / static_cast<float>(tw);
            const float sy = 1.0f / static_cast<float>(th);
            blurPass(result,           ssrPingRTV.Get(), sx,   0.0f);
            blurPass(ssrPingSRV.Get(), ssrReflRTV.Get(), 0.0f, sy);
            if (ssrQuality == 1)
            {
                blurPass(ssrReflSRV.Get(), ssrPingRTV.Get(), sx,   0.0f);
                blurPass(ssrPingSRV.Get(), ssrReflRTV.Get(), 0.0f, sy);
            }
            result = ssrReflSRV.Get();
        }

        // Leave the pass's whole register block clear: `result` goes back on as
        // t16 for the scene shader, and t8..t13 must not still name a texture
        // that becomes a render target again next frame.
        ID3D11ShaderResourceView* nulls6[6] = {};
        ctx->PSSetShaderResources(8, 6, nulls6);
        D3D11_VIEWPORT full{}; full.Width = float(pw); full.Height = float(ph); full.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &full);
        return result;
#endif
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
    ComPtr<ID3D11VertexShader>  giGBufVSInstanced; // GiGBufVSInstanced: {mvp, model} per instance at t3
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
        // Instanced G-buffer twin for GeometryPass batches. Optional, and kept
        // out of `ok` on purpose: a failure here must not take GI down with it —
        // every batch then loops through giGBufVS.
        if (ok && compile(kGiGBufHLSL, "GiGBufVSInstanced", "vs_5_0", b))
            device->CreateVertexShader(b->GetBufferPointer(), b->GetBufferSize(), nullptr, &giGBufVSInstanced);
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
            giGBufVS.Reset(); giGBufVSInstanced.Reset(); giGBufPS.Reset(); giShadowCS.Reset(); giProbeCS.Reset();
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
                const D3D11IndexRange range = DrawIndexRange(*dc, m.indexCount); // section or whole
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
                    ctx->DrawIndexed(range.count, range.start, 0);
                };
                // A GeometryPass batch is one instanced draw with the loop's
                // {mvp, model} per instance (same bytes as PerObjectCB's head).
                const std::vector<glm::mat4>& inst = dc->instanceTransforms;
                if (drawDepthInstanced(ctx, giGBufVSInstanced.Get(), giGBufVS.Get(),
                        range.count, range.start, static_cast<UINT>(inst.size()),
                        [&](UINT k, glm::mat4* pair) {
                            pair[0] = viewProj * inst[k];
                            pair[1] = inst[k];
                        }))
                {
                    static bool loggedOnce = false;
                    if (!loggedOnce)
                    {
                        loggedOnce = true;
                        HE_LOG_INFO(RHI, "D3D11Renderer: GI pre-pass instanced (first batch: %u instances)",
                                    static_cast<unsigned>(inst.size()));
                    }
                    continue;
                }
                if (!inst.empty())
                    for (const glm::mat4& t : inst) drawOne(t);
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
        if (postFxReady) createTaaPipeline();
        return postFxReady;
    }

    // TAA (A2/A3): optional on top of the post chain. A failed compile logs
    // (through `compile` above's twin) and leaves taaReady() false — the AA
    // combo then greys TAA out and a saved TAA setting resolves to SMAA.
    void createTaaPipeline()
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
                HE_LOG_ERROR(RHI, "D3D11Renderer: TAA shader '%s' failed: %s", entry,
                             err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
                return false;
            }
            return true;
        };
        ComPtr<ID3DBlob> velVS, velPS, resPS, shpPS;
        if (!compile(kTaaVelocityHLSL, "VSVelocity", "vs_5_0", velVS)
         || !compile(kTaaVelocityHLSL, "PSVelocity", "ps_5_0", velPS)
         || !compile(kTaaResolveHLSL,  "main",       "ps_5_0", resPS)
         || !compile(kTaaSharpenHLSL,  "main",       "ps_5_0", shpPS))
            return;
        device->CreateVertexShader(velVS->GetBufferPointer(), velVS->GetBufferSize(), nullptr, &taaVelocityVS);
        device->CreatePixelShader (velPS->GetBufferPointer(), velPS->GetBufferSize(), nullptr, &taaVelocityPS);
        device->CreatePixelShader (resPS->GetBufferPointer(), resPS->GetBufferSize(), nullptr, &taaResolvePS);
        device->CreatePixelShader (shpPS->GetBufferPointer(), shpPS->GetBufferSize(), nullptr, &taaSharpenPS);

        { D3D11_BUFFER_DESC bd{};
          bd.ByteWidth = sizeof(HE::TaaVelocityConstants); bd.Usage = D3D11_USAGE_DYNAMIC;
          bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
          device->CreateBuffer(&bd, nullptr, &taaVelocityCB); }

        // Test against the scene depth, never write it: the sky and the blended
        // tail after the velocity pass still test against the opaque depth.
        { D3D11_DEPTH_STENCIL_DESC ds{};
          ds.DepthEnable    = TRUE;
          ds.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
          ds.DepthFunc      = D3D11_COMPARISON_LESS_EQUAL;
          device->CreateDepthStencilState(&ds, &taaVelocityDSS); }
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

        // Typeless + DSV/SRV pair, same reason as createDepth (C1).
        viewportDSV.Reset();
        viewportDepthSRV.Reset();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = w; dd.Height = h;
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_R24G8_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Usage    = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &viewportDepth))) return;
        createDepthViews(viewportDepth.Get(), viewportDSV, viewportDepthSRV);

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
    std::vector<HE::UUID> pendingMatWarmup; // WarmupMaterials queue → drainMaterialWarmup()

    void createRTV()
    {
        ComPtr<ID3D11Texture2D> bb;
        swapchain->GetBuffer(0, __uuidof(ID3D11Texture2D),
                             reinterpret_cast<void**>(bb.GetAddressOf()));
        device->CreateRenderTargetView(bb.Get(), nullptr, &rtv);
    }

    // Checkpoint C1 of docs/decals-cross-backend-plan.md: the scene depth has to be
    // readable as a texture, not just writable as a depth target — the screen-space
    // decal pass reconstructs world positions from it. A DEPTH format cannot carry an
    // SRV, so the resource is TYPELESS and the two views name the concrete formats
    // (the shadow map two functions down has done exactly this all along). Precision
    // and stencil are unchanged: the same 24-bit depth + 8-bit stencil bits, only
    // reachable from a pixel shader now.
    void createDepth(int w, int h)
    {
        dsv.Reset();
        depthSRV.Reset();
        depthTex.Reset();
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width            = static_cast<UINT>(w);
        dd.Height           = static_cast<UINT>(h);
        dd.MipLevels        = 1;
        dd.ArraySize        = 1;
        dd.Format           = DXGI_FORMAT_R24G8_TYPELESS;
        dd.SampleDesc.Count = 1;
        dd.Usage            = D3D11_USAGE_DEFAULT;
        dd.BindFlags        = D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&dd, nullptr, &depthTex))) return;
        createDepthViews(depthTex.Get(), dsv, depthSRV);
    }

    // The DSV/SRV pair over a typeless R24G8 depth resource. A typeless resource
    // REJECTS a null view desc, so both descs are explicit.
    void createDepthViews(ID3D11Texture2D* tex,
                          ComPtr<ID3D11DepthStencilView>&   outDSV,
                          ComPtr<ID3D11ShaderResourceView>& outSRV)
    {
        D3D11_DEPTH_STENCIL_VIEW_DESC dvd{};
        dvd.Format        = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dvd.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
        device->CreateDepthStencilView(tex, &dvd, &outDSV);

        D3D11_SHADER_RESOURCE_VIEW_DESC svd{};
        svd.Format              = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // depth in .r, 0..1
        svd.ViewDimension       = D3D11_SRV_DIMENSION_TEXTURE2D;
        svd.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(tex, &svd, &outSRV);
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
        // Its instanced twin (one draw per same-mesh caster run, reads t3 like
        // VSMainInstanced). Optional: without it every run loops through VSDepth.
        ComPtr<ID3DBlob> divsBlob;
        if (SUCCEEDED(D3DCompile(sceneSource.c_str(), sceneSource.size(), "scene", nullptr, nullptr,
                                 "VSDepthInstanced", "vs_5_0", flags, 0, &divsBlob, &err)))
            device->CreateVertexShader(divsBlob->GetBufferPointer(), divsBlob->GetBufferSize(),
                                       nullptr, &depthVSInstanced);
        else
            HE_LOG_ERROR(RHI, "%s", (std::string("D3D11Renderer: VSDepthInstanced compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());

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

        // Clustered-lighting lists (t18..t20): dynamic structured buffers at the
        // LightPacking caps, refilled per frame with MAP_WRITE_DISCARD like the
        // instance buffer above. All three or none — the scene shader reads
        // them together whenever uClusterParams.x > 0.
        {
            auto makeSB = [&](UINT elements, UINT stride, ComPtr<ID3D11Buffer>& sb,
                              ComPtr<ID3D11ShaderResourceView>& srv) -> bool
            {
                D3D11_BUFFER_DESC bd{};
                bd.ByteWidth           = elements * stride;
                bd.Usage               = D3D11_USAGE_DYNAMIC;
                bd.BindFlags           = D3D11_BIND_SHADER_RESOURCE;
                bd.CPUAccessFlags      = D3D11_CPU_ACCESS_WRITE;
                bd.MiscFlags           = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
                bd.StructureByteStride = stride;
                if (FAILED(device->CreateBuffer(&bd, nullptr, &sb))) return false;
                D3D11_SHADER_RESOURCE_VIEW_DESC sd{};
                sd.Format              = DXGI_FORMAT_UNKNOWN;
                sd.ViewDimension       = D3D11_SRV_DIMENSION_BUFFER;
                sd.Buffer.FirstElement = 0;
                sd.Buffer.NumElements  = elements;
                return SUCCEEDED(device->CreateShaderResourceView(sb.Get(), &sd, &srv));
            };
            const bool ok =
                makeSB(static_cast<UINT>(HE::kMaxClusteredLights) * 4u, sizeof(glm::vec4),
                       clusterLightSB, clusterLightSRV)
             && makeSB(static_cast<UINT>(HE::kClusterCount), sizeof(glm::uvec2),
                       clusterGridSB, clusterGridSRV)
             && makeSB(static_cast<UINT>(HE::kMaxClusterIndices), sizeof(uint32_t),
                       clusterIdxSB, clusterIdxSRV);
            if (!ok)
            {
                clusterLightSRV.Reset(); clusterGridSRV.Reset(); clusterIdxSRV.Reset();
                forwardClustered = false;
                HE_LOG_WARN(RHI, "%s", "D3D11Renderer: cluster light buffers failed — "
                                       "staying on the 8-light window");
            }
            // HE_FORWARD_CLUSTER=0 keeps the mixed 8-light window (A/B guard,
            // the forward twin of Metal's HE_DEFERRED_CLUSTER).
            if (const char* cl = std::getenv("HE_FORWARD_CLUSTER"); cl && *cl && std::atoi(cl) == 0)
                forwardClustered = false;
        }

        // Cascade shadow-map array (kCsmCascades slices) + the state the two
        // shadow stages need: a point/clamp sampler for the PCF taps and a
        // rasterizer with depth bias for the caster pass (GL: glPolygonOffset
        // (2, 4); Metal: setDepthBias — D3D11 had none, so single-map acne
        // was masked only by the old, much larger receiver bias).
        createShadowArray();
        // Local (point/spot) shadow atlas — fixed size, built once. Rendering
        // gates on the SRV, so a failure just leaves local shadows off.
        createLocalShadowArray();
        {
            D3D11_SAMPLER_DESC ssd{};
            ssd.Filter         = D3D11_FILTER_MIN_MAG_MIP_POINT;
            ssd.AddressU = ssd.AddressV = ssd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            ssd.ComparisonFunc = D3D11_COMPARISON_NEVER;
            ssd.MaxLOD         = D3D11_FLOAT32_MAX;
            device->CreateSamplerState(&ssd, &shadowSampler);

            D3D11_RASTERIZER_DESC srd{};
            srd.FillMode             = D3D11_FILL_SOLID;
            srd.CullMode             = D3D11_CULL_NONE; // as the scene state: winding is not guaranteed
            srd.DepthClipEnable      = TRUE;
            srd.DepthBias            = 4;
            srd.SlopeScaledDepthBias = 2.0f;
            srd.DepthBiasClamp       = 0.0f;
            device->CreateRasterizerState(&srd, &shadowRasterState);
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
        createMaterialResources(); // A4: node-graph material CBs + sampler
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
    //
    // TextureAsset::srgb (the importer's colour-vs-data decision) selects the
    // _SRGB twin of each DXGI format: the sampler then decodes to linear, so colour
    // textures shade in linear light and only the tonemap's gamma encode re-curves
    // them. The twins share block/byte layout, so pitch math and the support check
    // are unchanged; the null SRV desc below inherits the resource format.
    ComPtr<ID3D11ShaderResourceView> createAlbedoSRV(const TextureAsset* tex)
    {
        ComPtr<ID3D11ShaderResourceView> srv;
        if (!tex || tex->data.empty() || tex->channels != 4 || tex->width == 0 || tex->height == 0)
            return srv;

        const bool srgb = tex->srgb;
        DXGI_FORMAT fmt; bool isBlock; UINT blockBytes = 16;
        switch (tex->format)
        {
        case TextureFormat::RGBA8: fmt = srgb ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; isBlock = false; break;
        case TextureFormat::BC7:   fmt = srgb ? DXGI_FORMAT_BC7_UNORM_SRGB      : DXGI_FORMAT_BC7_UNORM;      isBlock = true;  break;
        case TextureFormat::BC3:   fmt = srgb ? DXGI_FORMAT_BC3_UNORM_SRGB      : DXGI_FORMAT_BC3_UNORM;      isBlock = true;  break;
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
    // individually. Always compiled in: the shaders come either from the pak's
    // precompiled variants or from the runtime cross-compile, and only the second one
    // needs HE_HAVE_SHADERC (ShaderCompilerStub.cpp explains the flavour split).
    void createMaterialResources()
    {
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
        // The weightmap's own sampler (linear-clamp, s0 during the draw). Not
        // giLinearClamp: that one only exists once the GI path initialised.
        const D3D11_SAMPLER_DESC wsd = HE::d3d11mat::WeightmapSamplerDesc();
        const bool wSampOk = SUCCEEDED(device->CreateSamplerState(&wsd, &m_matWeightSampler));
        m_matReady = cbLight && cbObj && cbParam && sampOk && wSampOk;
        Logger::LogTo(HE::Log::Cat::RHI, m_matReady ? Logger::LogLevel::Info : Logger::LogLevel::Error,
            m_matReady ? "D3D11Renderer: A4 material resources created"
                       : "D3D11Renderer: A4 material resource allocation failed");
    }

    // Build (or fetch from cache) the per-material VS + PS + input layout from the
    // MaterialShaderLibrary HLSL. Cached by the shader hash ALONE: D3D11 shader objects
    // bake neither blend nor depth state (the enclosing pass binds those), so the opaque
    // and the blended draw of one material are the same VS/PS. `transparent` stays in
    // the signature for parity with the D3D12/Vulkan GetOrBuild* (whose PSOs DO bake it),
    // but mixing it into the key here compiled every material a second time through FXC
    // the moment one entity carried a tint alpha or a material instance went translucent.
    // Returns nullptr (and caches the miss so it never retries per-draw) on any failure.
    // `precompiled` (the pak's baked HLSL for this backend, MaterialShaderLibrary::
    // precompiledFor) wins over the runtime cross-compile — same split as GL's
    // GetOrBuildMaterialProgram and Metal's GetOrBuildMaterialPipeline, and the only
    // source of shaders in a flavour built without glslang.
    MatShaders* GetOrBuildMaterialShaders(uint64_t hash, const std::string& frag,
                                          const std::string& vertBody,
                                          const MaterialShaderVariant* precompiled,
                                          bool transparent)
    {
        (void)transparent; // blend/depth are pass state on D3D11, not shader state
        const uint64_t key = hash;
        if (auto it = m_materialShaders.find(key); it != m_materialShaders.end())
            return it->second.vs ? &it->second : nullptr; // null vs == cached miss

        using Backend = HE::MaterialShaderLibrary::Backend;
        UINT cflags = 0;
#ifdef _DEBUG
        cflags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        // SPIRV-Cross emits the GLSL-sourced entry point as `main` (not VSMain/PSMain).
        ComPtr<ID3DBlob> vsb, psb;
        auto compilePair = [&](const std::string& vsSrc, const std::string& psSrc, const char* origin) -> bool {
            ComPtr<ID3DBlob> cerr;
            vsb.Reset(); psb.Reset();
            if (FAILED(D3DCompile(vsSrc.c_str(), vsSrc.size(), "matVS", nullptr, nullptr,
                                  "main", "vs_5_0", cflags, 0, &vsb, &cerr)))
            {
                HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: A4 material VS compile failed (") + origin + "): "
                    + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
                return false;
            }
            if (FAILED(D3DCompile(psSrc.c_str(), psSrc.size(), "matPS", nullptr, nullptr,
                                  "main", "ps_5_0", cflags, 0, &psb, &cerr)))
            {
                HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: A4 material PS compile failed (") + origin + "): "
                    + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
                return false;
            }
            if (!m_matHlslLogged)
            {
                m_matHlslLogged = true;
                HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material VS HLSL:\n") + vsSrc).c_str());
                HE_LOG_INFO(RHI, "%s", (std::string("D3D11 A4 material PS HLSL:\n") + psSrc).c_str());
            }
            return true;
        };

        // The baked variant first. A variant that FXC rejects (a pak exported before the
        // HLSL sampler pins of 5e52d64e, say) is not the end: fall through to the runtime
        // cross-compile, which is what rendered that pak before variants were consumed
        // here at all. Only when both roads are closed is the miss cached.
        bool built = false;
        if (precompiled && !precompiled->vertex.empty() && !precompiled->fragment.empty())
        {
            built = compilePair(precompiled->vertex, precompiled->fragment, "baked variant");
            if (!built)
                HE_LOG_WARN(RHI, "%s", "D3D11Renderer: A4 baked material variant rejected — cross-compiling instead");
        }
        if (!built)
        {
            const HE::MaterialShaderLibrary::Compiled& vc = vertBody.empty()
                ? m_matShaderLib.standardVertex(Backend::HLSL)
                : m_matShaderLib.customVertex(std::hash<std::string>{}(vertBody), vertBody, Backend::HLSL);
            const HE::MaterialShaderLibrary::Compiled& fc = m_matShaderLib.fragment(hash, frag, Backend::HLSL);
            if (!vc.ok || !fc.ok || vc.source.empty() || fc.source.empty())
            {
                HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: A4 material shader cross-compile failed: ")
                    + vc.log + " " + fc.log).c_str());
                m_materialShaders.emplace(key, MatShaders{});
                return nullptr;
            }
            built = compilePair(vc.source, fc.source, "cross-compiled");
        }
        if (!built)
        {
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
    }

    // ── Screen-space decals ─────────────────────────────────────────────────
    // Build the decal shaders + fixed state once, lazily (like GL's
    // EnsureDecalProgram / Vulkan's EnsureDecalPipelines): a project without a
    // single DecalComponent never pays for it, and a failure is remembered so the
    // frame loop does not retry a D3DCompile per frame.
    bool EnsureDecalPipeline()
    {
#if !defined(HE_HAVE_SHADERC)
        return false;
#else
        if (decalReady)  return true;
        if (decalFailed) return false;
        decalFailed = true; // cleared only on full success below

        using Backend = HE::MaterialShaderLibrary::Backend;
        const HE::MaterialShaderLibrary::Compiled& vc = m_matShaderLib.decalVertex(Backend::HLSL);
        // The FORWARD variant, not the sampled one: with no G-buffer the decal has
        // to bring its own lighting (one directional + ambient, no shadows, no
        // point lights). That is the documented optical deviation of the three
        // forward backends — docs/decals-cross-backend-plan.md §6.
        const HE::MaterialShaderLibrary::Compiled& fc = m_matShaderLib.decalFragmentForward(Backend::HLSL);
        if (!vc.ok || !fc.ok || vc.source.empty() || fc.source.empty())
        {
            HE_LOG_WARN(RHI, "%s", "D3D11Renderer: decal shader cross-compile failed");
            return false;
        }

        UINT cflags = 0;
#ifdef _DEBUG
        cflags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        ComPtr<ID3DBlob> vsb, psb, cerr;
        if (FAILED(D3DCompile(vc.source.c_str(), vc.source.size(), "decalVS", nullptr, nullptr,
                              "main", "vs_5_0", cflags, 0, &vsb, &cerr)))
        {
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: decal VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(fc.source.c_str(), fc.source.size(), "decalPS", nullptr, nullptr,
                              "main", "ps_5_0", cflags, 0, &psb, &cerr)))
        {
            HE_LOG_WARN(RHI, "%s", (std::string("D3D11Renderer: decal PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(device->CreateVertexShader(vsb->GetBufferPointer(), vsb->GetBufferSize(), nullptr, &decalVS)) ||
            FAILED(device->CreatePixelShader (psb->GetBufferPointer(), psb->GetBufferSize(), nullptr, &decalPS)))
        {
            HE_LOG_ERROR(RHI, "%s", "D3D11Renderer: decal shader-object creation failed");
            return false;
        }

        {   // 368 B = 4 mat4 + 7 vec4; already a 16-byte multiple.
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth      = (static_cast<UINT>(sizeof(HE::MaterialShaderLibrary::DecalUniforms)) + 15u) & ~15u;
            bd.Usage          = D3D11_USAGE_DYNAMIC;
            bd.BindFlags      = D3D11_BIND_CONSTANT_BUFFER;
            bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            if (FAILED(device->CreateBuffer(&bd, nullptr, &decalCB))) return false;
        }
        {   // The projected decal texture: wrapped like every other albedo sampler.
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &decalTexSampler))) return false;
        }
        {   // The depth: point + clamp. uv = SV_Position.xy / viewport is texel-exact,
            // and filtering a depth format is not guaranteed by the hardware.
            D3D11_SAMPLER_DESC sd{};
            sd.Filter   = D3D11_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
            sd.MaxLOD   = D3D11_FLOAT32_MAX;
            if (FAILED(device->CreateSamplerState(&sd, &decalDepthSampler))) return false;
        }
        {   // FRONT faces culled so the projector still draws with the camera inside
            // the box. FrontCounterClockwise stays FALSE: D3D's window origin is top
            // left like Metal's, so its clockwise-is-front default picks the same
            // triangles GL picks with GL_CCW (plan §3 A5 / §6a).
            // DepthClipEnable must be set EXPLICITLY — the zero-initialised desc
            // means "off", the opposite of the GL/Vulkan default.
            D3D11_RASTERIZER_DESC rd{};
            rd.FillMode              = D3D11_FILL_SOLID;
            rd.CullMode              = D3D11_CULL_FRONT;
            rd.FrontCounterClockwise = FALSE;
            rd.DepthClipEnable       = TRUE;
            if (FAILED(device->CreateRasterizerState(&rd, &decalRast))) return false;
        }
        {   D3D11_DEPTH_STENCIL_DESC ds{};
            ds.DepthEnable = FALSE; // the box-space clip decides, not the z-test
            if (FAILED(device->CreateDepthStencilState(&ds, &decalNoDepth))) return false;
        }
        {   // Write mask RGB, NOT ALL: in the editor viewport the target's alpha is
            // what ImGui composites with — same reason Vulkan masks it.
            D3D11_BLEND_DESC bd{};
            auto& rt = bd.RenderTarget[0];
            rt.BlendEnable    = TRUE;
            rt.SrcBlend       = D3D11_BLEND_SRC_ALPHA;
            rt.DestBlend      = D3D11_BLEND_INV_SRC_ALPHA;
            rt.BlendOp        = D3D11_BLEND_OP_ADD;
            rt.SrcBlendAlpha  = D3D11_BLEND_ONE;
            rt.DestBlendAlpha = D3D11_BLEND_ZERO;
            rt.BlendOpAlpha   = D3D11_BLEND_OP_ADD;
            rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_RED
                                     | D3D11_COLOR_WRITE_ENABLE_GREEN
                                     | D3D11_COLOR_WRITE_ENABLE_BLUE;
            if (FAILED(device->CreateBlendState(&bd, &decalBlend))) return false;
        }

        decalFailed = false;
        decalReady  = true;
        HE_LOG_INFO(RHI, "%s", "D3D11Renderer: screen-space decal pipeline created");
        return true;
#endif
    }

    // DecalData::textureId is a TEXTURE uuid, not a material uuid, so the albedo /
    // override caches cannot serve it. Null = untextured (the shader's hasTexture
    // flag stays 0 and the white default is bound so the sampler is never dangling).
    // Like Vulkan: keyed by texture uuid, and NOT hot-reloaded — InvalidateMaterial
    // carries material uuids, so an edited decal texture stays stale until restart.
    ID3D11ShaderResourceView* resolveDecalTexture(const HE::UUID& textureId, ContentManager* cm)
    {
        if (textureId == HE::UUID{} || !cm) return nullptr;
        if (auto it = decalTexCache.find(textureId); it != decalTexCache.end())
            return it->second.Get();
        ComPtr<ID3D11ShaderResourceView> srv = createAlbedoSRV(cm->resolveTextureRef(textureId, {}));
        ID3D11ShaderResourceView* raw = srv.Get(); // cached even when null: no per-frame retry
        decalTexCache.emplace(textureId, std::move(srv));
        return raw;
    }

    // A graph material's project texture for one heTexP slot. resolveTextureRef LOADS a
    // loose asset synchronously, which can move every ContentManager pointer the caller
    // holds — callers snapshot the slot list first and re-fetch the material afterwards.
    ID3D11ShaderResourceView* resolveGraphTexture(const HE::UUID& id, const std::string& path,
                                                  ContentManager* cm)
    {
        const std::string key = graphTexKey(id, path);
        if (key.empty() || !cm) return nullptr;
        if (auto it = graphTexCache.find(key); it != graphTexCache.end())
            return it->second.Get();
        // RGBA8 + cooked BC7/BC3 with the pre-baked mip chain (skips a block format
        // this device can't sample → null → white default).
        ComPtr<ID3D11ShaderResourceView> srv = createAlbedoSRV(cm->resolveTextureRef(id, path));
        ID3D11ShaderResourceView* raw = srv.Get();
        graphTexCache.emplace(key, std::move(srv));
        return raw;
    }

    // Draw every decal of the frame into the currently bound colour target, between
    // the opaque and the transparent geometry — the same slot at which Metal and GL
    // put theirs into the G-buffer.
    //
    // THE ORDER BELOW IS THE WHOLE TRICK. D3D11 refuses to have one resource bound
    // as a depth-stencil view and as a shader resource at the same time, and it
    // resolves the conflict SILENTLY by unbinding one of them (the debug layer only
    // warns). So the DSV comes off the output merger BEFORE the depth SRV goes on,
    // and back on only after the SRV is gone. Get that backwards and the pass draws
    // nothing, with no error anywhere.
    void EncodeDecals(ID3D11DeviceContext* ctx, const glm::mat4& viewProj,
                      int width, int height, ContentManager* cm)
    {
#if !defined(HE_HAVE_SHADERC)
        (void)ctx; (void)viewProj; (void)width; (void)height; (void)cm;
#else
        if (m_renderWorld.decals.empty()) return;
        if (!EnsureDecalPipeline()) return;

        ComPtr<ID3D11RenderTargetView> curRTV;
        ComPtr<ID3D11DepthStencilView> curDSV;
        ctx->OMGetRenderTargets(1, curRTV.GetAddressOf(), curDSV.GetAddressOf());
        if (!curRTV || !curDSV) return; // not inside a scene pass

        // The SRV that reads what this pass has been writing depth into.
        ID3D11ShaderResourceView* depthRead =
              (curDSV.Get() == dsv.Get())         ? depthSRV.Get()
            : (curDSV.Get() == viewportDSV.Get()) ? viewportDepthSRV.Get()
                                                  : nullptr;
        if (!depthRead) return;

        const glm::mat4 invViewProj = glm::inverse(viewProj);
        // The forward variant shades itself, from the same source the material
        // HeLighting fill uses: the DOMINANT directional light, not the sky sun.
        glm::vec3 sunDir(0.0f), sunColor(0.0f);
        m_renderWorld.dominantDirectionalLight(sunDir, sunColor);

        ID3D11RenderTargetView* rtvOnly = curRTV.Get();
        ctx->OMSetRenderTargets(1, &rtvOnly, nullptr); // release the depth FIRST
        ID3D11ShaderResourceView* depthSrvs[1] = { depthRead };
        ctx->PSSetShaderResources(15, 1, depthSrvs);   // t15 heGBDepth

        ctx->VSSetShader(decalVS.Get(), nullptr, 0);
        ctx->PSSetShader(decalPS.Get(), nullptr, 0);
        ctx->IASetInputLayout(nullptr);                 // buffer-less 36-vertex cube
        ID3D11Buffer* noVB = nullptr; UINT zeroStride = 0, zeroOffset = 0;
        ctx->IASetVertexBuffers(0, 1, &noVB, &zeroStride, &zeroOffset);
        ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_R32_UINT, 0);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->RSSetState(decalRast.Get());
        ctx->OMSetDepthStencilState(decalNoDepth.Get(), 0);
        ctx->OMSetBlendState(decalBlend.Get(), nullptr, 0xFFFFFFFF);
        ID3D11SamplerState* samplers[2] = { decalTexSampler.Get(), decalDepthSampler.Get() };
        ctx->PSSetSamplers(14, 2, samplers);            // s14 texture, s15 depth

        for (const DecalData& dcl : m_renderWorld.decals)
        {
            HE::MaterialShaderLibrary::DecalUniforms du;
            const glm::mat4 invModel = glm::inverse(dcl.transform);
            std::memcpy(du.viewProj,    &viewProj[0][0],      16 * sizeof(float));
            std::memcpy(du.model,       &dcl.transform[0][0], 16 * sizeof(float));
            std::memcpy(du.invModel,    &invModel[0][0],      16 * sizeof(float));
            std::memcpy(du.invViewProj, &invViewProj[0][0],   16 * sizeof(float));
            du.color[0] = dcl.color.r; du.color[1] = dcl.color.g;
            du.color[2] = dcl.color.b; du.color[3] = dcl.color.a;
            ID3D11ShaderResourceView* tex = resolveDecalTexture(dcl.textureId, cm);
            du.params[0] = tex ? 1.0f : 0.0f;
            // D3D conventions, identical to Metal's: SV_Position.y counts from the
            // TOP while NDC y points UP, so uv.y = 0 maps to clip.y = +1 → sign -1;
            // depth is already 0..1 in both the texture and NDC → scale 1, bias 0.
            // (Vulkan: +1/1/0 — its NDC y points down. GL: +1/2/-1 — its NDC z is
            // -1..1.)
            du.params[1] = -1.0f;
            du.params[2] =  1.0f;
            du.params[3] =  0.0f;
            du.vp[0] = static_cast<float>(width);
            du.vp[1] = static_cast<float>(height);
            du.sunDir[0]   = sunDir.x;   du.sunDir[1]   = sunDir.y;   du.sunDir[2]   = sunDir.z;
            du.sunColor[0] = sunColor.r; du.sunColor[1] = sunColor.g; du.sunColor[2] = sunColor.b;
            du.ambient[0]  = m_renderWorld.ambient.r;
            du.ambient[1]  = m_renderWorld.ambient.g;
            du.ambient[2]  = m_renderWorld.ambient.b;
            du.camPos[0]   = m_renderWorld.camera.position.x;
            du.camPos[1]   = m_renderWorld.camera.position.y;
            du.camPos[2]   = m_renderWorld.camera.position.z;

            D3D11_MAPPED_SUBRESOURCE md{};
            if (FAILED(ctx->Map(decalCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &md))) continue;
            std::memcpy(md.pData, &du, sizeof(du));
            ctx->Unmap(decalCB.Get(), 0);
            ctx->VSSetConstantBuffers(13, 1, decalCB.GetAddressOf()); // b13 HeDecal, both stages
            ctx->PSSetConstantBuffers(13, 1, decalCB.GetAddressOf());

            // The shader references heDecalTex unconditionally, so an untextured
            // decal still needs something bound — the 1×1 white default, which the
            // hasTexture flag then makes a no-op.
            ID3D11ShaderResourceView* texSrv = tex ? tex : dummyTexture.Get();
            ctx->PSSetShaderResources(14, 1, &texSrv); // t14 heDecalTex

            ctx->Draw(36, 0);
            ++counters.draws;
            counters.tris += 12;
        }

        // Unbind t14/t15 BEFORE the depth goes back on the output merger.
        ID3D11ShaderResourceView* nulls2[2] = { nullptr, nullptr };
        ctx->PSSetShaderResources(14, 2, nulls2);
        ID3D11RenderTargetView* restoreRTV = curRTV.Get();
        ctx->OMSetRenderTargets(1, &restoreRTV, curDSV.Get());

        // Restore everything the pass clobbered for the transparent draws that
        // follow: shaders, input layout, raster/depth/blend state. Constant buffer
        // slot b13 and t14/t15 are the decal pass's own — nothing else in this
        // renderer binds them, which is exactly why the shader library pins them
        // there.
        ctx->VSSetShader(vs.Get(), nullptr, 0);
        ctx->PSSetShader(ps.Get(), nullptr, 0);
        ctx->IASetInputLayout(inputLayout.Get());
        ctx->RSSetState(rasterState.Get());
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
        ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
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
        for (const HE::UUID& id : pendingTexInval)
            graphTexCache.erase(graphTexKey(id, {}));
        pendingTexInval.clear();
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

    // Build the queued materials' VS/PS ahead of their first draw (WarmupMaterials).
    // Runs at DrawScene top so the FXC round lands before the loop below would pay it
    // per draw. A material instance shares its master's hash, so a whole family warms
    // on the first id; the rest are cache hits. The D3D11 cache is keyed by hash alone
    // (blend/depth are pass state), so one build covers opaque AND blended draws.
    // Built-in-PBR materials resolve no shader and are skipped.
    void drainMaterialWarmup(ContentManager* cm)
    {
        if (pendingMatWarmup.empty()) return;
        if (!m_matReady || !cm) { pendingMatWarmup.clear(); return; }
        std::vector<HE::UUID> ids;
        ids.swap(pendingMatWarmup);
        int built = 0;
        for (const HE::UUID& id : ids)
        {
            uint64_t hash = 0; std::string frag, vertBody;
            if (!m_matShaderLib.resolveShaders(*cm, id, hash, frag, vertBody)) continue;
            if (m_materialShaders.count(hash)) continue; // already warm (or a cached miss)
            const MaterialShaderVariant* pre =
                HE::MaterialShaderLibrary::precompiledFor(cm->getMaterial(id), HE::RendererBackend::D3D11);
            if (GetOrBuildMaterialShaders(hash, frag, vertBody, pre, /*transparent=*/false))
                ++built;
        }
        if (built > 0)
            HE_LOG_INFO(RHI, "%s",
                ("D3D11Renderer: warmed up " + std::to_string(built) + " material shader set(s)").c_str());
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
        // The GL sky (HorizonRendering/SkyShaderSource.h) cross-compiled to HLSL:
        // atmosphere, three-colour nebula, 3D aurora, phased moon, cirrus, 3D
        // volumetric clouds, god rays — the same text GL runs. Its loops exit on
        // the cloud transmittance, so FXC is asked for real loops instead of
        // unrolling them (test_sky_shader.cpp compiles with the same flags).
        // Without the cross-compiler, or if either compile fails, the older
        // kSkyPSHLSL below takes over.
        skyFullModel = false;
#if defined(HE_HAVE_SHADERC)
        {
            using he::shaderc::Stage;
            using namespace HE::glsl;
            const he::shaderc::Result hlsl = he::shaderc::compileHlslPinned(
                BuildSkyFragmentGLSL450(), Stage::Fragment, {
                    { Stage::Fragment, 0, kSkySlotEnv.binding,   kSkySlotEnv.hlslReg   },
                    { Stage::Fragment, 0, kSkySlotMoon.binding,  kSkySlotMoon.hlslReg  },
                    { Stage::Fragment, 0, kSkySlotNoise.binding, kSkySlotNoise.hlslReg },
                });
            const UINT savedFlags = flags;
            flags |= D3DCOMPILE_PREFER_FLOW_CONTROL;
            skyFullModel = hlsl.ok
                && compile(kSkyVSCrossHLSL, std::strlen(kSkyVSCrossHLSL), "VSSkyCross", "vs_5_0", vsB)
                && compile(hlsl.source.c_str(), hlsl.source.size(), "main", "ps_5_0", psB);
            flags = savedFlags;
            if (!skyFullModel)
                HE_LOG_WARN(RHI, "%s", "D3D11 sky: cross-compiled GL sky unavailable, "
                                       "falling back to the reduced kSkyPSHLSL");
        }
#endif
        if (!skyFullModel)
        {
            vsB.Reset(); psB.Reset();
            const std::string skyPS_src = std::string(kSkyFuncHLSL) + kSkyPSHLSL;
            if (!compile(kSkyVSHLSL, std::strlen(kSkyVSHLSL), "VSSky", "vs_5_0", vsB)) return false;
            if (!compile(skyPS_src.c_str(), skyPS_src.size(), "PSSky", "ps_5_0", psB)) return false;
        }
        device->CreateVertexShader(vsB->GetBufferPointer(), vsB->GetBufferSize(), nullptr, &skyVS);
        device->CreatePixelShader (psB->GetBufferPointer(), psB->GetBufferSize(), nullptr, &skyPS);
        D3D11_BUFFER_DESC bd{};
        // Sized for the larger of the two layouts, so either shader fits.
        bd.ByteWidth = (static_cast<UINT>(std::max(sizeof(HE::SkyFrameParams), sizeof(SkyCB))) + 15u) & ~15u;
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

    // `cameraPos` anchors the 3D clouds and the aurora; `time` drives their
    // drift. Parameters, not m_renderWorld / m_wallTime, because the world
    // preview draws this sky from ITS camera, with the clock stopped.
    void drawSky(ID3D11DeviceContext* ctx, const glm::mat4& invVP,
                 const glm::vec3& sunDir, const IRenderer::EnvironmentSettings& env,
                 const glm::vec3& cameraPos, float time)
    {
        if (!skyReady) return;
        if (!env.skyEnabled) return; // no Sky entity → leave the cleared background
        // Translate the environment through the SHARED sky-constants builder
        // instead of hand-assigning fields (which is how D3D11 previously ended up
        // with +cos where GL/Metal have -cos, drifting the clouds 180° the wrong
        // way). The cross-compiled GL sky's cbuffer IS SkyFrameParams (one
        // memcpy); the kSkyPSHLSL fallback's SkyCB is a small subset of it, so
        // there the named fields are read out — the layouts differ.
        HE::SkyFrameInputs skyIn;
        skyIn.invViewProj    = invVP;
        skyIn.sunDir         = sunDir;
        skyIn.cameraPos      = cameraPos; // 3D clouds + aurora are world-anchored
        skyIn.time           = time;
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
        {
            if (skyFullModel) std::memcpy(m.pData, &sp, sizeof(sp));
            else              std::memcpy(m.pData, &cb, sizeof(cb));
            ctx->Unmap(skyCB.Get(), 0);
        }
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

    // `lineDepth` overrides the depth state (null = the scene's test+write);
    // the world preview's grid passes a read-only one so it occludes nothing.
    void drawDebugLines(ID3D11DeviceContext* ctx, const glm::mat4& viewProj,
                        const std::vector<DebugLine>& lines,
                        ID3D11DepthStencilState* lineDepth = nullptr)
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
        ctx->OMSetDepthStencilState(lineDepth ? lineDepth : depthState.Get(), 0);
        ctx->RSSetState(rasterState.Get());
        ctx->Draw(static_cast<UINT>(lines.size() * 2), 0);
        ctx->OMSetDepthStencilState(depthState.Get(), 0);
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

    // ── World preview: pixel shaders + light cbuffer, built on first use ─────
    // Everything else the preview draws with already exists by then: the scene
    // VS + layout, the skinned VS + bone cbuffer, the sky, the debug-line
    // pipeline (the grid is pos3+color3 lines) and the tonemap.
    bool ensureWorldPreviewPipeline()
    {
        if (previewReady)  return true;
        if (previewFailed) return false;
        UINT flags = 0;
#ifdef _DEBUG
        flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#endif
        auto compile = [&](const char* entry, ComPtr<ID3D11PixelShader>& out) -> bool
        {
            ComPtr<ID3DBlob> b, err;
            if (FAILED(D3DCompile(kWorldPreviewPSHLSL, std::strlen(kWorldPreviewPSHLSL), "worldpreview",
                                  nullptr, nullptr, entry, "ps_5_0", flags, 0, &b, &err)))
            {
                HE_LOG_ERROR(RHI, "%s", (std::string("D3D11 world preview '") + entry + "': "
                    + (err ? static_cast<const char*>(err->GetBufferPointer()) : "?")).c_str());
                return false;
            }
            return SUCCEEDED(device->CreatePixelShader(b->GetBufferPointer(), b->GetBufferSize(),
                                                       nullptr, &out));
        };
        const bool ok = compile("PSPreviewMesh", previewMeshPS) && compile("PSPreviewSkinned", previewSkinnedPS);
        if (ok)
        {
            D3D11_BUFFER_DESC bd{};
            bd.ByteWidth = 4 * sizeof(glm::vec4); bd.Usage = D3D11_USAGE_DYNAMIC;
            bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
            device->CreateBuffer(&bd, nullptr, &previewLightCB);
        }
        previewReady  = ok && previewLightCB && vs && inputLayout && perObjectCB && postFxReady;
        previewFailed = !previewReady;
        return previewReady;
    }

    // (Re)creates a slot's targets at w×h. False leaves the slot empty.
    bool ensureWorldPreviewTarget(WorldPreviewTarget& wp, int w, int h)
    {
        if (wp.ldrSRV && wp.w == w && wp.h == h) return true;
        wp = WorldPreviewTarget{};
        auto colour = [&](DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex,
                          ComPtr<ID3D11RenderTargetView>& rtv, ComPtr<ID3D11ShaderResourceView>& srv)
        {
            D3D11_TEXTURE2D_DESC td{};
            td.Width = static_cast<UINT>(w); td.Height = static_cast<UINT>(h);
            td.MipLevels = td.ArraySize = 1;
            td.Format = fmt;
            td.SampleDesc.Count = 1;
            td.Usage = D3D11_USAGE_DEFAULT;
            td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            return SUCCEEDED(device->CreateTexture2D(&td, nullptr, &tex))
                && SUCCEEDED(device->CreateRenderTargetView(tex.Get(), nullptr, &rtv))
                && SUCCEEDED(device->CreateShaderResourceView(tex.Get(), nullptr, &srv));
        };
        D3D11_TEXTURE2D_DESC dd{};
        dd.Width = static_cast<UINT>(w); dd.Height = static_cast<UINT>(h);
        dd.MipLevels = dd.ArraySize = 1;
        dd.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
        dd.SampleDesc.Count = 1;
        dd.Usage = D3D11_USAGE_DEFAULT;
        dd.BindFlags = D3D11_BIND_DEPTH_STENCIL;
        const bool ok = colour(DXGI_FORMAT_R16G16B16A16_FLOAT, wp.hdrTex, wp.hdrRTV, wp.hdrSRV)
                     && colour(DXGI_FORMAT_R8G8B8A8_UNORM,     wp.ldrTex, wp.ldrRTV, wp.ldrSRV)
                     && SUCCEEDED(device->CreateTexture2D(&dd, nullptr, &wp.depthTex))
                     && SUCCEEDED(device->CreateDepthStencilView(wp.depthTex.Get(), nullptr, &wp.dsv));
        if (!ok)
        {
            HE_LOG_ERROR(RHI, "D3D11Renderer: world preview target %dx%d creation failed", w, h);
            wp = WorldPreviewTarget{};
            return false;
        }
        wp.w = w; wp.h = h;
        return true;
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

        // cbuffer: rect(16) + color(16) + uvRect(16) + viewport(8) + mode(4) +
        // pad(4) + rotation(16) = 80 bytes
        D3D11_BUFFER_DESC bd{};
        bd.ByteWidth      = 80u;
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

        D3D11_RASTERIZER_DESC rd{};
        rd.FillMode        = D3D11_FILL_SOLID;
        rd.CullMode        = D3D11_CULL_NONE;
        rd.ScissorEnable   = TRUE;
        rd.DepthClipEnable = TRUE;
        dev.CreateRasterizerState(&rd, &uiScissorRast);
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

        struct UICBData { glm::vec4 rect; glm::vec4 color; glm::vec4 uvRect; glm::vec2 viewport;
                          float mode; float pad; glm::vec4 rotation; };

        // Clipping is a scissor rectangle, set only when it CHANGES — a widget
        // tree emits its quads in tree order, so equally-clipped quads arrive in
        // runs. The rasterizer state is swapped in for clipped runs only and the
        // one the pass was entered with is restored at the end, because this
        // pass deliberately inherits whatever the scene left set.
        ComPtr<ID3D11RasterizerState> prevRast;
        ctx->RSGetState(prevRast.GetAddressOf());
        glm::vec4 appliedClip(-1.0f);
        bool scissorRastBound = false;
        auto applyClip = [&](const glm::vec4& c)
        {
            if (c == appliedClip) return;
            appliedClip = c;
            if (c.z <= 0.0f)
            {
                if (scissorRastBound) { ctx->RSSetState(prevRast.Get()); scissorRastBound = false; }
                return;
            }
            const float x0 = std::clamp(c.x, 0.0f, static_cast<float>(width));
            const float y0 = std::clamp(c.y, 0.0f, static_cast<float>(height));
            const float x1 = std::clamp(c.x + c.z, 0.0f, static_cast<float>(width));
            const float y1 = std::clamp(c.y + c.w, 0.0f, static_cast<float>(height));
            const D3D11_RECT r{ static_cast<LONG>(x0), static_cast<LONG>(y0),
                                static_cast<LONG>(std::max(x0, x1)),
                                static_cast<LONG>(std::max(y0, y1)) };
            ctx->RSSetScissorRects(1, &r);
            if (!scissorRastBound && uiScissorRast)
            { ctx->RSSetState(uiScissorRast.Get()); scissorRastBound = true; }
        };

        for (const UIRenderObject& obj : m_renderWorld.uiObjects)
        {
            applyClip(obj.clipRect);
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
            cb.rotation = glm::vec4(obj.rotation, obj.rotationPivot.x, obj.rotationPivot.y, 0.0f);
            D3D11_MAPPED_SUBRESOURCE mr{};
            if (SUCCEEDED(ctx->Map(uiCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mr)))
            {
                std::memcpy(mr.pData, &cb, sizeof(cb));
                ctx->Unmap(uiCB.Get(), 0);
            }
            ctx->Draw(4, 0);
        }

        // Restore: rasterizer as found, no atlas SRV, opaque blend, depth on
        if (scissorRastBound) ctx->RSSetState(prevRast.Get());
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
    m_impl->pendingMatWarmup.clear();
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
    m_impl->m_matWeightSampler.Reset();
    // Screen-space decals + graph project textures (ComPtr auto-release).
    m_impl->decalTexCache.clear();
    m_impl->graphTexCache.clear();
    m_impl->pendingTexInval.clear();
    m_impl->decalVS.Reset(); m_impl->decalPS.Reset(); m_impl->decalCB.Reset();
    m_impl->decalTexSampler.Reset(); m_impl->decalDepthSampler.Reset();
    m_impl->decalRast.Reset(); m_impl->decalNoDepth.Reset(); m_impl->decalBlend.Reset();
    m_impl->decalReady = false;
    m_impl->decalFailed = false;
    // Forward SSR: the trace/blur pipeline, the half-res ping-pong and the
    // reflection pre-pass (its targets hang off the SSAO ones and go with them).
    m_impl->destroySSRTargets();
    m_impl->ssrColorHistSRV.Reset(); m_impl->ssrColorHistTex.Reset();
    m_impl->ssrColorHistValid = false;
    m_impl->ssrTracePS.Reset(); m_impl->ssrBlurPS.Reset();
    m_impl->ssrTraceCB.Reset(); m_impl->ssrBlurCB.Reset();
    m_impl->ssrPointClamp.Reset(); m_impl->ssrLinearClamp.Reset();
    m_impl->ssrReady = false; m_impl->ssrFailed = false;
    m_impl->reflPrepassVS.Reset(); m_impl->reflPrepassPS.Reset();
    m_impl->reflPrepassIL.Reset(); m_impl->reflPrepassCB.Reset();
    m_impl->reflAttrRTV.Reset(); m_impl->reflAttrSRV.Reset(); m_impl->reflAttrTex.Reset();
    m_impl->reflNdcRTV.Reset();  m_impl->reflNdcSRV.Reset();  m_impl->reflNdcTex.Reset();
    m_impl->reflPrepassReady = false; m_impl->reflPrepassFailed = false;
    // TAA: history/velocity targets and the three shaders.
    m_impl->destroyTaaTargets();
    m_impl->taaVelocityVS.Reset(); m_impl->taaVelocityPS.Reset();
    m_impl->taaResolvePS.Reset();  m_impl->taaSharpenPS.Reset();
    m_impl->taaVelocityCB.Reset(); m_impl->taaVelocityDSS.Reset();
    m_impl->uiFontAtlases.clear();
    m_impl->uiSampler.Reset();
    m_impl->gpuTimerShutdown();
    m_impl->skyVS.Reset(); m_impl->skyPS.Reset(); m_impl->skyCB.Reset();
    m_impl->moonSRV.Reset(); m_impl->moonTex2D.Reset();
    m_impl->noiseSRV.Reset(); m_impl->noiseTex3D.Reset(); m_impl->skyNoiseSampler.Reset();
    m_impl->debugVS.Reset(); m_impl->debugPS.Reset(); m_impl->debugVB.Reset();
    m_impl->debugCB.Reset(); m_impl->debugIL.Reset();
    m_impl->ribbonVB.clear(); m_impl->ribbonIB.clear();
    // GI resources (accel buffers, targets, atlases, pipelines).
    m_impl->destroyGiAccel();
    m_impl->destroyGiTargets();
    m_impl->giGBufVS.Reset(); m_impl->giGBufVSInstanced.Reset(); m_impl->giGBufPS.Reset();
    m_impl->giShadowCS.Reset(); m_impl->giProbeCS.Reset();
    m_impl->giTemporalPS.Reset(); m_impl->giBlurPS.Reset();
    m_impl->giShadowCB.Reset(); m_impl->giCountCB.Reset(); m_impl->giTemporalCB.Reset();
    m_impl->giBlurCB.Reset(); m_impl->giProbeCB.Reset();
    m_impl->giLinearClamp.Reset();
    m_impl->rtv.Reset();
    m_impl->dsv.Reset();
    m_impl->depthSRV.Reset();
    m_impl->depthTex.Reset();
    m_impl->viewportDSV.Reset();
    m_impl->viewportDepthSRV.Reset();
    m_impl->viewportDepth.Reset();
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
    // Graph-material shaders queued by WarmupMaterials, built before any draw below can
    // stall on them.
    p.drainMaterialWarmup(m_contentManager);

    // Feed time-of-day so the extractor recomputes the sun/moon direction (otherwise the
    // sky never responds to the time slider). Mirrors OpenGL/Metal.
    p.m_extractor.setDayNight(m_environment.dayNightCycle, m_environment.timeOfDay,
                              m_environment.sunColor, m_environment.sunIntensity,
                              m_environment.moonColor, m_environment.moonIntensity,
                              m_environment.cloudCoverage);
    // Project shadow settings (SetShadowSettings): a changed resolution
    // re-creates the cascade array here, before any pass of this frame touches
    // it, and the extractor fits its cascades against the size that is
    // actually ALLOCATED — the texel snap must match the texture, or the
    // shadow edges crawl. Mirrors GL/Metal.
    if (p.shadowSizeDirty && p.device)
    {
        p.shadowSize = std::clamp(p.shadowSettings.resolution, 256, 8192);
        p.createShadowArray();
        p.shadowSizeDirty = false;
    }
    p.m_extractor.setShadowSettings(p.shadowSettings.distance, p.shadowSettings.cascadeCount,
                                    p.shadowSettings.splitLambda, p.shadowSize);
    p.m_extractor.setContentManager(m_contentManager);
    p.m_extractor.extract(*m_world, p.m_renderWorld,
                          static_cast<float>(width) / static_cast<float>(height),
                          &m_editorCamera);

    // ── TAA: this frame's jitter (A2) ───────────────────────────────────────
    // Chosen BEFORE anything builds a matrix, because every rasterising pass of
    // the frame must share one offset. Two view-projections, one rule (GL's):
    // the CLEAN one measures — velocity, the SSR/GI reprojections, the SSAO
    // pre-pass — and the JITTERED one rasterises everything that lands in the
    // image: sky, geometry, decals, transparency, debug lines. Without a TAA
    // frame (see taaFrame) the two are the same matrix.
    if (p.taaFrame)
        p.taaJitter = HE::taaJitter(p.taaFrameIndex++);
    const glm::mat4 viewProjClean = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;
    const glm::mat4 viewProj      = p.taaFrame
        ? HE::taaJitteredViewProj(viewProjClean, p.taaJitter, width, height)
        : viewProjClean;

    // Sky is independent of scene geometry — always draw it here so it renders
    // even when objects/sortedIndices are empty (early returns below).
    {
        ID3D11DeviceContext* skyCtx = p.context.Get();
        const glm::mat4& skyVP = viewProj;
        p.drawSky(skyCtx, glm::inverse(skyVP), p.m_renderWorld.sunDirection, m_environment,
                  p.m_renderWorld.camera.position, p.m_wallTime);
    }

    // Trails live in their own per-frame band list, not in `objects`, so a scene
    // that has nothing BUT a trail still has something to draw past here.
    if (p.m_renderWorld.objects.empty() && p.m_renderWorld.ribbonBatches.empty()) return;

    for (RenderObject& obj : p.m_renderWorld.objects)
    {
        if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);
    }
    // PBR scalars per object, per material slot and per skinned object, each from
    // its own material (+ the Translucent clamp) — what GL/Metal's per-draw
    // ResolveMaterialParams gives them, in time for partitionByOpacity.
    HE::resolveWorldMaterialScalars(p.m_renderWorld, m_contentManager);

    // GI acceleration structures: refresh the BLAS cache + per-frame instance
    // array right after extraction (UNCULLED — off-screen casters still occlude),
    // mirroring GL's UpdateGiAccel placement. No-op when GI is off.
    p.updateGiAccel(m_contentManager, p.m_renderWorld);

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
    p.counters.total   = static_cast<uint32_t>(p.m_renderWorld.objects.size());
    p.counters.visible = static_cast<uint32_t>(p.m_sortedIndices.size());
    if (p.m_sortedIndices.empty() && p.m_renderWorld.ribbonBatches.empty()) return;

    if (p.m_renderGraph.empty())
    {
        p.m_renderGraph.addPass(std::make_unique<ShadowPass>());
        p.m_renderGraph.addPass(std::make_unique<GeometryPass>());
    }

    const glm::mat4 camView   = p.m_renderWorld.camera.view;
    const glm::mat4 camProj   = p.m_renderWorld.camera.projection;
    const bool      shadows   = p.m_renderWorld.shadow.enabled && p.shadowSRV && p.depthVS;

    // ── Cascaded shadow-map frame constants (mirrors GL's shadowFrame) ─────
    // The extractor's cascade matrices are GL clip (z∈[-1,1]); kD3DClipFix
    // remaps them to D3D's [0,1] depth — the SAME fix the old single map got.
    // The planar camera forward (−Z of camera-to-world) MUST match the planar
    // splits the cascades were fit with; unused tail cascades get identity
    // (never sampled — the shader clamps the pick to the count) and a far
    // split of 1e9 so the pick never lands on them.
    const ShadowData& csm = p.m_renderWorld.shadow;
    const int nCascades  = std::clamp(csm.cascadeCount, 0, D3D11RendererImpl::kCsmCascades);
    glm::mat4 cascadeClip[D3D11RendererImpl::kCsmCascades];
    for (int c = 0; c < D3D11RendererImpl::kCsmCascades; ++c)
        cascadeClip[c] = c < nCascades ? HE::kD3DClipFix * csm.cascadeViewProj[c] : glm::mat4(1.0f);
    const glm::vec4 cascadeSplits(
        nCascades > 0 ? csm.cascadeSplit[0] : 1e9f,
        nCascades > 1 ? csm.cascadeSplit[1] : 1e9f,
        nCascades > 2 ? csm.cascadeSplit[2] : 1e9f,
        static_cast<float>(nCascades));
    const glm::vec3 camFwd =
        -glm::normalize(glm::vec3(glm::inverse(p.m_renderWorld.camera.view)[2]));

    // ── Local (point/spot) shadow atlas frame constants ─────────────────────
    // Independent of the CSM: night scenes with only shadow-casting point
    // lights still get their atlas. Per-layer light view-projs get the same
    // clip fix as the cascades; unused layers stay identity (never sampled —
    // lightParams.y gates on the extractor's layer assignment).
    const int nLocalLayers =
        std::clamp(csm.localLayerCount, 0, D3D11RendererImpl::kLocalShadowLayers);
    const bool localShadows = nLocalLayers > 0 && p.localShadowSRV && p.depthVS;
    glm::mat4 localClip[D3D11RendererImpl::kLocalShadowLayers];
    for (int v = 0; v < D3D11RendererImpl::kLocalShadowLayers; ++v)
        localClip[v] = v < nLocalLayers ? HE::kD3DClipFix * csm.localViewProj[v] : glm::mat4(1.0f);

    ID3D11DeviceContext* ctx = p.context.Get();
    ctx->IASetInputLayout(p.inputLayout.Get());
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
    ctx->RSSetState(p.rasterState.Get());
    ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());

    // ── Clustered lighting (plan P7 on the forward path) ─────────────────────
    // Point/spot lights leave the 8-light window and go into per-cluster lists
    // (HE::BuildClusterLights); the window then carries directional lights
    // only. Built once per frame — the light set and camera do not change
    // between the two fillPerFrame calls below; only the GI-mask channel lane
    // depends on the GI decision, and that is re-derived in the refill.
    const bool clustered = p.forwardClustered && p.clusterLightSRV
                        && !p.m_renderWorld.lights.empty();
    auto uploadClusters = [&](bool giActive)
    {
        const HE::ClusterLightBuild cb =
            HE::BuildClusterLights(p.m_renderWorld, localShadows, giActive && p.giLocalMaskSRV);
        if (cb.droppedLights > 0 && !p.clusterCapWarned)
        {
            p.clusterCapWarned = true;
            HE_LOG_WARN(RHI, "D3D11Renderer: %d point/spot light(s) exceed the cluster caps "
                             "(%d lights / %d indices) and are not lit",
                        cb.droppedLights, HE::kMaxClusteredLights, HE::kMaxClusterIndices);
        }
        auto upload = [&](ID3D11Buffer* sb, const void* data, size_t bytes)
        {
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(sb, 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                std::memcpy(m.pData, data, bytes);
                ctx->Unmap(sb, 0);
            }
        };
        upload(p.clusterLightSB.Get(), cb.lights.data(),  cb.lights.size()  * sizeof(glm::vec4));
        upload(p.clusterGridSB.Get(),  cb.grid.data(),    cb.grid.size()    * sizeof(glm::uvec2));
        upload(p.clusterIdxSB.Get(),   cb.indices.data(), cb.indices.size() * sizeof(uint32_t));
        ID3D11ShaderResourceView* srvs[3] = {
            p.clusterLightSRV.Get(), p.clusterGridSRV.Get(), p.clusterIdxSRV.Get() };
        ctx->PSSetShaderResources(18, 3, srvs);
        return std::pair<glm::vec4, glm::vec4>(cb.params, cb.camFwd);
    };

    // ── Per-frame constants (camera + up to 8 lights) ───────────────────────
    // A lambda because the GI/SSAO decision is only known inside the backbuffer
    // pass — the CB is refilled there with the final giActive/aoActive flags.
    auto fillPerFrame = [&](bool giActive, bool aoActive, bool ssrActive = false)
    {
        PerFrameCB f{};
        f.cameraPos     = glm::vec4(p.m_renderWorld.camera.position, 1.0f);
        if (clustered)
        {
            const HE::DirectionalLightWindow w = HE::BuildDirectionalLightWindow(p.m_renderWorld);
            f.lightCount = glm::ivec4(w.count, 0, 0, 0);
            for (int i = 0; i < w.count; ++i)
            {
                f.lightPos[i]    = w.pos[i];
                f.lightDir[i]    = w.dir[i];
                f.lightColor[i]  = w.color[i];
                f.lightParams[i] = w.params[i];
            }
            const auto clusterConsts = uploadClusters(giActive);
            f.clusterParams = clusterConsts.first;
            f.clusterCamFwd = clusterConsts.second;
        }
        else
        {
            const int count = std::min(static_cast<int>(p.m_renderWorld.lights.size()), 8);
            f.lightCount    = glm::ivec4(count, 0, 0, 0);
            for (int i = 0; i < count; ++i)
            {
                const LightData& l = p.m_renderWorld.lights[i];
                f.lightPos[i]    = glm::vec4(l.position,  static_cast<float>(l.type));
                f.lightDir[i]    = glm::vec4(l.direction, l.spotAngleCos);
                f.lightColor[i]  = glm::vec4(l.color,     l.intensity);
                // y = the light's base layer in the local shadow atlas, -1 = no
                // local shadow (built-in convention, same as Metal/GL).
                f.lightParams[i] = glm::vec4(l.range,
                                             localShadows ? static_cast<float>(l.shadowLayer) : -1.0f,
                                             0.0f, 0.0f);
            }
            // clusterParams stays zero → the shader's window loop lights everything.
        }
        for (int c = 0; c < D3D11RendererImpl::kCsmCascades; ++c) f.cascadeVP[c] = cascadeClip[c];
        for (int v = 0; v < D3D11RendererImpl::kLocalShadowLayers; ++v) f.localShadowVP[v] = localClip[v];
        f.localShadowParams = glm::vec4(1.0f / static_cast<float>(D3D11RendererImpl::kLocalShadowSize),
                                        localShadows ? 1.0f : 0.0f, 0.0f, 0.0f);
        f.cascadeSplits = cascadeSplits;
        f.cameraFwd     = glm::vec4(camFwd, 1.0f / static_cast<float>(std::max(p.shadowSize, 1)));
        f.shadowBias    = glm::vec4(p.shadowSettings.slopeBias, p.shadowSettings.minBias, 0.0f, 0.0f);
        f.shadowEnabled = glm::ivec4(shadows ? 1 : 0, p.debugShadowCascades ? 1 : 0, 0, 0);
        f.sunDir = glm::vec4(p.m_renderWorld.sunDirection, 0.0f);
        f.fog    = glm::vec4(m_environment.fogDensity, m_environment.fogHeightFalloff, 0, 0);
        f.viewport = glm::vec4(float(width), float(height), aoActive ? 1.0f : 0.0f, 0.0f);
        f.giParams     = glm::vec4(giActive ? 1.0f : 0.0f, p.giIndirectIntensity, 0.0f, 0.0f);
        f.giGridOrigin = glm::vec4(p.giGridOrigin, D3D11RendererImpl::kGIProbeSpacing);
        f.giGridCounts = glm::vec4(glm::vec3(p.giGridCounts), float(p.giProbesPerRow));
        // x is the gate the built-in scene shader's reflection cascade tests;
        // the roughness fade happens there, with the exact shading roughness,
        // because the half-res pre-pass carries none.
        f.ssrParams    = glm::vec4(ssrActive ? 1.0f : 0.0f, p.ssrIntensity,
                                   p.ssrMaxRoughness, 0.0f);
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
        // shaders. Shared fill (HE::FillMaterialLightWindow); with the local
        // (point/spot) atlas rendered this frame, lightParams[i].y carries the
        // light's base layer + 1 (0 = "casts no local shadow").
        HE::FillMaterialLightWindow(p.m_renderWorld, lit, /*localShadowsActive=*/localShadows);
        // Local atlas view-projs for heLocalShadowFactor (heLocalShadow,
        // preamble binding 13 → t13/s13, bound in the scene pass). Same
        // pre-baked conventions as csmVP below (uvFlipY * kD3DClipFix), so the
        // shared preamble stays convention-free.
        if (localShadows)
        {
            glm::mat4 lsFlipY(1.0f);
            lsFlipY[1][1] = -1.0f;
            for (int v = 0; v < nLocalLayers; ++v)
            {
                const glm::mat4 m = lsFlipY * localClip[v];
                std::memcpy(lit.localShadowVP[v], &m[0][0], 16 * sizeof(float));
            }
        }
        lit.giParams[0] = static_cast<float>(width);
        lit.giParams[1] = static_cast<float>(height);
        lit.giParams[2] = giActive ? 1.0f : 0.0f;
        // CSM fallback for graph materials (Lighting v2.2): only meaningful
        // when the GI masks are absent this frame — heLitP's directional
        // lights then sample the SAME cascade array as the built-in shader
        // (heCsm, preamble binding 12 → t12/s12, bound in the scene pass).
        // D3D's [0,1] depth remap AND the shadow map's top-left UV origin are
        // pre-baked into the matrices (uvFlipY * kD3DClipFix — exactly Metal's
        // fill, which shares both conventions), so the shared preamble's
        // heCsmShadow() stays convention-free (uv = p.xy*0.5+0.5, z in [0,1]).
        if (!giActive && shadows)
        {
            glm::mat4 uvFlipY(1.0f);
            uvFlipY[1][1] = -1.0f;
            for (int c = 0; c < nCascades; ++c)
            {
                const glm::mat4 m = uvFlipY * cascadeClip[c];
                std::memcpy(lit.csmVP[c], &m[0][0], 16 * sizeof(float));
            }
            lit.csmSplits[0] = cascadeSplits.x;
            lit.csmSplits[1] = cascadeSplits.y;
            lit.csmSplits[2] = cascadeSplits.z;
            lit.csmSplits[3] = cascadeSplits.w;
            lit.camFwd[0] = camFwd.x;
            lit.camFwd[1] = camFwd.y;
            lit.camFwd[2] = camFwd.z;
            lit.shadowBias[0] = p.shadowSettings.slopeBias;
            lit.shadowBias[1] = p.shadowSettings.minBias;
        }
        // lit.ssr stays 0, and that is NOT an oversight: the built-in
        // scene shader gets the forward reflection cascade (uSSRParams above),
        // graph materials do not. Their consumer is heSSRFwd on binding 31, and
        // the shared lighting preamble is emitted UNPINNED — SPIRV-Cross spells
        // that register(t31)/register(s31), and D3D11 has 16 sampler slots, so
        // the API cannot bind it. Setting the gate without the descriptor would
        // make every graph material sample an unbound sampler. Closing it means
        // pinning the whole preamble, which is its own, larger job
        // (docs/ssr-cross-backend-plan.md §2.3 head 1 / C5).
        D3D11_MAPPED_SUBRESOURCE lm{};
        if (SUCCEEDED(ctx->Map(p.m_matLightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &lm)))
        {
            std::memcpy(lm.pData, &lit, sizeof(lit));
            ctx->Unmap(p.m_matLightCB.Get(), 0);
        }
    };
    fillMatLight(false);

    const UINT stride = 8 * sizeof(float);
    const UINT offset = 0;

    // `noShadow` is the inverted DrawCall::receivesShadow (1 = ignore shadows).
    // Defaulted so the depth-only shadow pass, which shades nothing, can keep
    // calling this unchanged.
    auto uploadObject = [&](const glm::mat4& mvp, const glm::mat4& model,
                            const glm::vec3& baseColor, float hasTex,
                            float metallic, float roughness, float opacity = 1.0f,
                            float noShadow = 0.0f)
    {
        PerObjectCB o{};
        o.mvp   = mvp; o.model = model;
        o.color = glm::vec4(baseColor, hasTex);
        o.pbr   = glm::vec4(metallic, roughness, opacity, noShadow);
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
        // ── Shadow pass: cascaded depth maps from the light's POV ────────────
        // One depth render per cascade into its own array slice. Each cascade
        // re-culls the casters against ITS light frustum (not the camera and
        // not the graph's whole-scene ShadowPass set, which is culled against
        // the legacy single-map frustum): an off-screen object still casts
        // into the visible scene while it sits inside the cascade coverage.
        // Mirrors GL's renderDepthLayer / Metal's encodeDepthLayer.
        if (io.output.id == kShadowMapTarget)
        {
            // CSM needs a directional light; the local (point/spot) atlas is
            // independent of it — night scenes with only shadow-casting point
            // lights still render their atlas (mirrors Metal/GL).
            if (!shadows && !localShadows) return;
            // Save the active render target so we can restore it after the shadow pass.
            ComPtr<ID3D11RenderTargetView> savedRTV;
            ComPtr<ID3D11DepthStencilView> savedDSV;
            ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

            // Unbind the shadow SRVs wherever last frame left them (t1 built-in
            // cascades, t17 built-in local atlas, t12/t13 graph-material heCsm/
            // heLocalShadow) so the arrays can be bound as depth targets
            // without the runtime's implicit unbind.
            ID3D11ShaderResourceView* nullSrv = nullptr;
            ctx->PSSetShaderResources(1, 1, &nullSrv);
            ctx->PSSetShaderResources(12, 1, &nullSrv);
            ctx->PSSetShaderResources(13, 1, &nullSrv);
            ctx->PSSetShaderResources(17, 1, &nullSrv);
            ID3D11RenderTargetView* noRTV = nullptr;
            ctx->VSSetShader(p.depthVS.Get(), nullptr, 0);
            ctx->PSSetShader(nullptr, nullptr, 0);
            if (p.shadowRasterState) ctx->RSSetState(p.shadowRasterState.Get());

            // Depth-only render of every shadow caster into one array slice,
            // shared by the CSM cascades and the local (point/spot) views.
            // Cull + sort against the view's light frustum into scratch
            // buffers; the sort groups draws by mesh so the resolve stays
            // memoised. batchDepthCasters keeps castsShadow objects only and
            // drops `skipEntity`'s geometry: the entity the local light itself
            // sits on (a light authored onto a mesh entity would otherwise
            // render that mesh at z≈0 into its own map and shadow itself out);
            // kNoOwnerEntity (every cascade) skips nothing. A run of more than
            // one caster of the same mesh is ONE instanced draw (VSDepthInstanced
            // over instanceSB, {clip * model, model} per caster — VSDepth's own
            // product); a run of one, or a run the buffer cannot take, stays on
            // the per-caster draw. Mirrors GL's renderDepthLayer / Metal's
            // encodeDepthLayer.
            auto renderDepthLayer = [&](ID3D11DepthStencilView* dsv, int size,
                                        const glm::mat4& viewProj, const glm::mat4& clip,
                                        uint32_t skipEntity)
            {
                D3D11_VIEWPORT svp{}; svp.Width = svp.Height = static_cast<float>(size); svp.MaxDepth = 1.0f;
                ctx->RSSetViewports(1, &svp);
                ctx->OMSetRenderTargets(1, &noRTV, dsv);
                ctx->ClearDepthStencilView(dsv, D3D11_CLEAR_DEPTH, 1.0f, 0);
                p.m_culler.cull(p.m_renderWorld, viewProj, p.shadowVisible);
                p.m_sorter.sort(p.m_renderWorld, p.shadowVisible, p.shadowSorted);
                RenderSorter::batchDepthCasters(p.m_renderWorld, p.shadowSorted, skipEntity,
                                                p.shadowBatches);
                for (const RenderSorter::DepthBatch& b : p.shadowBatches.batches)
                {
                    const GpuMesh* mesh = p.resolveMesh(b.meshAssetId, m_contentManager);
                    const GpuMesh& m    = mesh ? *mesh : p.cube;
                    if (!m.vbuf || !m.ibuf) continue;
                    ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
                    ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
                    const glm::mat4* xf = p.shadowBatches.transforms.data() + b.first;
                    if (p.drawDepthInstanced(ctx, p.depthVSInstanced.Get(), p.depthVS.Get(),
                            m.indexCount, 0, b.count,
                            [&](UINT k, glm::mat4* pair) {
                                pair[0] = clip * xf[k];
                                pair[1] = xf[k];
                            }))
                    {
                        static bool loggedOnce = false; // the runtime witness on Windows
                        if (!loggedOnce)
                        {
                            loggedOnce = true;
                            HE_LOG_INFO(RHI, "D3D11Renderer: shadow pass instanced (first run: %u casters)",
                                        b.count);
                        }
                        continue;
                    }
                    for (uint32_t k = 0; k < b.count; ++k)
                    {
                        uploadObject(clip * xf[k], xf[k], glm::vec3(1.0f), 0.0f, 0.0f, 1.0f);
                        ctx->DrawIndexed(m.indexCount, 0, 0);
                    }
                }
            };
            if (shadows)
            {
                const int cascades = std::clamp(csm.cascadeCount, 1, D3D11RendererImpl::kCsmCascades);
                for (int c = 0; c < cascades; ++c)
                    renderDepthLayer(p.shadowDSV[c].Get(), p.shadowSize,
                                     csm.cascadeViewProj[c], cascadeClip[c], kNoOwnerEntity);
            }
            if (localShadows)
                for (int v = 0; v < nLocalLayers; ++v)
                    renderDepthLayer(p.localShadowDSV[v].Get(), D3D11RendererImpl::kLocalShadowSize,
                                     csm.localViewProj[v], localClip[v], csm.localOwnerEntity[v]);
            // Restore saved target + viewport + raster state + scene shaders.
            ID3D11RenderTargetView* restoreRTV = savedRTV.Get();
            ctx->OMSetRenderTargets(1, &restoreRTV, savedDSV.Get());
            D3D11_VIEWPORT vp{}; vp.Width = static_cast<float>(width); vp.Height = static_cast<float>(height); vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
            ctx->RSSetState(p.rasterState.Get());
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
        // sectionAware: every draw below applies DrawCall::indexOffset/indexCount
        // (DrawIndexRange), so a multi-section mesh's per-slot DCs all come
        // through — one draw per material slot, as on GL and Metal.
        std::vector<const DrawCall*> opaqueDCs_, transparentDCs_;
        RenderSorter::partitionByOpacity(cmds.drawCalls(), opaqueDCs_, transparentDCs_,
                                         /*sectionAware=*/true);

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

            giShadowSRV = p.runGiShadow(ctx, opaqueDCs_, viewProjClean,
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

        // ── Forward screen-space reflections (plan checkpoint C) ─────────────
        // The radiance source is the previous frame's HDR colour, so SSR runs
        // only where Render() actually bound hdrRTV. That is the C6 hole, stated
        // as one gate: the swapchain branch (the packaged game, no editor
        // viewport) draws straight into the backbuffer, has no HDR target, and
        // therefore no SSR. Deliberately not fixed here — giving the swapchain
        // path an HDR target changes the packaged game's frame layout and needs
        // its own decision.
        bool ssrFrameActive = false;
#if defined(HE_HAVE_SHADERC)
        {
            ComPtr<ID3D11RenderTargetView> boundRTV;
            ctx->OMGetRenderTargets(1, boundRTV.GetAddressOf(), nullptr);
            const bool hdrBound = p.hdrRTV && boundRTV.Get() == p.hdrRTV.Get();
            ssrFrameActive = p.ssrEnabled && hdrBound
                          && p.EnsureReflPrepassPipeline() && p.EnsureSSRPipelines();
        }
#endif
        // The reflection MRT pre-pass is the trace's only geometry input, so it
        // runs whenever SSR does — including on frames where SSAO is off or the
        // GI probes replaced it, which is exactly the case the block below used
        // to skip entirely.
        const bool aoWanted = !giShadingActive && p.ssaoEnabled && p.ssaoReady;
        ID3D11ShaderResourceView* aoSRV = p.whiteSRV.Get(); // default: unoccluded
        ID3D11ShaderResourceView* ssrSRV = nullptr;
        if (aoWanted || ssrFrameActive) {
            // Save and restore render target around SSAO passes
            ComPtr<ID3D11RenderTargetView> savedRTV;
            ComPtr<ID3D11DepthStencilView> savedDSV;
            ctx->OMGetRenderTargets(1, savedRTV.GetAddressOf(), savedDSV.GetAddressOf());

            // Unjittered while TAA is on (GL's call): the pre-pass measures, and
            // the jitter between it and the scene raster is at most half a pixel
            // in the AO/reflection lookups — invisible, while a jittered
            // "previous" matrix would read the jitter as camera motion.
            aoSRV = p.runSSAO(ctx, opaqueDCs_, viewProjClean, camView, camProj, width, height,
                [&](HE::UUID id) -> const GpuMesh* { return p.resolveMesh(id, m_contentManager); },
                p.cube, p.inputLayout.Get(), p.depthState.Get(), p.rasterState.Get(),
                /*reflMrt=*/ssrFrameActive, aoWanted);
            if (ssrFrameActive)
                ssrSRV = p.RenderForwardSSR(ctx, width, height, viewProjClean, camView);

            // Restore the scene render target and viewport
            ID3D11RenderTargetView* restRTV = savedRTV.Get();
            ctx->OMSetRenderTargets(1, &restRTV, savedDSV.Get());
            D3D11_VIEWPORT vp{}; vp.Width = float(width); vp.Height = float(height); vp.MaxDepth = 1.0f;
            ctx->RSSetViewports(1, &vp);
        }
        // Same gate shape as GL's: a real trace result AND a non-zero intensity.
        // Off → nothing is bound at t16 and the cascade folds away on uSSRParams.x.
        const bool ssrActive = ssrSRV != nullptr && p.ssrIntensity > 0.0f;

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
        // CSM depth array on t1, its point/clamp sampler on s3. Always bound
        // while shadows are on: the sampling is gated by uShadowEnabled, the
        // per-cascade matrices/splits/forward drive the slice pick.
        ID3D11ShaderResourceView* shadowSrv_ = shadows ? p.shadowSRV.Get() : nullptr;
        ctx->PSSetShaderResources(1, 1, &shadowSrv_);
        if (p.shadowSampler) ctx->PSSetSamplers(3, 1, p.shadowSampler.GetAddressOf());
        // Local (point/spot) shadow atlas on t17, same point/clamp sampler.
        // Bound only when the atlas was rendered this frame; the shader gates
        // on uLightParams.y (-1 when it wasn't), so a null SRV is never read.
        ID3D11ShaderResourceView* localShadowSrv_ = localShadows ? p.localShadowSRV.Get() : nullptr;
        ctx->PSSetShaderResources(17, 1, &localShadowSrv_);
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
            // Forward SSR result on t16 — one bind for the whole pass, sampled
            // through s2 (linear clamp). Nothing is bound when the trace did not
            // run; uSSRParams.x is 0 then and the branch is never taken.
            ctx->PSSetShaderResources(16, 1, &ssrSRV);
            fillPerFrame(giShadingActive,
                         aoWanted && aoSRV != p.whiteSRV.Get(), ssrActive);
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
            // heCsm (preamble binding 12 → t12/s12): the cascade array for the
            // GI-off fallback, same point/clamp sampler as the built-in s3.
            // The SSR pre-pass block (t8..t13) is unbound again by the time
            // the scene pass runs, so the slot is free here.
            ctx->PSSetShaderResources(12, 1, &shadowSrv_);
            if (p.shadowSampler) ctx->PSSetSamplers(12, 1, p.shadowSampler.GetAddressOf());
            // heLocalShadow (preamble binding 13 → t13/s13): the local atlas.
            // Gated in the preamble by lightParams[i].y (0 = none, which is
            // what FillMaterialLightWindow writes when the atlas is off).
            ctx->PSSetShaderResources(13, 1, &localShadowSrv_);
            if (p.shadowSampler) ctx->PSSetSamplers(13, 1, p.shadowSampler.GetAddressOf());
        }

        const glm::vec3 camPos = p.m_renderWorld.camera.position;

        // Reuse already-collected opaque/transparent DC lists from the SSAO prepass above.
        std::vector<const DrawCall*>& opaqueDCs = opaqueDCs_;
        std::vector<const DrawCall*>& transparentDCs = transparentDCs_;

        // ── Motion trails join the blended list ──────────────────────────────
        // A RibbonBatch is per-frame CPU geometry in the cooked vertex layout, so
        // it needs no pass and no shader of its own: upload it into the dynamic
        // pool and append a perfectly ordinary DrawCall, which then takes part in
        // the sort below and goes through drawDC exactly like a translucent mesh —
        // including the graph-material path, which is what lets the age in uv.v
        // drive colour and fade (docs/rope-trail-plan.md §6.2). Appended AFTER the
        // partition on purpose: a trail is blended by nature, never opaque, so it
        // bypasses partitionByOpacity the way Metal and GL do. Appended after the
        // SSAO/GI prepass too — those read opaqueDCs_ only, and a trail is not in
        // the AO prepass by design.
        std::vector<DrawCall> ribbonDCs;
        std::vector<GpuMesh>  ribbonMeshes;
        std::unordered_map<const DrawCall*, const GpuMesh*> ribbonMeshByDC;
        if (!p.m_renderWorld.ribbonBatches.empty())
        {
            // Reserve BOTH before taking any address: transparentDCs below holds
            // bare pointers into ribbonDCs, and a growth reallocation kills them.
            ribbonDCs.reserve(p.m_renderWorld.ribbonBatches.size());
            ribbonMeshes.reserve(p.m_renderWorld.ribbonBatches.size());
            std::vector<float> rebased;
            for (size_t rbi = 0; rbi < p.m_renderWorld.ribbonBatches.size(); ++rbi)
            {
                const RibbonBatch& rb = p.m_renderWorld.ribbonBatches[rbi];
                if (rb.vertices.empty() || rb.indices.empty()) continue;
                const glm::vec3 pivot = rebaseRibbonVertices(rb, rebased);
                GpuMesh rmesh{};
                if (!p.uploadRibbon(ctx, rbi, rebased, rb.indices, rmesh)) continue;

                DrawCall dc{};
                dc.materialAssetId = rb.materialAssetId;
                dc.transform       = glm::mat4(1.0f);
                dc.transform[3]    = glm::vec4(pivot, 1.0f);   // pure translation, see rebaseRibbonVertices
                dc.entityId        = rb.entityId;
                dc.contributesAO   = false;   // trails never enter the AO prepass
                // receivesShadow stays TRUE: Metal and GL leave the shadow flag at its
                // default for a ribbon, and a trail that is shadowed on two backends
                // and unshadowed on three is exactly the kind of split this port exists
                // to avoid. Trails do not CAST a shadow (they are not in `objects`).
                if (const MaterialAsset* mat = (m_contentManager && rb.materialAssetId != HE::UUID{})
                        ? m_contentManager->getMaterial(rb.materialAssetId) : nullptr)
                {
                    dc.baseColor = { mat->baseColor[0], mat->baseColor[1], mat->baseColor[2] };
                    dc.metallic  = mat->metallic;
                    dc.roughness = mat->roughness;
                    dc.opacity   = mat->opacity;
                }
                else
                    dc.baseColor = glm::vec3(0.55f);   // material not loaded yet — flat grey, as Metal/GL
                // Force the blended class. RenderSorter::isTransparent decides both
                // which pass a draw belongs to and which graph-material variant it
                // gets, so a trail at opacity 1 would otherwise pick the
                // depth-WRITING variant inside the blended pass. This is the same
                // clamp ResolveMaterialParams applies to blendMode == 2 materials.
                dc.opacity = std::min(dc.opacity, 0.998f);

                ribbonMeshes.push_back(rmesh);
                ribbonDCs.push_back(std::move(dc));
                ribbonMeshByDC.emplace(&ribbonDCs.back(), &ribbonMeshes.back());
                transparentDCs.push_back(&ribbonDCs.back());
            }
        }

        // Sort transparent back-to-front by distance.
        RenderSorter::sortBackToFront(transparentDCs, camPos);

        // A3: real instancing applies to the opaque pass only; the transparent pass
        // reuses drawDC with a blend state + per-instance depth sort, so it keeps the
        // per-instance loop (allowInstancing is set false before that pass).
        bool allowInstancing = true;
        auto drawDC = [&](const DrawCall& dc) {
            // A trail carries no mesh asset — its geometry sits in the ribbon
            // pool, looked up by the DrawCall that was synthesised for it.
            const GpuMesh* mesh = nullptr;
            if (!ribbonMeshByDC.empty())
            {
                if (auto it = ribbonMeshByDC.find(&dc); it != ribbonMeshByDC.end())
                    mesh = it->second;
            }
            if (!mesh) mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            const GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.vbuf || !m.ibuf) return;
            // Section draw → its own slice of the index buffer; whole-mesh draw
            // (every one-section mesh, every ribbon) → all of it. Both the
            // graph-material path and the built-in one below draw this range.
            const D3D11IndexRange range = DrawIndexRange(dc, m.indexCount);
            if (range.count == 0) return; // a slot clamped away on the fallback cube

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
                    // The pak's baked HLSL when the export carried one (any D3D tag),
                    // else cross-compile now. resolveShaders just fetched this material,
                    // so the pointer is fresh; it is consumed before anything can load.
                    const MaterialShaderVariant* matPre = HE::MaterialShaderLibrary::precompiledFor(
                        m_contentManager->getMaterial(dc.materialAssetId), HE::RendererBackend::D3D11);
                    D3D11RendererImpl::MatShaders* sh =
                        p.GetOrBuildMaterialShaders(matHash, matFrag, matVertBody, matPre, matTransp);
                    if (sh && sh->vs && sh->ps && sh->il)
                    {
                        // heTex0 = the material's base texture, matching the built-in selection +
                        // hasTex flag: an override material's texture wins (A2), else the mesh's
                        // baked texture (A1), else the white default.
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

                        // heTexP0..3 = the graph's project textures (Texture Sample nodes),
                        // white where a slot is empty or unloadable. The slot list is
                        // snapshotted BEFORE any resolve: a resolve may load, and a load
                        // can move the material asset out from under a held pointer —
                        // which is also why `ma` below is fetched only AFTER this block.
                        ID3D11ShaderResourceView* heTexP[HE::kMatMaxGraphTextures] = {
                            p.dummyTexture.Get(), p.dummyTexture.Get(),
                            p.dummyTexture.Get(), p.dummyTexture.Get() };
                        {
                            HE::UUID    gIds[HE::kMatMaxGraphTextures]{};
                            std::string gPaths[HE::kMatMaxGraphTextures];
                            size_t nTex = 0;
                            if (const MaterialAsset* ma0 = m_contentManager->getMaterial(dc.materialAssetId))
                            {
                                nTex = std::min<size_t>(HE::kMatMaxGraphTextures,
                                    std::max(ma0->graphTexturePaths.size(), ma0->graphTextureIds.size()));
                                for (size_t i = 0; i < nTex; ++i)
                                {
                                    if (i < ma0->graphTextureIds.size())   gIds[i]   = ma0->graphTextureIds[i];
                                    if (i < ma0->graphTexturePaths.size()) gPaths[i] = ma0->graphTexturePaths[i];
                                }
                            }
                            for (size_t i = 0; i < nTex; ++i)
                                if (ID3D11ShaderResourceView* srv = p.resolveGraphTexture(gIds[i], gPaths[i], m_contentManager))
                                    heTexP[i] = srv;
                        }

                        // heLandscapeWeights (t14) = the object's landscape weightmap, PER
                        // DRAW like GL unit 13 / Metal slot 13: it belongs to the terrain
                        // the chunk is part of, not to the material, so two landscapes can
                        // share one material and keep their own paint. Anything that is
                        // not a landscape chunk gets the 1x1 (1,0,0,0) default, so a
                        // Landscape Layer Blend resolves to layer 0 instead of black; the
                        // white dummy is the last resort so a live sample never hits a
                        // null SRV. Resolved HERE, inside the snapshot block's rules: the
                        // resolve may load, and a load can move the material asset —
                        // which is why `ma` below is fetched only after this.
                        ID3D11ShaderResourceView* heWeights = nullptr;
                        if (dc.weightmapTextureId != HE::UUID{})
                            heWeights = p.resolveGraphTexture(dc.weightmapTextureId, {}, m_contentManager);
                        if (!heWeights)
                            heWeights = p.resolveGraphTexture(HE::kDefaultLayer0WeightTextureId, {}, m_contentManager);
                        if (!heWeights) heWeights = p.dummyTexture.Get();

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
                        // heTex0 (t2 PS) + heTexP0..3 (t4..t7 PS) + linear-wrap samplers
                        // (s2 + s4..s7). t3 is intentionally unused by the mesh path.
                        ctx->PSSetShaderResources(2, 1, &heTex0);
                        static_assert(HE::kMatMaxGraphTextures == 4, "heTexP0..3 occupy t4..t7");
                        ctx->PSSetShaderResources(4, 4, heTexP);
                        ID3D11SamplerState* matSamp = p.m_matSampler.Get();
                        ctx->PSSetSamplers(2, 1, &matSamp);
                        ID3D11SamplerState* matSamp4[4] = { matSamp, matSamp, matSamp, matSamp };
                        ctx->PSSetSamplers(4, 4, matSamp4);
                        // heLandscapeWeights (t14 PS) + its linear-CLAMP sampler on s0 —
                        // explicitly, because s0 is where the enclosing built-in pass keeps
                        // its albedo sampler (p.sampler, linear-wrap): without this the
                        // weightmap is filtered with whatever the last built-in pass left
                        // on s0. Restored below. (D3D11MaterialBindings.h, Thema 57)
                        HE::d3d11mat::BindLandscapeWeights(ctx, heWeights, p.m_matWeightSampler.Get());

                        auto drawMatInstance = [&](const glm::mat4& model) {
                            // std140 U block (176 B) at b1 VS.
                            struct MatU { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; glm::vec4 flags; glm::vec4 pbr; };
                            static_assert(sizeof(MatU) == 176, "material U block must be std140 176 B");
                            MatU u;
                            u.mvp   = viewProj * model;
                            u.model = model;
                            // Per-instance tint (RenderObject::instanceTint), as Metal and GL
                            // multiply it in: a graph reads it through Vertex Color (the editor
                            // icons wear their light's colour that way). Identity for the rest.
                            u.color = glm::vec4(dc.baseColor * glm::vec3(dc.instanceTint), 1.0f);
                            u.flags = glm::vec4(matTextured ? 1.0f : 0.0f, 0.0f, 0.0f, 0.0f);
                            u.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity * dc.instanceTint.a, 0.0f);
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
                            ctx->DrawIndexed(range.count, range.start, 0);
                            ++p.counters.draws;
                            p.counters.tris += range.count / 3;
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
                        // restored here too since HeLighting overwrote it. And PS s0: the
                        // weightmap's clamp sampler sat there for the draw, the built-in
                        // shader's albedo sampler (p.sampler, set once per pass) goes back;
                        // t14 (heLandscapeWeights) comes off with it.
                        HE::d3d11mat::RestoreAfterMaterialDraw(ctx, p.sampler.Get());
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
                                 dc.metallic, dc.roughness, dc.opacity,
                                 dc.receivesShadow ? 0.0f : 1.0f);
                    ctx->VSSetShader(p.vsInstanced.Get(), nullptr, 0);
                    ctx->VSSetShaderResources(3, 1, p.instanceSRV.GetAddressOf());
                    ctx->DrawIndexedInstanced(range.count, count, range.start, 0, 0);
                    // Restore the non-instanced VS and unbind t3 before the next draw/Map.
                    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
                    ID3D11ShaderResourceView* nullSRV = nullptr;
                    ctx->VSSetShaderResources(3, 1, &nullSRV);
                    ++p.counters.draws;
                    p.counters.tris += (range.count / 3) * count;
                }
                else
                {
                    for (const glm::mat4& t : dc.instanceTransforms) { // fallback: transparent / ring full
                        uploadObject(viewProj * t, t, dc.baseColor, hasTex,
                                     dc.metallic, dc.roughness, dc.opacity,
                                     dc.receivesShadow ? 0.0f : 1.0f);
                        ctx->DrawIndexed(range.count, range.start, 0);
                        ++p.counters.draws;
                        p.counters.tris += range.count / 3;
                    }
                }
            }
            else {
                uploadObject(viewProj * dc.transform, dc.transform,
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity,
                             dc.receivesShadow ? 0.0f : 1.0f);
                ctx->DrawIndexed(range.count, range.start, 0);
                ++p.counters.draws;
                p.counters.tris += range.count / 3;
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
                             dc.baseColor, hasTex, dc.metallic, dc.roughness, dc.opacity,
                             dc.receivesShadow ? 0.0f : 1.0f);

                // Bind three vertex buffer slots
                const UINT strides[3] = { 32u, 16u, 16u };
                const UINT offs[3]    = { 0u, 0u, 0u };
                ID3D11Buffer* vbs[3] = { sm->vb.Get(), sm->boneIdVb.Get(), sm->boneWgtVb.Get() };
                ctx->IASetVertexBuffers(0, 3, vbs, strides, offs);
                ctx->IASetIndexBuffer(sm->ib.Get(), DXGI_FORMAT_R32_UINT, 0);

                ID3D11ShaderResourceView* albedoSrv = albedo ? albedo : p.dummyTexture.Get();
                ctx->PSSetShaderResources(0, 1, &albedoSrv);

                // Section or whole — a multi-section skinned mesh arrives as one
                // SkinnedDrawCall per slot (GeometryPass), each with its range.
                const D3D11IndexRange range = DrawIndexRange(dc, static_cast<UINT>(sm->indexCount));
                if (range.count == 0) continue;
                ctx->DrawIndexed(range.count, range.start, 0);
                ++p.counters.draws;
                p.counters.tris += static_cast<uint32_t>(range.count / 3);
            }

            // Restore scene VS + layout for the transparent pass
            ctx->VSSetShader(p.vs.Get(), nullptr, 0);
            ctx->IASetInputLayout(p.inputLayout.Get());
        }

        // ── TAA velocity (A2): screen-space motion of the opaque geometry, right
        // after the passes whose depth it tests against and before the decals
        // (which bind that depth as an SRV). Self-restoring.
        if (p.taaFrame && p.taaVelocityRTV)
            p.renderTaaVelocity(ctx, viewProjClean, viewProj, m_contentManager);

        // ── Screen-space decals ──────────────────────────────────────────────
        // After all opaque geometry (its depth is what the decal projects onto)
        // and before the transparent draws — the slot Metal and GL use for their
        // G-buffer decals. Self-restoring; a frame without decals costs one
        // empty() check.
        p.EncodeDecals(ctx, viewProj, width, height, m_contentManager);

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
        // Forward SSR: keep a full-res copy of the finished HDR frame (opaque +
        // sky + transparency) — NEXT frame's trace reprojects its hits into it.
        // Taken here, at the very end of the geometry pass, for the same reason
        // Metal takes it after its scene encoder: earlier and the reflection
        // would show a half-drawn world. The colour target comes off the output
        // merger first — CopyResource must not race a bound RTV.
        if (ssrFrameActive)
        {
            ID3D11RenderTargetView* n = nullptr;
            ctx->OMSetRenderTargets(1, &n, nullptr);
            p.captureSSRColorHistory();
        }
        // Unbind the AO and the reflection SRV before leaving: t16 names a
        // texture that is a render target again in the next frame's trace.
        { ID3D11ShaderResourceView* nullAO = nullptr; ctx->PSSetShaderResources(2, 1, &nullAO);
          ctx->PSSetShaderResources(16, 1, &nullAO); }
    });
}

// The offscreen viewport frame, up to the RGBA8 viewport texture ImGui samples.
// Render() follows it with the swapchain part (ImGui overlay, Present);
// RenderSceneImage() follows it with a readback instead.
void D3D11Renderer::DrawViewportFrame()
{
    auto& p = *m_impl;
    const float bgColor[4] = { 0.0f, 0.0f, 0.0f, 1.0f };

    D3D11_VIEWPORT vvp{};
    vvp.Width    = static_cast<float>(p.viewportW);
    vvp.Height   = static_cast<float>(p.viewportH);
    vvp.MaxDepth = 1.0f;

    // When PostFX is available, render geometry into the RGBA16F HDR target;
    // otherwise fall back to the RGBA8 viewport target directly.
    const bool useHDR = p.postFxReady && p.hdrRTV && p.ldrRTV && p.viewportRTV;
    ID3D11RenderTargetView* sceneRTV = useHDR ? p.hdrRTV.Get() : p.viewportRTV.Get();

    // TAA runs only where this frame's post chain resolves it. Its targets are
    // freed as soon as the mode is off, and the history with them — a stale one
    // would blend against a different world the moment TAA comes back on.
    p.taaFrame = useHDR && p.aaMethod == HE::AAMethod::TAA && p.taaReady()
              && p.ensureTaaTargets(p.viewportW, p.viewportH);
    if (!p.taaFrame && p.taaHistoryTex[0])
    {
        p.destroyTaaTargets();
        p.taaJitter = glm::vec2(0.0f);
    }
    if (p.taaFrame)
    {
        const float zero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };   // zero = "did not move"
        p.context->ClearRenderTargetView(p.taaVelocityRTV.Get(), zero);
    }

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

        // Temporal accumulation on the tonemapped image (A3): (ldr, history[prev],
        // velocity) → history[cur]. The AA-resolve slot below then reads the
        // resolved history instead of ldrSRV. Running on the LDR image keeps a
        // single bright HDR sample from poisoning the next dozen frames.
        ID3D11ShaderResourceView* aaSrc = p.ldrSRV.Get();
        const bool aaTaa = p.taaFrame && p.taaHistoryRTV[0]
                        && p.taaW == p.viewportW && p.taaH == p.viewportH;
        if (aaTaa)
        {
            const int cur  = p.taaHistoryCur;
            const int prev = 1 - cur;
            const float cb[4] = { 1.0f / float(p.viewportW), 1.0f / float(p.viewportH),
                                  p.taaHistoryValid ? HE::kTaaHistoryBlend : 0.0f, 0.0f };
            p.updatePostFxCB(cb);
            p.context->OMSetRenderTargets(1, p.taaHistoryRTV[cur].GetAddressOf(), nullptr);
            p.context->PSSetShader(p.taaResolvePS.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[3] = {
                p.ldrSRV.Get(), p.taaHistorySRV[prev].Get(), p.taaVelocitySRV.Get() };
            p.context->PSSetShaderResources(0, 3, srvs);
            p.context->Draw(3, 0);
            ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr);
            ID3D11ShaderResourceView* nulls[3] = {};
            p.context->PSSetShaderResources(0, 3, nulls);
            // One line per session, so a capture log shows TAA really ran.
            static bool s_taaLogged = false;
            if (!s_taaLogged)
            {
                s_taaLogged = true;
                HE_LOG_INFO(RHI, "D3D11Renderer: TAA resolve active (%ux%u)", p.viewportW, p.viewportH);
            }
            // This frame's result IS next frame's history: flip the ping-pong.
            aaSrc = p.taaHistorySRV[cur].Get();
            p.taaHistoryCur   = prev;
            p.taaHistoryValid = true;
        }

        // AA resolve: ldrSRV (or the TAA result) → viewportRTV (final output
        // sampled by ImGui). Always drawn — the method only picks the pixel
        // shader; TAA's slot is the sharpen the temporal blur asks for.
        { const float cb[4] = { 1.0f / float(p.viewportW),
                                1.0f / float(p.viewportH), aaTaa ? p.aaSharpness : 0.0f, 0 };
          p.updatePostFxCB(cb);
          p.context->OMSetRenderTargets(1, p.viewportRTV.GetAddressOf(), nullptr);
          p.context->PSSetShader(aaTaa                             ? p.taaSharpenPS.Get()
                               : p.aaMethod == HE::AAMethod::Off  ? p.aaBlitPS.Get()
                               : p.aaMethod == HE::AAMethod::SMAA ? p.smaaPS.Get()
                                                                  : p.fxaaPS.Get(), nullptr, 0);
          p.context->PSSetShaderResources(0, 1, &aaSrc);
          p.context->Draw(3, 0);
          ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }

        // Clear stale bindings, restore scene pipeline state for any future draws.
        // t0..t2: the history the sharpen just read is next frame's resolve target.
        { ID3D11ShaderResourceView* nulls[3] = {}; p.context->PSSetShaderResources(0, 3, nulls); }
        p.context->OMSetDepthStencilState(p.depthState.Get(), 0);
        p.context->RSSetState(p.rasterState.Get());
        p.context->PSSetSamplers(0, 1, p.sampler.GetAddressOf());
    }

    // UI canvas pass: draw onto the final composited viewport target (after tonemap/FXAA).
    p.context->OMSetRenderTargets(1, p.viewportRTV.GetAddressOf(), nullptr);
    p.context->RSSetViewports(1, &vvp);
    p.renderUIPass(p.context.Get(), static_cast<int>(p.viewportW), static_cast<int>(p.viewportH));
    { ID3D11RenderTargetView* n = nullptr; p.context->OMSetRenderTargets(1, &n, nullptr); }
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
        DrawViewportFrame();

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
        // No post chain here, so nothing would resolve a jitter (see taaFrame).
        p.taaFrame = false;
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
    // Forward SSR needs an HDR scene target to read radiance out of, and D3D11
    // only has one in the editor viewport path (docs/ssr-cross-backend-plan.md
    // C6). postFxReady is the honest answer: in the swapchain path the switch
    // exists but does nothing, exactly as on Vulkan.
    c.supportsScreenSpaceReflections = m_impl->postFxReady;
    // TAA (A2/A3): velocity pass + temporal resolve + sharpen, on the same
    // editor-viewport post chain SSR needs — false only if a TAA shader failed
    // to compile. The swapchain path renders unjittered either way (taaFrame).
    c.supportsTemporalAA = m_impl->postFxReady && m_impl->taaReady();
    return c;
}

// Screen-space reflections (docs/ssr-cross-backend-plan.md checkpoint C). The
// forward path only — D3D11 has no G-buffer, so kSSRCompositeFS is out of reach
// and the scene shader mixes the one traced texture itself.
void D3D11Renderer::SetSSRSettings(const SSRSettings& s)
{
    auto& p = *m_impl;
    p.ssrEnabled      = s.enabled;
    p.ssrIntensity    = std::clamp(s.intensity, 0.0f, 1.0f);
    p.ssrMaxRoughness = std::clamp(s.maxRoughness, 0.0f, 1.0f);
    p.ssrMaxDistance  = std::max(1.0f, s.maxDistance);
    p.ssrThickness    = std::max(1e-3f, s.thickness);
    p.ssrQuality      = std::clamp(s.quality, 0, 2);
}

void D3D11Renderer::SetShadowSettings(const ShadowSettings& s)
{
    // The array is not touched here: this is called from the editor's frame
    // push, which may land between passes. DrawScene re-creates it at its top
    // when the size no longer matches (and hands the extractor the size that
    // is actually allocated). Mirrors GL/Metal.
    auto& p = *m_impl;
    p.shadowSizeDirty |= (s.resolution != p.shadowSettings.resolution);
    p.shadowSettings   = s;
}

void D3D11Renderer::SetShadowDebug(bool on)
{
    m_impl->debugShadowCascades = on;
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

// ─── One still from somebody else's camera ────────────────────────────────────
// The contract is in IRenderer.h; the shape is Metal's (MetalRenderer.mm,
// RenderSceneImage): the live viewport pair is SET ASIDE (moved out of the
// members, not released — createViewportRT starts by resetting whatever is in
// them), createViewportRT builds a fresh pair at the requested size, one
// DrawViewportFrame fills it (the same frame Render() draws, minus the
// swapchain part: no overlay, no Present), CaptureViewport reads it back, the
// pair is released and the live pair moved back. GetViewportTexture never
// answers with the screenshot's texture, so the UI built later this frame
// shows the editor's own view, not the client's.
//
// Two things this backend has to do that Metal and GL get for free:
//
// * The HDR/LDR/bloom/SSAO targets are built ONLY by createViewportRT, and
//   Render() only calls that when the request differs from the live size —
//   which it will not, once the live size is back. So the intermediates are
//   rebuilt at the live size here, or the next real frame would draw a
//   request-sized HDR image into the live viewport.
// * The per-camera temporal state is TAA's history and forward SSR's (the
//   previous frame's HDR copy plus its half-res ping-pong). Both get GL's TAA
//   rule: invalid before the still (explicitly for TAA — a request at exactly
//   the viewport's size keeps its targets; implicitly for SSR, createHDRTargets
//   drops the copy and destroySSRTargets the ping-pong) and invalid after, so
//   the screenshot does not blend or reflect the viewport's past and the
//   viewport not the screenshot's. One unconverged / SSR-less frame on each
//   side, no ghost. GI probe history is world-space and stays.
//
// Not run: the GPU-timer frame, the overlay, Present, the counter reset —
// those belong to REAL frames. Per-request path, not per-frame.
bool D3D11Renderer::RenderSceneImage(const EditorCameraOverride& camera, uint32_t width,
                                     uint32_t height, std::vector<uint8_t>& rgba)
{
    auto& p = *m_impl;
    if (!p.device || !p.context) return false;
    if (width == 0 || height == 0) return false;

    // Everything the frame reads that the request changes, saved by value.
    ComPtr<ID3D11Texture2D>          liveTex      = std::move(p.viewportTex);
    ComPtr<ID3D11RenderTargetView>   liveRTV      = std::move(p.viewportRTV);
    ComPtr<ID3D11ShaderResourceView> liveSRV      = std::move(p.viewportSRV);
    ComPtr<ID3D11Texture2D>          liveDepth    = std::move(p.viewportDepth);
    ComPtr<ID3D11DepthStencilView>   liveDSV      = std::move(p.viewportDSV);
    ComPtr<ID3D11ShaderResourceView> liveDepthSRV = std::move(p.viewportDepthSRV);
    const uint32_t                   liveW        = p.viewportW;
    const uint32_t                   liveH        = p.viewportH;
    const uint32_t                   liveReqW     = p.viewportReqW;
    const uint32_t                   liveReqH     = p.viewportReqH;
    const EditorCameraOverride       liveCam      = m_editorCamera;
    // The counters are the profiler's picture of the last real frame; the
    // screenshot's draws would sit there until the next Render() otherwise.
    const D3D11RendererImpl::FrameCounters liveCounters = p.counters;

    p.viewportW    = 0;
    p.viewportH    = 0;
    p.viewportReqW = width;
    p.viewportReqH = height;
    m_editorCamera = camera;

    // Fresh pair at the request (plus HDR & co. at that size; SSR history gone).
    p.createViewportRT(width, height);
    p.taaHistoryValid = false;

    bool ok = p.viewportRTV && p.viewportDSV;
    if (ok)
    {
        DrawViewportFrame();
        // The staging Map waits for the GPU; no world → the cleared target,
        // honestly black, which is what the viewport would show too.
        uint32_t gotW = 0, gotH = 0;
        ok = CaptureViewport(rgba, gotW, gotH) && gotW == width && gotH == height;
    }

    // The screenshot pair is released the way createViewportRT releases every
    // viewport target (the runtime keeps in-flight references alive); the live
    // pair comes straight back.
    p.viewportTex.Reset(); p.viewportRTV.Reset(); p.viewportSRV.Reset();
    p.viewportDepth.Reset(); p.viewportDSV.Reset(); p.viewportDepthSRV.Reset();
    p.viewportTex      = std::move(liveTex);
    p.viewportRTV      = std::move(liveRTV);
    p.viewportSRV      = std::move(liveSRV);
    p.viewportDepth    = std::move(liveDepth);
    p.viewportDSV      = std::move(liveDSV);
    p.viewportDepthSRV = std::move(liveDepthSRV);
    p.viewportW        = liveW;
    p.viewportH        = liveH;
    p.viewportReqW     = liveReqW;
    p.viewportReqH     = liveReqH;
    m_editorCamera     = liveCam;
    p.counters         = liveCounters;
    // Intermediates back to the live size (see above). No live viewport (the
    // direct-to-swapchain path) means nothing reads them; leave them.
    if (liveW && liveH && (liveW != width || liveH != height))
        p.createHDRTargets(liveW, liveH);
    // The SSR history is the screenshot camera's now (a same-size request kept
    // the targets, and the frame just captured into them). So is TAA's: the
    // still's resolve marked it valid.
    p.ssrColorHistValid = false;
    p.ssrHistValid      = false;
    p.taaHistoryValid   = false;

    if (!ok) rgba.clear();
    return ok;
}

// ─── Any world into a preview target ──────────────────────────────────────────
// The contract is in IRenderer.h; the steps are GL's RenderWorldPreview
// (OpenGLRenderer.cpp) one for one: its OWN extractor (m_extractor carries the
// scene's day-night state), sky first without depth, grid lines that write no
// depth, static meshes, skinned meshes, then the scene's tonemap into the RGBA8
// texture ImGui samples. Same lighting numbers (kWorldPreviewPSHLSL), same grid
// (WorldPreviewGrid.h), same background.
//
// Called while the editor builds its UI, i.e. between frames. The immediate
// context's target and viewport are put back anyway, so a call from anywhere
// else cannot leave the next pass drawing into a preview texture.
void* D3D11Renderer::RenderWorldPreview(ContentManager& cm, HorizonWorld& world,
                                        uint32_t width, uint32_t height,
                                        const EditorCameraOverride& camera,
                                        const glm::vec3& origin,
                                        const WorldPreviewEnv& env,
                                        glm::mat4* outViewProj,
                                        uint32_t slot)
{
    auto& p = *m_impl;
    if (!p.device || !p.context) return nullptr;
    const int W = std::clamp(static_cast<int>(width),  32, 4096);
    const int H = std::clamp(static_cast<int>(height), 32, 4096);
    if (!m_contentManager) m_contentManager = &cm;
    if (!p.ensureWorldPreviewPipeline()) return nullptr;
    D3D11RendererImpl::WorldPreviewTarget& wp = p.worldPreview[std::min(slot, kWorldPreviewSlots - 1)];
    if (!p.ensureWorldPreviewTarget(wp, W, H)) return nullptr;

    // Camera, snapshot, sky and light — shared with D3D12/Vulkan. viewProj is
    // GL clip (what the caller rebuilds with worldPreviewProjection); the draws
    // get it with the D3D depth fix applied exactly once.
    HE::WorldPreviewFrame frame;
    HE::buildWorldPreviewFrame(m_contentManager, world, camera, env,
                               static_cast<float>(W) / static_cast<float>(H), frame);
    const RenderWorld& snapshot = frame.snapshot;
    const glm::mat4    viewProj = frame.viewProj;
    const glm::mat4    drawVP   = HE::kD3DClipFix * viewProj;
    const glm::vec3    camPos   = frame.camPos;
    if (outViewProj) *outViewProj = viewProj;

    ID3D11DeviceContext* ctx = p.context.Get();
    ComPtr<ID3D11RenderTargetView> prevRTV;
    ComPtr<ID3D11DepthStencilView> prevDSV;
    ctx->OMGetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.GetAddressOf());
    UINT prevVPCount = 1;
    D3D11_VIEWPORT prevVP{};
    ctx->RSGetViewports(&prevVPCount, &prevVP);

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(W);
    vp.Height   = static_cast<float>(H);
    vp.MaxDepth = 1.0f;
    // Studio background, LINEAR — the tonemap below lifts it (kPreviewBackground).
    const float clear[4] = { HE::kPreviewBackground[0], HE::kPreviewBackground[1],
                             HE::kPreviewBackground[2], 1.0f };
    ctx->OMSetRenderTargets(1, wp.hdrRTV.GetAddressOf(), wp.dsv.Get());
    ctx->ClearRenderTargetView(wp.hdrRTV.Get(), clear);
    ctx->ClearDepthStencilView(wp.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);
    ctx->RSSetViewports(1, &vp);
    ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);

    // ── Sky FIRST, unlike the scene: the grid writes no depth, so a sky drawn
    // after it would paint straight over it. Clock stopped (HE_SKY_TIME for a
    // reproducible headless shot), as on Metal — nothing drifts in a preview.
    if (env.sky && p.skyReady)
    {
        float skyClock = 0.0f;
        if (const char* ov = std::getenv("HE_SKY_TIME"); ov && *ov)
            skyClock = static_cast<float>(std::atof(ov));
        p.drawSky(ctx, glm::inverse(viewProj), snapshot.sunDirection, frame.sky, camPos, skyClock);
    }

    // ── Grid + origin marker: lines only, no depth writes, so neither the
    // underside of the mesh nor (with a sky) the lower half of the world is
    // hidden.
    if (env.grid && p.debugReady)
    {
        std::vector<float> verts;
        HE::buildPreviewGrid(HE::worldPreviewGridExtent(camPos, origin), 1.0f, verts, origin);
        std::vector<DebugLine> lines;
        lines.reserve(verts.size() / 12);
        for (size_t i = 0; i + 11 < verts.size(); i += 12)
            lines.push_back({ { verts[i],     verts[i + 1], verts[i + 2] },
                              { verts[i + 6], verts[i + 7], verts[i + 8] },
                              { verts[i + 3], verts[i + 4], verts[i + 5] } });
        p.drawDebugLines(ctx, drawVP, lines, p.depthReadOnlyState.Get());
    }

    auto uploadObject = [&](const glm::mat4& model, const glm::vec3& color, bool hasTex,
                            float metallic, float roughness)
    {
        PerObjectCB o{};
        o.mvp   = drawVP * model;
        o.model = model;
        o.color = glm::vec4(color, hasTex ? 1.0f : 0.0f);
        o.pbr   = glm::vec4(metallic, roughness, 1.0f, 0.0f);
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(p.perObjectCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &o, sizeof(o));
            ctx->Unmap(p.perObjectCB.Get(), 0);
        }
    };
    {
        const glm::vec4 light[4] = { glm::vec4(camPos, 1.0f), frame.sun,
                                     glm::vec4(frame.sunColor, 1.0f), glm::vec4(frame.ambient, 1.0f) };
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(p.previewLightCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, light, sizeof(light));
            ctx->Unmap(p.previewLightCB.Get(), 0);
        }
    }
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
    ctx->RSSetState(p.rasterState.Get());   // the scene's: no culling (winding not guaranteed), depth clip on
    ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
    ctx->PSSetConstantBuffers(1, 1, p.previewLightCB.GetAddressOf());
    ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());

    // ── Static meshes.
    ctx->IASetInputLayout(p.inputLayout.Get());
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.previewMeshPS.Get(), nullptr, 0);
    for (const RenderObject& obj : snapshot.objects)
    {
        const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager);
        if (!mesh || !mesh->vbuf || !mesh->ibuf || mesh->indexCount == 0) continue;
        uploadObject(obj.transform, obj.baseColor, mesh->texture.Get() != nullptr, obj.metallic, obj.roughness);
        ID3D11ShaderResourceView* srv = mesh->texture ? mesh->texture.Get() : p.dummyTexture.Get();
        ctx->PSSetShaderResources(0, 1, &srv);
        const UINT stride = 8 * sizeof(float), offset = 0;
        ctx->IASetVertexBuffers(0, 1, mesh->vbuf.GetAddressOf(), &stride, &offset);
        ctx->IASetIndexBuffer(mesh->ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
        ctx->DrawIndexed(mesh->indexCount, 0, 0);
    }

    // ── Skinned meshes (the pose the AnimatorHost last wrote, or the bind pose).
    if (p.skinnedVS && p.skinnedLayout && p.bonesCB && !snapshot.skinnedObjects.empty())
    {
        constexpr int kMaxBones = 128;
        ctx->IASetInputLayout(p.skinnedLayout.Get());
        ctx->VSSetShader(p.skinnedVS.Get(), nullptr, 0);
        ctx->PSSetShader(p.previewSkinnedPS.Get(), nullptr, 0);
        ctx->VSSetConstantBuffers(2, 1, p.bonesCB.GetAddressOf());
        std::vector<glm::mat4> bones(kMaxBones);
        for (const SkinnedRenderObject& obj : snapshot.skinnedObjects)
        {
            const GpuSkeletalMesh* smesh = p.resolveSkeletalMesh(obj.meshAssetId, m_contentManager);
            if (!smesh || !smesh->vb || !smesh->ib || smesh->indexCount <= 0) continue;
            std::fill(bones.begin(), bones.end(), glm::mat4(1.0f));
            const size_t boneCount = std::min(obj.boneMatrices.size(), static_cast<size_t>(kMaxBones));
            std::copy_n(obj.boneMatrices.begin(), boneCount, bones.begin());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(p.bonesCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
            {
                std::memcpy(m.pData, bones.data(), kMaxBones * sizeof(glm::mat4));
                ctx->Unmap(p.bonesCB.Get(), 0);
            }
            // Pbr zero: GL's skinned preview has no highlight to drive.
            uploadObject(obj.transform, obj.baseColor, smesh->srv.Get() != nullptr, 0.0f, 0.0f);
            ID3D11ShaderResourceView* srv = smesh->srv ? smesh->srv.Get() : p.dummyTexture.Get();
            ctx->PSSetShaderResources(0, 1, &srv);
            ID3D11Buffer* vbs[3]     = { smesh->vb.Get(), smesh->boneIdVb.Get(), smesh->boneWgtVb.Get() };
            const UINT    strides[3] = { 8 * sizeof(float), 4 * sizeof(uint32_t), 4 * sizeof(float) };
            const UINT    offsets[3] = { 0, 0, 0 };
            ctx->IASetVertexBuffers(0, 3, vbs, strides, offsets);
            ctx->IASetIndexBuffer(smesh->ib.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(static_cast<UINT>(smesh->indexCount), 0, 0);
        }
    }

    // ── Tonemap resolve: HDR → the RGBA8 texture ImGui shows, through the
    // scene's own tonemap. Bloom at strength 0 (a preview is not a film
    // camera); the second slot still needs something bound.
    {
        ID3D11RenderTargetView* nullRTV = nullptr;
        ctx->OMSetRenderTargets(1, &nullRTV, nullptr);
        const float cb[4] = { 1.0f, 0.0f, 0.0f, 0.0f };
        p.updatePostFxCB(cb);
        ctx->OMSetRenderTargets(1, wp.ldrRTV.GetAddressOf(), nullptr);
        ctx->IASetInputLayout(nullptr);
        ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ctx->VSSetShader(p.fsVS.Get(), nullptr, 0);
        ctx->PSSetShader(p.tonemapPS.Get(), nullptr, 0);
        ctx->OMSetDepthStencilState(p.noDepthDSS.Get(), 0);
        ctx->RSSetState(p.fsRastState.Get());
        ctx->PSSetSamplers(0, 1, p.linearSampler.GetAddressOf());
        ctx->VSSetConstantBuffers(0, 1, p.postFxCB.GetAddressOf());
        ctx->PSSetConstantBuffers(0, 1, p.postFxCB.GetAddressOf());
        ID3D11ShaderResourceView* srvs[2] = { wp.hdrSRV.Get(), p.dummyTexture.Get() };
        ctx->PSSetShaderResources(0, 2, srvs);
        ctx->Draw(3, 0);
        ID3D11ShaderResourceView* nulls[2] = {};
        ctx->PSSetShaderResources(0, 2, nulls);
    }

    // Headless witness (HE_WORLD_PREVIEW_DUMP=<file.ppm>), same convention as
    // GL/Metal: the LDR result, i.e. what the editor shows.
    if (const char* dp = std::getenv("HE_WORLD_PREVIEW_DUMP"); dp && *dp)
    {
        D3D11_TEXTURE2D_DESC sd{};
        wp.ldrTex->GetDesc(&sd);
        sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
        ComPtr<ID3D11Texture2D> staging;
        if (SUCCEEDED(p.device->CreateTexture2D(&sd, nullptr, &staging)))
        {
            ctx->CopyResource(staging.Get(), wp.ldrTex.Get());
            D3D11_MAPPED_SUBRESOURCE m{};
            if (SUCCEEDED(ctx->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m)))
            {
                HE::writeWorldPreviewDump(dp, static_cast<const uint8_t*>(m.pData), W, H, m.RowPitch);
                ctx->Unmap(staging.Get(), 0);
            }
        }
    }

    // Scene state back: the next DrawScene assumes its pipeline is bound.
    ctx->OMSetRenderTargets(1, prevRTV.GetAddressOf(), prevDSV.Get());
    if (prevVPCount > 0) ctx->RSSetViewports(1, &prevVP);
    ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
    ctx->RSSetState(p.rasterState.Get());
    ctx->PSSetSamplers(0, 1, p.sampler.GetAddressOf());
    ctx->IASetInputLayout(p.inputLayout.Get());
    ctx->VSSetShader(p.vs.Get(), nullptr, 0);
    ctx->PSSetShader(p.ps.Get(), nullptr, 0);
    ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
    ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
    ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
    ctx->PSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
    return wp.ldrSRV.Get();
}

void D3D11Renderer::SetVSync(bool enabled)
{
    HE_LOG_INFO(RHI, "%s", enabled ? "D3D11Renderer: VSync enabled" : "D3D11Renderer: VSync disabled");
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

void D3D11Renderer::SetAntiAliasingSettings(const AntiAliasingSettings& s)
{
    m_impl->aaMethod    = IRenderer::ResolveAAMethod(s.method, GetCapabilities());
    m_impl->aaSharpness = std::clamp(s.sharpness, 0.0f, 1.0f);
}

void D3D11Renderer::InvalidateMaterial(const HE::UUID& materialId)
{
    // Deferred to the next DrawScene (same thread), where the cache is safe to touch.
    if (m_impl && materialId != HE::UUID{})
        m_impl->pendingMatInval.push_back(materialId);
}

void D3D11Renderer::WarmupMaterials(const std::vector<HE::UUID>& materialIds)
{
    // Queued for the next DrawScene (same deferral as the invalidations, same thread
    // guarantee). Non-material ids — the streaming poll hands over everything it
    // registered — resolve no shader in the drain and cost a map lookup each.
    if (!m_impl) return;
    for (const HE::UUID& id : materialIds)
        if (id != HE::UUID{}) m_impl->pendingMatWarmup.push_back(id);
}

void D3D11Renderer::InvalidateMesh(const HE::UUID& meshId)
{
    if (m_impl && meshId != HE::UUID{})
        m_impl->pendingMeshInval.push_back(meshId);
}

void D3D11Renderer::InvalidateTexture(const HE::UUID& textureId)
{
    // Same deferral — the graph-texture cache is keyed by "hi:lo" for UUIDs.
    if (m_impl && textureId != HE::UUID{})
        m_impl->pendingTexInval.push_back(textureId);
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
