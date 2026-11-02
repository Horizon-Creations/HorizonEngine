// FXC bytecode cache of the D3D11/D3D12 backends (Thema 196).
//
// Both renderers compile every shader from HLSL source at start; measured on
// NN-WS03 that was ~8 s of an 8.3 s (D3D11) / 8.9 s (D3D12) renderer start,
// see docs/d3d-startzeit-messung-2026-10-13.md. timedD3DCompile now answers a
// repeated request from disk (Backends/D3D_Shared/D3DShaderCache.h). What a
// cache can get wrong, and what this checks, through the same entry point the
// renderers call:
//   * a hit hands back exactly the bytes FXC produced, with S_OK and no error blob;
//   * anything that changes FXC's output (flags, source) is a new key;
//   * a damaged file (flipped byte, truncation) is a miss and gets rewritten,
//     never a shader the device would reject — the renderers have no fallback
//     for a CreatePixelShader failure but a missing feature;
//   * a failed compile writes nothing.
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include "Backends/D3D_Shared/D3DStartupTiming.h"
#include <wrl/client.h>
#include "doctest.h"
#include "TestFsUtil.h"
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace
{

const char* kPS = R"(
cbuffer C : register(b0) { float4 tint; };
float4 main(float4 pos : SV_Position) : SV_Target
{
    float4 c = tint;
    [loop] for (int i = 0; i < 4; ++i) c.rgb = c.rgb * 0.5 + 0.25;
    return c;
}
)";

struct Compiled
{
    HRESULT           hr = E_FAIL;
    std::vector<char> code;
    bool              hadErrors = false;
    bool              cached = false;
};

Compiled compile(const std::string& src, UINT flags)
{
    using namespace HE::d3dtiming;
    const uint32_t before = compileStats().cached;
    ComPtr<ID3DBlob> code, err;
    Compiled c;
    c.hr = timedD3DCompile(src.data(), src.size(), "cachetest", nullptr, nullptr,
                           "main", "ps_5_0", flags, 0, &code, &err);
    if (code)
        c.code.assign(static_cast<const char*>(code->GetBufferPointer()),
                      static_cast<const char*>(code->GetBufferPointer()) + code->GetBufferSize());
    c.hadErrors = err != nullptr;
    c.cached    = compileStats().cached != before;
    return c;
}

// The file the cache keeps (or would keep) for this request.
fs::path fileOf(const std::string& src, UINT flags)
{
    return HE::d3dcache::fileFor(
        HE::d3dcache::makeRequest(src.data(), src.size(), "main", "ps_5_0", flags, 0));
}

std::vector<char> readAll(const fs::path& p)
{
    std::ifstream in(p, std::ios::binary);
    return std::vector<char>(std::istreambuf_iterator<char>(in), {});
}

void writeAll(const fs::path& p, const std::vector<char>& b)
{
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(b.data(), static_cast<std::streamsize>(b.size()));
}

// cacheDir() and enabled() read the environment once per process, so the
// directory is fixed on first use; no other test goes through timedD3DCompile.
const fs::path& testCacheDir()
{
    static const fs::path dir = [] {
        const fs::path d = fs::temp_directory_path() /
            ("he_d3dcache_" + std::to_string(GetCurrentProcessId()) + "_" +
             std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        _putenv_s("HE_D3D_SHADER_CACHE_DIR", d.string().c_str());
        _putenv_s("HE_D3D_SHADER_CACHE", "1");
        return d;
    }();
    return dir;
}

} // namespace

