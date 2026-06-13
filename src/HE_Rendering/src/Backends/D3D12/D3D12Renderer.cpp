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
#include <stdexcept>
#include <unordered_map>
#include <vector>
#include <memory>
#include <cstring>
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
static const char* kSceneHLSL = R"HLSL(
cbuffer PerObject : register(b0)
{
    float4x4 uMVP;
    float4x4 uModel;
    float4   uColor;
};
cbuffer PerFrame : register(b1)
{
    float4 uCameraPos;
    int4   uLightCount;
    float4 uLightPos[8];
    float4 uLightDir[8];
    float4 uLightColor[8];
    float4 uLightParams[8];
};
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
float4 PSMain(VSOut i) : SV_TARGET
{
    float3 base = uColor.rgb;
    float3 N    = normalize(i.normal);
    if (uLightCount.x == 0)
    {
        float3 L    = normalize(float3(0.5, 0.8, 0.6));
        float  diff = 0.35 + 0.65 * max(dot(N, L), 0.0);
        return float4(base * diff, 1.0);
    }
    float3 V      = normalize(uCameraPos.xyz - i.worldPos);
    float3 result = 0.08 * base;
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
        float diff = max(dot(N, L), 0.0);
        float3 H = normalize(L + V);
        float spec = pow(max(dot(N, H), 0.0), 32.0) * 0.25;
        result += (base * diff + spec.xxx) * uLightColor[li].rgb * uLightColor[li].w * atten;
    }
    return float4(result, 1.0);
}
)HLSL";

namespace
{
    struct PerObjectCB { glm::mat4 mvp; glm::mat4 model; glm::vec4 color; };
    struct PerFrameCB
    {
        glm::vec4  cameraPos;
        glm::ivec4 lightCount;
        glm::vec4  lightPos[8];
        glm::vec4  lightDir[8];
        glm::vec4  lightColor[8];
        glm::vec4  lightParams[8];
    };

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
    ComPtr<ID3D12DescriptorHeap> dsvHeap;
    ComPtr<ID3D12Resource>       depthBuffer;

    // ── Scene pipeline ──────────────────────────────────────────────────────
    ComPtr<ID3D12RootSignature>  rootSig;
    ComPtr<ID3D12PipelineState>  pso;
    ComPtr<ID3D12Resource>       perFrameCB[k_frameCount];   // upload, persistently mapped
    uint8_t*                     perFramePtr[k_frameCount]{};
    ComPtr<ID3D12Resource>       perObjectRing[k_frameCount]; // upload, persistently mapped
    uint8_t*                     perObjectPtr[k_frameCount]{};

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
        hd.NumDescriptors = 1;
        hd.Type           = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
        device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dsvHeap));

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
        device->CreateDepthStencilView(depthBuffer.Get(), nullptr,
            dsvHeap->GetCPUDescriptorHandleForHeapStart());
    }

    bool createPipeline()
    {
        // Root signature: two root CBVs (b0 per-object, b1 per-frame).
        D3D12_ROOT_PARAMETER params[2]{};
        params[0].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[0].Descriptor       = { 0, 0 }; // b0
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType    = D3D12_ROOT_PARAMETER_TYPE_CBV;
        params[1].Descriptor       = { 1, 0 }; // b1
        params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

        D3D12_ROOT_SIGNATURE_DESC rsd{};
        rsd.NumParameters = 2;
        rsd.pParameters   = params;
        rsd.Flags         = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

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
        ComPtr<ID3DBlob> vs, ps, cerr;
        if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
                              "VSMain", "vs_5_0", flags, 0, &vs, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, std::string("D3D12Renderer: VS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : ""));
            return false;
        }
        if (FAILED(D3DCompile(kSceneHLSL, std::strlen(kSceneHLSL), "scene", nullptr, nullptr,
                              "PSMain", "ps_5_0", flags, 0, &ps, &cerr)))
        {
            Logger::Log(Logger::LogLevel::Error, std::string("D3D12Renderer: PS compile failed: ")
                + (cerr ? static_cast<const char*>(cerr->GetBufferPointer()) : ""));
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

        // Per-frame + per-object upload rings, one set per frame in flight.
        for (UINT f = 0; f < k_frameCount; ++f)
        {
            perFrameCB[f]    = createUploadBuffer(alignUp(sizeof(PerFrameCB), k_cbSlot),
                                                  reinterpret_cast<void**>(&perFramePtr[f]));
            perObjectRing[f] = createUploadBuffer(static_cast<UINT64>(k_maxDraws) * k_cbSlot,
                                                  reinterpret_cast<void**>(&perObjectPtr[f]));
        }
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
    m_impl->pso.Reset();
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
        if (const GpuMesh* mesh = p.resolveMesh(obj.meshAssetId, m_contentManager);
            mesh && mesh->localBounds.isValid())
            obj.worldBounds = mesh->localBounds.transformed(obj.transform);

    p.m_culler.cull(p.m_renderWorld, p.m_visible);
    p.m_sorter.sort(p.m_renderWorld, p.m_visible, p.m_sortedIndices);
    if (p.m_sortedIndices.empty()) return;

    if (p.m_renderGraph.empty())
        p.m_renderGraph.addPass(std::make_unique<GeometryPass>());
    p.m_renderGraph.execute(p.m_renderWorld, p.m_sortedIndices, p.m_cmds);

    const glm::mat4 viewProj = p.m_renderWorld.camera.projection * p.m_renderWorld.camera.view;

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
        if (p.perFramePtr[p.frameIndex])
            std::memcpy(p.perFramePtr[p.frameIndex], &f, sizeof(f));
    }

    cl->SetGraphicsRootSignature(p.rootSig.Get());
    cl->SetPipelineState(p.pso.Get());
    cl->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cl->SetGraphicsRootConstantBufferView(1, p.perFrameCB[p.frameIndex]->GetGPUVirtualAddress());

    const D3D12_GPU_VIRTUAL_ADDRESS ringBase = p.perObjectRing[p.frameIndex]->GetGPUVirtualAddress();
    uint8_t* ringPtr = p.perObjectPtr[p.frameIndex];

    UINT drawIdx = 0;
    for (const DrawCall& dc : p.m_cmds.drawCalls())
    {
        if (drawIdx >= k_maxDraws) break;
        const GpuMesh* mesh = p.resolveMesh(dc.meshAssetId, m_contentManager);
        const GpuMesh& m    = mesh ? *mesh : p.cube;
        if (!m.indexCount) continue;

        PerObjectCB o{};
        o.mvp   = viewProj * dc.transform;
        o.model = dc.transform;
        o.color = glm::vec4(0.85f, 0.55f, 0.25f, 0.0f);
        if (ringPtr)
            std::memcpy(ringPtr + static_cast<size_t>(drawIdx) * k_cbSlot, &o, sizeof(o));
        cl->SetGraphicsRootConstantBufferView(0, ringBase + static_cast<UINT64>(drawIdx) * k_cbSlot);

        cl->IASetVertexBuffers(0, 1, &m.vbv);
        cl->IASetIndexBuffer(&m.ibv);
        cl->DrawIndexedInstanced(m.indexCount, 1, 0, 0, 0);
        ++drawIdx;
    }
}

