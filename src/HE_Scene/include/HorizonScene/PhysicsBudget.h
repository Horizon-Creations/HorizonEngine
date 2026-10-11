#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>

// The time budget for the work AROUND Jolt's Update (Thema 178, step 3): the
// characters' ExtendedUpdate, the bodies a streamed cell adds and removes, the
// contact events handed to gameplay, and the scene queries a script fires.
//
// Update() itself is not in it: it is what the fixed step is FOR, and the way to
// bound it is the 5-steps-a-frame cap of advanceFixedSteps plus the thread count.
// What this bounds is everything that scales with the number of ENTITIES rather
// than with the number of contacts, and that used to run all at once, in whatever
// step the scene happened to ask for it.
//
// Plain numbers and rules, kept out of PhysicsWorld.cpp so a test can pin each of
// them without building a Jolt world. Measurements: docs/physics-threading-budget.md.
namespace HE {

struct PhysicsBudgetConfig
{
    // Milliseconds of budgeted work one step may spend (queries and body work since
    // the previous step count against it first, then the characters take what is
    // left). 0 = no budget: every character is stepped, every event handed out, as
    // before this existed. 4 ms is a quarter of a 60 Hz frame; the measured cost of
    // 500 standing characters is 1.2 ms and of 1 000 walking ones 4.7 ms, so an
    // ordinary scene never reaches it and a crowd of a few thousand is spread over
    // several steps.
    double   frameMs = 4.0;
    // Contact events one poll hands out, per list (enter, exit, overlap enter,
    // overlap exit). The rest stay queued, in order, for the next poll. 0 = all.
    // Counts rather than time: the events are cheap to buffer and expensive to
    // CONSUME (a script runs for each), and a count is the same on every machine.
    uint32_t eventsPerPoll = 4096;
};

// What the latest step spent, for the profiler and for tests. "Used" is the
// budgeted work only (never Update()).
struct PhysicsBudgetStats
{
    double   budgetMs      = 0.0;   // the limit in force (0 = none)
    double   usedMs        = 0.0;   // queries + body work + characters, latest step
    double   charactersMs  = 0.0;   // …of which ExtendedUpdate and its write-back
    double   queriesMs     = 0.0;   // …of which scene queries fired since the previous step
    double   bodiesMs      = 0.0;   // …of which body add/remove since the previous step
    uint32_t characters        = 0; // characters that exist
    uint32_t charactersStepped = 0; // …stepped in the latest step
    uint32_t charactersDeferred = 0; // …put off to a later step
    uint32_t maxOwedSteps  = 0;     // longest wait among the characters right now, in steps
    uint32_t queries       = 0;     // scene queries since the previous step
    uint32_t eventsBacklog = 0;     // contact events held back for a later poll
    uint64_t steps                  = 0;   // steps run with a budget in force
    uint64_t overBudgetSteps        = 0;   // …whose budgeted work reached the limit
    uint64_t deferredCharacterSteps = 0;   // characters put off, summed over steps
    uint64_t eventsDeferred         = 0;   // events held back, summed over polls
};

namespace PhysicsBudgetRules {

// A character that waits is stepped later with the time it missed, so it covers the
// same distance and falls the same way, just later. How much it may owe is capped:
// ExtendedUpdate with a very long dt tunnels and mis-detects ground, and a crowd
// that never catches up should slow down rather than jump. Five steps' worth is
// 83 ms at 60 Hz, inside what a frame hitch already hands a character today.
inline constexpr int kMaxOwedSteps = 4;

// Characters that are stepped every step whatever the budget says. A scene with a
// handful of characters (every game, every test) must never see one put off because
// a cold cache or a busy machine made one ExtendedUpdate slow; the budget is for
// crowds. 32 characters cost about 150 us.
inline constexpr uint32_t kGuaranteedCharacters = 32;

inline constexpr double kMaxFrameMs = 1000.0;

// HE_PHYSICS_BUDGET_MS in the environment wins over the project, like
// HE_PHYSICS_THREADS: negative = not set. 0 is a valid answer (no budget).
inline double frameMsFromEnv()
{
    const char* text = std::getenv("HE_PHYSICS_BUDGET_MS");
    if (!text || !*text)
        return -1.0;
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !std::isfinite(value) || value < 0.0)
        return -1.0;
    return std::min(value, kMaxFrameMs);
}

inline double resolveFrameMs(double requested, double envOverride)
{
    const double want = envOverride >= 0.0 ? envOverride : requested;
    if (!std::isfinite(want) || want <= 0.0)
        return 0.0;
    return std::min(want, kMaxFrameMs);
}

// The time a character that was put off owes after one more step it sat out.
inline float carryOwed(float owed, float dt)
{
    return std::min(owed + dt, static_cast<float>(kMaxOwedSteps) * dt);
}

// The dt a character is stepped with: this step's and what it owes.
inline float steppedDt(float dt, float owed)
{
    return dt + std::max(owed, 0.0f);
}

// How many steps a character has waited, from what it owes.
inline uint32_t waitedSteps(float owed, float dt)
{
    return dt > 0.0f ? static_cast<uint32_t>(std::lround(std::max(owed, 0.0f) / dt)) : 0u;
}

} // namespace PhysicsBudgetRules
} // namespace HE