TEST_CASE("D3D shader cache: hit returns FXC's bytes, damage and new keys recompile")
{
    const fs::path& dir = testCacheDir();
    REQUIRE(HE::d3dcache::enabled());
    REQUIRE(HE::d3dcache::cacheDir() == dir);

    // doctest runs this body once per SUBCASE: a fresh source each pass, so
    // the first compile below is always a real FXC run.
    static int pass = 0;
    const std::string src = std::string(kPS) + "// pass " + std::to_string(++pass) + " " +
                            dir.filename().string() + "\n";

    const Compiled cold = compile(src, 0);
    REQUIRE(SUCCEEDED(cold.hr));
    CHECK_FALSE(cold.cached);
    REQUIRE(cold.code.size() > 4);
    CHECK(std::string(cold.code.data(), 4) == "DXBC");
    const fs::path file = fileOf(src, 0);
    REQUIRE(fs::exists(file));

    SUBCASE("a repeat is a hit with identical bytes")
    {
        const Compiled warm = compile(src, 0);
        CHECK(SUCCEEDED(warm.hr));
        CHECK(warm.cached);
        CHECK_FALSE(warm.hadErrors);
        CHECK(warm.code == cold.code);
    }

    SUBCASE("flags and source are part of the key")
    {
        const Compiled flow = compile(src, D3DCOMPILE_PREFER_FLOW_CONTROL);
        CHECK(SUCCEEDED(flow.hr));
        CHECK_FALSE(flow.cached);
        CHECK(fileOf(src, D3DCOMPILE_PREFER_FLOW_CONTROL) != file);
        const Compiled edited = compile(src + "// edited\n", 0);
        CHECK(SUCCEEDED(edited.hr));
        CHECK_FALSE(edited.cached);
        CHECK(fs::exists(fileOf(src + "// edited\n", 0)));
    }

    SUBCASE("a flipped byte in the bytecode is a miss, then rewritten")
    {
        std::vector<char> b = readAll(file);
        REQUIRE(b.size() > cold.code.size());
        b[b.size() - cold.code.size() / 2] ^= 0x40;   // inside the bytecode, past the DXBC magic
        writeAll(file, b);
        const Compiled again = compile(src, 0);
        CHECK(SUCCEEDED(again.hr));
        CHECK_FALSE(again.cached);
        CHECK(again.code == cold.code);
        const Compiled healed = compile(src, 0);
        CHECK(healed.cached);
        CHECK(healed.code == cold.code);
    }

    SUBCASE("a truncated file is a miss")
    {
        std::vector<char> b = readAll(file);
        b.resize(b.size() / 2);
        writeAll(file, b);
        const Compiled again = compile(src, 0);
        CHECK(SUCCEEDED(again.hr));
        CHECK_FALSE(again.cached);
        CHECK(again.code == cold.code);
    }

    SUBCASE("a failed compile writes nothing")
    {
        const std::string broken = src + "this is not hlsl;\n";
        const Compiled bad = compile(broken, 0);
        CHECK(FAILED(bad.hr));
        CHECK(bad.hadErrors);
        CHECK_FALSE(fs::exists(fileOf(broken, 0)));
    }

    he_test::removeAllQuiet(dir);
}

// ─── Parallel prefetch (Thema 200, D3DShaderPrefetch.h) ─────────────────────
// The renderers queue their startup compiles on the job pool and
// timedD3DCompile joins them. What that may not change: the bytes, the error
// path, and that only the exact request is answered.

namespace
{

std::vector<char> fxcDirect(const std::string& src, UINT flags)
{
    ComPtr<ID3DBlob> code, err;
    if (FAILED(::D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr,
                            "main", "ps_5_0", flags, 0, &code, &err)) || !code)
        return {};
    const char* p = static_cast<const char*>(code->GetBufferPointer());
    return std::vector<char>(p, p + code->GetBufferSize());
}

} // namespace

TEST_CASE("D3D shader prefetch: a joined request returns FXC's result")
{
    const fs::path& dir = testCacheDir();
    REQUIRE(HE::d3dprefetch::enabled());
    using HE::d3dprefetch::submit;
    using HE::d3dtiming::compileStats;

    static int pass = 0;
    const std::string src = std::string(kPS) + "// prefetch pass " + std::to_string(++pass) + " " +
                            dir.filename().string() + "\n";

    SUBCASE("same bytes as a direct D3DCompile, and the job filled the cache")
    {
        const std::vector<char> ref = fxcDirect(src, 0);
        REQUIRE(ref.size() > 4);
        submit(src, "prefetchtest", "main", "ps_5_0", 0);
        submit(src, "prefetchtest", "main", "ps_5_0", 0);     // queued once
        CHECK(HE::d3dprefetch::state().stats.submitted == 1);
        const uint32_t before = compileStats().prefetched;
        const Compiled got = compile(src, 0);
        CHECK(SUCCEEDED(got.hr));
        CHECK_FALSE(got.hadErrors);
        CHECK(compileStats().prefetched == before + 1);
        CHECK(got.code == ref);
        CHECK(fs::exists(fileOf(src, 0)));
        // A second ask (D3D12 builds some shaders twice) joins the same slot.
        const Compiled again = compile(src, 0);
        CHECK(compileStats().prefetched == before + 2);
        CHECK(again.code == ref);
    }

    SUBCASE("a failing compile reports its errors through the join, writes nothing")
    {
        const std::string broken = src + "this is not hlsl;\n";
        submit(broken, "prefetchtest", "main", "ps_5_0", 0);
        const uint32_t before = compileStats().prefetched;
        const Compiled bad = compile(broken, 0);
        CHECK(FAILED(bad.hr));
        CHECK(bad.hadErrors);
        CHECK(bad.code.empty());
        CHECK(compileStats().prefetched == before + 1);
        CHECK_FALSE(fs::exists(fileOf(broken, 0)));
    }

    SUBCASE("a request that drifted from its prefetch is not answered by it")
    {
        submit(src + "// as queued\n", "prefetchtest", "main", "ps_5_0", 0);
        submit(src, "prefetchtest", "main", "ps_5_0", D3DCOMPILE_PREFER_FLOW_CONTROL);
        const uint32_t before = compileStats().prefetched;
        const Compiled got = compile(src, 0);     // other source than the first, other flags than the second
        CHECK(SUCCEEDED(got.hr));
        CHECK(compileStats().prefetched == before);
        CHECK(got.code == fxcDirect(src, 0));
    }

    HE::d3dprefetch::drain();
    CHECK_FALSE(HE::d3dprefetch::active());
    he_test::removeAllQuiet(dir);
}

