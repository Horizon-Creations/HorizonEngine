#pragma once
#include <algorithm>
#include <cmath>

// The fixed-timestep accumulator both application loops drive physics with.
//
// It used to exist twice, once per application, and the two had already drifted:
// the game loop bounded the catch-up steps and the editor's preview did not, so a
// long stall in play-in-editor spiralled into ever more catch-up steps while the
// shipped game shrugged the same stall off. A preview that behaves differently
// from the thing it previews is worse than no preview, so the rule lives here
// once and both callers spend it.
//
// The step RATE is not in here on purpose — that is PhysicsWorld::kFixedDt, and
// it belongs to the simulation rather than to the pacing.
namespace HE {

struct FixedStepResult
{
    int  steps   = 0;      // whole steps run this frame
    bool dropped = false;  // the cap hit and the outstanding remainder was thrown away
};

// Adds one frame's worth of GAME time to `accumulator` and runs `step(fixedDt)`
// once per whole step it contains.
//
// The cap RIDES the time scale: at scale 5 a single frame legitimately owes ~5×
// the steps, and a fixed cap of 5 would saturate every frame and dump the
// remainder — fast-forward would silently decay into slow motion. The rate itself
// never moves; only how many steps one frame may spend.
//
// What is left over above one whole step after the cap is DISCARDED rather than
// carried: keeping it is how a stall becomes permanent slow motion, because every
// following frame then starts already in debt.
template <class StepFn>
FixedStepResult advanceFixedSteps(float& accumulator, float frameDt, float fixedDt,
                                  float timeScale, StepFn&& step)
{
    FixedStepResult r{};
    if (!(fixedDt > 0.0f)) return r;          // a zero or NaN rate would loop forever
    if (frameDt > 0.0f) accumulator += frameDt;

    const int maxSteps = 5 * std::max(1, (int)std::ceil(timeScale));
    while (accumulator >= fixedDt && r.steps < maxSteps)
    {
        step(fixedDt);
        accumulator -= fixedDt;
        ++r.steps;
    }
    if (accumulator > fixedDt) { accumulator = 0.0f; r.dropped = true; }
    return r;
}

} // namespace HE
