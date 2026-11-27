#pragma once
#ifdef _WIN32
// AMD FSR on D3D12 (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md): the FidelityFX API of the FSR SDK, loaded at run time
// from amd_fidelityfx_loader_dx12.dll beside the executable (cmake/HeFidelityFX.cmake puts it there). Without the DLL, or in a
// build without HE_HAVE_FSR, available() is false and the renderer falls back (IRenderer::ResolveAAMethod).
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <glm/glm.hpp>
#include <atomic>
#include <cstdint>
#include <string>

namespace HE
{
class D3D12Fsr
{
public:
    D3D12Fsr() = default;
    ~D3D12Fsr();
    D3D12Fsr(const D3D12Fsr&) = delete;
    D3D12Fsr& operator=(const D3D12Fsr&) = delete;

    // Opens the loader DLL once; true when FSR can be used on this machine. Logs why not, once.
    bool load();
    bool available() const { return m_loaded; }

    // The context for these sizes (made again when they change; the caller has waited for the GPU then). False: no FSR.
    bool ensureContext(ID3D12Device* device, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH);
    bool hasContext() const { return m_context != nullptr; }
    bool sizedFor(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH) const
    {
        return m_context && m_rw == renderW && m_rh == renderH && m_dw == displayW && m_dh == displayH;
    }
    // The version the loader picked ("3.1.5", "4.0.2"), after the first context.
    const std::string& version() const { return m_version; }

    // FSR's jitter for frame `index` in render pixels, exactly as the dispatch must be told it (FSR's sign convention).
    glm::vec2 jitter(uint32_t index, uint32_t renderW, uint32_t displayW);

    struct Dispatch
    {
        ID3D12GraphicsCommandList* cl = nullptr;
        ID3D12Resource* color = nullptr;      // HDR, render size, PIXEL|NON_PIXEL_SHADER_RESOURCE
        ID3D12Resource* depth = nullptr;      // R32 (typeless D32), render size, PIXEL|NON_PIXEL_SHADER_RESOURCE
        ID3D12Resource* velocity = nullptr;   // RG16F uvNow - uvPrev, render size, PIXEL|NON_PIXEL_SHADER_RESOURCE
        ID3D12Resource* output = nullptr;     // RGBA16F with UAV, display size, UNORDERED_ACCESS
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        glm::vec2 jitter{ 0.0f };             // what jitter() returned for this frame
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f;   // the depth buffer's own near plane (see the caller)
        float frameMs = 16.6f;
        float sharpness = 0.0f;               // 0 = FSR's sharpening off
        bool  reset = false;                  // history discarded (first frame, cut, resize)
        ID3D12Resource* reactive = nullptr;   // R8 render size, PIXEL|NON_PIXEL read; null = none (generateReactive)
    };
    bool dispatch(const Dispatch& d);

    // FSR's own reactive mask from the frame before and after its blended tail (particles, glass, water): where the
    // tail changed the image FSR trusts its history less, so what moves without velocity does not smear. All render
    // size; opaque and color PIXEL|NON_PIXEL read, reactive R8_UNORM with UAV in UNORDERED_ACCESS.
    bool generateReactive(ID3D12GraphicsCommandList* cl, ID3D12Resource* opaque, ID3D12Resource* color,
                          ID3D12Resource* reactive, uint32_t renderW, uint32_t renderH);

    void destroy();   // the context (the caller has waited for the GPU); the DLL stays loaded

    // ── Frame generation (FSR 3.1 FG, HE_HAVE_FSR_FG): one generated frame per rendered one, the game's swapchain ──
    // AMD's sample pattern: the window's swapchain is REPLACED by FSR's frame interpolation swapchain (made by the
    // swapchain context for the same hwnd and description), kept for the rest of the run; FG is then switched on and
    // off per frame by configure, and the FG context follows the display size.
    bool frameGenAvailable(ID3D12Device* device);   // the FG provider answers on this device (cached)
    // `sc` must be the hwnd's only swapchain, its back buffers released, the GPU idle. On success it is the proxy
    // (Present, GetBuffer, ResizeBuffers go through it); on failure a plain swapchain made again from the same desc.
    bool replaceSwapchain(Microsoft::WRL::ComPtr<IDXGISwapChain3>& sc, ID3D12CommandQueue* queue);
    bool swapchainReplaced() const { return m_scContext != nullptr; }
    bool ensureFrameGen(ID3D12Device* device, uint32_t displayW, uint32_t displayH, DXGI_FORMAT backBuffer);
    bool hasFrameGen() const { return m_fgContext != nullptr; }

    struct FrameGen
    {
        ID3D12GraphicsCommandList* cl = nullptr;
        IDXGISwapChain* swapchain = nullptr;  // the proxy
        bool enabled = false;                 // false: configure only (FG off; waits for the proxy's presents)
        uint64_t frameID = 0;                 // +1 per presented frame, the same in configure and prepare
        ID3D12Resource* hudless = nullptr;    // the frame before its UI, back-buffer format and size, PIXEL|NON_PIXEL read
        ID3D12Resource* depth = nullptr;      // as Dispatch: render size, PIXEL|NON_PIXEL read
        ID3D12Resource* velocity = nullptr;
        uint32_t renderW = 0, renderH = 0, displayW = 0, displayH = 0;
        glm::vec2 jitter{ 0.0f };
        float nearZ = 0.1f, farZ = 1000.0f, fovY = 1.0f, frameMs = 16.6f;
        bool reset = false;
        glm::vec3 camPos{ 0.0f }, camUp{ 0.0f, 1.0f, 0.0f }, camRight{ 1.0f, 0.0f, 0.0f }, camForward{ 0.0f, 0.0f, -1.0f };
    };
    bool frameGen(const FrameGen& f);   // configure, then (enabled) the prepare dispatch on f.cl
    // FG off and its context gone (before ResizeBuffers, at shutdown); the proxy swapchain stays.
    void destroyFrameGen(IDXGISwapChain* swapchain);
    void destroySwapchainContext();     // at shutdown, after destroyFrameGen and before the swapchain is released
    // Frames FSR generated so far (counted in its dispatch callback, on the proxy's present thread).
    uint64_t generatedFrames() const { return m_fgGenerated.load(std::memory_order_relaxed); }

private:
    void*    m_module = nullptr;    // HMODULE of the loader
    void*    m_fns = nullptr;       // ffxFunctions
    void*    m_context = nullptr;   // ffxContext
    bool     m_loaded = false, m_tried = false;
    uint32_t m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
    std::string m_version;
    void*    m_scContext = nullptr;   // the frame interpolation swapchain's ffxContext
    void*    m_fgContext = nullptr;   // the frame generation ffxContext
    uint32_t m_fgW = 0, m_fgH = 0;
    int      m_fgAvail = -1;          // frameGenAvailable's answer, -1 = not asked yet
    uint64_t m_fgLastFrameID = 0;
    std::atomic<uint64_t> m_fgGenerated{ 0 };
};
} // namespace HE
#endif
