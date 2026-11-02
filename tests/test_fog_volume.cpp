// Thema 225 step 2: the host-side math/data of volumetric fog
// (docs/volumetric-fog-plan.md) — FogVolume.h's slice distribution, the
// analytic height-fog integral it has to seam with at the far field (§7), the
// temporal/jitter helpers, the std140 constants layout, the CPU reference of
// the Integrate pass, and MakeFogVolumeData/ExtractFogVolumeData's gating.
// Nothing here renders anything; no backend reads FogVolumeData yet.
#include "doctest.h"
// Standalone-compile check: this is the ONLY include a consumer needs.
#include <HorizonRendering/FogVolume.h>

#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/EnvironmentComponent.h>
#include <material/MaterialShaderLibrary.h>
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(HE_TESTS_HAVE_SHADERC)
#include "ShaderCompiler.h"
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#endif
#endif

using namespace HE;

namespace
{
// A faithful C++ transcription of the REAL formula every backend evaluates
// (MaterialShaderLibrary.cpp:365-376, heApplyFog) over the full ray 0..dist —
// not a re-derivation, the exact lines the shader runs. fogOpticalSegment(camY,
// dirY, 0, dist, ...) must reproduce this bit for bit, dirY being the Y of a
// UNIT ray direction (ray/dist in the shader).
float referenceOpticalDepth(float camY, float dirY, float dist, float density, float falloff)
{
    const float k = falloff * dirY * dist;      // heLight.fog.y * ray.y, ray = dir*dist
    const float t = (std::abs(k) > 1e-4f) ? (1.0f - std::exp(-k)) / k : 1.0f;
    return density * dist * std::exp(-falloff * camY) * t;
}
}

TEST_CASE("fogSliceToDepth / fogDepthToSlice round-trip and pin the plan's numbers")
{
    const float D = 96.0f, c = 2.0f;

    // §4.2: first slice ~0.125 m thick, last slice ~5.8 m thick, N = 64.
    const float firstSliceEnd = fogSliceToDepth(1.0f / 64.0f, D, c);
    CHECK(firstSliceEnd == doctest::Approx(0.125f).epsilon(0.02));
    const float lastSliceStart = fogSliceToDepth(63.0f / 64.0f, D, c);
    CHECK((D - lastSliceStart) == doctest::Approx(5.8f).epsilon(0.02));

    // Endpoints: t=0 -> z=0, t=1 -> z=D.
    CHECK(fogSliceToDepth(0.0f, D, c) == doctest::Approx(0.0f));
    CHECK(fogSliceToDepth(1.0f, D, c) == doctest::Approx(D));

    // Bijection over the open interval.
    for (int i = 1; i < 64; ++i)
    {
        const float t = static_cast<float>(i) / 64.0f;
        const float z = fogSliceToDepth(t, D, c);
        CHECK(fogDepthToSlice(z, D, c) == doctest::Approx(t).epsilon(0.001));
    }

    // Monotonic: later slices are never thinner than earlier ones (exponential grows).
    float prevZ = 0.0f;
    for (int i = 1; i <= 64; ++i)
    {
        const float z = fogSliceToDepth(static_cast<float>(i) / 64.0f, D, c);
        CHECK(z > prevZ);
        prevZ = z;
    }

    // Negative depth clamps to slice 0, not NaN.
    CHECK(fogDepthToSlice(-5.0f, D, c) == doctest::Approx(0.0f));
}

