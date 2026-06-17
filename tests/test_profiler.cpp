#include "doctest.h"
#include <Diagnostics/Profiler.h>

// Compile-time test: all three profiling macros must expand without errors
// regardless of whether Tracy is enabled (default: OFF → no-ops).

TEST_CASE("Profiler macros compile and execute as no-ops when Tracy is disabled")
{
    // HE_PROFILE_SCOPE and HE_PROFILE_SCOPE_N are block-scoped — wrap each in
    // its own scope so multiple declarations don't collide.
    { HE_PROFILE_SCOPE(); }
    { HE_PROFILE_SCOPE_N("TestZone"); }
    HE_PROFILE_FRAME();
    CHECK(true); // if we got here the macros compiled and ran cleanly
}

TEST_CASE("Profiler scope macro can nest")
{
    {
        HE_PROFILE_SCOPE_N("outer");
        {
            HE_PROFILE_SCOPE_N("inner");
        }
    }
    CHECK(true);
}
