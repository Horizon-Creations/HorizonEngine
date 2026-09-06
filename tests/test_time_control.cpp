// Time control: the composed clock (authored scale / pause reasons / hit-stop)
// and the fixed-step accumulator both application loops drive physics with.
//
// The whole point of this file is that these three channels do NOT overwrite each
// other. With one float, a hit effect's slow motion ending at scale 1 silently
// lifted a pause menu somebody else had set, and a hit-stop was not expressible
// at all. Every case below is one of those collisions, held apart.
//
// The registry rows (time.pause, time.hitStop, …) and the script frontends are a
// later step; this is the C++ core, tested through the C++ API.
#include "doctest.h"
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/FixedStep.h>
#include <vector>

using HE::api::time::PauseReason;
// Not `tm` and not `clock`: both names are already taken at global scope by the C
// library (struct tm, ::clock), and an alias would redefine them.
namespace hetime = HE::api::time;

// ═══ The three channels ═══════════════════════════════════════════════════════

TEST_CASE("Time: a hit-stop returns to the slow motion it interrupted, not to 1")
{
    hetime::reset();
    hetime::setTimeScale(0.25f);

    // 100 ms of freeze. It burns REAL seconds, so it ends even though it is the
    // thing holding game time at zero.
    hetime::hitStop(0.1f);
    CHECK(hetime::isFrozen());
    CHECK(hetime::effectiveScale() == doctest::Approx(0.0f));

    hetime::advance(0.06f);
    CHECK(hetime::deltaTime()   == doctest::Approx(0.0f));   // frozen
    CHECK(hetime::isFrozen());
    CHECK(hetime::timeScale()   == doctest::Approx(0.25f));  // untouched underneath

    hetime::advance(0.06f);                                  // 0.12 s > 0.1 s → over
    CHECK_FALSE(hetime::isFrozen());

    // Back to EXACTLY the slow motion, because the freeze never wrote the scale.
    hetime::advance(0.4f);
    CHECK(hetime::effectiveScale() == doctest::Approx(0.25f));
    CHECK(hetime::deltaTime()      == doctest::Approx(0.1f));

    hetime::reset();
}

TEST_CASE("Time: a hit-stop is not a pause — input must survive it")
{
    // PlayerHost::tick DROPS input events while isPaused() (PlayerHost.cpp), which
    // is right for a pause menu and wrong for a 100 ms freeze: the player would
    // lose the button press they made during the hit. So the two states have to be
    // distinguishable even though both stop the clock.
    hetime::reset();
    hetime::hitStop(0.1f);
    CHECK(hetime::isFrozen());
    CHECK_FALSE(hetime::isPaused());
    CHECK(hetime::effectiveScale() == doctest::Approx(0.0f));   // the clock does stand still

    // And the other way round: a pause is not a freeze.
    hetime::reset();
    hetime::pause(PauseReason::Menu);
    CHECK(hetime::isPaused());
    CHECK_FALSE(hetime::isFrozen());

    hetime::reset();
}

TEST_CASE("Time: re-triggering a hit-stop takes the longer window, never the sum")
{
    hetime::reset();
    hetime::hitStop(0.1f);
    hetime::advance(0.05f);        // 0.05 left
    hetime::hitStop(0.08f);        // longer than what remains → 0.08
    hetime::advance(0.05f);        // 0.03 left — a sum would still be frozen here
    CHECK(hetime::isFrozen());
    hetime::advance(0.04f);
    CHECK_FALSE(hetime::isFrozen());

    // A computed duration of zero is a no-op, not a cancel: a graph that works out
    // "no freeze this time" must not lift somebody else's.
    hetime::hitStop(0.1f);
    hetime::hitStop(0.0f);
    hetime::hitStop(-5.0f);
    CHECK(hetime::isFrozen());

    hetime::reset();
}

TEST_CASE("Time: pause reasons are a set — the last resume does not win")
{
    hetime::reset();
    hetime::pause(PauseReason::Menu);
    hetime::pause(PauseReason::FocusLost);
    CHECK(hetime::isPaused());

    // The window comes back. The menu is still open, so the game stays paused —
    // this is the case a single bool or a bare setTimeScale(1) got wrong.
    hetime::resume(PauseReason::FocusLost);
    CHECK(hetime::isPaused());
    hetime::resume(PauseReason::Menu);
    CHECK_FALSE(hetime::isPaused());

    // A set, not a counter: twice in, once out, and it is out.
    hetime::pause(PauseReason::Menu);
    hetime::pause(PauseReason::Menu);
    hetime::resume(PauseReason::Menu);
    CHECK_FALSE(hetime::isPaused());

    // Reasons do not reach across channels either: a script's pause leaves the
    // window's alone.
    hetime::pause(PauseReason::FocusLost);
    hetime::pause(PauseReason::Script);
    hetime::resume(PauseReason::Script);
    CHECK(hetime::isPaused());

    hetime::reset();
}

