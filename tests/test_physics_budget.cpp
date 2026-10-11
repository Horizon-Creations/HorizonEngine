// Thema 178, step 3: the time budget for the work around the physics step - the
// characters' update, scene queries and body work (charged), and the contact events
// handed to gameplay (capped per poll, queued in order). What the budget must do when
// it runs out, and that it does nothing when it is off.
#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/PhysicsBudget.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <Project/ProjectSettings.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr float kDt = 1.0f / 60.0f;

// HE_PHYSICS_BUDGET_MS for the length of a scope, put back the way it was (setenv()
// does not exist on Windows and _putenv_s() does not on the rest).
class ScopedBudgetEnv
{
public:
    explicit ScopedBudgetEnv(const char* value)
    {
        const char* now = std::getenv(kName);
        m_had = now != nullptr;
        if (m_had) m_previous = now;
        set(value);
    }
    ~ScopedBudgetEnv() { set(m_had ? m_previous.c_str() : nullptr); }

private:
    static constexpr const char* kName = "HE_PHYSICS_BUDGET_MS";
    static void set(const char* value)
    {
#if defined(_WIN32)
        _putenv_s(kName, value ? value : "");
#else
        if (value) setenv(kName, value, 1);
        else       unsetenv(kName);
#endif
    }
    bool        m_had = false;
    std::string m_previous;
};

// A budget small enough that it is spent by the first character: every character
// past the guaranteed ones is put off, whatever the machine.
HE::PhysicsBudgetConfig tinyBudget(uint32_t eventsPerPoll = 4096)
{
    HE::PhysicsBudgetConfig config;
    config.frameMs       = 1.0e-6;
    config.eventsPerPoll = eventsPerPoll;
    return config;
}

// `count` characters on a grid 4 m apart in the air without gravity, in creation order.
// pushWalkers sets them walking along +x; with nothing to touch, each step moves one
// by velocity * dt along x, which is what the tests read.
std::vector<Entity> addWalkers(HorizonWorld& world, int count)
{
    std::vector<Entity> walkers;
    for (int i = 0; i < count; ++i)
    {
        Entity e = world.createEntity("Walker");
        TransformComponent t;
        t.position = { static_cast<float>(i % 12) * 4.0f, 50.0f, static_cast<float>(i / 12) * 4.0f };
        world.addComponent(e, t);
        CharacterControllerComponent cc;
        cc.gravity = 0.0f;   // x is all that is read, and it is not touched by a fall
        world.addComponent(e, cc);
        walkers.push_back(e);
    }
    return walkers;
}

void pushWalkers(PhysicsWorld& phys, const std::vector<Entity>& walkers, float speed)
{
    for (Entity e : walkers)
        phys.setCharacterVelocity(static_cast<uint32_t>(e), { speed, 0.0f, 0.0f });
}

float xOf(HorizonWorld& world, Entity e)
{
    return world.registry().get<TransformComponent>(e).position.x;
}

float startX(int index) { return static_cast<float>(index % 12) * 4.0f; }

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

// A floor and 8x8x3 boxes dropped from uneven heights: they land, bounce, touch and
// part again, so there are many enters and exits.
void buildPile(HorizonWorld& world)
{
    addBox(world, { 0.0f, -0.5f, 0.0f }, false, { 60.0f, 1.0f, 60.0f });
    for (int layer = 0; layer < 3; ++layer)
        for (int ix = 0; ix < 8; ++ix)
            for (int iz = 0; iz < 8; ++iz)
            {
                const float wobble = 0.07f * static_cast<float>((ix * 7 + iz * 3 + layer) % 5);
                addBox(world, { (static_cast<float>(ix) - 4.0f) * 1.05f,
                                1.0f + static_cast<float>(layer) * 1.6f + wobble,
                                (static_cast<float>(iz) - 4.0f) * 1.05f }, true);
            }
}

using Pair = std::pair<uint32_t, uint32_t>;

struct Delivery
{
    std::vector<Pair> entered, exited;   // in the order handed out, across all polls
    size_t maxEnterBatch = 0, maxExitBatch = 0;
    bool   exitOvertookEnter = false;    // an exit handed out before the enter of its contact
    bool   backlogSeen       = false;    // a poll left events queued behind its limit
};

