// Thema 178, step 2: how a physics step spends threads, collision steps and scratch
// memory, and what it now says about itself. The numbers behind the expectations
// are the baseline's (docs/physics-threading-baseline.md).
#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/PhysicsTuning.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <glm/glm.hpp>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr float kDt = 1.0f / 60.0f;

// HE_PHYSICS_THREADS for the length of a scope, put back the way it was. setenv()
// does not exist on Windows and _putenv_s() does not on the rest, so the two are
// spelled out once, here.
class ScopedThreadsEnv
{
public:
    explicit ScopedThreadsEnv(const char* value)
    {
        const char* now = std::getenv(kName);
        m_had = now != nullptr;
        if (m_had) m_previous = now;
        set(value);
    }
    ~ScopedThreadsEnv() { set(m_had ? m_previous.c_str() : nullptr); }

private:
    static constexpr const char* kName = "HE_PHYSICS_THREADS";

    static void set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(kName, value ? value : "");   // an empty value removes it
#else
        if (value) setenv(kName, value, 1);
        else       unsetenv(kName);
#endif
    }

    bool        m_had = false;
    std::string m_previous;
};

Entity addBox(HorizonWorld& world, const glm::vec3& at, bool dynamic, const glm::vec3& scale = glm::vec3(1.0f))
{
    Entity e = world.createEntity(dynamic ? "Box" : "Floor");
    TransformComponent t;
    t.position = at;
    t.scale    = scale;
    world.addComponent(e, t);
    RigidBodyComponent rb;
    rb.type = dynamic ? RigidBodyType::Dynamic : RigidBodyType::Static;
    rb.mass = 1.0f;
    world.addComponent(e, rb);
    return e;
}

// A floor and `side` x `side` boxes in `layers` layers, dropped from slightly
// uneven heights so they land, bounce, touch each other and settle. Returns the
// boxes in creation order (which is also the order their bodies get ids in).
std::vector<Entity> buildPile(HorizonWorld& world, int side, int layers)
{
    addBox(world, { 0.0f, -0.5f, 0.0f }, false, { 60.0f, 1.0f, 60.0f });
    std::vector<Entity> boxes;
    for (int layer = 0; layer < layers; ++layer)
        for (int ix = 0; ix < side; ++ix)
            for (int iz = 0; iz < side; ++iz)
            {
                const float wobble = 0.07f * static_cast<float>((ix * 7 + iz * 3 + layer) % 5);
                boxes.push_back(addBox(world,
                    { (static_cast<float>(ix) - 0.5f * side) * 1.05f,
                      1.0f + static_cast<float>(layer) * 1.6f + wobble,
                      (static_cast<float>(iz) - 0.5f * side) * 1.05f }, true));
            }
    return boxes;
}

struct Outcome
{
    std::vector<float>                       pose;      // x y z rx ry rz of every box
    std::vector<PhysicsWorld::CollisionEvent> entered;
    std::vector<PhysicsWorld::CollisionEvent> exited;
};

// Runs the same pile with `threads` threads (0 = automatic). If `switchAt` is
// non-negative, the count is changed to `switchTo` before that step, so the
// workers are restarted in the middle of a running simulation.
Outcome runPile(int threads, int steps, int switchAt = -1, int switchTo = 0)
{
    HorizonWorld world;
    const std::vector<Entity> boxes = buildPile(world, 8, 3);

    PhysicsWorld phys;
    phys.setThreadCount(threads);
    phys.initialize(world);

    Outcome out;
    for (int i = 0; i < steps; ++i)
    {
        if (i == switchAt)
            phys.setThreadCount(switchTo);
        phys.step(world, kDt);
        for (const auto& e : phys.pollCollisionEnter()) out.entered.push_back(e);
        for (const auto& e : phys.pollCollisionExit())  out.exited.push_back(e);
    }
    for (const Entity e : boxes)
    {
        const auto& t = world.registry().get<TransformComponent>(e);
        out.pose.insert(out.pose.end(), { t.position.x, t.position.y, t.position.z,
                                          t.rotation.x, t.rotation.y, t.rotation.z });
    }
    return out;
}

bool sameBits(const std::vector<float>& a, const std::vector<float>& b)
{
    return a.size() == b.size()
        && (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(float)) == 0);
}

bool sameEvents(const std::vector<PhysicsWorld::CollisionEvent>& a,
                const std::vector<PhysicsWorld::CollisionEvent>& b)
{
    if (a.size() != b.size())
        return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].entityA != b[i].entityA || a[i].entityB != b[i].entityB)
            return false;
    return true;
}

} // namespace

