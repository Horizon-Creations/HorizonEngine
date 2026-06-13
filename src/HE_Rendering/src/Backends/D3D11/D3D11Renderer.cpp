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
    float4   uColor;   // rgb = flat color, a = hasTexture (0/1)
};
cbuffer PerFrame : register(b1)
{
    float4 uCameraPos;        // xyz
    int4   uLightCount;       // x = count
    float4 uLightPos[8];      // xyz pos,  w type (0 dir / 1 point / 2 spot)
    float4 uLightDir[8];      // xyz dir,  w cos(spot half angle)
    float4 uLightColor[8];    // rgb,      w intensity
    float4 uLightParams[8];   // x range
};

Texture2D    uTexture : register(t0);
SamplerState uSampler : register(s0);

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

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 base = (uColor.a > 0.5) ? uTexture.Sample(uSampler, i.uv).rgb : uColor.rgb;
    float3 N    = normalize(i.normal);

    if (uLightCount.x == 0)
    {
        float3 L    = normalize(float3(0.5, 0.8, 0.6));
        float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        return float4(base * diff, 1.0);
    }

    float3 V      = normalize(uCameraPos.xyz - i.worldPos);
    float3 result = 0.08 * base; // ambient floor

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
        float  diff = max(dot(N, L), 0.0);
        float3 H    = normalize(L + V);
        float  spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;
        result += (base * diff + spec.xxx) * uLightColor[li].rgb * uLightColor[li].w * atten;
    }
    return float4(result, 1.0);
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
        glm::vec4 color; // rgb + hasTexture in .a
    };
    struct PerFrameCB
    {
        glm::vec4 cameraPos;
        glm::ivec4 lightCount;
        glm::vec4 lightPos[8];
        glm::vec4 lightDir[8];
        glm::vec4 lightColor[8];
        glm::vec4 lightParams[8];
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

    // ── Scene pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D11VertexShader>   vs;
    ComPtr<ID3D11PixelShader>    ps;
    ComPtr<ID3D11InputLayout>    inputLayout;
    ComPtr<ID3D11Buffer>         perObjectCB;
    ComPtr<ID3D11Buffer>         perFrameCB;
    ComPtr<ID3D11SamplerState>   sampler;
    ComPtr<ID3D11DepthStencilState> depthState;
    ComPtr<ID3D11RasterizerState>   rasterState;
    ComPtr<ID3D11ShaderResourceView> dummyTexture; // 1x1 white, for untextured meshes

    GpuMesh cube;

    RenderExtractor m_extractor;
    RenderWorld     m_renderWorld;
    FrustumCuller   m_culler;
    RenderSorter    m_sorter;
    RenderGraph     m_renderGraph;
    CommandBuffer   m_cmds;
    std::vector<bool>     m_visible;
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
            Logger::Log(Logger::LogLevel::Error, std::string("D3D11Renderer: VS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : "")) ;
            return false;
        }
        if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &psBlob, &err)))
        {
            Logger::Log(Logger::LogLevel::Error, std::string("D3D11Renderer: PS compile failed: ")
                + (err ? static_cast<const char*>(err->GetBufferPointer()) : ""));
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
        if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
    if (p.m_sortedIndices.empty()) return;

    if (p.m_renderGraph.empty())
        p.m_renderGraph.addPass(std::make_unique<GeometryPass>());

    const glm::mat4 viewProj = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;

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
        D3D11_MAPPED_SUBRESOURCE m{};
        if (SUCCEEDED(ctx->Map(p.perFrameCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &m)))
        {
            std::memcpy(m.pData, &f, sizeof(f));
            ctx->Unmap(p.perFrameCB.Get(), 0);
        }
        ctx->VSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
        ctx->PSSetConstantBuffers(1, 1, p.perFrameCB.GetAddressOf());
    }

    // Per-pass sink: today the only pass renders to the backbuffer (the bound
    // RTV). Offscreen targets (id != backbuffer) arrive with shadows/HDR.
    const UINT stride = 8 * sizeof(float);
    const UINT offset = 0;
    p.m_renderGraph.execute(p.m_renderWorld, p.m_sortedIndices,
        [&](const RenderPass&, const RenderPassIO& io, const CommandBuffer& cmds)
    {
        if (io.output.id != kBackbufferTarget) return;
        for (const DrawCall& dc : cmds.drawCalls())
        {
            const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
            const GpuMesh& m    = mesh ? *mesh : p.cube;
            if (!m.vbuf || !m.ibuf) continue;

            PerObjectCB o{};
            o.mvp   = viewProj * dc.transform;
            o.model = dc.transform;
            o.color = glm::vec4(0.85f, 0.55f, 0.25f, m.texture ? 1.0f : 0.0f);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (SUCCEEDED(ctx->Map(p.perObjectCB.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
            {
                std::memcpy(mapped.pData, &o, sizeof(o));
                ctx->Unmap(p.perObjectCB.Get(), 0);
            }
            ctx->VSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());
            ctx->PSSetConstantBuffers(0, 1, p.perObjectCB.GetAddressOf());

            ID3D11ShaderResourceView* srv = m.texture ? m.texture.Get() : p.dummyTexture.Get();
            ctx->PSSetShaderResources(0, 1, &srv);

            ctx->IASetVertexBuffers(0, 1, m.vbuf.GetAddressOf(), &stride, &offset);
            ctx->IASetIndexBuffer(m.ibuf.Get(), DXGI_FORMAT_R32_UINT, 0);
            ctx->DrawIndexed(m.indexCount, 0, 0);
        }
    });
}

void D3D11Renderer::Render()
{
    auto& p = *m_impl;
    const float color[4] = { 0.18f, 0.18f, 0.20f, 1.0f };

    p.context->OMSetRenderTargets(1, p.rtv.GetAddressOf(), p.dsv.Get());
    p.context->ClearRenderTargetView(p.rtv.Get(), color);
    if (p.dsv)
        p.context->ClearDepthStencilView(p.dsv.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0f, 0);

    D3D11_VIEWPORT vp{};
    vp.Width    = static_cast<float>(p.width);
    vp.Height   = static_cast<float>(p.height);
    vp.MaxDepth = 1.0f;
    p.context->RSSetViewports(1, &vp);

    DrawScene(p.width, p.height);

    if (m_overlayCallback) m_overlayCallback(nullptr);
    p.swapchain->Present(p.vsync ? 1 : 0, 0);
}

IRenderer::Capabilities D3D11Renderer::GetCapabilities() const { return { true, true, true }; }

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
