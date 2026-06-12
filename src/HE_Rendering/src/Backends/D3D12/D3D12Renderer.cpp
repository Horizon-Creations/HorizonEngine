#include "Backends/D3D12/D3D12Renderer.h"
#include <Window/Window.h>
#include <SDL3/SDL.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <stdexcept>
#include <Diagnostics/Logger.h>

using Microsoft::WRL::ComPtr;
static constexpr UINT k_frameCount = 2;

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

    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle(UINT index) const
    {
        D3D12_CPU_DESCRIPTOR_HANDLE h = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        h.ptr += static_cast<SIZE_T>(index) * rtvDescSize;
        return h;
    }

    // Wait until the GPU signals that it has finished with this frame's resources.
    // Called at the START of a frame to ensure the previous use of this slot is done.
    void waitForFrame(UINT frame)
    {
        const UINT64 target = fenceValues[frame];
        if (fence->GetCompletedValue() < target)
        {
            fence->SetEventOnCompletion(target, fenceEvent);
            WaitForSingleObject(fenceEvent, INFINITE);
        }
    }

    // Used at shutdown to drain all in-flight frames.
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

#ifdef _DEBUG
    // Enable the D3D12 debug layer so validation errors appear in the output window.
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
    BOOL tearingSupport = FALSE;
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
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: initialized successfully");
}

void D3D12Renderer::Shutdown()
{
    Logger::Log(Logger::LogLevel::Info, "D3D12Renderer: shutdown — waiting for GPU");
    m_impl->waitForAllFrames();
    if (m_impl->fenceEvent) { CloseHandle(m_impl->fenceEvent); m_impl->fenceEvent = nullptr; }

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

void D3D12Renderer::Render()
{
    auto& p = *m_impl;

    // Wait until the GPU has finished with this frame slot before reusing it.
    p.waitForFrame(p.frameIndex);

    p.cmdAllocators[p.frameIndex]->Reset();
    p.cmdList->Reset(p.cmdAllocators[p.frameIndex].Get(), nullptr);

    // Transition back-buffer: PRESENT → RENDER_TARGET
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource   = p.renderTargets[p.frameIndex].Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    p.cmdList->ResourceBarrier(1, &barrier);

    auto rtv = p.rtvHandle(p.frameIndex);
    p.cmdList->OMSetRenderTargets(1, &rtv, FALSE, nullptr);

    const float color[4] = { 0.18f, 0.18f, 0.20f, 1.0f };
    p.cmdList->ClearRenderTargetView(rtv, color, 0, nullptr);

    // Overlay (ImGui) records into this command list.
    if (m_overlayCallback) m_overlayCallback(p.cmdList.Get());

    // Transition back-buffer: RENDER_TARGET → PRESENT
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter  = D3D12_RESOURCE_STATE_PRESENT;
    p.cmdList->ResourceBarrier(1, &barrier);
    p.cmdList->Close();

    ID3D12CommandList* lists[] = { p.cmdList.Get() };
    p.cmdQueue->ExecuteCommandLists(1, lists);
    if (p.vsync)
    {
        // For VSync, use standard Present which blocks until vsync and syncs to it.
        p.swapchain->Present(1, 0);
    }
    else
    {
        // For no VSync, use Present with the "tearing allowed" flag so it can present immediately.
        // This may cause tearing but is what users expect when disabling VSync.
        p.swapchain->Present(0, DXGI_PRESENT_ALLOW_TEARING);
	}

    // Signal fence for the frame we JUST submitted, then advance to the next index.
    // This must happen after Present so the signal matches the correct frame slot.
    const UINT64 signalValue    = ++p.fenceValues[p.frameIndex];
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