// ─── The pure rules ──────────────────────────────────────────────────────────

TEST_CASE("PhysicsTuning::autoWorkers: half the cores minus the caller, at most four")
{
    using HE::PhysicsTuning::autoWorkers;
    CHECK(autoWorkers(0)  == 0);   // hardware_concurrency() may not know
    CHECK(autoWorkers(1)  == 0);
    CHECK(autoWorkers(2)  == 0);
    CHECK(autoWorkers(3)  == 0);
    CHECK(autoWorkers(4)  == 1);
    CHECK(autoWorkers(6)  == 2);
    CHECK(autoWorkers(8)  == 3);
    CHECK(autoWorkers(10) == 4);   // the measured knee, on the machine the baseline ran on
    CHECK(autoWorkers(12) == 4);
    CHECK(autoWorkers(64) == 4);
}

TEST_CASE("PhysicsTuning::resolveThreads: the project asks, the environment overrides, 0 is automatic")
{
    using HE::PhysicsTuning::resolveThreads;
    using HE::PhysicsTuning::kMaxThreads;

    // Automatic is the workers plus the caller.
    CHECK(resolveThreads(0, -1, 10) == 5);
    CHECK(resolveThreads(0, -1, 2)  == 1);
    // A number is a number, in [1, kMaxThreads].
    CHECK(resolveThreads(1, -1, 10) == 1);
    CHECK(resolveThreads(7, -1, 10) == 7);
    CHECK(resolveThreads(7, -1, 2)  == 7);   // asked for, so given, even on a small machine
    CHECK(resolveThreads(1000, -1, 10) == kMaxThreads);
    CHECK(resolveThreads(-3, -1, 10) == 5);  // nonsense is "automatic", not zero threads
    // The environment wins over the project, including when it says automatic.
    CHECK(resolveThreads(7, 2, 10) == 2);
    CHECK(resolveThreads(7, 0, 10) == 5);
    CHECK(resolveThreads(0, 3, 10) == 3);
}

TEST_CASE("PhysicsTuning::threadsFromEnv: a whole number of at least zero, else not set")
{
    using HE::PhysicsTuning::threadsFromEnv;
    {
        ScopedThreadsEnv env(nullptr);
        CHECK(threadsFromEnv() == -1);
    }
    {
        ScopedThreadsEnv env("4");
        CHECK(threadsFromEnv() == 4);
    }
    {
        ScopedThreadsEnv env("0");
        CHECK(threadsFromEnv() == 0);
    }
    {
        ScopedThreadsEnv env("1000");
        CHECK(threadsFromEnv() == HE::PhysicsTuning::kMaxThreads);
    }
    for (const char* bad : { "many", "-3", "4x", " " })
    {
        ScopedThreadsEnv env(bad);
        CHECK(threadsFromEnv() == -1);
    }
}

TEST_CASE("PhysicsTuning::collisionStepsFor: one step up to about 45 Hz, more below, never zero")
{
    using HE::PhysicsTuning::collisionStepsFor;
    // Everything a project is likely to run at, including the default, is untouched.
    CHECK(collisionStepsFor(1.0f / 480.0f) == 1);
    CHECK(collisionStepsFor(1.0f / 120.0f) == 1);
    CHECK(collisionStepsFor(1.0f / 60.0f)  == 1);
    CHECK(collisionStepsFor(0.02f)         == 1);   // 50 Hz
    CHECK(collisionStepsFor(1.0f / 45.0f)  == 1);
    // Jolt's limit is 1/60 s per collision step.
    CHECK(collisionStepsFor(1.0f / 30.0f)  == 2);
    CHECK(collisionStepsFor(1.0f / 20.0f)  == 3);
    CHECK(collisionStepsFor(1.0f / 10.0f)  == 6);   // the project's slowest rate
    CHECK(collisionStepsFor(5.0f) == HE::PhysicsTuning::kMaxCollisionSteps);
    // Nonsense degrades to one step; Update() is not asked for zero.
    CHECK(collisionStepsFor(0.0f)  == 1);
    CHECK(collisionStepsFor(-1.0f) == 1);
    CHECK(collisionStepsFor(std::nanf("")) == 1);
}