// Steps the pile for `steps` steps, polling both lists after every step, then polls
// until the queues are empty. The enter poll comes first within a round, as in the game.
Delivery runPile(uint32_t eventsPerPoll, int steps)
{
    HorizonWorld world;
    buildPile(world);
    PhysicsWorld phys;
    phys.setBudget({ 0.0, eventsPerPoll });   // the events only; no time budget
    phys.initialize(world);

    Delivery out;
    std::map<Pair, int> open;   // enters minus exits delivered so far
    const auto round = [&]
    {
        const auto in = phys.pollCollisionEnter();
        const auto ex = phys.pollCollisionExit();
        out.maxEnterBatch = std::max(out.maxEnterBatch, in.size());
        out.maxExitBatch  = std::max(out.maxExitBatch,  ex.size());
        for (const auto& e : in) { out.entered.emplace_back(e.entityA, e.entityB); ++open[{ e.entityA, e.entityB }]; }
        for (const auto& e : ex)
        {
            out.exited.emplace_back(e.entityA, e.entityB);
            if (--open[{ e.entityA, e.entityB }] < 0)
                out.exitOvertookEnter = true;
        }
        if (phys.budgetStats().eventsBacklog > 0)
            out.backlogSeen = true;
        return !in.empty() || !ex.empty();
    };
    for (int i = 0; i < steps; ++i)
    {
        phys.step(world, kDt);
        round();
    }
    for (int guard = 0; guard < 2000 && round(); ++guard) {}
    return out;
}

std::vector<Pair> sorted(std::vector<Pair> v)
{
    std::sort(v.begin(), v.end());
    return v;
}

} // namespace

// ─── The rules ───────────────────────────────────────────────────────────────

TEST_CASE("PhysicsBudgetRules::resolveFrameMs: the project asks, the environment wins, 0 is off")
{
    using namespace HE::PhysicsBudgetRules;
    CHECK(resolveFrameMs(4.0, -1.0) == doctest::Approx(4.0));
    CHECK(resolveFrameMs(4.0, 2.0)  == doctest::Approx(2.0));   // the environment wins
    CHECK(resolveFrameMs(4.0, 0.0)  == 0.0);                    // and may switch it off
    CHECK(resolveFrameMs(0.0, -1.0) == 0.0);
    CHECK(resolveFrameMs(-3.0, -1.0) == 0.0);                   // nonsense is "off", not a negative limit
    CHECK(resolveFrameMs(std::nan(""), -1.0) == 0.0);
    CHECK(resolveFrameMs(1.0e9, -1.0) == doctest::Approx(kMaxFrameMs));
}

TEST_CASE("PhysicsBudgetRules::frameMsFromEnv: a number of at least zero, else not set")
{
    using HE::PhysicsBudgetRules::frameMsFromEnv;
    { ScopedBudgetEnv e(nullptr); CHECK(frameMsFromEnv() < 0.0); }
    { ScopedBudgetEnv e("");      CHECK(frameMsFromEnv() < 0.0); }
    { ScopedBudgetEnv e("2.5");   CHECK(frameMsFromEnv() == doctest::Approx(2.5)); }
    { ScopedBudgetEnv e("0");     CHECK(frameMsFromEnv() == 0.0); }
    { ScopedBudgetEnv e("-1");    CHECK(frameMsFromEnv() < 0.0); }
    { ScopedBudgetEnv e("fast");  CHECK(frameMsFromEnv() < 0.0); }
    { ScopedBudgetEnv e("3ms");   CHECK(frameMsFromEnv() < 0.0); }
}

