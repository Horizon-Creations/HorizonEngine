#pragma once

// ─── Parallel FXC prefetch for the D3D11 and D3D12 start (Thema 200) ─────────
// The bytecode cache (D3DShaderCache.h, Thema 196) makes every start after the
// first one fast, but the first start — and the first after a shader or a
// d3dcompiler_47.dll change — still runs ~41-46 compiles back to back on the
// main thread: 7.9 s of FXC, 5 s of it the cross-compiled sky pixel shader
// (docs/d3d-startzeit-messung-2026-10-13.md). FXC is single-threaded per call,
// so the machine sits mostly idle while the editor waits.
//
// Here the backend hands over the requests it is about to make, early in
// Initialize, and they compile on the global job pool. timedD3DCompile later
// asks take() first and gets the finished result (or waits for the running
// job; a job still queued runs inline on the caller — JobHandle::wait). So
// the start costs about as long as the slowest single shader, not the sum.
//
// The join is by the cache key of the request (source bytes, entry, target,
// flags), and the slot repeats the request field by field. A prefetch the
// call site no longer matches — its source drifted — is never handed out; it
// only cost a worker some time and is logged as unused at the first frame.
//
// Workers run nothing but D3DCompile and the cache read/write: the sources
// are built by the backend on its own thread (shaderc, MaterialShaderLibrary
// are not known to be safe to call concurrently), and the slot owns a copy.
//
// HE_D3D_SHADER_PREFETCH=0 turns it off (the serial path, same binary).
#include "Backends/D3D_Shared/D3DShaderCache.h"

#include <Diagnostics/Log.h>
#include <JobSystem/JobSystem.h>

#include <d3dcompiler.h>
#include <wrl/client.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>