TEST_CASE("PhysicsTuning::tempBytesFor: the scratch block clears every measured peak with room to spare")
{
    using HE::PhysicsTuning::tempBytesFor;
    constexpr std::size_t kMiB = std::size_t(1) << 20;
    constexpr double      kMB  = 1.0e6;   // the baseline's table is in decimal MB

    // Measured peaks (docs/physics-threading-baseline.md §6), body table 65 536:
    //   10 240 constraints  9.9 MB   (the old fixed block was 10 MiB and std::abort()ed past it)
    //   40 960 constraints 34.06 MB
    //   81 920 constraints 67.96 MB
    // At least 40 % on top of each.
    CHECK(static_cast<double>(tempBytesFor(65536, 10240)) >= 9.9   * kMB * 1.4);
    CHECK(static_cast<double>(tempBytesFor(65536, 40960)) >= 34.06 * kMB * 1.4);
    CHECK(static_cast<double>(tempBytesFor(65536, 81920)) >= 67.96 * kMB * 1.4);

    // Whole MiB, a floor of one, and it only ever grows with the caps.
    CHECK(tempBytesFor(0, 0) == kMiB);
    CHECK(tempBytesFor(65536, 40960) % kMiB == 0);
    CHECK(tempBytesFor(65536, 20480) < tempBytesFor(65536, 40960));
    CHECK(tempBytesFor(1000, 10240)  < tempBytesFor(65536, 10240));
}

// ─── The world ───────────────────────────────────────────────────────────────

TEST_CASE("PhysicsWorld::setThreadCount: the count in force follows the request")
{
    using namespace HE::PhysicsTuning;
    const unsigned hw  = std::thread::hardware_concurrency();
    const int      env = threadsFromEnv();   // a pinned run sets all of these to the same number; the rule still holds

    PhysicsWorld phys;
    CHECK(phys.threadCount() == resolveThreads(0, env, hw));   // starts on automatic

    for (const int asked : { 1, 3, 0, 2, 100 })
    {
        phys.setThreadCount(asked);
        CHECK(phys.threadCount() == resolveThreads(asked, env, hw));
        CHECK(phys.threadCount() >= 1);
        CHECK(phys.threadCount() <= kMaxThreads);
    }

    // And a world that has to step with whatever it ended up on.
    HorizonWorld world;
    buildPile(world, 4, 1);
    phys.initialize(world);
    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);
    CHECK(phys.stepStats().steps == 30);
}

TEST_CASE("PhysicsWorld: the result does not depend on the number of threads")
{
    // Jolt promises it, the engine's contact listener has to keep it (events are
    // collected under a lock from several workers and handed out sorted), and
    // nothing here may break it: the pose of 192 boxes after 150 steps of landing,
    // bouncing and stacking is the same BITS with one thread, with four, and when
    // the workers are restarted halfway through.
    const Outcome single   = runPile(1, 150);
    const Outcome four     = runPile(4, 150);
    const Outcome eight    = runPile(8, 150);
    const Outcome restarted = runPile(1, 150, /*switchAt=*/60, /*switchTo=*/4);

    REQUIRE(single.pose.size() == 192u * 6u);
    CHECK(sameBits(single.pose, four.pose));
    CHECK(sameBits(single.pose, eight.pose));
    CHECK(sameBits(single.pose, restarted.pose));

    // The pile did touch: this would be vacuous on a scene without contacts.
    CHECK(single.entered.size() > 192u);
    CHECK(sameEvents(single.entered, four.entered));
    CHECK(sameEvents(single.exited,  four.exited));
    CHECK(sameEvents(single.entered, restarted.entered));
    CHECK(sameEvents(single.exited,  restarted.exited));
}

TEST_CASE("PhysicsWorld: a full contact cache is counted instead of thrown away, and the world carries on")
{
    // Sixteen contact constraints and sixty-four body pairs for a 6 x 6 field of
    // boxes that touch: far more than fits. Jolt drops contacts and says so in
    // Update()'s return value, which the engine used to discard.
    HE::PhysicsLimits tiny;
    tiny.maxBodyPairs          = 64;
    tiny.maxContactConstraints = 16;

    HorizonWorld cramped;
    buildPile(cramped, 6, 1);
    PhysicsWorld small(tiny);
    CHECK(small.limits().maxContactConstraints == 16u);
    small.initialize(cramped);
    for (int i = 0; i < 90; ++i)
        small.step(cramped, kDt);

    const PhysicsWorld::StepStats stats = small.stepStats();
    CHECK(stats.steps == 90u);
    CHECK(stats.saturatedSteps > 0u);
    CHECK(stats.saturatedSteps <= stats.steps);
    // A small cap makes a small block, but never less than a MiB; and the scene did not abort the process.
    CHECK(small.tempBytes() >= (std::size_t(1) << 20));

    // The same scene under the engine's own limits is clean.
    HorizonWorld roomy;
    buildPile(roomy, 6, 1);
    PhysicsWorld normal;
    normal.initialize(roomy);
    for (int i = 0; i < 90; ++i)
        normal.step(roomy, kDt);
    CHECK(normal.stepStats().steps == 90u);
    CHECK(normal.stepStats().saturatedSteps == 0u);
    CHECK(normal.stepStats().lastErrors == 0u);

    // …and the default block is the one the rule gives for the default limits.
    CHECK(normal.limits().maxContactConstraints == HE::PhysicsLimits{}.maxContactConstraints);
    CHECK(normal.tempBytes() == HE::PhysicsTuning::tempBytesFor(PhysicsWorld::kMaxBodies,
                                                                 normal.limits().maxContactConstraints));
}