TEST_CASE("PhysicsBudgetRules: a character owes what it missed, up to a cap")
{
    using namespace HE::PhysicsBudgetRules;
    float owed = 0.0f;
    CHECK(steppedDt(kDt, owed) == doctest::Approx(kDt));          // nothing owed: the step's own dt
    owed = carryOwed(owed, kDt);
    CHECK(owed == doctest::Approx(kDt));
    CHECK(steppedDt(kDt, owed) == doctest::Approx(2.0f * kDt));   // stepped with what it missed
    CHECK(waitedSteps(owed, kDt) == 1u);
    for (int i = 0; i < 20; ++i)
        owed = carryOwed(owed, kDt);
    // A crowd that can never be caught up with slows down; it does not step with an
    // ever longer dt (ExtendedUpdate tunnels and loses the ground with a huge one).
    CHECK(owed == doctest::Approx(static_cast<float>(kMaxOwedSteps) * kDt));
    CHECK(steppedDt(kDt, owed) == doctest::Approx(static_cast<float>(kMaxOwedSteps + 1) * kDt));
}

TEST_CASE("ProjectPhysicsSettings: the budget defaults agree with the physics world's, and round-trip")
{
    HE::ProjectPhysicsSettings fresh;
    const HE::PhysicsBudgetConfig config;
    CHECK(fresh.budgetMs == doctest::Approx(static_cast<float>(config.frameMs)));
    CHECK(static_cast<uint32_t>(fresh.eventsPerPoll) == config.eventsPerPoll);

    HE::ProjectSettings a;
    a.physics.budgetMs      = 2.5f;
    a.physics.eventsPerPoll = 77;
    nlohmann::json j;
    a.toJson(j);
    CHECK(j["physics"]["budgetMs"] == doctest::Approx(2.5));
    CHECK(j["physics"]["eventsPerPoll"] == 77);
    HE::ProjectSettings b;
    b.fromJson(j);
    CHECK(b.physics.budgetMs == doctest::Approx(2.5f));
    CHECK(b.physics.eventsPerPoll == 77);
    CHECK(a == b);

    // Part of equality, so the editor sees the edit; clamped, so a bad file cannot hold a step hostage.
    HE::ProjectSettings c = a;
    c.physics.budgetMs = 3.0f;
    CHECK_FALSE(a == c);
    c.physics.budgetMs = -5.0f;
    c.physics.eventsPerPoll = -1;
    c.clamp();
    CHECK(c.physics.budgetMs == 0.0f);
    CHECK(c.physics.eventsPerPoll == 0);
    c.physics.budgetMs = 1.0e9f;
    c.clamp();
    CHECK(c.physics.budgetMs == doctest::Approx(HE::ProjectPhysicsSettings::kMaxBudgetMs));

    // A project from before the setting keeps today's behaviour.
    HE::ProjectSettings old;
    old.fromJson(nlohmann::json::object());
    CHECK(old.physics.budgetMs == doctest::Approx(4.0f));
}

// ─── Characters: overflow and catch-up ───────────────────────────────────────

TEST_CASE("PhysicsWorld budget: with no budget every character is stepped every step (negative control)")
{
    HorizonWorld world;
    const auto walkers = addWalkers(world, 64);
    PhysicsWorld phys;
    phys.setBudget({ 0.0, 4096 });
    phys.initialize(world);
    pushWalkers(phys, walkers, 2.0f);

    for (int i = 0; i < 4; ++i)
        phys.step(world, kDt);

    const auto b = phys.budgetStats();
    CHECK(b.budgetMs == 0.0);
    CHECK(b.characters == 64u);
    CHECK(b.charactersStepped == 64u);
    CHECK(b.charactersDeferred == 0u);
    CHECK(b.deferredCharacterSteps == 0u);
    CHECK(b.maxOwedSteps == 0u);
    // All of them covered the same four steps' distance.
    for (size_t i = 0; i < walkers.size(); ++i)
        CHECK(xOf(world, walkers[i]) - startX(static_cast<int>(i)) == doctest::Approx(2.0f * 4.0f * kDt).epsilon(0.02));
}

