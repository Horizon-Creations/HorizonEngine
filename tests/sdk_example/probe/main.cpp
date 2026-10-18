// SDK probe (Thema 211 step 5): compiled OUTSIDE the engine tree against an
// installed SDK by tests/sdk_example/run_sdk_example.cmake. One exported call
// into each engine library (so every import really resolves at link and load
// time), and the profile the SDK's imported targets handed this TU.
#include <Platform/BuildProfile.h>
#include <Hpak/ProjectConfig.h>
#include <Hpak/GameBuildConfig.h>
#include <Net/HttpClient.h>
#include <HorizonScene/HcCodegen.h>
#include <HorizonEngine/HorizonEngineApi.h>   // what generated HorizonCode includes (step 6)
#include <HorizonRendering/SsaoKernel.h>
#include <HorizonVersion.h>

#include <cstdio>
#include <string>

#if HE_BUILD_PROFILE_DEFAULTED
#  error "the SDK's imported targets did not carry HE_BUILD_PROFILE_DEV/RELEASE"
#endif
#ifndef HE_EXPECT_PROFILE
#  error "configure the probe with HE_EXPECT_PROFILE"
#endif

int main()
{
    ProjectConfig cfg;   // (global namespace) unencrypted: no key, no mask needed
    const std::string header = HE::generateGameBuildConfigHeader(cfg, nullptr, HE::kBuildProfileName);

    HE::Net::HttpUrl url;
    const bool urlOk = HE::Net::httpParseUrl("http://example.com:8080/sdk", url) && url.port == 8080;

    const std::string cmake = HE::hccg::generateCMakeLists(HE::hccg::Options{}, {"probe.cpp"});

    const auto kernel = HE::BuildSSAOKernel(4);

    // The engine API as generated code calls it: a Free row and a Ctx row
    // through a Scope (unbound here, so the null Ctx answers).
    HorizonCode::Context unbound;
    HE::api::Scope api(unbound);
    const bool apiOk = HE::api::math::clamp(5.0f, 0.0f, 1.0f) == 1.0f &&
                       HE::api::entity::self(api) == 0u;

    std::printf("engine=%s profile=%s features=%d%d%d%d\n", HE_VERSION_STRING, HE::kBuildProfileName,
                HE_FEATURE_DEV_HOTKEYS, HE_FEATURE_PROFILER_CAPTURE, HE_FEATURE_TEST_HOOKS,
                HE_FEATURE_ENV_OVERRIDES);
    std::printf("core=%d net=%d scene=%d rendering=%d api=%d\n", header.empty() ? 0 : 1, urlOk ? 1 : 0,
                cmake.find("HorizonCodeGen") != std::string::npos ? 1 : 0, kernel.size() == 4 ? 1 : 0,
                apiOk ? 1 : 0);

    const bool profileOk = std::string(HE::kBuildProfileName) == HE_EXPECT_PROFILE;
    return (profileOk && !header.empty() && urlOk && !cmake.empty() && kernel.size() == 4 && apiOk)
        ? 0 : 1;
}