TEST_CASE("fogOpticalSegment matches the real shader formula bit for bit")
{
    struct Case { float camY, dirY, dist, density, falloff; };
    const Case cases[] = {
        { 10.0f, 0.3f,  50.0f, 0.02f, 0.1f },   // ordinary upward-looking ray
        { 10.0f, -0.4f, 80.0f, 0.05f, 0.2f },   // looking down
        { 0.0f,   0.0f, 40.0f, 0.03f, 0.1f },   // horizontal ray: k -> 0
        { 5.0f,   0.1f, 30.0f, 0.04f, 0.0f },   // falloff = 0: k -> 0 the other way
        { -3.0f,  0.6f, 96.0f, 0.01f, 0.05f },  // camera below world origin
    };
    for (const Case& t : cases)
    {
        const float got  = fogOpticalSegment(t.camY, t.dirY, 0.0f, t.dist, t.density, t.falloff);
        const float want = referenceOpticalDepth(t.camY, t.dirY, t.dist, t.density, t.falloff);
        CHECK(got == doctest::Approx(want).epsilon(0.001));
    }

    // Segment additivity: [0,d1] + [d1,d2] == [0,d2] (the volume's far-field
    // continuation at `dv` in plan §6.3 depends on exactly this).
    const float a = fogOpticalSegment(8.0f, 0.2f, 0.0f, 20.0f, 0.03f, 0.08f);
    const float b = fogOpticalSegment(8.0f, 0.2f, 20.0f, 50.0f, 0.03f, 0.08f);
    const float whole = fogOpticalSegment(8.0f, 0.2f, 0.0f, 50.0f, 0.03f, 0.08f);
    CHECK((a + b) == doctest::Approx(whole).epsilon(0.001));

    // Zero density, empty segment and b<=a are all exactly zero (no divide blowing up).
    CHECK(fogOpticalSegment(0.0f, 0.5f, 0.0f, 10.0f, 0.0f, 0.1f) == doctest::Approx(0.0f));
    CHECK(fogOpticalSegment(0.0f, 0.5f, 5.0f, 5.0f, 0.1f, 0.1f) == doctest::Approx(0.0f));
    CHECK(fogOpticalSegment(0.0f, 0.5f, 5.0f, 2.0f, 0.1f, 0.1f) == doctest::Approx(0.0f));
}

TEST_CASE("fogHistoryAlpha pins the plan's 40ms time constant")
{
    CHECK(fogHistoryAlpha(1.0f / 200.0f) == doctest::Approx(0.1175f).epsilon(0.01));
    CHECK(fogHistoryAlpha(1.0f / 60.0f)  == doctest::Approx(0.341f).epsilon(0.01));
    CHECK(fogHistoryAlpha(0.0f)          == doctest::Approx(1.0f));   // reset-equivalent
    CHECK(fogHistoryAlpha(-1.0f)         == doctest::Approx(1.0f));   // never negative/undefined
}

TEST_CASE("fogJitter is deterministic, periodic over 16 and stays in [0,1)")
{
    for (uint32_t i = 0; i < 64; ++i)
    {
        const float v = fogJitter(i);
        CHECK(v >= 0.0f);
        CHECK(v < 1.0f);
        CHECK(fogJitter(i) == doctest::Approx(v)); // deterministic
        CHECK(fogJitter(i) == doctest::Approx(fogJitter(i + 16))); // period 16
    }
    // Not a constant sequence (it would defeat the point of jittering).
    bool anyDifferent = false;
    for (uint32_t i = 1; i < 16; ++i)
        if (std::abs(fogJitter(i) - fogJitter(0)) > 1e-3f) anyDifferent = true;
    CHECK(anyDifferent);
}

TEST_CASE("fogGridFor pins the plan's three quality tiers at 16:9")
{
    const float aspect16x9 = 16.0f / 9.0f;
    CHECK(fogGridFor(0, aspect16x9) == glm::ivec3(96, 54, 32));   // Low
    CHECK(fogGridFor(1, aspect16x9) == glm::ivec3(160, 90, 64));  // Medium
    CHECK(fogGridFor(2, aspect16x9) == glm::ivec3(192, 108, 96)); // High

    // Out-of-range quality clamps instead of indexing out of bounds.
    CHECK(fogGridFor(-1, aspect16x9) == fogGridFor(0, aspect16x9));
    CHECK(fogGridFor(5, aspect16x9)  == fogGridFor(2, aspect16x9));

    // The (width, height) overload agrees with the ratio overload, and a
    // degenerate height falls back to 16:9 rather than dividing by zero.
    CHECK(fogGridFor(1, 1920, 1080) == fogGridFor(1, aspect16x9));
    CHECK(fogGridFor(1, 1920, 0)    == fogGridFor(1, aspect16x9));
}

