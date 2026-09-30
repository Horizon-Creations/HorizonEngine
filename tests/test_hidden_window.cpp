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

// ── Background throttle ────────────────────────────────────────────────────
// The orphaned hidden editor of perf audit B1 ran with HE_HIDDEN_WINDOW=1 and
// nothing else, and took a third to a half of the GPU. That run has to be
// throttled; a screenshot or a frame budget, which are hidden too, must not be.

namespace
{
    constexpr uint64_t kDefaultNs =
        static_cast<uint64_t>(1.0e9 / HE::kDefaultBackgroundFps);
}

TEST_CASE("Background throttle: a hidden run with nothing to draw for is throttled")
{
    // The orphan: hidden mode came from HE_HIDDEN_WINDOW alone, which is not
    // an input here at all — it is exactly the case with no exemption.
    const uint64_t interval = HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, nullptr, nullptr);
    CHECK(interval == kDefaultNs);
    CHECK(HE::backgroundFrameIntervalFromEnv("", "", "", "") == kDefaultNs);
    // A 1 ms frame in a hidden/occluded/minimised window sleeps the rest.
    CHECK(HE::backgroundThrottleDelayNs(interval, true, false, 1'000'000) == interval - 1'000'000);
    // A frame that already took longer than the interval does not sleep more.
    CHECK(HE::backgroundThrottleDelayNs(interval, true, false, interval + 1) == 0);
}

TEST_CASE("Background throttle: a window someone can see is never throttled")
{
    const uint64_t interval = HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, nullptr, nullptr);
    CHECK(HE::backgroundThrottleDelayNs(interval, false, false, 0) == 0);
    CHECK(HE::backgroundThrottleDelayNs(interval, false, false, 1'000'000) == 0);
}

TEST_CASE("Background throttle: dump, frame budget and capture runs keep full frames")
{
    CHECK(HE::backgroundFrameIntervalFromEnv(nullptr, "30", nullptr, nullptr) == 0);
    CHECK(HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, "/tmp/shot.bmp", nullptr) == 0);
    CHECK(HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, nullptr, "120") == 0);
    // …even when somebody also asked for a background rate: the picture wins.
    CHECK(HE::backgroundFrameIntervalFromEnv("5", "30", nullptr, nullptr) == 0);
    CHECK(HE::backgroundFrameIntervalFromEnv("5", nullptr, "/tmp/shot.bmp", nullptr) == 0);
    // And an unthrottled run does not sleep in the background either.
    CHECK(HE::backgroundThrottleDelayNs(0, true, false, 0) == 0);
    // A budget or capture of zero is none (Application reads them the same way).
    CHECK(HE::backgroundFrameIntervalFromEnv(nullptr, "0", nullptr, nullptr) == kDefaultNs);
    CHECK(HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, nullptr, "0") == kDefaultNs);
}

TEST_CASE("Background throttle: HE_BACKGROUND_FPS sets the rate, 0 turns it off")
{
    CHECK(HE::backgroundFrameIntervalFromEnv("0", nullptr, nullptr, nullptr) == 0);
    CHECK(HE::backgroundFrameIntervalFromEnv("10", nullptr, nullptr, nullptr) == 100'000'000);
    CHECK(HE::backgroundFrameIntervalFromEnv("2.5", nullptr, nullptr, nullptr) == 400'000'000);
    // Unreadable or negative is not a request: the default stays.
    CHECK(HE::backgroundFrameIntervalFromEnv("fast", nullptr, nullptr, nullptr) == kDefaultNs);
    CHECK(HE::backgroundFrameIntervalFromEnv("-3", nullptr, nullptr, nullptr) == kDefaultNs);
}

TEST_CASE("Background throttle: a profiler capture records uncapped")
{
    const uint64_t interval = HE::backgroundFrameIntervalFromEnv(nullptr, nullptr, nullptr, nullptr);
    CHECK(HE::backgroundThrottleDelayNs(interval, true, true, 0) == 0);
}
