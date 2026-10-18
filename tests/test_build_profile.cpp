#include "doctest.h"
#include "build_profile_probe.h"

#include <string>

// Platform/BuildProfile.h and he_target_build_profile() (cmake/HorizonBuildProfile.cmake).
// he_tests itself has ONE profile, so each profile is looked at through a probe:
// build_profile_probe.cpp compiled into its own static library per profile.
// The probes go through the CMake function, so a wrong definition there fails
// here too, not only a wrong macro.

using HE::test::BuildProfileProbe;

namespace {

void checkFeatures(const BuildProfileProbe& p, int expected)
{
    CHECK(p.devHotkeys == expected);
    CHECK(p.profilerCapture == expected);
    CHECK(p.testHooks == expected);
    CHECK(p.envOverrides == expected);
}

} // namespace

TEST_CASE("Build profile dev: dev macros on, every feature on")
{
    const BuildProfileProbe p = HE::test::buildProfileProbeDev();
    CHECK(p.dev == 1);
    CHECK(p.release == 0);
    CHECK(p.defaulted == 0);
    checkFeatures(p, 1);
    CHECK(p.devOnly == 3);
    CHECK(p.releaseOnly == 0);
    CHECK(p.kDev);
    CHECK_FALSE(p.kRelease);
    CHECK(std::string(p.name) == "dev");
}

TEST_CASE("Build profile release: dev code and every feature are compiled out")
{
    const BuildProfileProbe p = HE::test::buildProfileProbeRelease();
    CHECK(p.dev == 0);
    CHECK(p.release == 1);
    CHECK(p.defaulted == 0);
    checkFeatures(p, 0);
    CHECK(p.devOnly == 0);
    CHECK(p.releaseOnly == 7);
    CHECK_FALSE(p.kDev);
    CHECK(p.kRelease);
    CHECK(std::string(p.name) == "release");
}

TEST_CASE("Build profile: a target CMake gave no profile is a dev build, and says so")
{
    // Every target that does not call he_target_build_profile yet (the whole
    // engine, as of step 2) must keep compiling as it did before.
    const BuildProfileProbe p = HE::test::buildProfileProbeDefault();
    const BuildProfileProbe dev = HE::test::buildProfileProbeDev();
    CHECK(p.defaulted == 1);
    CHECK(p.dev == dev.dev);
    CHECK(p.release == dev.release);
    checkFeatures(p, 1);
    CHECK(p.devOnly == dev.devOnly);
    CHECK(p.releaseOnly == dev.releaseOnly);
    CHECK(std::string(p.name) == "dev");
}

TEST_CASE("Build profile: a FEATURES override changes one switch, not the profile")
{
    const BuildProfileProbe p = HE::test::buildProfileProbeOverride();
    CHECK(p.dev == 0);
    CHECK(p.release == 1);
    CHECK(p.testHooks == 1);
    CHECK(p.devHotkeys == 0);
    CHECK(p.profilerCapture == 0);
    CHECK(p.envOverrides == 0);
    CHECK(p.devOnly == 0);
    CHECK(p.releaseOnly == 7);
}