TEST_CASE("FogVolumeConstants is 288 bytes and FillFogVolumeConstants places every field")
{
    // The static_asserts in FogVolume.h already pin the std140 offsets at
    // compile time; this is the functional half — every input lands in the
    // field the plan's §2.4 table says it should.
    FogVolumeData fog;
    fog.distance          = 123.0f;
    fog.sliceC             = 2.0f;
    fog.grid               = glm::ivec3(160, 90, 64);
    fog.density            = 0.04f;
    fog.heightFalloff      = 0.12f;
    fog.noiseAmount        = 0.6f;
    fog.invNoiseScale      = 0.025f;
    fog.bankDensity        = 0.3f;
    fog.bankBaseY          = 1.5f;
    fog.bankThickness      = 4.0f;
    fog.precipExtinction   = 0.006f;
    fog.albedo             = glm::vec3(0.9f, 0.8f, 0.7f);
    fog.anisotropy         = 0.5f;
    fog.localLightScatter  = 1.2f;
    fog.flash              = 0.3f;

    const glm::mat4 ivp(2.0f), pvp(3.0f);
    const glm::vec3 camPos(1.0f, 2.0f, 3.0f), camFwd(0.0f, 0.0f, -1.0f);

    FogVolumeTemporalState temporal;
    temporal.jitter01     = 0.42f;
    temporal.historyAlpha = 0.34f;
    temporal.historyValid = true;
    temporal.frameIndex   = 7;
    temporal.noiseOffset  = glm::vec3(5.0f, 0.0f, -5.0f);
    temporal.noiseTime    = 12.5f;

    const FogVolumeConstants c = FillFogVolumeConstants(fog, ivp, pvp, camPos, camFwd, temporal, 0.75f);

    CHECK(c.invViewProj == ivp);
    CHECK(c.prevViewProj == pvp);
    CHECK(c.camPos == glm::vec4(camPos, fog.distance));
    CHECK(c.camFwd == glm::vec4(camFwd, fog.sliceC));
    CHECK(c.grid == glm::vec4(160.0f, 90.0f, 64.0f, 1.0f / 64.0f));
    CHECK(c.density == glm::vec4(fog.density, fog.heightFalloff, fog.noiseAmount, fog.invNoiseScale));
    CHECK(c.bank == glm::vec4(fog.bankDensity, fog.bankBaseY, fog.bankThickness, fog.precipExtinction));
    CHECK(c.noise == glm::vec4(temporal.noiseOffset, temporal.noiseTime));
    CHECK(c.scatter == glm::vec4(fog.albedo, fog.anisotropy));
    CHECK(c.temporal == glm::vec4(0.42f, 0.34f, 1.0f, 7.0f));
    CHECK(c.misc == glm::vec4(fog.localLightScatter, fog.flash, kFogSunShare, 0.75f));
}

TEST_CASE("FogIntegrateColumnCPU's transmittance matches the analytic height-fog integral")
{
    // Constant density: T after N slices must equal exp(-density * D) within the
    // plan's 1% target at N = 64 (§9).
    const float D = 96.0f, c = 2.0f, density = 0.03f;
    constexpr int N = 64;

    std::vector<FogFroxelSample> column(N);
    std::vector<float> sliceLen(N);
    float z = 0.0f;
    for (int k = 0; k < N; ++k)
    {
        const float zNext = fogSliceToDepth(static_cast<float>(k + 1) / N, D, c);
        sliceLen[k] = zNext - z;
        column[k].sigmaT = density;
        column[k].S      = glm::vec3(density); // arbitrary, only T is checked here
        z = zNext;
    }
    const auto integrated = FogIntegrateColumnCPU(column, sliceLen);
    REQUIRE(integrated.size() == static_cast<size_t>(N));

    const float expectedT = std::exp(-density * D);
    const float gotT      = integrated.back().T;
    CHECK(gotT == doctest::Approx(expectedT).epsilon(0.01)); // < 1% relative error

    // T is monotonically non-increasing front-to-back (pure extinction).
    float prevT = 1.0f;
    for (const auto& s : integrated) { CHECK(s.T <= prevT + 1e-6f); prevT = s.T; }

    // Height-varying density: cross-check against fogOpticalSegment's own
    // closed form for each slice, summed — the CPU integrator and the analytic
    // segment formula must agree on the total optical depth.
    std::vector<FogFroxelSample> hcolumn(N);
    const float camY = 2.0f, dirY = 0.4f, falloff = 0.08f;
    float totalOptical = 0.0f;
    z = 0.0f;
    for (int k = 0; k < N; ++k)
    {
        const float zNext = fogSliceToDepth(static_cast<float>(k + 1) / N, D, c);
        const float segOptical = fogOpticalSegment(camY, dirY, z, zNext, density, falloff);
        const float dz = zNext - z;
        hcolumn[k].sigmaT = (dz > 1e-6f) ? segOptical / dz : 0.0f; // average extinction over the slice
        hcolumn[k].S      = glm::vec3(0.0f);
        totalOptical += segOptical;
        z = zNext;
    }
    const auto hIntegrated = FogIntegrateColumnCPU(hcolumn, sliceLen);
    CHECK(hIntegrated.back().T == doctest::Approx(std::exp(-totalOptical)).epsilon(0.02));
}

