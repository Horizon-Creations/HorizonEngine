#include "Backends/D3D12/D3D12Renderer.h"
#include <Window/Window.h>
#include <ContentManager/ContentManager.h>
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
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>
#include <cmath>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;
static constexpr UINT k_frameCount = 2;
static constexpr UINT k_maxDraws   = 4096;          // per-object CB ring capacity
static constexpr UINT k_cbSlot     = 256;           // CBV alignment

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
    float    uMilkyWay;     float  uFlash; int uHasMoonTex; float _skyPad;
};
Texture2D    uMoonTex   : register(t0);
SamplerState uSkyLinear : register(s0);

float starHash(float3 p){p=frac(p*0.1031f);p+=dot(p,p.zyx+31.32f);return frac((p.x+p.y)*p.z);}
float starNoise3(float3 p){float3 i=floor(p),f=frac(p),u=f*f*(3.0f-2.0f*f);return lerp(lerp(lerp(starHash(i),starHash(i+float3(1,0,0)),u.x),lerp(starHash(i+float3(0,1,0)),starHash(i+float3(1,1,0)),u.x),u.y),lerp(lerp(starHash(i+float3(0,0,1)),starHash(i+float3(1,0,1)),u.x),lerp(starHash(i+float3(0,1,1)),starHash(i+float3(1,1,1)),u.x),u.y),u.z);}
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
float3 applyClouds(float3 baseSky,float3 dir,float3 sunDir,float t,float coverage,float3 sunColor,float3 wind)
{
    if(coverage<=0.0f||dir.y<0.02f)return baseSky;
    const int N=8;
    float sBase=1.0f/max(dir.y,1e-3f),sTop=2.6f/max(dir.y,1e-3f),ds=(sTop-sBase)/float(N);
    float jitter=cloudHash(dir.xz*173.3f+dir.y);
    float sunY=clamp(sunDir.y,-0.2f,1.0f),day=smoothstep(-0.10f,0.10f,sunY);
    float dusk=smoothstep(-0.06f,0.05f,sunY)*(1.0f-smoothstep(0.05f,0.28f,sunY));
    float costh=max(dot(dir,sunDir),0.0f),g=0.6f,g2=g*g;
    float phase=(1.0f-g2)/(12.566371f*pow(max(1.0f+g2-2.0f*g*costh,1e-4f),1.5f));
    float T=1.0f;float3 L=(float3)0;
    for(int i=0;i<N;++i){
        float s=sBase+(float(i)+jitter)*ds;float3 pos=dir*s;
        float lo=lerp(0.70f,0.22f,clamp(coverage,0.0f,1.0f));
        float dens=smoothstep(lo,lo+0.13f,cloudFbm(pos.xz*1.2f+wind.xz*t));
        if(dens>0.001f){
            float sh=exp(-smoothstep(lo,lo+0.13f,cloudFbm((pos+sunDir*0.5f).xz*1.2f+wind.xz*t))*1.7f);
            float powder=1.0f-exp(-dens*3.0f),lit=sh*powder;
            float3 dayCol=lerp(float3(0.17f,0.20f,0.29f),sunColor*1.12f,lit);
            float3 nightCol=lerp(float3(0.015f,0.018f,0.035f),float3(0.26f,0.29f,0.45f),lit);
            float3 cc=lerp(nightCol,dayCol,day);
            cc=lerp(cc,sunColor*float3(1.25f,0.55f,0.28f),dusk*lit*0.9f);
            cc+=sunColor*(phase*sh*0.9f*max(day,dusk));
            float hT=smoothstep(1.0f,2.6f,pos.y);cc*=lerp(0.5f,1.15f,hT);
            float a=1.0f-exp(-dens*ds*7.0f);L+=T*a*cc;T*=1.0f-a;if(T<0.02f)break;
        }
    }
    float horizon=smoothstep(0.02f,0.16f,dir.y);T=1.0f-(1.0f-T)*horizon;L*=horizon;
    return baseSky*T+L;
}

