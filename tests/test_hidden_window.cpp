#include "doctest.h"
#include <Window/Window.h>

// Hidden mode decides whether a test or script run puts a window on somebody's
// screen. The decision is three environment variables deep, and the one case
// that matters most is the one easiest to get backwards: HE_HIDDEN_WINDOW=0 has
// to WIN over a frame budget, or there is no way left to watch a budgeted run.

TEST_CASE("Hidden mode: nothing set means a normal, visible run")
{
    CHECK_FALSE(HE::hiddenWindowFromEnv(nullptr, nullptr, nullptr));
    CHECK_FALSE(HE::hiddenWindowFromEnv("", "", ""));
}

TEST_CASE("Hidden mode: HE_HIDDEN_WINDOW decides on its own")
{
    CHECK(HE::hiddenWindowFromEnv("1", nullptr, nullptr));
    CHECK(HE::hiddenWindowFromEnv("yes", nullptr, nullptr));
    CHECK_FALSE(HE::hiddenWindowFromEnv("0", nullptr, nullptr));
    // "00" and "0x" are not the literal "0" — anything but that hides.
    CHECK(HE::hiddenWindowFromEnv("00", nullptr, nullptr));
}

TEST_CASE("Hidden mode: a frame budget or a dump hides by default")
{
    CHECK(HE::hiddenWindowFromEnv(nullptr, "30", nullptr));
    CHECK(HE::hiddenWindowFromEnv(nullptr, nullptr, "/tmp/shot.bmp"));
    // A budget of zero is no budget (Application reads it the same way).
    CHECK_FALSE(HE::hiddenWindowFromEnv(nullptr, "0", nullptr));
    CHECK_FALSE(HE::hiddenWindowFromEnv(nullptr, "", nullptr));
}

TEST_CASE("Hidden mode: HE_HIDDEN_WINDOW=0 wins over the automatic triggers")
{
    CHECK_FALSE(HE::hiddenWindowFromEnv("0", "30", nullptr));
    CHECK_FALSE(HE::hiddenWindowFromEnv("0", nullptr, "/tmp/shot.bmp"));
    CHECK_FALSE(HE::hiddenWindowFromEnv("0", "30", "/tmp/shot.bmp"));
    // …and an empty HE_HIDDEN_WINDOW is "not set", not "0".
    CHECK(HE::hiddenWindowFromEnv("", "30", nullptr));
}