namespace HE::d3dprefetch
{

inline bool enabled()
{
    static const bool on = [] {
        const char* v = std::getenv("HE_D3D_SHADER_PREFETCH");
        return !(v && v[0] == '0');
    }();
    return on;
}

// One requested compile. Owns everything the Request points into, so it is
// only ever handled through a shared_ptr and never moves.
struct Slot
{
    std::string       src, sourceName, entry, target;
    UINT              flags1 = 0, flags2 = 0;
    d3dcache::Request req;
    HE::JobHandle     job;
    // Written by the job, read after job.wait():
    HRESULT                          hr = E_FAIL;
    Microsoft::WRL::ComPtr<ID3DBlob> code, errors;
    double                           workerMs = 0.0;
    bool                             cached = false;
    // Main thread only:
    bool                             taken = false;
};

struct Stats
{
    uint32_t submitted = 0;
    uint32_t joined    = 0;     // distinct slots handed out at least once
    uint32_t cached    = 0;     // of joined, answered from the disk cache by the worker
    double   workerMs  = 0.0;   // of joined: FXC / cache time spent on workers
};

struct State
{
    std::mutex                                          m;
    std::unordered_map<uint64_t, std::shared_ptr<Slot>> slots;
    Stats                                               stats;
};
inline State& state()
{
    static State s;
    return s;
}

// The job body: the same cache-then-FXC sequence timedD3DCompile runs.
inline void run(Slot& s)
{
    const auto t0 = std::chrono::steady_clock::now();
    const bool useCache = d3dcache::enabled();
    if (useCache)
        if (ID3DBlob* hit = d3dcache::load(s.req))
        {
            s.code.Attach(hit);
            s.hr     = S_OK;
            s.cached = true;
        }
    if (!s.cached)
    {
        s.hr = ::D3DCompile(s.src.data(), s.src.size(),
                            s.sourceName.empty() ? nullptr : s.sourceName.c_str(),
                            nullptr, nullptr, s.entry.c_str(), s.target.c_str(),
                            s.flags1, s.flags2, s.code.GetAddressOf(), s.errors.GetAddressOf());
        if (useCache && SUCCEEDED(s.hr)) d3dcache::store(s.req, s.code.Get());
    }
    s.workerMs = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
}

// Queue one compile. Arguments exactly as the later timedD3DCompile call will
// pass them (sourceName is not part of the key, it only names the log line).
// A request already queued is not queued twice.
inline void submit(std::string src, const char* sourceName, const char* entry,
                   const char* target, UINT flags1, UINT flags2 = 0)
{
    if (!enabled() || src.empty() || !entry || !target) return;
    auto slot = std::make_shared<Slot>();
    slot->src        = std::move(src);
    slot->sourceName = sourceName ? sourceName : "";
    slot->entry      = entry;
    slot->target     = target;
    slot->flags1     = flags1;
    slot->flags2     = flags2;
    slot->req = d3dcache::makeRequest(slot->src.data(), slot->src.size(), slot->entry.c_str(),
                                      slot->target.c_str(), flags1, flags2);
    State& st = state();
    {
        std::lock_guard<std::mutex> lock(st.m);
        if (!st.slots.emplace(slot->req.key, slot).second) return;
        ++st.stats.submitted;
    }
    HE::JobDesc desc;
    desc.name = "D3DCompilePrefetch";
    // The job keeps its slot alive itself: drain() may drop the map entry
    // while a worker still writes into it.
    slot->job = ::globalPool().schedule([slot] { run(*slot); }, std::move(desc));
}

// Anything queued at all? (timedD3DCompile only hashes for a join if so.)
inline bool active()
{
    State& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    return !st.slots.empty();
}

// The prefetched result for `req`, waiting for its job if needed. False when
// nothing matching was queued or the job did not run — the caller then
// compiles itself. On true, *code / *errors are AddRef'd like D3DCompile's.
inline bool take(const d3dcache::Request& req, HRESULT& hr, ID3DBlob** code, ID3DBlob** errors,
                 double& workerMs, bool& cached)
{
    State& st = state();
    std::shared_ptr<Slot> s;
    {
        std::lock_guard<std::mutex> lock(st.m);
        const auto it = st.slots.find(req.key);
        if (it == st.slots.end()) return false;
        s = it->second;
    }
    // The key is a hash: the slot has to be the same request, not a collision.
    if (s->req.srcHash != req.srcHash || s->req.srcSize != req.srcSize
        || s->flags1 != req.flags1 || s->flags2 != req.flags2
        || s->entry != req.entry || s->target != req.target)
        return false;
    try { s->job.wait(); } catch (...) { return false; }
    if (s->job.status() != HE::JobStatus::Done) return false;

    hr = s->hr;
    if (code)
    {
        *code = s->code.Get();
        if (*code) (*code)->AddRef();
    }
    if (errors)
    {
        *errors = s->errors.Get();
        if (*errors) (*errors)->AddRef();
    }
    workerMs = s->workerMs;
    cached   = s->cached;
    if (!s->taken)
    {
        s->taken = true;
        std::lock_guard<std::mutex> lock(st.m);
        ++st.stats.joined;
        if (s->cached) ++st.stats.cached;
        st.stats.workerMs += s->workerMs;
    }
    return true;
}

// End of the start (first frame): one summary line, unused requests named
// (a call site whose source drifted from its prefetch), finished slots freed.
// A freed request that is asked for later finds the disk cache the job filled.
inline void finishStartup(const char* backend)
{
    State& st = state();
    std::lock_guard<std::mutex> lock(st.m);
    if (st.stats.submitted == 0) return;
    std::string unused;
    uint32_t    unusedCount = 0;
    for (auto it = st.slots.begin(); it != st.slots.end();)
    {
        Slot& s = *it->second;
        if (!s.taken)
        {
            ++unusedCount;
            if (unused.size() < 400)
                unused += " " + s.sourceName + "/" + s.entry;
        }
        if (s.job.finished()) it = st.slots.erase(it);
        else                  ++it;
    }
    HE_LOG_INFO(RHI, "[startup] %s shader prefetch: submitted=%u joined=%u cached=%u workerMs=%.1f unused=%u%s",
                backend, st.stats.submitted, st.stats.joined, st.stats.cached, st.stats.workerMs,
                unusedCount, unused.c_str());
    st.stats = Stats{};
}

// Shutdown: drop what has not started (a wait() would run it inline — an
// editor closed in its first seconds must not compile the sky first), wait
// for what is running, forget them all — the renderer may be re-created in
// the same process (backend switch).
inline void drain()
{
    State& st = state();
    std::unordered_map<uint64_t, std::shared_ptr<Slot>> slots;
    {
        std::lock_guard<std::mutex> lock(st.m);
        slots.swap(st.slots);
        st.stats = Stats{};
    }
    for (auto& [key, s] : slots) { (void)key; s->job.cancel(); }
    for (auto& [key, s] : slots)
    {
        (void)key;
        try { s->job.wait(); } catch (...) {}
    }
}

} // namespace HE::d3dprefetch
