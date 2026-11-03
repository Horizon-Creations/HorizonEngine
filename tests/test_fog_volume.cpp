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
#include <Backends/Vulkan/VulkanMaterialLayout.h> // the fog passes' descriptor tables (Thema 225 step 5)
#include <cmath>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#if defined(HE_TESTS_HAVE_SHADERC)
#include "ShaderCompiler.h"
#include <spirv_cross.hpp>   // Vulkan reflection (the same linkage as test_material_graph.cpp)
#include <vector>
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
    SUBCASE("a Sky added today starts volumetric, the struct default stays analytic (step 7)")
    {
        HorizonWorld world;
        const Entity sky = world.addSky();
        CHECK(world.registry().get<EnvironmentComponent>(sky).fogMode == 1);
        CHECK(EnvironmentComponent{}.fogMode == 0);        // what a scene without the key loads as
        // On, but nothing to draw yet: no density, no bank -> no volume, no cost.
        CHECK_FALSE(HE::ExtractFogVolumeData(world, true, false, 16.0f / 9.0f, 250.0f).enabled);
        // A backend without the volume (D3D11/D3D12) keeps the analytic fog under mode 1.
        world.registry().get<EnvironmentComponent>(sky).fogDensity = 0.03f;
        CHECK_FALSE(HE::ExtractFogVolumeData(world, false, false, 16.0f / 9.0f, 250.0f).enabled);
        CHECK(HE::ExtractFogVolumeData(world, true, false, 16.0f / 9.0f, 250.0f).enabled);
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

// ═══ Plan §10 resolved: the step-2 feasibility probe (a hand-assembled compute kernel
// text-pinned out of the preamble) is gone — step 3's production kernels below go through
// the same glslang -> SPIRV-Cross pin path for real. ═══
#if defined(HE_TESTS_HAVE_SHADERC)
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

    // D3D (step 6) and GL 4.3 keep the analytic fog for now: a clear "not yet".
    CHECK_FALSE(lib.fogInject(B::HLSL).ok);
    CHECK_FALSE(lib.fogApply(B::GLSL430).ok);
    CHECK_FALSE(lib.fogIntegrate(B::HLSL).ok);
}

// ═══ Step 4: the GL fragment-path kernels (one fullscreen quad per Z slice,
// no compute/image-load-store — GL 4.1 core has neither) ════════════════════
TEST_CASE("Fog kernels (GL fragment path): Inject/Integrate/Apply cross-compile for GLSL410")
{
    HE::MaterialShaderLibrary lib;
    using B = HE::MaterialShaderLibrary::Backend;

    const auto& inj = lib.fogInject(B::GLSL410);
    INFO("inject: ", inj.log);
    REQUIRE(inj.ok);
    CHECK(inj.source.find("void main") != std::string::npos);
    // A real fragment shader, unlike Metal's compute kernel: no imageStore, a
    // plain `out` write; the slice index rides the UBO (fv.pad.w), never a
    // loose non-block uniform (illegal under the Vulkan GLSL semantics this
    // source compiles with).
    CHECK(inj.source.find("imageStore") == std::string::npos);
    CHECK(inj.source.find("gl_FragCoord") != std::string::npos);
    // The real shadow functions came along (one set of shadow maths, same as
    // Metal's inject): the CSM array, HeLighting, history/noise.
    const bool hasShadowFns = inj.source.find("heCsmShadow") != std::string::npos
                            || inj.source.find("HeLighting") != std::string::npos;
    CHECK(hasShadowFns);

    const auto& integ = lib.fogIntegrate(B::GLSL410);
    INFO("integrate: ", integ.log);
    REQUIRE(integ.ok);
    CHECK(integ.source.find("imageStore") == std::string::npos);
    CHECK(integ.source.find("imageLoad") == std::string::npos);
    // MRT: the next accumulator AND this slice's layer of the integrated volume.
    {
        const std::string g = HE::MaterialShaderLibrary::fogIntegrateGlslGL();
        CHECK(g.find("location = 0) out vec4 oAccum") != std::string::npos);
        CHECK(g.find("location = 1) out vec4 oLayer") != std::string::npos);
    }

    const auto& apply = lib.fogApply(B::GLSL410);
    INFO("apply: ", apply.log);
    REQUIRE(apply.ok);
    CHECK(apply.source.find("void main") != std::string::npos);
    CHECK(apply.source.find("heFogIntegrated") != std::string::npos);

    // D3D stays off until step 6: a clear "not yet", same shape as the Metal test above.
    CHECK_FALSE(lib.fogInject(B::HLSL).ok);
    CHECK_FALSE(lib.fogIntegrate(B::HLSL).ok);
    CHECK_FALSE(lib.fogApply(B::GLSL430).ok);
}

