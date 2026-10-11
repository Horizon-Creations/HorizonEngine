#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

// How a physics step spends threads, collision steps and scratch memory (Thema 178).
//
// Plain functions of a few numbers (cores, a step length, two caps), kept out of
// PhysicsWorld.cpp so a test can pin every answer without building a Jolt world,
// and so the rule is written down once. Baseline and measurements:
// docs/physics-threading-baseline.md.
namespace HE {

// What Jolt sizes its collision caches by when the world is built. Fixed for the
// life of the world: they cannot be changed afterwards, and a scene that fills
// them does not crash, it silently loses contacts (see PhysicsWorld::StepStats).
//
// The defaults are four times what the engine ran with (65 536 body pairs, 10 240
// contact constraints, copied from Jolt's HelloWorld). A pile of ten thousand
// boxes filled the old caps in every step and Jolt dropped contacts for it; with
// these it runs clean and, because Jolt no longer retries what it cut, in about
// half the time. The price is address space, not memory: the caches and the
// scratch block are only touched as far as a scene uses them.
struct PhysicsLimits
{
    uint32_t maxBodyPairs          = 262144;
    uint32_t maxContactConstraints = 40960;
};

namespace PhysicsTuning {

// ── Threads ──────────────────────────────────────────────────────────────────
// "Threads" below always means the threads that take part in one step, the one
// that calls step() included: 1 is single-threaded, 5 is the caller and four
// Jolt workers. The simulation is deterministic whatever the count (Jolt,
// Docs/Architecture.md "Deterministic Simulation"), so this is a pure speed
// setting.

inline constexpr int kMaxThreads = 32;

// Workers when nobody asked for a number: half the cores minus the caller, at
// most four. Four sits at the knee of the measured curve (20 000 bouncing bodies:
// 27 ms with none, 8.7 ms with four, 7.9 ms with nine on ten cores), and half the
// cores is what leaves the rest to the things that run beside a step: the engine
// pool (streaming is capped at two cores short of all of it, render extraction
// runs between steps) and the audio device thread.
inline constexpr int kAutoMaxWorkers = 4;

inline int autoWorkers(unsigned hardwareThreads)
{
    const int half = static_cast<int>(hardwareThreads / 2u) - 1;
    return std::clamp(half, 0, kAutoMaxWorkers);
}

// The thread count a world runs with. `requested` is the project's setting (0 =
// automatic), `envOverride` is HE_PHYSICS_THREADS (negative = not set; it wins,
// so a benchmark or a bug report can pin a number without touching the project).
inline int resolveThreads(int requested, int envOverride, unsigned hardwareThreads)
{
    const int want = envOverride >= 0 ? envOverride : requested;
    if (want <= 0)
        return autoWorkers(hardwareThreads) + 1;
    return std::min(want, kMaxThreads);
}

// HE_PHYSICS_THREADS as a number, or -1 when it is unset or not a whole number
// of at least zero.
inline int threadsFromEnv()
{
    const char* text = std::getenv("HE_PHYSICS_THREADS");
    if (!text || !*text)
        return -1;
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0)
        return -1;
    return static_cast<int>(std::min<long>(value, kMaxThreads));
}

// ── Collision steps ──────────────────────────────────────────────────────────
// Jolt asks for at most 1/60 s per collision step: it integrates once per step
// and then resolves contacts, so a longer step lets a fast body cross a thin
// collider between two looks and gives the solver a bigger jump to settle. The
// project's fixed rate is free (10 to 480 Hz); a 30 Hz project fed Update(1/30, 1)
// was running at twice Jolt's own limit.
//
// round() rather than ceil() on purpose: 50 Hz (1.2) and 45 Hz (1.33) stay at
// one step, as does everything above, so the default rate and every ordinary
// one are untouched; 30 Hz gets two, 10 Hz six. The cost per simulated second
// stays about the same, because the project steps correspondingly less often.
inline constexpr float kCollisionStepSeconds = 1.0f / 60.0f;
inline constexpr int   kMaxCollisionSteps    = 8;

inline int collisionStepsFor(float dt)
{
    if (!(dt > 0.0f))
        return 1;
    const float ratio = dt / kCollisionStepSeconds;
    if (!(ratio < static_cast<float>(kMaxCollisionSteps)))
        return kMaxCollisionSteps;
    return std::max(1, static_cast<int>(std::lround(ratio)));
}

// ── Scratch memory ───────────────────────────────────────────────────────────
// What Update() and the characters' ExtendedUpdate() take from the temp
// allocator while a step runs. Measured with a counting wrapper
// (scripts/perf/physics_load_bench.cpp): the peak is close to independent of the
// scene, because Jolt sizes its per-step buffers by the CAP rather than by what
// is in contact, at about 830 bytes per contact constraint plus 20 bytes per
// body (8.6 MB at 10 240 constraints and 1 000 bodies, 9.6 MB at 50 000 bodies,
// 34 MB at 40 960 constraints).
//
// The block is sized from the caps with half again on top, and the allocator
// the world uses falls back to the heap when it is exceeded, so a misjudged
// number costs a malloc instead of ending the process (Jolt's plain
// TempAllocatorImpl calls std::abort(); the block was 10 MB against a peak of
// 8.6 to 9.9 MB).
inline constexpr std::size_t kTempBytesPerConstraint = 900;
inline constexpr std::size_t kTempBytesPerBody       = 32;
inline constexpr std::size_t kTempMinBytes           = std::size_t(1) << 20;

inline std::size_t tempBytesFor(uint32_t maxBodies, uint32_t maxContactConstraints)
{
    const std::size_t peak = kTempBytesPerConstraint * maxContactConstraints
                           + kTempBytesPerBody * maxBodies;
    const std::size_t sized = peak + peak / 2;
    const std::size_t block = std::max(sized, kTempMinBytes);
    // Whole MiB, so the figure is easy to read in a log.
    const std::size_t mib = std::size_t(1) << 20;
    return (block + mib - 1) / mib * mib;
}

} // namespace PhysicsTuning
} // namespace HE