// The prefetch only pays off if FXC may run on several threads at once and
// still produce the bytes it produces alone, and if several writers of one
// key leave exactly one intact file (two editors, or two jobs, racing).
TEST_CASE("D3D shader cache: FXC on eight threads gives the serial bytes, one file per key")
{
    const fs::path& dir = testCacheDir();
    const std::string tag = dir.filename().string();
    const std::string srcA = std::string(kPS) + "// parallel A " + tag + "\n";
    std::string srcB = srcA;
    srcB.replace(srcB.find("i < 4"), 5, "i < 9");
    srcB += "// parallel B\n";
    const std::vector<char> refA = fxcDirect(srcA, D3DCOMPILE_PREFER_FLOW_CONTROL);
    const std::vector<char> refB = fxcDirect(srcB, D3DCOMPILE_PREFER_FLOW_CONTROL);
    REQUIRE(refA.size() > 4);
    REQUIRE(refB.size() > 4);
    REQUIRE(refA != refB);

    constexpr int kThreads = 8;
    std::vector<std::vector<char>> got(kThreads * 2);
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
        threads.emplace_back([&, t] {
            // Half start with A, half with B, so different sources overlap too.
            for (int k = 0; k < 2; ++k)
            {
                const bool a = ((t + k) % 2) == 0;
                const std::string& src = a ? srcA : srcB;
                const auto req = HE::d3dcache::makeRequest(src.data(), src.size(), "main", "ps_5_0",
                                                           D3DCOMPILE_PREFER_FLOW_CONTROL, 0);
                ComPtr<ID3DBlob> code, err;
                if (SUCCEEDED(::D3DCompile(src.data(), src.size(), nullptr, nullptr, nullptr, "main",
                                           "ps_5_0", D3DCOMPILE_PREFER_FLOW_CONTROL, 0, &code, &err)))
                {
                    HE::d3dcache::store(req, code.Get());
                    const char* p = static_cast<const char*>(code->GetBufferPointer());
                    got[t * 2 + k].assign(p, p + code->GetBufferSize());
                }
                got[t * 2 + k].insert(got[t * 2 + k].begin(), a ? 'A' : 'B');
            }
        });
    for (std::thread& th : threads) th.join();

    int mismatches = 0;
    for (const std::vector<char>& g : got)
    {
        REQUIRE_FALSE(g.empty());
        const std::vector<char>& ref = g[0] == 'A' ? refA : refB;
        if (std::vector<char>(g.begin() + 1, g.end()) != ref) ++mismatches;
    }
    CHECK(mismatches == 0);

    int dxbc = 0, tmp = 0;
    for (const auto& e : fs::directory_iterator(dir))
    {
        if (e.path().extension() == ".dxbc") ++dxbc;
        if (e.path().extension() == ".tmp")  ++tmp;
    }
    CHECK(dxbc == 2);
    CHECK(tmp == 0);
    for (const std::string* p : std::initializer_list<const std::string*>{ &srcA, &srcB })
    {
        const auto req = HE::d3dcache::makeRequest(p->data(), p->size(), "main", "ps_5_0",
                                                   D3DCOMPILE_PREFER_FLOW_CONTROL, 0);
        ComPtr<ID3DBlob> hit;
        hit.Attach(HE::d3dcache::load(req));
        REQUIRE(hit);
        const char* b = static_cast<const char*>(hit->GetBufferPointer());
        CHECK(std::vector<char>(b, b + hit->GetBufferSize()) == (p == &srcA ? refA : refB));
    }
    he_test::removeAllQuiet(dir);
}
#endif