TEST_CASE("Time: a pause overrides the authored scale without erasing it")
{
    hetime::reset();
    hetime::setTimeScale(2.0f);
    hetime::pause(PauseReason::Menu);

    hetime::advance(0.5f);
    CHECK(hetime::deltaTime()         == doctest::Approx(0.0f));
    CHECK(hetime::unscaledDeltaTime() == doctest::Approx(0.5f));
    CHECK(hetime::effectiveScale()    == doctest::Approx(0.0f));
    CHECK(hetime::timeScale()         == doctest::Approx(2.0f));  // still readable

    hetime::resume(PauseReason::Menu);
    hetime::advance(0.5f);
    CHECK(hetime::deltaTime() == doctest::Approx(1.0f));          // straight back to 2×

    hetime::reset();
}

TEST_CASE("Time: setTimeScale(0) alone is a standstill, not a pause")
{
    // A deliberate change of meaning, and the one place it shows: isPaused() used
    // to be `timeScale() <= 0`. It answers a question about the game's STATE now,
    // so that a hit-stop does not read as paused (see the input case above). Slow
    // motion dialled all the way down still is not a pause.
    hetime::reset();
    hetime::setTimeScale(0.0f);
    CHECK(hetime::timeScale()      == doctest::Approx(0.0f));
    CHECK(hetime::effectiveScale() == doctest::Approx(0.0f));
    CHECK_FALSE(hetime::isPaused());
    CHECK_FALSE(hetime::isFrozen());

    hetime::advance(0.5f);
    CHECK(hetime::deltaTime() == doctest::Approx(0.0f));

    hetime::reset();
}

TEST_CASE("Time: unscaledElapsed runs through a pause, elapsed does not")
{
    hetime::reset();
    hetime::advance(0.5f);
    hetime::pause(PauseReason::Menu);
    hetime::advance(0.5f);
    hetime::resume(PauseReason::Menu);
    hetime::hitStop(0.2f);
    hetime::advance(0.1f);            // still inside the freeze window

    CHECK(hetime::elapsed()         == doctest::Approx(0.5f));   // only the first frame counted
    CHECK(hetime::unscaledElapsed() == doctest::Approx(1.1f));   // a session clock, unbothered
    CHECK(hetime::frameCount()      == 3);

    hetime::reset();
    CHECK(hetime::unscaledElapsed() == doctest::Approx(0.0f));
    CHECK(hetime::elapsed()         == doctest::Approx(0.0f));
}

TEST_CASE("Time: reset clears all three channels, resetControls keeps the session clock")
{
    hetime::reset();
    hetime::setTimeScale(0.25f);
    hetime::pause(PauseReason::Menu);
    hetime::pause(PauseReason::FocusLost);
    hetime::hitStop(0.2f);
    hetime::advance(0.5f);

    // Play-start: never paused, never frozen, never in slow motion, clock at zero.
    hetime::reset();
    CHECK(hetime::timeScale() == doctest::Approx(1.0f));
    CHECK_FALSE(hetime::isPaused());
    CHECK_FALSE(hetime::isFrozen());
    CHECK(hetime::elapsed()         == doctest::Approx(0.0f));
    CHECK(hetime::unscaledElapsed() == doctest::Approx(0.0f));
    CHECK(hetime::frameCount()      == 0);

    // A scene swap in the packaged game: the controls go, the session clock stays.
    hetime::advance(0.5f);
    hetime::setTimeScale(0.25f);
    hetime::pause(PauseReason::Menu);
    hetime::pause(PauseReason::FocusLost);
    hetime::hitStop(0.2f);
    hetime::resetControls();
    CHECK(hetime::timeScale() == doctest::Approx(1.0f));
    CHECK_FALSE(hetime::isFrozen());
    CHECK(hetime::unscaledElapsed() == doctest::Approx(0.5f));   // did NOT restart
    CHECK(hetime::frameCount()      == 1);

    // …except FocusLost, which belongs to the window and not to the scene: loading
    // a level while alt-tabbed must not resume the game, and nothing would ever put
    // that reason back — the focus event that set it is long gone.
    CHECK(hetime::isPaused());
    hetime::resume(PauseReason::FocusLost);
    CHECK_FALSE(hetime::isPaused());

    hetime::reset();
}