TEST_CASE("PhysicsWorld budget: a crowd past the budget is put off, nobody is lost, and the lag is bounded")
{
    constexpr int kCount = 64;   // two groups of the 32 that are always stepped
    constexpr int kSteps = 6;
    constexpr float kSpeed = 2.0f;

    // The reference: the same crowd with no budget.
    std::vector<float> reference(kCount);
    {
        HorizonWorld world;
        const auto walkers = addWalkers(world, kCount);
        PhysicsWorld phys;
        phys.setBudget({ 0.0, 4096 });
        phys.initialize(world);
        pushWalkers(phys, walkers, kSpeed);
        for (int i = 0; i < kSteps; ++i)
            phys.step(world, kDt);
        for (int i = 0; i < kCount; ++i)
            reference[static_cast<size_t>(i)] = xOf(world, walkers[static_cast<size_t>(i)]) - startX(i);
    }
    REQUIRE(reference[0] > 0.1f);   // they did walk; this would be vacuous otherwise

    HorizonWorld world;
    const auto walkers = addWalkers(world, kCount);
    PhysicsWorld phys;
    phys.setBudget(tinyBudget());
    phys.initialize(world);
    pushWalkers(phys, walkers, kSpeed);

    phys.step(world, kDt);
    {
        const auto b = phys.budgetStats();
        CHECK(b.budgetMs > 0.0);
        CHECK(b.characters == static_cast<uint32_t>(kCount));
        CHECK(b.charactersStepped == HE::PhysicsBudgetRules::kGuaranteedCharacters);
        CHECK(b.charactersDeferred == kCount - HE::PhysicsBudgetRules::kGuaranteedCharacters);
        CHECK(b.usedMs >= b.budgetMs);
        CHECK(b.overBudgetSteps == 1u);
    }
    for (int i = 1; i < kSteps; ++i)
        phys.step(world, kDt);

    // Every character moved, none further than the reference, none behind by more
    // than a step of the lag (two groups: one step), and some really are behind - the
    // deferral happened, it is not a run that never reached the budget.
    float behind = 0.0f;
    for (int i = 0; i < kCount; ++i)
    {
        const float moved = xOf(world, walkers[static_cast<size_t>(i)]) - startX(i);
        CHECK(moved > 0.0f);
        CHECK(moved <= reference[static_cast<size_t>(i)] + 1.0e-4f);
        CHECK(moved >= reference[static_cast<size_t>(i)] - kSpeed * kDt * 1.1f);
        behind = std::max(behind, reference[static_cast<size_t>(i)] - moved);
    }
    CHECK(behind > 0.5f * kSpeed * kDt);

    const auto b = phys.budgetStats();
    CHECK(b.deferredCharacterSteps == static_cast<uint64_t>(kSteps) * (kCount - HE::PhysicsBudgetRules::kGuaranteedCharacters));
    CHECK(b.maxOwedSteps >= 1u);
    CHECK(b.maxOwedSteps <= static_cast<uint32_t>(HE::PhysicsBudgetRules::kMaxOwedSteps));
}

TEST_CASE("PhysicsWorld budget: the ones that waited go first, so every character is stepped within a few steps")
{
    constexpr int kCount = 96;   // three groups
    HorizonWorld world;
    const auto walkers = addWalkers(world, kCount);
    PhysicsWorld phys;
    phys.setBudget(tinyBudget());
    phys.initialize(world);
    pushWalkers(phys, walkers, 2.0f);

    std::vector<float> last(kCount, -1.0f);
    for (int step = 1; step <= 9; ++step)
    {
        phys.step(world, kDt);
        // After the third step, each step moves the 32 that waited longest and nobody
        // else: the set of characters that advanced is the same size every step.
        int advanced = 0;
        for (int i = 0; i < kCount; ++i)
        {
            const float x = xOf(world, walkers[static_cast<size_t>(i)]);
            if (x > last[static_cast<size_t>(i)] + 1.0e-6f) ++advanced;
            last[static_cast<size_t>(i)] = x;
        }
        if (step >= 2)
            CHECK(advanced == 32);
    }
    // Nobody starved: within the nine steps every character moved a lot.
    for (int i = 0; i < kCount; ++i)
        CHECK(xOf(world, walkers[static_cast<size_t>(i)]) - startX(i) > 2.0f * 5.0f * kDt);
    CHECK(phys.budgetStats().maxOwedSteps <= static_cast<uint32_t>(HE::PhysicsBudgetRules::kMaxOwedSteps));
}