void D3D12Renderer::Render()
{
    auto& p = *m_impl;

    p.waitForFrame(p.frameIndex);
    p.cmdAllocators[p.frameIndex]->Reset();
    p.cmdList->Reset(p.cmdAllocators[p.frameIndex].Get(), nullptr);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = p.renderTargets[p.frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &barrier);

    auto rtv = p.rtvHandle(p.frameIndex);
    auto dsv = p.dsvHeap ? p.dsvHeap->GetCPUDescriptorHandleForHeapStart() : D3D12_CPU_DESCRIPTOR_HANDLE{};
    p.cmdList->OMSetRenderTargets(1, &rtv, FALSE, p.dsvHeap ? &dsv : nullptr);

    const float color[4] = { 0.18f, 0.18f, 0.20f, 1.0f };
    p.cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);
    if (p.dsvHeap)
        p.cmdList->ClearDepthStencilView(dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

    D3D12_VIEWPORT vp{ 0, 0, static_cast<float>(p.width), static_cast<float>(p.height), 0.0f, 1.0f };
    D3D12_RECT     sc{ 0, 0, p.width, p.height };
    p.cmdList->RSSetViewports(1, &vp);
    p.cmdList->RSSetScissorRects(1, &sc);

    DrawScene(p.cmdList.Get(), p.width, p.height);

    // Overlay (ImGui) records into this command list and binds its own SRV heap.
    if (m_overlayCallback) m_overlayCallback(p.cmdList.Get());

    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    p.cmdList->ResourceBarrier(1, &barrier);
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

IRenderer::Capabilities D3D12Renderer::GetCapabilities() const { return { true, true, true }; }

void* D3D12Renderer::GetDevice()       const { return m_impl->device.Get(); }
void* D3D12Renderer::GetCommandQueue() const { return m_impl->cmdQueue.Get(); }

void D3D12Renderer::SetVSync(bool enabled)
{
    Logger::Log(Logger::LogLevel::Info, enabled ? "D3D12Renderer: VSync enabled" : "D3D12Renderer: VSync disabled");
    m_impl->vsync = enabled;
}