struct SkyVSOut { float4 pos : SV_POSITION; float2 ndc : TEXCOORD0; };
float4 PSSky(SkyVSOut i) : SV_TARGET
{
    float4 wp1=mul(uInvViewProj,float4(i.ndc,1.0f,1.0f));
    float4 wp0=mul(uInvViewProj,float4(i.ndc,0.0f,1.0f));
    float3 dir=wp1.xyz/wp1.w - wp0.xyz/wp0.w;
    float3 col=skyColor(dir,uSunDir);
    float nightF=1.0f-smoothstep(-0.10f,0.10f,clamp(normalize(uSunDir).y,-0.2f,1.0f));
    if(nightF>0.0f){
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
    float4   uColor;    // rgb = base color, a unused
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
    float4   uSunDir;   // xyz = sun direction
    float4   uFog;      // x=fogDensity, y=fogHeightFalloff
};
Texture2D    uShadowMap : register(t0);
SamplerState uShadowSamp : register(s0);

struct VSIn  { float3 pos : POSITION; float3 normal : NORMAL; float2 uv : TEXCOORD0; };
struct VSOut { float4 clip : SV_POSITION; float3 worldPos : TEXCOORD0; float3 normal : TEXCOORD1; };

VSOut VSMain(VSIn i)
{
    VSOut o;
    o.worldPos = mul(uModel, float4(i.pos, 1.0)).xyz;
    o.normal   = mul((float3x3)uModel, i.normal);
    o.clip     = mul(uMVP, float4(i.pos, 1.0));
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
    float3 base  = uColor.rgb;
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
    float3 result  = ambDiff * 0.35f + ambSpec * (1.0f - 0.6f * rough);
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
        glm::vec4  sunDir;   // xyz = sun direction
        glm::vec4  fog;      // x=fogDensity, y=fogHeightFalloff
    };

    struct SkyCB {
        glm::mat4 invViewProj;
        glm::vec3 sunDir;    float timeOfDay;
        glm::vec3 sunColor;  float cloudCoverage;
        glm::vec3 wind;      float time;
        glm::vec3 auroraColor; float aurora;
        float milkyWay;      float flash; int hasMoonTex; float _pad;
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
    };

    UINT alignUp(UINT v, UINT a) { return (v + a - 1u) & ~(a - 1u); }
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
    UINT64                            fenceValues[k_frameCount]{};
    UINT                              rtvDescSize = 0;
    UINT                              frameIndex  = 0;
    bool                              vsync       = true;
    int                               width = 0, height = 0;

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

    void createViewportRT(UINT w, UINT h)
    {
        // Wait for GPU to finish using old resources before destroying them.
        waitForAllFrames();

        viewportRT.Reset();
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
        ccv.Color[0] = 0.18f; ccv.Color[1] = 0.18f; ccv.Color[2] = 0.20f; ccv.Color[3] = 1.0f;
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
            device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvH));
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
        device->CreateShaderResourceView(nullptr, nullptr, srvCpuHandle(0));

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

        // Root sig: [0] CBV b0 (sky env), [1] SRV table t0 (moon), static sampler s0.
        {
            D3D12_DESCRIPTOR_RANGE r{};
            r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV; r.NumDescriptors = 1; r.BaseShaderRegister = 0;
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
            D3D12_ROOT_SIGNATURE_DESC rsd{};
            rsd.NumParameters = 2; rsd.pParameters = params;
            rsd.NumStaticSamplers = 1; rsd.pStaticSamplers = &samp;
            ComPtr<ID3DBlob> sig, err;
            if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)) ||
                FAILED(device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                       IID_PPV_ARGS(&skyRootSig))))
                return false;
        }

        // SRV heap for moon texture (1 slot). Create a null SRV as placeholder.
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd{};
            hd.NumDescriptors = 1;
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
        }

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
        auto* pso12 = usingHDR ? skyHdrPso.Get() : (skyLdrPso ? skyLdrPso.Get() : nullptr);
        if (!pso12) return;
        SkyCB cb{};
        cb.invViewProj = invVP;
        cb.sunDir    = sunDir;       cb.timeOfDay     = env.timeOfDay;
        cb.sunColor  = env.sunColor; cb.cloudCoverage = env.cloudCoverage;
        cb.wind = glm::vec3(
            std::sin(glm::radians(env.windDirection)) * env.windSpeed,
            0.0f,
            std::cos(glm::radians(env.windDirection)) * env.windSpeed);
        cb.time      = m_wallTime;
        cb.auroraColor  = env.auroraColor; cb.aurora    = env.auroraIntensity;
        cb.milkyWay     = env.milkyWayIntensity;
        cb.flash        = env.flash;
        cb.hasMoonTex   = moonTex12 ? 1 : 0;
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
        cl->Draw(3, 0, 0, 0);
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
        cl->Draw(static_cast<UINT>(lines.size() * 2), 0, 0, 0);
    }

    // ── Scene pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  rootSig;
    ComPtr<ID3D12PipelineState>  pso;
    ComPtr<ID3D12PipelineState>  transparentPSO; // alpha-blend, depth read-only
    ComPtr<ID3D12Resource>       perFrameCB[k_frameCount];   // upload, persistently mapped
    uint8_t*                     perFramePtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       perObjectRing[k_frameCount]; // upload, persistently mapped
    uint8_t*                     perObjectPtr[k_frameCount]{};

    GpuMesh cube;

    // ── Sky pipeline ─────────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  skyRootSig;
    ComPtr<ID3D12PipelineState>  skyHdrPso;        // RGBA16F target
    ComPtr<ID3D12PipelineState>  skyLdrPso;        // RGBA8 target
    ComPtr<ID3D12Resource>       skyCBuf[k_frameCount];
    uint8_t*                     skyCBufPtr[k_frameCount]{};
    ComPtr<ID3D12DescriptorHeap> skyEnvHeap;       // 1-slot SRV heap for moon
    ComPtr<ID3D12Resource>       moonTex12;
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
        for (UINT i = 0; i < k_frameCount; ++i)
        {
            const UINT64 val = ++fenceValues[i];
            cmdQueue->Signal(fence.Get(), val);
            if (fence->GetCompletedValue() < val)
            {
                fence->SetEventOnCompletion(val, fenceEvent);
                WaitForSingleObject(fenceEvent, INFINITE);
            }
        }
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
        }
    }

    bool createPipeline()
    {
        // Root signature: two root CBVs (b0 per-object, b1 per-frame) + an SRV
        // table (t0, shadow map) + a static sampler (s0).
        D3D12_DESCRIPTOR_RANGE srvRange{};
        srvRange.RangeType          = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        srvRange.NumDescriptors     = 1;
        srvRange.BaseShaderRegister = 0; // t0

        D3D12_ROOT_PARAMETER params[3]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor       = { 0, 0 }; // b0
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor       = { 1, 0 }; // b1
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[2].ParameterType                       = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2].DescriptorTable.NumDescriptorRanges = 1;
        params[2].DescriptorTable.pDescriptorRanges   = &srvRange;
        params[2].ShaderVisibility                    = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_STATIC_SAMPLER_DESC samp{};
        samp.Filter         = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        samp.ShaderRegister = 0; // s0
        samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters     = 3;
        rsd.pParameters       = params;
        rsd.NumStaticSamplers = 1;
        rsd.pStaticSamplers   = &samp;
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
        }
        createSkyPipeline();
        createDebugLinePipeline();
        return rootSig && pso;
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

    const GpuMesh* resolveMesh(const HE::UUID& assetId, ContentManager* cm)
    {
        if (assetId == HE::UUID{} || !cm) return nullptr;
        if (auto it = meshCache.find(assetId); it != meshCache.end()) return &it->second;

        const StaticMeshAsset* asset = cm->getStaticMesh(assetId);
        if (!asset || asset->vertices.empty() || asset->indices.empty()) return nullptr;

        const size_t vertexCount = asset->vertices.size() / 3;
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
        GpuMesh mesh;
        mesh.localBounds = HE::AABB::fromPositions(asset->vertices.data(), vertexCount);
        uploadMesh(mesh, interleaved, asset->indices);
        return &meshCache.emplace(assetId, mesh).first->second;
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

#ifdef _DEBUG
    {
        ComPtr<ID3D12Debug> debugController;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debugController))))
            debugController->EnableDebugLayer();
    }
