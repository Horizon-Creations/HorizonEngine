#pragma once

// What one translation unit saw of Platform/BuildProfile.h. build_profile_probe.cpp
// is compiled four times, each time under another profile, into its own static
// library (tests/CMakeLists.txt); test_build_profile.cpp compares the four.
// This struct is the same in every profile, only the function bodies differ.

namespace HE::test {

struct BuildProfileProbe {
    int dev = -1;
    int release = -1;
    int defaulted = -1;
    int devHotkeys = -1;
    int profilerCapture = -1;
    int testHooks = -1;
    int envOverrides = -1;
    int devOnly = -1;      // what HE_DEV_ONLY(...) let through
    int releaseOnly = -1;  // what HE_RELEASE_ONLY(...) let through
    bool kDev = false;
    bool kRelease = false;
    const char* name = "";
};

BuildProfileProbe buildProfileProbeDev();       // he_target_build_profile(PROFILE dev)
BuildProfileProbe buildProfileProbeRelease();   // he_target_build_profile(PROFILE release)
BuildProfileProbe buildProfileProbeDefault();   // no he_target_build_profile call
BuildProfileProbe buildProfileProbeOverride();  // release + FEATURES TEST_HOOKS=1

} // namespace HE::test