TEST_CASE("PhysicsWorld budget: a small scene is never put off, whatever the budget")
{
    HorizonWorld world;
    const auto walkers = addWalkers(world, 10);
    PhysicsWorld phys;
    phys.setBudget(tinyBudget());
    phys.initialize(world);
    for (int i = 0; i < 5; ++i)
        phys.step(world, kDt);
    const auto b = phys.budgetStats();
    CHECK(b.characters == 10u);
    CHECK(b.charactersStepped == 10u);
    CHECK(b.charactersDeferred == 0u);
    CHECK(b.deferredCharacterSteps == 0u);
}

TEST_CASE("PhysicsWorld budget: removing a character that was put off leaves the rest running")
{
    HorizonWorld world;
    const auto walkers = addWalkers(world, 64);
    PhysicsWorld phys;
    phys.setBudget(tinyBudget());
    phys.initialize(world);
    phys.step(world, kDt);
    REQUIRE(phys.budgetStats().charactersDeferred == 32u);

    // Whichever of them it was (the map's order decides which 32 were put off), the
    // rest of the crowd has to carry on without it.
    // (the walkers were never given a velocity here: they were put off if they have not been stepped,
    // which shows as the budget's own count, so pick any and check the count after)
    const Entity victim = walkers.back();
    phys.removeEntity(static_cast<uint32_t>(victim));
    world.destroyEntity(victim);

    for (int i = 0; i < 4; ++i)
        phys.step(world, kDt);
    CHECK(phys.budgetStats().characters == 63u);
}

TEST_CASE("PhysicsWorld::setBudget: the environment wins, and 0 turns the budget off")
{
    PhysicsWorld phys;
    phys.setBudget({ 3.0, 100 });
    CHECK(phys.budget().frameMs == doctest::Approx(3.0));
    CHECK(phys.budget().eventsPerPoll == 100u);
    {
        ScopedBudgetEnv env("1.5");
        phys.setBudget({ 3.0, 100 });
        CHECK(phys.budget().frameMs == doctest::Approx(1.5));
    }
    {
        ScopedBudgetEnv env("0");
        phys.setBudget({ 3.0, 100 });
        CHECK(phys.budget().frameMs == 0.0);
    }
    phys.setBudget({ 0.0, 0 });
    CHECK(phys.budget().frameMs == 0.0);
}

// ─── Queries and body work are charged ───────────────────────────────────────

