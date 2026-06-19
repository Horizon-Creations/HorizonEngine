#include "Backends/D3D11/D3D11Renderer.h"
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
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;

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
};

Texture2D    uTexture   : register(t0);
Texture2D    uShadowMap : register(t1);
SamplerState uSampler   : register(s0);

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
    float3 result = 0.03 * base * (1.0-met);  // ambient

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
    return float4(result, uPBR.z);
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
    };

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
    float bloomStrength  = 0.25f;
    float bloomThreshold = 1.0f;
    float bloomKnee      = 0.1f;
    bool  bloomEnabled   = true;
    bool  fxaaEnabled    = true;

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
        if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
                              "VSMain", "vs_5_0", flags, 0, &vsBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, (std::string("D3D11Renderer: VS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")).c_str());
            return false;
        }
        if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
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
        if (SUCCEEDED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
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
        uploadBuffers(mesh, interleaved, asset->indices);

        if (!asset->materialPath.empty())
        {
            const HE::UUID matId = cm->loadAsset(asset->materialPath);
            if (const MaterialAsset* mat = cm->getMaterial(matId); mat && !mat->texturePaths.empty())
            {
                const HE::UUID texId = cm->loadAsset(mat->texturePaths[0]);
                if (const TextureAsset* tex = cm->getTexture(texId);
                    tex && !tex->data.empty() && tex->channels == 4)
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
        }
        return &meshCache.emplace(assetId, mesh).first->second;
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

        // Shadow map on t1 for sampling.
        ID3D11ShaderResourceView* shadowSrv = shadows ? p.shadowSRV.Get() : nullptr;
        ctx->PSSetShaderResources(1, 1, &shadowSrv);

        const glm::vec3 camPos = p.m_renderWorld.camera.position;

        // Split into opaque (opacity ≥ 1) and transparent (opacity < 1) draw calls.
        std::vector<const DrawCall*> opaqueDCs, transparentDCs;
        for (const DrawCall& dc : cmds.drawCalls())
            (dc.opacity < 0.999f ? transparentDCs : opaqueDCs).push_back(&dc);

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

        if (!transparentDCs.empty()) {
            ctx->OMSetBlendState(p.alphaBlendState.Get(), nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(p.depthReadOnlyState.Get(), 0);
            for (const DrawCall* dc : transparentDCs) drawDC(*dc);
            ctx->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
            ctx->OMSetDepthStencilState(p.depthState.Get(), 0);
        }
    });
}

void D3D11Renderer::Render()
{
    auto& p = *m_impl;
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