#endif

    if (FAILED(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&m_impl->device))))
        throw std::runtime_error("D3D12Renderer: D3D12CreateDevice failed");

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(m_impl->device->CreateCommandQueue(&qd, IID_PPV_ARGS(&m_impl->cmdQueue))))
        throw std::runtime_error("D3D12Renderer: CreateCommandQueue failed");

    ComPtr<IDXGIFactory4> factory;
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))))
        throw std::runtime_error("D3D12Renderer: CreateDXGIFactory2 failed");

    DXGI_SWAP_CHAIN_DESC1 scd{};
    scd.BufferCount      = k_frameCount;
    scd.Flags            = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
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
    m_impl->cube = {};
    m_impl->viewportRT.Reset();
    m_impl->viewportDepth.Reset();
    m_impl->viewportReadback.Reset();
    m_impl->viewportRtvHeap.Reset();
    m_impl->viewportDsvHeap.Reset();
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
    }
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

    p.m_extractor.extract(*m_world, p.m_renderWorld,
                          static_cast<float>(width) / static_cast<float>(height),
                          &m_editorCamera);
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
    const glm::mat4 invVP     = glm::inverse(viewProj);
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
        if (p.perFramePtr[p.frameIndex])
            std::memcpy(p.perFramePtr[p.frameIndex], &f, sizeof(f));
    }

    cl->SetGraphicsRootSignature(p.rootSig.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());

    const D3D12_GPU_VIRTUAL_ADDRESS ringBase = p.perObjectRing[p.frameIndex]->GetGPUVirtualAddress();
    uint8_t* ringPtr = p.perObjectPtr[p.frameIndex];

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

        // Sky background: draw before geometry so depth test kills it behind meshes.
        // (hdrRT clear was already done by Render(); sky writes to same bound RTV.)
        p.drawSky(cl, p.frameIndex, invVP, p.m_renderWorld.sunDirection, m_environment);

        // Restore scene root signature + per-frame CBV after sky pass changed them.
        cl->SetGraphicsRootSignature(p.rootSig.Get());
        cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());

        // ── Geometry pass: scene PSO + shadow map bound for sampling ────────
        if (p.shadowSrvHeap)
        {
            ID3D12DescriptorHeap* heaps[] = { p.shadowSrvHeap.Get() };
            cl->SetDescriptorHeaps(1, heaps);
            cl->SetGraphicsRootDescriptorTable(2, p.shadowSrvHeap->GetGPUDescriptorHandleForHeapStart());
        }

        const glm::vec3 camPos = p.m_renderWorld.camera.position;
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        for (const DrawCall& dc : cmds.drawCalls())
            (dc.opacity < 0.999f ? transparentDCs : opaqueDCs).push_back(&dc);
        std::sort(transparentDCs.begin(), transparentDCs.end(),
            [&](const DrawCall* a, const DrawCall* b) {
                return glm::length(glm::vec3(a->transform[3]) - camPos) >
                       glm::length(glm::vec3(b->transform[3]) - camPos);
            });

        auto drawDC12 = [&](const DrawCall& dc) {
            if (drawIdx >= k_maxDraws) return;
            const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            const GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.indexCount) return;
            cl->IASetVertexBuffers(0, 1, &m.vbv);
            cl->IASetIndexBuffer(&m.ibv);
            auto drawOne = [&](const glm::mat4& model) {
                if (drawIdx >= k_maxDraws) return;
                PerObjectCB o{};
                o.mvp   = viewProj * model;
                o.model = model;
                o.color = glm::vec4(dc.baseColor, 0.0f);
                o.pbr   = glm::vec4(dc.metallic, dc.roughness, dc.opacity, 0.0f);
                if (ringPtr)
                    std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
                cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);
                cl->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
                ++drawIdx;
            };
            if (!dc.instanceTransforms.empty())
                for (const glm::mat4& t : dc.instanceTransforms) drawOne(t);
            else
                drawOne(dc.transform);
        };

        auto* scenePso = p.usingHDR && p.hdrPso ? p.hdrPso.Get() : p.pso.Get();
        auto* transePso = p.usingHDR && p.hdrTransparentPso ? p.hdrTransparentPso.Get() : p.transparentPSO.Get();
        cl->SetPipelineState(scenePso);
        for (const DrawCall* dc : opaqueDCs) drawDC12(*dc);

        if (!transparentDCs.empty() && transePso) {
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

    // Resize viewport RT if the editor requested a different size.
    if (p.viewportReqW > 0 && p.viewportReqH > 0 &&
        (p.viewportReqW != p.viewportW || p.viewportReqH != p.viewportH))
        p.createViewportRT(p.viewportReqW, p.viewportReqH);

    const bool useViewport = p.viewportRT && p.viewportW > 0 && p.viewportH > 0;

    p.waitForFrame(p.frameIndex);
    p.cmdAllocators[p.frameIndex]->Reset();
    p.cmdList->Reset(p.cmdAllocators[p.frameIndex].Get(), nullptr);

    const float bgColor[4] = { 0.18f, 0.18f, 0.20f, 1.0f };

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
    }

    // Overlay (ImGui) records into this command list and binds its own SRV heap.
    if (m_overlayCallback) m_overlayCallback(p.cmdList.Get());

    swapBarrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    swapBarrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    p.cmdList->ResourceBarrier(1, &swapBarrier);
    p.cmdList->Close();

    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    if (p.vsync)
        p.swapchain->Present(1, 0);
    else
        p.swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);

    const UINT64 signalValue = ++p.fenceValues[p.frameIndex];
    p.cmdQueue->Signal(p.fence.Get(), signalValue);
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