// ═══ The fixed-step accumulator ═══════════════════════════════════════════════

namespace
{
    constexpr float kRate = 0.01f;   // a round step rate, so the arithmetic below is readable

    // Runs one frame through the helper and reports how many steps it spent.
    int stepsFor(float& accum, float frameDt, float scale)
    {
        return HE::advanceFixedSteps(accum, frameDt, kRate, scale, [](float){}).steps;
    }
}

TEST_CASE("FixedStep: a stalled frame is bounded and the debt is dropped, not carried")
{
    // The editor's preview loop had no cap at all, so a 5-second stall in
    // play-in-editor left it in catch-up steps while the shipped game shrugged the
    // same stall off. Both spend this helper now.
    float accum = 0.0f;
    const HE::FixedStepResult r =
        HE::advanceFixedSteps(accum, 5.0f, kRate, 1.0f, [](float){});
    CHECK(r.steps == 5);          // the cap at scale 1
    CHECK(r.dropped);
    CHECK(accum == doctest::Approx(0.0f));   // no debt into the next frame

    // …and the next frame is an ordinary one again, which is the actual point:
    // carrying the remainder is how one stall becomes permanent slow motion.
    CHECK(stepsFor(accum, kRate, 1.0f) == 1);
}

TEST_CASE("FixedStep: the cap rides the time scale so fast-forward stays fast")
{
    // A quarter second of game time at a 1/64 s rate — sixteen whole steps, and the
    // numbers are powers of two so the count is exact rather than a float's opinion.
    // At scale 1 that is a stall and the cap cuts it off; at scale 5 it is what one
    // ordinary frame legitimately owes, and a cap that did not move would saturate
    // every single frame and dump the rest — fast-forward would quietly decay into
    // slow motion.
    constexpr float kExactRate = 1.0f / 64.0f;

    float capped = 0.0f;
    const HE::FixedStepResult slow =
        HE::advanceFixedSteps(capped, 0.25f, kExactRate, 1.0f, [](float){});
    CHECK(slow.steps == 5);       // the cap at scale 1
    CHECK(slow.dropped);

    float full = 0.0f;
    const HE::FixedStepResult fast =
        HE::advanceFixedSteps(full, 0.25f, kExactRate, 5.0f, [](float){});
    CHECK(fast.steps == 16);      // all of them: the cap is 5 × ceil(5) = 25 here
    CHECK_FALSE(fast.dropped);    // kept up, nothing was thrown away
    CHECK(full == doctest::Approx(0.0f));
}

TEST_CASE("FixedStep: whole steps only, the remainder waits for the next frame")
{
    // 6 ms per frame at a 10 ms rate: not every frame carries a step, and none of
    // the leftovers may be lost, or the simulation runs slow forever.
    float accum = 0.0f;
    CHECK(stepsFor(accum, 0.006f, 1.0f) == 0);
    CHECK(accum == doctest::Approx(0.006f));
    CHECK(stepsFor(accum, 0.006f, 1.0f) == 1);
    CHECK(accum == doctest::Approx(0.002f));
    CHECK(stepsFor(accum, 0.006f, 1.0f) == 0);
    CHECK(stepsFor(accum, 0.006f, 1.0f) == 1);
    CHECK(accum == doctest::Approx(0.004f));
}

TEST_CASE("FixedStep: a paused frame steps nothing and keeps what it had")
{
    // gameDt is 0 while the game is paused or frozen, so the accumulator neither
    // advances nor drains — resuming picks up exactly where it left off.
    float accum = 0.0f;
    CHECK(stepsFor(accum, 0.006f, 1.0f) == 0);
    const float before = accum;
    CHECK(stepsFor(accum, 0.0f, 0.0f) == 0);
    CHECK(accum == doctest::Approx(before));

    // A degenerate rate must return rather than loop forever.
    float other = 1.0f;
    CHECK(HE::advanceFixedSteps(other, 1.0f, 0.0f, 1.0f, [](float){}).steps == 0);
}

TEST_CASE("FixedStep: every step gets the fixed rate, never the frame's own dt")
{
    // A game that simulates at a different rate than it was authored against is not
    // the same game — so the callable sees the fixed rate each time, whatever the
    // frame happened to be worth.
    std::vector<float> seen;
    float accum = 0.0f;
    HE::advanceFixedSteps(accum, 0.035f, kRate, 1.0f, [&](float s){ seen.push_back(s); });
    REQUIRE(seen.size() == 3);
    for (float s : seen) CHECK(s == doctest::Approx(kRate));
    CHECK(accum == doctest::Approx(0.005f));
}
