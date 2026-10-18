#pragma once

// ─── Startup timing for the D3D11 and D3D12 backends (Thema 196) ─────────────
// The editor takes far longer to come up on D3D than on Metal/Vulkan. The
// suspects all sit in Initialize() and the first frame: device/swapchain
// creation, the debug layer, and FXC — every D3DCompile runs from source on
// every launch (no OS or driver cache sits in front of FXC; the driver cache
// only covers Create*Shader / CreateGraphicsPipelineState afterwards).
//
// Everything this writes starts with "[startup]" so one grep pulls the whole
// timeline out of HorizonEngine.log:
//   [startup] D3D11 phase=createPipeline ms=1234.5 compiles=12 compileMs=1200.3
//   [startup] D3DCompile scene/PSMain ps_5_0 ms=845.2 bytes=41210
//   [startup] D3D11 first frame: ms=2345.6 since Initialize, compiles=31 compileMs=2100.0
//
// The backends route every D3DCompile through timedD3DCompile (same signature),
// so the compile bucket is complete, including the lazy compiles the first
// frame triggers (material warmup, on-demand passes). Until the first frame is
// done every compile is logged; after it only the slow ones, which are the
// hitches a user would notice mid-session.
#include <Diagnostics/Log.h>

#include "Backends/D3D_Shared/D3DShaderCache.h"

#include <d3dcompiler.h>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace HE::d3dtiming
{

using Clock = std::chrono::steady_clock;

inline double msSince(Clock::time_point t0)
{
    return std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
}

// Process-wide running totals. One backend lives per process, so they are that
// backend's numbers; a phase reports the difference across its own scope.
struct CompileStats
{
    uint32_t calls  = 0;
    uint32_t cached = 0;    // of calls, answered from the bytecode cache
    double   ms     = 0.0;
};
inline CompileStats& compileStats()
{
    static CompileStats s;
    return s;
}

// True from Initialize() until the first frame has been rendered.
inline bool& startupOpen()
{
    static bool open = false;
    return open;
}

// After startup only compiles at least this slow are logged.
inline constexpr double kLogSlowCompileMs = 50.0;

// D3DCompile with a stopwatch, in front of the bytecode cache
// (D3DShaderCache.h). Drop-in: same parameters, same result — a hit hands back
// the bytes FXC produced for the same request earlier, with no error blob.
inline HRESULT timedD3DCompile(LPCVOID src, SIZE_T srcSize, LPCSTR sourceName,
                               const D3D_SHADER_MACRO* defines, ID3DInclude* include,
                               LPCSTR entry, LPCSTR target, UINT flags1, UINT flags2,
                               ID3DBlob** code, ID3DBlob** errors)
{
    const Clock::time_point t0 = Clock::now();
    const bool cacheable = d3dcache::enabled() && !defines && !include && code && src;
    d3dcache::Request req;
    if (cacheable || std::getenv("HE_D3D_SHADER_DUMP"))
    {
        req = d3dcache::makeRequest(src, srcSize, entry, target, flags1, flags2);
        d3dcache::dumpSource(req, sourceName);
    }
    ID3DBlob* hit = cacheable ? d3dcache::load(req) : nullptr;
    HRESULT hr = S_OK;
    if (hit)
    {
        *code = hit;
        if (errors) *errors = nullptr;
    }
    else
    {
        hr = ::D3DCompile(src, srcSize, sourceName, defines, include,
                          entry, target, flags1, flags2, code, errors);
        if (cacheable && SUCCEEDED(hr)) d3dcache::store(req, *code);
    }
    const double ms = msSince(t0);
    CompileStats& s = compileStats();
    ++s.calls;
    s.ms += ms;
    if (hit) ++s.cached;
    if (startupOpen() || ms >= kLogSlowCompileMs)
        HE_LOG_INFO(RHI, "[startup] D3DCompile %s/%s %s ms=%.1f bytes=%zu%s",
                    sourceName ? sourceName : "-", entry ? entry : "-",
                    target ? target : "-", ms, static_cast<size_t>(srcSize),
                    hit ? " cached" : SUCCEEDED(hr) ? "" : " FAILED");
    return hr;
}

// One named step of Initialize(): wall time plus the compiles inside it.
class Phase
{
public:
    Phase(const char* backend, const char* name)
        : m_backend(backend), m_name(name), m_t0(Clock::now()), m_c0(compileStats()) {}
    ~Phase()
    {
        const CompileStats& s = compileStats();
        HE_LOG_INFO(RHI, "[startup] %s phase=%s ms=%.1f compiles=%u compileMs=%.1f",
                    m_backend, m_name, msSince(m_t0), s.calls - m_c0.calls, s.ms - m_c0.ms);
    }
    Phase(const Phase&)            = delete;
    Phase& operator=(const Phase&) = delete;

private:
    const char*       m_backend;
    const char*       m_name;
    Clock::time_point m_t0;
    CompileStats      m_c0;
};

// The same line as Phase, for straight-line code whose locals outlive any
// block one could put around a step: mark(name) closes the step that began at
// the previous mark (or at construction).
class Laps
{
public:
    explicit Laps(const char* backend)
        : m_backend(backend), m_t0(Clock::now()), m_c0(compileStats()) {}
    void mark(const char* name)
    {
        const CompileStats& s = compileStats();
        HE_LOG_INFO(RHI, "[startup] %s phase=%s ms=%.1f compiles=%u compileMs=%.1f",
                    m_backend, name, msSince(m_t0), s.calls - m_c0.calls, s.ms - m_c0.ms);
        m_t0 = Clock::now();
        m_c0 = s;
    }

private:
    const char*       m_backend;
    Clock::time_point m_t0;
    CompileStats      m_c0;
};

// Initialize() start → end of the first Render(). Begin opens the window in
// which every compile is logged, endFirstFrame closes it (once) with the totals.
struct StartupClock
{
    Clock::time_point t0{};
    bool              done = false;

    void begin()
    {
        t0           = Clock::now();
        done         = false;
        startupOpen() = true;
    }
    void endFirstFrame(const char* backend)
    {
        if (done) return;
        done          = true;
        startupOpen() = false;
        const CompileStats& s = compileStats();
        HE_LOG_INFO(RHI, "[startup] %s first frame: ms=%.1f since Initialize, compiles=%u cached=%u compileMs=%.1f",
                    backend, msSince(t0), s.calls, s.cached, s.ms);
    }
};

} // namespace HE::d3dtiming
