// Compiled once per profile into its own static library; HE_PROBE_FN names the
// function each copy defines (see tests/CMakeLists.txt). Mixing profiles in one
// binary is safe only because this file includes nothing but the macro header,
// whose C++ constants have internal linkage. Engine code must never do this.

#include "Platform/BuildProfile.h"
#include "build_profile_probe.h"

namespace {
int sum(int a, int b) { return a + b; }
} // namespace

namespace HE::test {

BuildProfileProbe HE_PROBE_FN()
{
    BuildProfileProbe p;
    p.dev = HE_BUILD_PROFILE_DEV;
    p.release = HE_BUILD_PROFILE_RELEASE;
    p.defaulted = HE_BUILD_PROFILE_DEFAULTED;
    p.devHotkeys = HE_FEATURE_DEV_HOTKEYS;
    p.profilerCapture = HE_FEATURE_PROFILER_CAPTURE;
    p.testHooks = HE_FEATURE_TEST_HOOKS;
    p.envOverrides = HE_FEATURE_ENV_OVERRIDES;
    // The argument carries a comma on purpose: the helpers are variadic.
    p.devOnly = 0;
    HE_DEV_ONLY(p.devOnly = sum(1, 2);)
    p.releaseOnly = 0;
    HE_RELEASE_ONLY(p.releaseOnly = sum(3, 4);)
    p.kDev = HE::kDevBuild;
    p.kRelease = HE::kReleaseBuild;
    p.name = HE::kBuildProfileName;
    return p;
}

} // namespace HE::test
