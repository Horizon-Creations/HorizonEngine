#pragma once
#ifdef _WIN32
// AMD FSR on D3D12 (docs/upscalers-fsr-dlss-xess-plan-2026-11-27.md): the FidelityFX API of the FSR SDK, loaded at run time
// from amd_fidelityfx_loader_dx12.dll beside the executable (cmake/HeFidelityFX.cmake puts it there). Without the DLL, or in a
// build without HE_HAVE_FSR, available() is false and the renderer falls back (IRenderer::ResolveAAMethod).
#include <d3d12.h>
#include <glm/glm.hpp>
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
    };
    bool dispatch(const Dispatch& d);

    void destroy();   // the context (the caller has waited for the GPU); the DLL stays loaded

private:
    void*    m_module = nullptr;    // HMODULE of the loader
    void*    m_fns = nullptr;       // ffxFunctions
    void*    m_context = nullptr;   // ffxContext
    bool     m_loaded = false, m_tried = false;
    uint32_t m_rw = 0, m_rh = 0, m_dw = 0, m_dh = 0;
    std::string m_version;
};
} // namespace HE
#endif