TEST_CASE("PhysicsWorld: a step longer than 1/60 s is split into collision steps")
{
    HorizonWorld world;
    addBox(world, { 0.0f, 20.0f, 0.0f }, true);
    PhysicsWorld phys;
    phys.initialize(world);

    CHECK(phys.stepStats().steps == 0u);
    phys.step(world, 1.0f / 60.0f);
    CHECK(phys.stepStats().collisionSteps == 1);
    phys.step(world, 1.0f / 30.0f);
    CHECK(phys.stepStats().collisionSteps == 2);
    phys.step(world, 0.1f);
    CHECK(phys.stepStats().collisionSteps == 6);
    phys.step(world, 1.0f / 60.0f);
    CHECK(phys.stepStats().collisionSteps == 1);
    CHECK(phys.stepStats().steps == 4u);

    // A step that was not run (no time) is not counted.
    phys.step(world, 0.0f);
    CHECK(phys.stepStats().steps == 4u);
}

// ─── The region hold's probe ─────────────────────────────────────────────────

TEST_CASE("PhysicsWorld::setRegionHold: a probe that says no keeps the test from being asked")
{
    HorizonWorld world;
    addBox(world, { 0.0f, -0.5f, 0.0f }, false, { 60.0f, 1.0f, 60.0f });
    for (int i = 0; i < 20; ++i)
        addBox(world, { static_cast<float>(i) * 1.5f, 3.0f, 0.0f }, true);

    PhysicsWorld phys;
    int  asked   = 0;
    bool mayHold = false;
    phys.setRegionHold([&](const glm::vec3&) { ++asked; return false; },
                       [&] { return mayHold; });
    phys.initialize(world);

    for (int i = 0; i < 30; ++i)
        phys.step(world, kDt);
    CHECK(asked == 0);   // twenty dynamic bodies, thirty steps, and not one question

    mayHold = true;
    phys.step(world, kDt);
    CHECK(asked == 20);  // the moment the probe says yes, every dynamic body is asked, as before
}

TEST_CASE("PhysicsWorld::setRegionHold: without a probe the test is asked every step, as it always was")
{
    HorizonWorld world;
    for (int i = 0; i < 5; ++i)
        addBox(world, { static_cast<float>(i) * 2.0f, 30.0f, 0.0f }, true);

    PhysicsWorld phys;
    int asked = 0;
    phys.setRegionHold([&](const glm::vec3&) { ++asked; return false; });
    phys.initialize(world);
    for (int i = 0; i < 10; ++i)
        phys.step(world, kDt);
    CHECK(asked == 50);
}

TEST_CASE("PhysicsWorld::setRegionHold: a held body is let go even when the probe has gone quiet")
{
    HorizonWorld world;
    const Entity box = addBox(world, { 0.0f, 50.0f, 0.0f }, true);

    PhysicsWorld phys;
    bool hold = true;
    // Probe and test agree: both follow `hold`.
    phys.setRegionHold([&](const glm::vec3&) { return hold; }, [&] { return hold; });
    phys.initialize(world);

    for (int i = 0; i < 20; ++i)
        phys.step(world, kDt);
    CHECK(phys.heldCount() == 1u);
    CHECK(world.registry().get<TransformComponent>(box).position.y == doctest::Approx(50.0f));

    // Now the probe says no AND the test says no. The body is still held, so the
    // pass has to run to let it go; a probe may never keep a body out of the world.
    hold = false;
    for (int i = 0; i < 60; ++i)
        phys.step(world, kDt);
    CHECK(phys.heldCount() == 0u);
    CHECK(world.registry().get<TransformComponent>(box).position.y < 49.0f);
}