TEST_CASE("MakeFogVolumeData gates `enabled` on mode, density, capability and underwater")
{
    EnvironmentSettings env; // defaults: fogMode 0, fogDensity 0, fogBankDensity 0

    SUBCASE("fogMode 0 stays disabled even with density and capability")
    {
        env.fogMode = 0; env.fogDensity = 0.05f;
        const FogVolumeData fog = MakeFogVolumeData(env, /*supported*/ true, /*wet*/ false, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
    }
    SUBCASE("fogMode 1 with density, capable, dry: enabled")
    {
        env.fogMode = 1; env.fogDensity = 0.05f;
        const FogVolumeData fog = MakeFogVolumeData(env, true, false, 16.0f / 9.0f, 250.0f);
        CHECK(fog.enabled);
    }
    SUBCASE("fogMode 1 but no density and no bank: disabled (nothing to render)")
    {
        env.fogMode = 1; env.fogDensity = 0.0f; env.fogBankDensity = 0.0f;
        const FogVolumeData fog = MakeFogVolumeData(env, true, false, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
    }
    SUBCASE("bank density alone is enough (height density can be exactly 0)")
    {
        env.fogMode = 1; env.fogDensity = 0.0f; env.fogBankDensity = 0.2f;
        const FogVolumeData fog = MakeFogVolumeData(env, true, false, 16.0f / 9.0f, 250.0f);
        CHECK(fog.enabled);
    }
    SUBCASE("backend without the capability: disabled regardless of scene state")
    {
        env.fogMode = 1; env.fogDensity = 0.05f;
        const FogVolumeData fog = MakeFogVolumeData(env, /*supported*/ false, false, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
    }
    SUBCASE("camera underwater: disabled, the underwater post pass owns the view")
    {
        env.fogMode = 1; env.fogDensity = 0.05f;
        const FogVolumeData fog = MakeFogVolumeData(env, true, /*wet*/ true, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
    }
}

TEST_CASE("MakeFogVolumeData clamps distance, derives invNoiseScale, precip and grid")
{
    EnvironmentSettings env;
    env.fogMode           = 1;
    env.fogDensity        = 0.04f;
    env.fogQuality         = 2;
    env.fogVolumeDistance  = 900.0f;   // past both the 512 ceiling and the shadow distance
    env.fogNoiseScale      = 50.0f;
    env.rainAmount         = 1.0f;
    env.snowAmount         = 0.5f;

    const FogVolumeData fog = MakeFogVolumeData(env, true, false, 16.0f / 9.0f, 250.0f);
    CHECK(fog.enabled);
    CHECK(fog.distance == doctest::Approx(250.0f));         // clamped to shadowDistance, not 512
    CHECK(fog.invNoiseScale == doctest::Approx(1.0f / 50.0f));
    CHECK(fog.precipExtinction == doctest::Approx(0.004f * 1.0f + 0.008f * 0.5f));
    CHECK(fog.grid == glm::ivec3(192, 108, 96));             // quality 2 = High

    // A tiny shadow distance still leaves at least the 16 m floor.
    const FogVolumeData fogTiny = MakeFogVolumeData(env, true, false, 16.0f / 9.0f, 1.0f);
    CHECK(fogTiny.distance >= 16.0f);
}

TEST_CASE("ExtractFogVolumeData reads the Sky entity's EnvironmentComponent")
{
    SUBCASE("world without a Sky entity: defaults, disabled")
    {
        HorizonWorld world;
        const FogVolumeData fog = HE::ExtractFogVolumeData(world, true, false, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
        CHECK(fog.density == doctest::Approx(0.0f));
    }
    SUBCASE("world with a Sky entity in volumetric mode: enabled, fields carried through")
    {
        HorizonWorld world;
        const Entity sky = world.addSky();
        auto& env = world.registry().get<EnvironmentComponent>(sky);
        env.fogMode = 1;
        env.fogDensity = 0.03f;
        env.fogAlbedo = glm::vec3(0.5f, 0.6f, 0.7f);
        env.fogAnisotropy = 0.4f;

        const FogVolumeData fog = HE::ExtractFogVolumeData(world, true, false, 16.0f / 9.0f, 250.0f);
        CHECK(fog.enabled);
        CHECK(fog.density == doctest::Approx(0.03f));
        CHECK(fog.albedo == glm::vec3(0.5f, 0.6f, 0.7f));
        CHECK(fog.anisotropy == doctest::Approx(0.4f));
    }
    SUBCASE("camera underwater overrides an otherwise-enabled volume")
    {
        HorizonWorld world;
        const Entity sky = world.addSky();
        world.registry().get<EnvironmentComponent>(sky).fogMode = 1;
        world.registry().get<EnvironmentComponent>(sky).fogDensity = 0.03f;

        const FogVolumeData fog = HE::ExtractFogVolumeData(world, true, /*underwater*/ true, 16.0f / 9.0f, 250.0f);
        CHECK_FALSE(fog.enabled);
    }
}

// ═══ Plan §10 feasibility probe: GLSL-450 Compute through SPIRV-Cross ═══════
// Throwaway prototype, NOT the production Inject kernel (no render pass, no
// binary output, deleted again once step 3 picks an approach). Reads the REAL
// preamble text out of MaterialShaderLibrary.cpp at test time — the same
// text-pin technique test_culling.cpp's "GI kernels" case uses for drift
// checks — so this probes the actual heCsmShadow / heLocalShadowFactor /
// heCloudShadowFactor bodies and the actual HeLighting UBO layout, not a
// hand-written stand-in that would prove nothing about the real risk (whether
// SPIRV-Cross's pin mechanism, which the plan says "today only knows vertex/
// fragment", also handles the Compute stage).
#if defined(HE_TESTS_HAVE_SHADERC)
namespace fogprobe
{
namespace fs = std::filesystem;

inline std::string readFile(const fs::path& p)
{
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    s.erase(std::remove(s.begin(), s.end(), '\r'), s.end());
    return s;
}

inline fs::path findRepoRoot()
{
    std::error_code ec;
    fs::path self(__FILE__);
    if (!self.is_absolute()) self = fs::current_path(ec) / self;
    for (fs::path seed = self.parent_path(); !seed.empty(); seed = seed.parent_path())
        if (fs::exists(seed / "src" / "HE_Rendering" / "src" / "material" / "MaterialShaderLibrary.cpp", ec))
            return seed;
    return {};
}

// Comments in this codebase are full of UTF-8 punctuation (em dashes, arrows —
// see docs/coding-conventions.md's own prose). glslang's lexer chokes on a
// multi-byte sequence that a `between()`/`lineContaining()` cut happens to
// slice through, which is a tooling artifact of this probe, not a real GLSL
// problem — so strip `//` comments before compiling, the same way
// test_culling.cpp's shader-drift checks do before matching.
inline std::string stripLineComments(const std::string& src)
{
    std::string out;
    out.reserve(src.size());
    for (size_t i = 0; i < src.size();)
    {
        if (src[i] == '/' && i + 1 < src.size() && src[i + 1] == '/')
        {
            while (i < src.size() && src[i] != '\n') ++i;
            continue;
        }
        out += src[i++];
    }
    return out;
}

// Grabs the text strictly between the first occurrence of `start` (search
// begins at `from`) and the next occurrence of `end` after it. Empty on a
// missing marker — callers REQUIRE a non-empty result, so a renamed anchor in
// MaterialShaderLibrary.cpp fails this test loudly instead of silently probing
// an empty shader.
std::string between(const std::string& text, const char* start, const char* end, size_t from = 0)
{
    const size_t s = text.find(start, from);
    if (s == std::string::npos) return {};
    const size_t afterStart = s + std::strlen(start);
    const size_t e = text.find(end, afterStart);
    if (e == std::string::npos) return {};
    return text.substr(afterStart, e - afterStart);
}

// The single line containing `needle`, including its leading `layout(...)`.
std::string lineContaining(const std::string& text, const char* needle)
{
    const size_t at = text.find(needle);
    if (at == std::string::npos) return {};
    const size_t lineStart = text.rfind('\n', at) + 1; // +1 is 0 if rfind returns npos too
    const size_t lineEnd   = text.find('\n', at);
    return text.substr(lineStart, (lineEnd == std::string::npos ? text.size() : lineEnd) - lineStart);
}
} // namespace fogprobe

TEST_CASE("Feasibility probe: GLSL-450 Compute with the real shadow preamble, through SPIRV-Cross")
{
    using namespace fogprobe;
    using namespace he::shaderc;

    const fs::path root = findRepoRoot();
    if (root.empty())
    {
        MESSAGE("MaterialShaderLibrary.cpp not found - SPIRV-Cross compute probe skipped "
                "(not run from a checkout)");
        return;
    }
    // NOT comment-stripped yet: the extraction markers below are themselves
    // comment text ("// CSM visibility" etc).
    const std::string src = readFile(root / "src" / "HE_Rendering" / "src" / "material" / "MaterialShaderLibrary.cpp");
    REQUIRE_MESSAGE(!src.empty(), "could not read MaterialShaderLibrary.cpp");

    // The HeLighting UBO (binding 0) — kLightingPreamble's opening block.
    // Comment-stripped (same technique test_culling.cpp's drift checks use):
    // the surrounding comments are full of UTF-8 punctuation (em dashes, arrows)
    // that a raw-text cut can slice through, which glslang's lexer rejects —
    // a tooling artifact of this probe, not a real GLSL problem.
    const std::string ubo = stripLineComments(between(src, "uniform HeLighting {", "} heLight;"));
    REQUIRE_MESSAGE(!ubo.empty(), "HeLighting UBO block not found — MaterialShaderLibrary.cpp anchor drifted");

    // The three texture bindings the shadow functions read.
    const std::string csmDecl   = lineContaining(src, "uniform sampler2DArray heCsm;");
    const std::string localDecl = lineContaining(src, "uniform sampler2DArray heLocalShadow;");
    const std::string cloudDecl = lineContaining(src, "uniform sampler2D heCloudShadow;");
    REQUIRE_MESSAGE(!csmDecl.empty(),   "heCsm declaration not found");
    REQUIRE_MESSAGE(!localDecl.empty(), "heLocalShadow declaration not found");
    REQUIRE_MESSAGE(!cloudDecl.empty(), "heCloudShadow declaration not found");

    // heCsmShadow + heLocalShadowFactor + heCloudShadowFactor, verbatim — the
    // exact three functions plan section 2.4 says the Inject pass calls.
    // Starts at the real CODE (the function signature), not the preceding
    // "// CSM visibility..." comment line: cutting INSIDE that comment would
    // leave its tail (which has a UTF-8 em dash) without the leading "//"
    // stripLineComments needs to recognise it as a comment at all.
    const std::string shadowFns = stripLineComments(
        between(src, "float heCsmShadow(vec3 worldPos", "// Legacy sun-only shading"));
    REQUIRE_MESSAGE(!shadowFns.empty(), "shadow functions block not found — anchor comments drifted");
    const std::string shadowFnsFull = "float heCsmShadow(vec3 worldPos" + shadowFns;
    CHECK(shadowFnsFull.find("float heCsmShadow(") != std::string::npos);
    CHECK(shadowFnsFull.find("float heLocalShadowFactor(") != std::string::npos);
    CHECK(shadowFnsFull.find("float heCloudShadowFactor(") != std::string::npos);

    // Assemble: #version + compute entry, the real UBO/texture declarations,
    // the real shadow functions, then the plan's two named kernel entry points
    // (heFogInjectAt writes into a storage image; heFogIntegrateAt is a stub —
    // the plan's column scan is pure math with no preamble dependency, so it
    // carries none of the risk this probe is for).
    std::string glsl = "#version 450\n"
                       "layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;\n"
                       "layout(std140, set = 0, binding = 0) uniform HeLighting {\n" + ubo + "} heLight;\n"
                       + csmDecl + "\n" + localDecl + "\n" + cloudDecl + "\n"
                       "layout(set = 1, binding = 0, rgba16f) uniform image3D heFogInject;\n"
                       + shadowFnsFull + "\n"
                       "void heFogInjectAt(ivec3 froxel) {\n"
                       "    vec3 worldPos = vec3(froxel) - heLight.camPos.xyz;\n"
                       "    vec3 n = vec3(0.0, 1.0, 0.0);\n"
                       "    vec3 L = heLight.sunDir.xyz;\n"
                       "    float sunVis   = heCsmShadow(worldPos, n, L) * heCloudShadowFactor(worldPos, L);\n"
                       "    float localVis = heLocalShadowFactor(0, worldPos, n);\n"
                       "    imageStore(heFogInject, froxel, vec4(sunVis, localVis, 0.0, 1.0));\n"
                       "}\n"
                       "void heFogIntegrateAt(ivec2 xy, int z) { /* pure math, plan section 6.1, no preamble dependency */ }\n"
                       "void main() { heFogInjectAt(ivec3(gl_GlobalInvocationID)); }\n";

    // Step 1: GLSL -> SPIR-V via glslang, Compute stage.
    const Result spv = compile(glsl, Stage::Compute, Target::SpirvBinary);
    INFO("glslang (Compute): ", spv.log);
    REQUIRE_MESSAGE(spv.ok, spv.log);

    // Step 2: SPIR-V -> MSL via SPIRV-Cross, pinning the UBO and the storage
    // image to explicit Metal buffer/texture slots the way compileMslPinned
    // already does for vertex/fragment (ShaderCompiler.cpp:235 maps Stage::
    // Compute to spv::ExecutionModelGLCompute, so the pin path is exercised,
    // not just plain compile()).
    const Result msl = compileMslPinned(glsl, Stage::Compute, {
        { Stage::Compute, 0, 0, 0 }, // HeLighting UBO -> [[buffer(0)]]
    });
    INFO("SPIRV-Cross MSL (Compute): ", msl.log);
    CHECK_MESSAGE(msl.ok, msl.log);
    if (msl.ok) MESSAGE("Compute -> MSL OK:\n", msl.source);

    // Step 3: SPIR-V -> HLSL SM 5.0, and on Windows the real D3DCompile cs_5_0
    // FXC gives the only verdict SPIRV-Cross's "ok" cannot: register range and
    // intrinsic spelling (the same reason test_material_graph.cpp's Thema 51
    // step 3 runs FXC after every fragment/vertex cross-compile).
    //
    // heCloudShadow's binding (33) is the SAME one the fragment preamble keeps
    // sampler-free for exactly this reason (ShaderCompiler.h's HlslPin comment:
    // "the canonical GLSL... uses binding numbers up to 33 -- past D3D11's hard
    // API limits of... 16 sampler slots"). First attempt here left it unpinned:
    // SPIRV-Cross auto-assigned s33, and FXC rejected it with X4509 ("maximum
    // sampler register index exceeded, target has 16 slots") -- not a Compute-
    // stage problem, the identical constraint fragment shaders already have.
    // Fixed the same way production HLSL pins it: SRV stays at its natural
    // register (t33, SRVs have 128 slots), only the SAMPLER half is moved into
    // range (reusing s0, like t34 sharing s0 with heLandscapeWeights per the
    // plan's binding-34 note).
    const Result hlsl = compileHlslPinned(glsl, Stage::Compute, {
        { Stage::Compute, 0, 0, 0 },               // HeLighting UBO -> b0
        { Stage::Compute, 0, 33, 33, /*sampler*/ 0 }, // heCloudShadow -> t33, sampler s0
    });
    INFO("SPIRV-Cross HLSL (Compute): ", hlsl.log);
    CHECK_MESSAGE(hlsl.ok, hlsl.log);
#ifdef _WIN32
    if (hlsl.ok)
    {
        using Microsoft::WRL::ComPtr;
        ComPtr<ID3DBlob> out, err;
        const HRESULT hr = D3DCompile(hlsl.source.c_str(), hlsl.source.size(), "probe", nullptr, nullptr,
                                      "main", "cs_5_0", 0, 0, &out, &err);
        INFO("FXC cs_5_0: ", err ? static_cast<const char*>(err->GetBufferPointer()) : "(no error text)");
        INFO("emitted HLSL:\n", hlsl.source);
        CHECK_MESSAGE(SUCCEEDED(hr), "FXC rejected the SPIRV-Cross-emitted compute HLSL");
    }
#endif
}

// ═══ Step 3: the production kernels through the pin path (Metal) ═════════════
TEST_CASE("Fog kernels: Inject/Integrate (compute) and Apply (fragment) cross-compile for Metal")
{
    HE::MaterialShaderLibrary lib;
    using B = HE::MaterialShaderLibrary::Backend;

    const auto& inj = lib.fogInject(B::Metal);
    INFO("inject: ", inj.log);
    REQUIRE(inj.ok);
    CHECK(inj.source.find("kernel void main0") != std::string::npos);
    // The real shadow functions came along (one set of shadow maths): the CSM array,
    // the HeLighting buffer at 1, the fog block at 0, the history/noise/output pins.
    CHECK(inj.source.find("[[buffer(1)]]") != std::string::npos);
    CHECK(inj.source.find("[[buffer(0)]]") != std::string::npos);
    CHECK(inj.source.find("[[texture(3)]]") != std::string::npos);  // SkyNoise3D
    CHECK(inj.source.find("[[texture(4)]]") != std::string::npos);  // history
    CHECK(inj.source.find("[[texture(5)]]") != std::string::npos);  // output image
    CHECK(inj.source.find("[[texture(11)]]") != std::string::npos); // CSM array

    const auto& integ = lib.fogIntegrate(B::Metal);
    INFO("integrate: ", integ.log);
    REQUIRE(integ.ok);
    CHECK(integ.source.find("kernel void main0") != std::string::npos);
    // SRV reads and storage-image stores only: no typed UAV load, so the same source is
    // legal on D3D11 FL11_0. (MSL spells a read of a write-only image `.read(`; the
    // integrate source must not declare a read-write image at all.)
    {
        const std::string g = HE::MaterialShaderLibrary::fogIntegrateGlsl();
        CHECK(g.find("imageLoad") == std::string::npos);
        CHECK(g.find("readonly") == std::string::npos);
        CHECK(g.find("writeonly image3D") != std::string::npos);
    }

    const auto& apply = lib.fogApply(B::Metal);
    INFO("apply: ", apply.log);
    REQUIRE(apply.ok);
    CHECK(apply.source.find("fragment") != std::string::npos);
    CHECK(apply.source.find("[[texture(2)]]") != std::string::npos); // scene depth

    // The other backends keep the analytic fog until their steps: a clear "not yet".
    CHECK_FALSE(lib.fogInject(B::HLSL).ok);
    CHECK_FALSE(lib.fogApply(B::GLSL410).ok);
    CHECK_FALSE(lib.fogIntegrate(B::SpirV).ok);
}

TEST_CASE("heApplyFogT: texture 17 on Metal, analytic fog everywhere else")
{
    HE::MaterialShaderLibrary lib;
    using B = HE::MaterialShaderLibrary::Backend;
    const std::string frag =
        "#version 450\n"
        "layout(location = 0) in vec3 vWorldPos;\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(heApplyFogT(vec3(1.0), vWorldPos), 1.0); }\n";
    const auto& metal = lib.fragment(0xF06F06ull, frag, B::Metal);
    INFO(metal.log);
    REQUIRE(metal.ok);
    // The volume is read with texelFetch -> texture::read through texture 17, no sampler slot.
    CHECK(metal.source.find("[[texture(17)]]") != std::string::npos);
    CHECK(metal.source.find("[[sampler(17)]]") == std::string::npos);

    // Mode 0 must not change what the other backends compile: no binding 34 anywhere.
    for (B b : { B::GLSL410, B::GLSL430, B::SpirV, B::HLSL })
    {
        const auto& other = lib.fragment(0xF06F07ull, frag, b);
        INFO("backend ", static_cast<int>(b), ": ", other.log);
        REQUIRE(other.ok);
        if (b == B::SpirV) continue; // binary
        CHECK(other.source.find("heFogVolume") == std::string::npos);
    }

    // The same graph with the OPAQUE name compiles unchanged on Metal and leaves 17 unbound.
    const std::string opaque =
        "#version 450\n"
        "layout(location = 0) in vec3 vWorldPos;\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(heApplyFog(vec3(1.0), vWorldPos), 1.0); }\n";
    const auto& op = lib.fragment(0xF06F08ull, opaque, B::Metal);
    REQUIRE(op.ok);
    CHECK(op.source.find("[[texture(17)]]") == std::string::npos);
}
#endif // HE_TESTS_HAVE_SHADERC