TEST_CASE("heApplyFogT: texture 17 on Metal, heFogVolume by name on GL, analytic fog elsewhere")
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

    // GL (step 4): heFogVolume declared and bound by NAME (GLSL410 has no
    // layout(binding=)) — OpenGLRenderer::GetOrBuildMaterialProgram's
    // setupProgram pins it to texture unit 21 (kGlFogVolumeTextureUnit).
    const auto& gl = lib.fragment(0xF06F09ull, frag, B::GLSL410);
    INFO(gl.log);
    REQUIRE(gl.ok);
    CHECK(gl.source.find("heFogVolume") != std::string::npos);
    CHECK(gl.source.find("sampler3D") != std::string::npos);

    // Mode 0 must not change what D3D / the clustered GL variant compile:
    // no binding 34 there until step 6 binds it too. (Vulkan joined in step 5: see the
    // "Fog kernels (Vulkan)" case below, which pins binding 34 in the SPIR-V.)
    for (B b : { B::GLSL430, B::HLSL })
    {
        const auto& other = lib.fragment(0xF06F07ull, frag, b);
        INFO("backend ", static_cast<int>(b), ": ", other.log);
        REQUIRE(other.ok);
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

// ═══ Step 5: Vulkan. GLSL 450 -> SPIR-V as it stands (compute for Inject/Integrate), the
// descriptors each module uses held against the tables the renderer builds its layouts from
// (the one Vulkan check CI's device-less jobs have; the lavapipe image test is the other) ══════
namespace
{
struct FogUse
{
    uint32_t binding = 0;
    int      kind    = 0; // HE::vkmat::FogDesc
    bool operator<(const FogUse& o) const { return binding != o.binding ? binding < o.binding : kind < o.kind; }
    bool operator==(const FogUse& o) const { return binding == o.binding && kind == o.kind; }
};

// The STATICALLY used descriptors of a module (set must be 0), as the renderer's table kinds.
std::vector<FogUse> activeFogDescriptors(const std::vector<uint32_t>& spirv, std::string& err)
{
    std::vector<FogUse> out;
    try
    {
        spirv_cross::Compiler comp(spirv);
        const spirv_cross::ShaderResources r = comp.get_shader_resources(comp.get_active_interface_variables());
        using K = HE::vkmat::FogDesc;
        auto add = [&](const spirv_cross::SmallVector<spirv_cross::Resource>& list, K kind) {
            for (const spirv_cross::Resource& res : list)
            {
                if (comp.get_decoration(res.id, spv::DecorationDescriptorSet) != 0) err += " set!=0";
                out.push_back({ comp.get_decoration(res.id, spv::DecorationBinding), static_cast<int>(kind) });
            }
        };
        add(r.uniform_buffers,  K::Ubo);
        add(r.sampled_images,   K::Cis);
        add(r.separate_images,  K::SampledImage);
        add(r.storage_images,   K::StorageImage);
        if (!r.storage_buffers.empty() || !r.separate_samplers.empty() || !r.push_constant_buffers.empty())
            err += " unexpected resource kind";
    }
    catch (const std::exception& e) { err = e.what(); }
    std::sort(out.begin(), out.end());
    return out;
}

// What `uses` needs that `table` (for `stage`) does not provide, as "binding:kind" text.
std::string uncoveredFog(const std::vector<FogUse>& uses, const HE::vkmat::FogBinding* table, uint32_t n, uint32_t stage)
{
    std::string miss;
    for (const FogUse& u : uses)
    {
        bool ok = false;
        for (uint32_t i = 0; i < n && !ok; ++i)
            ok = table[i].binding == u.binding && static_cast<int>(table[i].kind) == u.kind && (table[i].stages & stage);
        if (!ok) miss += " " + std::to_string(u.binding) + ":" + std::to_string(u.kind);
    }
    return miss;
}
} // namespace

TEST_CASE("Fog kernels (Vulkan): Inject/Integrate (compute) and Apply (fragment) SPIR-V use only the fog layouts' descriptors (Thema 225 step 5)")
{
    HE::MaterialShaderLibrary lib;
    using B = HE::MaterialShaderLibrary::Backend;
    namespace vk = HE::vkmat;

    const auto& inj = lib.fogInject(B::SpirV);
    const auto& integ = lib.fogIntegrate(B::SpirV);
    const auto& apply = lib.fogApply(B::SpirV);
    INFO("inject: ", inj.log);
    INFO("integrate: ", integ.log);
    INFO("apply: ", apply.log);
    REQUIRE(inj.ok);
    REQUIRE(integ.ok);
    REQUIRE(apply.ok);
    REQUIRE_FALSE(inj.spirv.empty());
    CHECK(inj.spirv[0] == 0x07230203u);
    CHECK(integ.spirv[0] == 0x07230203u);
    CHECK(apply.spirv[0] == 0x07230203u);

    std::string err;
    const std::vector<FogUse> ui = activeFogDescriptors(inj.spirv, err);
    const std::vector<FogUse> ug = activeFogDescriptors(integ.spirv, err);
    const std::vector<FogUse> ua = activeFogDescriptors(apply.spirv, err);
    CHECK_MESSAGE(err.empty(), err);

    CHECK_MESSAGE(uncoveredFog(ui, vk::kFogInjectBindings, vk::kFogInjectBindingCount, vk::kStageCompute).empty(),
                  "inject uses, not in the layout:", uncoveredFog(ui, vk::kFogInjectBindings, vk::kFogInjectBindingCount, vk::kStageCompute));
    CHECK_MESSAGE(uncoveredFog(ug, vk::kFogIntegrateBindings, vk::kFogIntegrateBindingCount, vk::kStageCompute).empty(),
                  "integrate uses, not in the layout:", uncoveredFog(ug, vk::kFogIntegrateBindings, vk::kFogIntegrateBindingCount, vk::kStageCompute));
    CHECK_MESSAGE(uncoveredFog(ua, vk::kFogApplyBindings, vk::kFogApplyBindingCount, vk::kStageFragment).empty(),
                  "apply uses, not in the layout:", uncoveredFog(ua, vk::kFogApplyBindings, vk::kFogApplyBindingCount, vk::kStageFragment));

    // The kernels' own resources are really in use (the table is not vacuous): the noise and the
    // history as combined samplers, the output as a storage image, the constants block.
    auto has = [](const std::vector<FogUse>& u, uint32_t b, vk::FogDesc k) {
        return std::find(u.begin(), u.end(), FogUse{ b, static_cast<int>(k) }) != u.end();
    };
    CHECK(has(ui, 36, vk::FogDesc::Ubo));
    CHECK(has(ui, 0, vk::FogDesc::Ubo));
    CHECK(has(ui, 35, vk::FogDesc::Cis));
    CHECK(has(ui, 37, vk::FogDesc::Cis));
    CHECK(has(ui, 38, vk::FogDesc::StorageImage));
    CHECK(has(ui, 12, vk::FogDesc::Cis));   // the real CSM array came along
    CHECK(has(ug, 37, vk::FogDesc::SampledImage));
    CHECK(has(ug, 38, vk::FogDesc::StorageImage));
    CHECK(has(ua, 34, vk::FogDesc::Cis));
    CHECK(has(ua, 39, vk::FogDesc::SampledImage));

    // Negative control: a table without the history row does NOT cover the inject kernel.
    {
        std::vector<vk::FogBinding> cut;
        for (const vk::FogBinding& b : vk::kFogInjectBindings) if (b.binding != 37) cut.push_back(b);
        CHECK_FALSE(uncoveredFog(ui, cut.data(), static_cast<uint32_t>(cut.size()), vk::kStageCompute).empty());
    }

    // Integrate fetches and stores, never loads from the typed image.
    CHECK(HE::MaterialShaderLibrary::fogIntegrateGlsl().find("imageLoad") == std::string::npos);
}

TEST_CASE("heApplyFogT on Vulkan: binding 34 is a fragment sampler of the material layout, absent when the layout drops it (Thema 225 step 5)")
{
    using B = HE::MaterialShaderLibrary::Backend;
    namespace vk = HE::vkmat;
    const std::string frag =
        "#version 450\n"
        "layout(location = 0) in vec3 vWorldPos;\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(heApplyFogT(vec3(1.0), vWorldPos), 1.0); }\n";
    auto uses34 = [&](HE::MaterialShaderLibrary& lib, uint64_t h) {
        const auto& f = lib.fragment(h, frag, B::SpirV);
        REQUIRE_MESSAGE(f.ok, f.log);
        std::string err;
        spirv_cross::Compiler comp(f.spirv);
        const spirv_cross::ShaderResources r = comp.get_shader_resources(comp.get_active_interface_variables());
        bool found = false;
        for (const spirv_cross::Resource& res : r.sampled_images)
            found = found || comp.get_decoration(res.id, spv::DecorationBinding) == vk::kFogVolumeBinding;
        return found;
    };

    HE::MaterialShaderLibrary lib;
    CHECK(lib.spirvFogVolume());
    CHECK(uses34(lib, 0xF06F20ull));
    // The row exists in the layout, in the fragment stage, as a combined sampler.
    bool row = false;
    for (uint32_t i = 0; i < vk::kBindingCount; ++i)
        row = row || (vk::kBindings[i].binding == vk::kFogVolumeBinding
                      && vk::kBindings[i].kind == vk::DescKind::CombinedImageSampler
                      && (vk::kBindings[i].stages & vk::kStageFragment));
    CHECK(row);
    // ... ahead of the cluster lists (they stay the LAST rows).
    CHECK(vk::kBindings[vk::kPreClusterBindingCount - 1].binding == vk::kFogVolumeBinding);

    // A device that cannot hold the 18th sampler compiles without it (and without the row).
    // (Not named `small`: rpcndr.h #defines it to `char` on Windows - MSVC C2628, CI run 41529249343.)
    HE::MaterialShaderLibrary noFogLib;
    noFogLib.setSpirvFogVolume(false);
    CHECK_FALSE(uses34(noFogLib, 0xF06F21ull));

    // The OPAQUE name never touches the volume on Vulkan (the Apply pass owns the opaque fog).
    const std::string opaque =
        "#version 450\n"
        "layout(location = 0) in vec3 vWorldPos;\n"
        "layout(location = 0) out vec4 oColor;\n"
        "void main() { oColor = vec4(heApplyFog(vec3(1.0), vWorldPos), 1.0); }\n";
    const auto& op = lib.fragment(0xF06F22ull, opaque, B::SpirV);
    REQUIRE_MESSAGE(op.ok, op.log);
    spirv_cross::Compiler comp(op.spirv);
    const spirv_cross::ShaderResources r = comp.get_shader_resources(comp.get_active_interface_variables());
    for (const spirv_cross::Resource& res : r.sampled_images)
        CHECK(comp.get_decoration(res.id, spv::DecorationBinding) != vk::kFogVolumeBinding);
}
#endif // HE_TESTS_HAVE_SHADERC