TEST_CASE("PhysicsWorld budget: scene queries are counted, charged, and reset by the next step")
{
    HorizonWorld world;
    buildPile(world);
    PhysicsWorld phys;
    phys.initialize(world);
    phys.step(world, kDt);

    // No query since the last step: nothing to charge (the negative control).
    phys.step(world, kDt);
    CHECK(phys.budgetStats().queries == 0u);
    CHECK(phys.budgetStats().queriesMs == 0.0);

    for (int i = 0; i < 100; ++i)
        phys.raycast({ 0.0f, 20.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 100.0f);
    phys.overlapSphere({ 0.0f, 1.0f, 0.0f }, 3.0f);
    phys.sphereCast({ 0.0f, 20.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 0.5f, 100.0f);
    phys.step(world, kDt);
    CHECK(phys.budgetStats().queries == 102u);
    CHECK(phys.budgetStats().queriesMs > 0.0);

    phys.step(world, kDt);
    CHECK(phys.budgetStats().queries == 0u);
    // What the profiler reads is the newest step of the process.
    CHECK(PhysicsWorld::lastBudgetStats().queries == phys.budgetStats().queries);
    CHECK(PhysicsWorld::lastBudgetStats().characters == phys.budgetStats().characters);
}

TEST_CASE("PhysicsWorld budget: queries squeeze the characters' share of the step")
{
    constexpr double kBudgetMs = 10.0;
    constexpr int    kCount    = 64;

    // The control: the crowd alone fits the budget, nobody is put off.
    {
        HorizonWorld world;
        const auto walkers = addWalkers(world, kCount);
        PhysicsWorld phys;
        phys.setBudget({ kBudgetMs, 4096 });
        phys.initialize(world);
        pushWalkers(phys, walkers, 2.0f);
        phys.step(world, kDt);
        phys.step(world, kDt);
        const auto b = phys.budgetStats();
        CHECK(b.charactersDeferred == 0u);
        CHECK(b.charactersStepped == static_cast<uint32_t>(kCount));
        CHECK(b.usedMs < kBudgetMs);
    }

    // The same crowd after a script spent well over the budget on queries since the
    // last step: the characters get the guaranteed ones and the rest wait.
    HorizonWorld world;
    const auto walkers = addWalkers(world, kCount);
    PhysicsWorld phys;
    phys.setBudget({ kBudgetMs, 4096 });
    phys.initialize(world);
    pushWalkers(phys, walkers, 2.0f);
    phys.step(world, kDt);

    const auto start = std::chrono::steady_clock::now();
    uint32_t   asked = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(80))
    {
        phys.raycast({ 0.0f, 100.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, 500.0f);
        ++asked;
    }
    phys.step(world, kDt);
    const auto b = phys.budgetStats();
    CHECK(b.queries == asked);
    CHECK(b.queriesMs > kBudgetMs);
    CHECK(b.charactersStepped == HE::PhysicsBudgetRules::kGuaranteedCharacters);
    CHECK(b.charactersDeferred == static_cast<uint32_t>(kCount) - HE::PhysicsBudgetRules::kGuaranteedCharacters);
    CHECK(b.usedMs >= b.queriesMs);

    // And it passes: the next step has no queries, and the ones that waited catch up.
    phys.step(world, kDt);
    CHECK(phys.budgetStats().queries == 0u);
    CHECK(phys.budgetStats().charactersDeferred == 0u);
    CHECK(phys.budgetStats().charactersStepped == static_cast<uint32_t>(kCount));
}

// ─── Contact events: the cap and the order ───────────────────────────────────

TEST_CASE("PhysicsWorld events: with no cap everything goes out at once, exits never before enters (negative control)")
{
    const Delivery all = runPile(0, 150);
    REQUIRE(all.entered.size() > 192u);
    CHECK_FALSE(all.exited.empty());
    CHECK_FALSE(all.exitOvertookEnter);
    CHECK_FALSE(all.backlogSeen);
    CHECK(all.maxEnterBatch > 16u);   // so a cap of 16 below does cut something
}

TEST_CASE("PhysicsWorld events: a cap queues the rest in order and loses none")
{
    const Delivery all    = runPile(0, 150);
    const Delivery capped = runPile(16, 150);

    // Never more than the cap per list and poll...
    CHECK(capped.maxEnterBatch <= 16u);
    CHECK(capped.maxExitBatch  <= 16u);
    // ...it did hold events back...
    CHECK(capped.backlogSeen);
    // ...nothing was lost, nothing delivered twice (the same events, once the queue is drained)...
    CHECK(sorted(capped.entered) == sorted(all.entered));
    CHECK(sorted(capped.exited)  == sorted(all.exited));
    // ...and an exit was never handed out ahead of the enter of its contact.
    CHECK_FALSE(capped.exitOvertookEnter);
}

TEST_CASE("PhysicsWorld events: a removed entity's queued events are dropped, the others kept")
{
    HorizonWorld world;
    buildPile(world);
    PhysicsWorld phys;
    phys.setBudget({ 0.0, 8 });
    phys.initialize(world);
    for (int i = 0; i < 60; ++i)
        phys.step(world, kDt);
    const auto first = phys.pollCollisionEnter();
    REQUIRE(first.size() == 8u);
    REQUIRE(phys.budgetStats().eventsBacklog == 0u);   // the stats are from the step; the backlog shows after the next one
    phys.step(world, kDt);
    REQUIRE(phys.budgetStats().eventsBacklog > 0u);

    // Take out the box the first queued event names; nothing queued may name it afterwards.
    const uint32_t gone = first.front().entityA;
    phys.removeEntity(gone);
    world.destroyEntity(static_cast<Entity>(gone));
    for (int guard = 0; guard < 2000; ++guard)
    {
        const auto in = phys.pollCollisionEnter();
        const auto ex = phys.pollCollisionExit();
        for (const auto& e : in) { CHECK(e.entityA != gone); CHECK(e.entityB != gone); }
        for (const auto& e : ex) { CHECK(e.entityA != gone); CHECK(e.entityB != gone); }
        if (in.empty() && ex.empty())
            break;
    }
}
