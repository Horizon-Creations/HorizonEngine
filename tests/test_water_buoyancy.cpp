// Thema 179, step 2: buoyancy, drag and current on real Jolt bodies in a lake
// (PhysicsWorld::applyWater). A box of half the water's density floats, a heavy one
// sinks and rests, the current carries a floating one sideways, a body that is not
// meant to feel the water does not - and a scene without water is the scene it was.
//
// The lake is 40 × 40 m with its surface at y = 3 over flat ground at y = 0, inside a
// 64 × 64 m landscape. The waves it carries are the material's (Physics/WaterWaves.h),
// ±0.24 m, so a floating body rides them and "floats half out" is a mean, not a value.
#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/PhysicsBudget.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace water = HE::water;

namespace
{
    constexpr float kDt    = 1.0f / 60.0f;
    constexpr float kLevel = 3.0f;      // the lake's surface; the ground is at 0

    struct BoxSpec
    {
        glm::vec3     at{ 0.0f, 6.0f, 0.0f };
        glm::vec3     halfExtents{ 0.5f, 0.5f, 0.5f };
        float         mass     = 500.0f;
        float         density  = 500.0f;     // RigidBodyComponent::density; 0 = from the mass
        bool          affected = true;
        RigidBodyType type     = RigidBodyType::Dynamic;
        bool          trigger  = false;
        bool          sphere   = false;      // a ball of radius halfExtents.x instead of a box
    };

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    // A landscape with (or without) a lake, bodies added to it, and the world that steps them.
    struct Pond
    {
        HorizonWorld world;
        Entity       terrain = entt::null;
        uint16_t     lake    = water::kNoBody;
        PhysicsWorld phys;

        explicit Pond(bool withLake = true, float level = kLevel)
        {
            terrain = world.createEntity("Landscape");
            world.addComponent(terrain, TransformComponent{});
            TerrainComponent tc;
            tc.sizeX = tc.sizeZ = 64.0f;
            tc.resolution = 9;             // flat ground: no seed, no sculpted heights
            tc.water.res  = 64;            // a cell is a metre
            world.addComponent(terrain, tc);
            if (withLake)
            {
                lake = tcRef().water.createBody(level);
                REQUIRE(water::addPolygon(tcRef(), lake, rect(-20, -20, 20, 20)).ok);
            }
        }

        TerrainComponent& tcRef() { return world.registry().get<TerrainComponent>(terrain); }

        Entity addBox(const BoxSpec& s)
        {
            Entity e = world.createEntity("Box");
            TransformComponent t;
            t.position = s.at;
            world.addComponent(e, t);
            RigidBodyComponent rb;
            rb.type            = s.type;
            rb.mass            = s.mass;
            rb.density         = s.density;
            rb.affectedByWater = s.affected;
            world.addComponent(e, rb);
            ColliderComponent col;
            col.shape       = s.sphere ? ColliderShape::Sphere : ColliderShape::Box;
            col.halfExtents = s.halfExtents;
            col.radius      = s.halfExtents.x;
            col.isTrigger   = s.trigger;
            world.addComponent(e, col);
            return e;
        }

        // After the bodies are added: build the physics world. Independent of whatever
        // HE_PHYSICS_WATER says on the machine that runs the suite.
        void start(bool waterOn = true)
        {
            phys.initialize(world);
            phys.setThreadCount(1);
            phys.setWaterPhysics(waterOn);
        }
        void step(int n = 1) { for (int i = 0; i < n; ++i) phys.step(world, kDt); }

        glm::vec3 pos(Entity e) { return world.registry().get<TransformComponent>(e).position; }
        glm::vec3 turn(Entity e) { return world.registry().get<TransformComponent>(e).rotation; }
        static uint32_t id(Entity e) { return static_cast<uint32_t>(e); }
    };

    // Steps until `e` is at or below y, at most `limit` steps; the number of steps it took
    // (limit + 1 when it never got there).
    int stepsToReach(Pond& pond, Entity e, float y, int limit)
    {
        for (int i = 1; i <= limit; ++i)
        {
            pond.step();
            if (pond.pos(e).y <= y) return i;
        }
        return limit + 1;
    }

    bool sameBits(const glm::vec3& a, const glm::vec3& b) { return std::memcmp(&a, &b, sizeof(glm::vec3)) == 0; }

    // The positions and turns of every body, in creation order.
    std::vector<glm::vec3> poseOf(Pond& pond, const std::vector<Entity>& bodies)
    {
        std::vector<glm::vec3> out;
        for (Entity e : bodies) { out.push_back(pond.pos(e)); out.push_back(pond.turn(e)); }
        return out;
    }

    bool identical(const std::vector<glm::vec3>& a, const std::vector<glm::vec3>& b)
    {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) if (!sameBits(a[i], b[i])) return false;
        return true;
    }

    // 64 boxes in a 4 × 4 × 4 lattice, 2 m apart, above the middle of the landscape.
    std::vector<Entity> addLattice(Pond& pond)
    {
        std::vector<Entity> out;
        for (int y = 0; y < 4; ++y)
            for (int z = 0; z < 4; ++z)
                for (int x = 0; x < 4; ++x)
                {
                    BoxSpec s;
                    s.at       = { -3.0f + 2.0f * x, 2.0f + 2.0f * y, -3.0f + 2.0f * z };
                    s.halfExtents = { 0.4f, 0.4f, 0.4f };
                    s.mass     = 40.0f + 3.0f * static_cast<float>((x + 2 * y + 3 * z) % 5);   // not all alike
                    s.density  = 0.0f;
                    out.push_back(pond.addBox(s));
                }
        return out;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// Floating and sinking
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water buoyancy: a box of half the water's density floats, half out, and rides the waves")
{
    Pond pond;
    const Entity box = pond.addBox({});                    // 1 m³, 500 kg, density 500
    pond.start();
    pond.step(360);                                        // falls in, bobs, finds the surface

    // 28 s: two periods of the swell. The share under water averages the half the
    // densities say, though the waves move it up and down around it.
    double sum = 0.0;
    float lo = 1.0e9f, hi = -1.0e9f;
    int run = 0, frozen = 0, steps = 0;
    glm::vec3 prev = pond.pos(box);
    for (int i = 0; i < 1680; ++i)
    {
        pond.step();
        const glm::vec3 p = pond.pos(box);
        sum += pond.phys.submergedFraction(Pond::id(box));
        lo = std::min(lo, p.y);
        hi = std::max(hi, p.y);
        // A sleeping body does not move at all. (One repeated float at the top of a
        // swell is not sleep; a run of them is.)
        run = (p.y == prev.y) ? run + 1 : 0;
        frozen = std::max(frozen, run);
        prev = p;
        ++steps;
    }
    const double mean = sum / steps;
    CHECK(std::abs(mean - 0.5) < 0.1);
    CHECK(lo > kLevel - 0.8f);                             // it never sinks: the bed is 2.5 m below its centre
    CHECK(hi < kLevel + 0.8f);                             // and never leaves the surface
    CHECK(hi - lo > 0.2f);                                 // it follows the swell
    CHECK(frozen < 5);                                     // floating on moving water, it never falls asleep
    CHECK(pond.pos(box).y > 1.0f);                         // nowhere near the bed
}

TEST_CASE("Water buoyancy: bodies afloat never fall asleep on the moving surface")
{
    // The water serves only ACTIVE bodies (a stone asleep on the bed is not woken every
    // step). A body riding a swell passes through moments where its speed is under Jolt's
    // sleep threshold (0.03 m/s) for half a second - at every turn of the swell - and one
    // that fell asleep there would hang in the air while the water moved on. So the water
    // forbids sleep while a body is afloat. Six boxes and six balls in different phases
    // of the waves, forty seconds: none may stand still for a run of steps.
    Pond pond;
    std::vector<Entity> boxes;
    for (int i = 0; i < 12; ++i)
    {
        BoxSpec s;
        s.at = { -15.0f + 2.7f * static_cast<float>(i), 4.0f, (i % 2 ? 6.0f : -6.0f) + 0.37f * static_cast<float>(i) };
        s.sphere = (i % 2 == 0);                           // balls do not rock back: the likeliest to hang in a lull
        boxes.push_back(pond.addBox(s));
    }
    pond.start();
    pond.step(600);                                        // in, bobbing, settled onto the surface

    // A body asleep is not served, so the water's count of the bodies it served is the
    // sleep detector: all twelve, every step, or one of them has fallen asleep.
    uint32_t fewest = 12;
    for (int i = 0; i < 2400; ++i)
    {
        pond.step();
        fewest = std::min(fewest, pond.phys.budgetStats().wetBodies);
    }
    CHECK(fewest == 12);
    for (Entity e : boxes) CHECK(pond.phys.submergedFraction(Pond::id(e)) > 0.0f);
}

TEST_CASE("Water buoyancy: the same box with affectedByWater off falls to the ground")
{
    Pond pond;
    BoxSpec s; s.affected = false;
    const Entity box = pond.addBox(s);
    pond.start();
    pond.step(180);
    CHECK(pond.pos(box).y < 0.7f);                         // on the ground, a half extent over it
    CHECK(pond.phys.submergedFraction(Pond::id(box)) == 0.0f);   // the water never looked at it
}

TEST_CASE("Water buoyancy: a heavy body sinks, comes to rest on the bed, and sleeps there")
{
    Pond pond;
    BoxSpec s; s.mass = 2500.0f; s.density = 2500.0f;
    const Entity stone = pond.addBox(s);
    pond.start();
    pond.step(420);                                        // 7 s: in, down, still

    CHECK(pond.pos(stone).y == doctest::Approx(0.5f).epsilon(0.06));   // on the bed
    CHECK(pond.phys.submergedFraction(Pond::id(stone)) == doctest::Approx(1.0f));   // 3 m of water over it
    // Resting is resting: nothing moves it, and the water does not keep it awake. A body
    // that sleeps is not served (there is nothing to do for it), so it is not in the
    // water's count; one that lay awake would be served - and counted - every step.
    // (Position alone cannot tell: a body at rest does not move whether it sleeps or not.)
    int frozen = 0;
    uint32_t served = 0;
    glm::vec3 prev = pond.pos(stone);
    for (int i = 0; i < 60; ++i)
    {
        pond.step();
        if (pond.pos(stone).y == prev.y) ++frozen;
        served = std::max(served, pond.phys.budgetStats().wetBodies);
        prev = pond.pos(stone);
    }
    CHECK(frozen == 60);
    CHECK(served == 0);
    CHECK(pond.phys.submergedFraction(Pond::id(stone)) == doctest::Approx(1.0f));   // and it is still "fully under"
}

TEST_CASE("Water buoyancy: the water slows a sinking body, and drag slows it more")
{
    BoxSpec s; s.mass = 1500.0f; s.density = 1500.0f;     // 1.5 times the water: sinks, but not like a stone

    // With the water doing nothing (the control) it falls in free fall.
    Pond dry;
    const Entity freeFall = dry.addBox(s);
    dry.start(false);
    const int tFree = stepsToReach(dry, freeFall, 0.6f, 600);

    Pond wet;
    const Entity sinker = wet.addBox(s);
    wet.start();
    const int tWet = stepsToReach(wet, sinker, 0.6f, 600);

    water::BodyPhysics sticky;
    sticky.linearDrag = 10.0f;
    Pond thick;
    REQUIRE(water::setPhysics(thick.tcRef(), thick.lake, sticky));
    const Entity slowSinker = thick.addBox(s);
    thick.start();
    const int tThick = stepsToReach(thick, slowSinker, 0.6f, 900);

    CHECK(tFree < 120);
    CHECK(tWet > tFree + 8);                               // the buoyancy and the water's drag held it back
    CHECK(tThick > tWet + 40);                             // thicker water holds it back far longer
}

TEST_CASE("Water buoyancy: a body of the water's own density hangs where it is put")
{
    BoxSpec s; s.at = { 0.0f, 1.6f, 0.0f }; s.mass = 1000.0f; s.density = 1000.0f;   // fully under, neutral

    Pond pond;
    const Entity box = pond.addBox(s);
    pond.start();
    pond.step(180);
    CHECK(std::abs(pond.pos(box).y - 1.6f) < 0.15f);       // neither sinking nor rising
    CHECK(pond.phys.submergedFraction(Pond::id(box)) == doctest::Approx(1.0f));

    // The control: without the water's help the same box drops to the bed.
    Pond control;
    BoxSpec c = s; c.affected = false;
    const Entity dropped = control.addBox(c);
    control.start();
    control.step(180);
    CHECK(control.pos(dropped).y < 0.7f);
}

TEST_CASE("Water buoyancy: the density is the body of water's, so brine carries what fresh water drops")
{
    // Two lakes side by side on one landscape: fresh water on the left, brine (1500)
    // on the right. A box of density 1200 sinks in the first and floats in the second.
    Pond pond(false);
    TerrainComponent& tc = pond.tcRef();
    const uint16_t fresh = tc.water.createBody(kLevel);
    const uint16_t brine = tc.water.createBody(kLevel);
    REQUIRE(water::addPolygon(tc, fresh, rect(-20, -10, -2, 10)).ok);
    REQUIRE(water::addPolygon(tc, brine, rect(2, -10, 20, 10)).ok);
    water::BodyPhysics salty;
    salty.density = 1500.0f;
    REQUIRE(water::setPhysics(tc, brine, salty));

    BoxSpec s; s.mass = 1200.0f; s.density = 1200.0f;
    s.at = { -10.0f, 5.0f, 0.0f };
    const Entity inFresh = pond.addBox(s);
    s.at = { 10.0f, 5.0f, 0.0f };
    const Entity inBrine = pond.addBox(s);
    pond.start();
    pond.step(480);

    CHECK(pond.pos(inFresh).y < 0.7f);                     // 1200 > 1000: it sinks
    CHECK(pond.pos(inBrine).y > 2.0f);                     // 1200 < 1500: it floats (80 % under)
    CHECK(pond.pos(inBrine).y < 3.8f);
}

TEST_CASE("Water buoyancy: thin and light bodies settle instead of ringing")
{
    // A 2 × 0.05 × 2 m plate of 0.5 kg is about 2 kg/m³, the extreme the factor is clamped
    // for: floating at a quarter under. Explicit buoyancy is a spring; it must not blow up.
    Pond pond;
    BoxSpec s;
    s.at = { 0.0f, 5.0f, 0.0f }; s.halfExtents = { 1.0f, 0.025f, 1.0f };
    s.mass = 0.5f; s.density = 0.0f;                       // from the mass: the 1 000 000-fold lighter case
    const Entity plate = pond.addBox(s);
    pond.start();
    pond.step(300);

    float fastest = 0.0f, worst = 0.0f;
    glm::vec3 prev = pond.pos(plate);
    for (int i = 0; i < 600; ++i)
    {
        pond.step();
        const glm::vec3 p = pond.pos(plate);
        REQUIRE(std::isfinite(p.x));
        REQUIRE(std::isfinite(p.y));
        fastest = std::max(fastest, std::abs(p.y - prev.y) / kDt);
        // Where the surface is under it right now.
        water::Surface sf;
        const water::WaterIndex idx = water::WaterIndex::build(pond.world);
        if (idx.surfaceAt(p.x, p.z, pond.phys.simTime(), sf))
            worst = std::max(worst, std::abs(p.y - sf.height));
        prev = p;
    }
    CHECK(fastest < 2.0f);                                 // never the speed of a body being flung
    CHECK(worst < 0.6f);                                   // and it stays on the surface it rides
}

// ═════════════════════════════════════════════════════════════════════════════
// The current, and the drags
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water buoyancy: a current carries a floating body sideways, and none does not")
{
    BoxSpec s; s.at = { -2.0f, 4.0f, 2.0f };

    SUBCASE("along +x")
    {
        Pond pond;
        water::BodyPhysics river;
        river.flow = { 1.0f, 0.0f };
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, river));
        const Entity box = pond.addBox(s);
        pond.start();
        pond.step(720);                                    // 12 s
        CHECK(pond.pos(box).x - s.at.x > 5.0f);            // carried along the flow…
        CHECK(std::abs(pond.pos(box).z - s.at.z) < 1.5f);  // …and not across it
        CHECK(pond.pos(box).y > 2.0f);                     // still afloat
    }
    SUBCASE("along -z")
    {
        Pond pond;
        water::BodyPhysics river;
        river.flow = { 0.0f, -1.0f };
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, river));
        const Entity box = pond.addBox(s);
        pond.start();
        pond.step(720);
        CHECK(pond.pos(box).z - s.at.z < -5.0f);
        CHECK(std::abs(pond.pos(box).x - s.at.x) < 1.5f);
    }
    SUBCASE("no current: the waves rock it, they do not carry it away")
    {
        // A box rocking on a swell does creep a little (the drag is quadratic, so the
        // push and pull of a rocking body do not cancel exactly), but nothing like the
        // 5+ m the 1 m/s current above carries it in the same time.
        Pond pond;
        const Entity box = pond.addBox(s);
        pond.start();
        pond.step(720);
        CHECK(std::abs(pond.pos(box).x - s.at.x) < 2.5f);
        CHECK(std::abs(pond.pos(box).z - s.at.z) < 2.5f);
    }
}

TEST_CASE("Water buoyancy: the linear drag slows a body moving through the water")
{
    const auto speedAfter = [](float linearDrag)
    {
        Pond pond;
        water::BodyPhysics p;
        p.linearDrag = linearDrag;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, p));
        BoxSpec s; s.at = { -10.0f, 3.0f, 0.0f };
        const Entity box = pond.addBox(s);
        pond.start();
        pond.step(240);                                     // settle on the surface
        pond.phys.setVelocity(Pond::id(box), { 4.0f, 0.0f, 0.0f });
        pond.step(60);                                      // one second
        return pond.phys.getVelocity(Pond::id(box)).x;
    };
    // 3 is a strong drag (Jolt suggests about 0.5). Far beyond it the explicit step is
    // outside its stability region: the drag acts at the centre of buoyancy, below the
    // centre of mass, so a huge impulse also kicks the body's rotation and the speed
    // does not settle (at 10 it jumps to ~0 in one step and creeps back to ~0.8 m/s).
    const float free  = speedAfter(0.0f);
    const float thick = speedAfter(3.0f);
    CHECK(free > 3.0f);                                     // nothing but the body's own tiny damping
    CHECK(thick < 1.0f);
    CHECK(thick < free * 0.3f);
}

TEST_CASE("Water buoyancy: the angular drag stops a spinning body in the water")
{
    const auto spinAfter = [](float angularDrag)
    {
        Pond pond;
        water::BodyPhysics p;
        p.angularDrag = angularDrag;
        REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, p));
        BoxSpec s; s.at = { 0.0f, 3.0f, 0.0f };
        const Entity box = pond.addBox(s);
        pond.start();
        pond.step(240);
        pond.phys.setAngularVelocity(Pond::id(box), { 0.0f, 3.0f, 0.0f });   // yaw: no restoring torque
        pond.step(30);                                      // half a second
        return std::abs(pond.phys.getAngularVelocity(Pond::id(box)).y);
    };
    const float free  = spinAfter(0.0f);
    const float thick = spinAfter(10.0f);
    CHECK(free > 2.5f);
    CHECK(thick < 0.5f);
}

// ═════════════════════════════════════════════════════════════════════════════
// What the water does not touch
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water buoyancy: water switched off in the settings does nothing to a body")
{
    Pond pond;
    water::BodyPhysics off;
    off.enabled = false;
    REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, off));
    const Entity box = pond.addBox({});
    pond.start();
    pond.step(180);
    CHECK(pond.pos(box).y < 0.7f);                         // it falls through the lake
    CHECK(pond.phys.submergedFraction(Pond::id(box)) == 0.0f);

    // Switched on again while the game runs, the next step serves it (it is on the ground
    // now, 2.5 m under the surface, and asleep or not, it must float up once woken).
    water::BodyPhysics on;
    REQUIRE(water::setPhysics(pond.tcRef(), pond.lake, on));
    pond.phys.setVelocity(Pond::id(box), { 0.0f, 0.5f, 0.0f });   // a nudge wakes it
    pond.step(240);
    CHECK(pond.pos(box).y > 2.0f);
}

TEST_CASE("Water buoyancy: land is land - a body beside the lake falls exactly as it does without water")
{
    BoxSpec s; s.at = { 28.0f, 6.0f, 0.0f };               // on the landscape, beyond the lake's 20 m
    Pond wet;
    const Entity a = wet.addBox(s);
    wet.start();
    wet.step(200);

    Pond off;
    const Entity b = off.addBox(s);
    off.start(false);
    off.step(200);

    CHECK(wet.pos(a).y < 0.7f);
    CHECK(sameBits(wet.pos(a), off.pos(b)));               // not served, not touched, to the bit
    CHECK(wet.phys.submergedFraction(Pond::id(a)) == 0.0f);
}

TEST_CASE("Water buoyancy: kinematic bodies and sensors are not the water's")
{
    Pond pond;
    BoxSpec platform; platform.at = { -8.0f, 3.0f, 0.0f }; platform.type = RigidBodyType::Kinematic;
    BoxSpec volume;   volume.at = { 8.0f, 3.0f, 0.0f }; volume.trigger = true;
    const Entity k = pond.addBox(platform);
    const Entity t = pond.addBox(volume);
    pond.start();
    pond.step(120);
    CHECK(sameBits(pond.pos(k), platform.at));             // moved by code, not by water
    CHECK(pond.phys.submergedFraction(Pond::id(k)) == 0.0f);
    CHECK(pond.phys.submergedFraction(Pond::id(t)) == 0.0f);
}

// ═════════════════════════════════════════════════════════════════════════════
// A scene without water is the scene it was
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water buoyancy: a scene without water is bit for bit the scene it was")
{
    // 64 boxes falling and piling up on a landscape that has no water. The run with the
    // water physics on and the one with it off must agree to the last bit: the water
    // leaves the step after one test.
    Pond dry(false);
    const std::vector<Entity> a = addLattice(dry);
    dry.start();
    dry.step(300);

    Pond reference(false);
    const std::vector<Entity> b = addLattice(reference);
    reference.start(false);
    reference.step(300);

    REQUIRE(a.size() == 64);
    CHECK(identical(poseOf(dry, a), poseOf(reference, b)));
    CHECK(dry.phys.budgetStats().buoyancyMs == 0.0);       // and the accounting did not move either
    CHECK(dry.phys.budgetStats().wetBodies == 0);

    // The control that gives the comparison its meaning: the same boxes over a lake do differ.
    Pond lake(true);
    const std::vector<Entity> c = addLattice(lake);
    lake.start();
    lake.step(300);
    CHECK_FALSE(identical(poseOf(lake, c), poseOf(reference, b)));
}

TEST_CASE("Water buoyancy: water in the scene that no body is near changes nothing either")
{
    // The lake is in the far corner; the boxes are in the middle. Bodies never in a box
    // of water are never served, so the step is the step it was.
    Pond corner(false);
    {
        TerrainComponent& tc = corner.tcRef();
        const uint16_t pond = tc.water.createBody(kLevel);
        REQUIRE(water::addPolygon(tc, pond, rect(18, 18, 30, 30)).ok);
    }
    const std::vector<Entity> a = addLattice(corner);
    corner.start();
    corner.step(300);

    Pond reference(false);
    const std::vector<Entity> b = addLattice(reference);
    reference.start(false);
    reference.step(300);

    CHECK(identical(poseOf(corner, a), poseOf(reference, b)));
}

TEST_CASE("Water buoyancy: two runs of the same wet scene agree to the last bit")
{
    const auto run = []
    {
        Pond pond;
        std::vector<Entity> bodies = addLattice(pond);
        BoxSpec floater; floater.at = { 7.0f, 5.0f, 7.0f };
        bodies.push_back(pond.addBox(floater));
        pond.start();
        pond.step(300);
        return poseOf(pond, bodies);
    };
    CHECK(identical(run(), run()));
}

// ═════════════════════════════════════════════════════════════════════════════
// The budget
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
    // 300 boxes floating in the lake, 2 m apart, centred on the surface.
    std::vector<Entity> addFloaters(Pond& pond)
    {
        std::vector<Entity> out;
        for (int z = 0; z < 15; ++z)
            for (int x = 0; x < 20; ++x)
            {
                BoxSpec s;
                s.at = { -19.0f + 2.0f * x, kLevel, -14.0f + 2.0f * z };
                out.push_back(pond.addBox(s));
            }
        return out;
    }

    int wetCount(Pond& pond, const std::vector<Entity>& bodies)
    {
        int n = 0;
        for (Entity e : bodies) if (pond.phys.submergedFraction(Pond::id(e)) > 0.0f) ++n;
        return n;
    }
}

TEST_CASE("Water buoyancy: a crowd past the per-step count is served over several steps, not dropped")
{
    SUBCASE("a cap of 100: 200 wait, and everyone has been served within three steps")
    {
        Pond pond;
        const std::vector<Entity> boxes = addFloaters(pond);
        REQUIRE(boxes.size() == 300);
        pond.start();
        HE::PhysicsBudgetConfig cfg = pond.phys.budget();
        cfg.waterBodiesPerStep = 100;
        pond.phys.setBudget(cfg);

        pond.step();
        CHECK(pond.phys.budgetStats().buoyancyDeferred == 200);
        CHECK(wetCount(pond, boxes) <= 100);               // only the ones served have a share…
        CHECK(wetCount(pond, boxes) > 0);                  // …and some were

        pond.step(2);
        CHECK(wetCount(pond, boxes) == 300);               // the ones that waited were not forgotten
        CHECK(pond.phys.budgetStats().buoyancyDeferred == 200);   // (the cap still holds each step)
        CHECK(pond.phys.budgetStats().wetBodies == 100);
    }
    SUBCASE("no cap: all 300 in one step")
    {
        Pond pond;
        const std::vector<Entity> boxes = addFloaters(pond);
        pond.start();
        HE::PhysicsBudgetConfig cfg = pond.phys.budget();
        cfg.waterBodiesPerStep = 0;
        pond.phys.setBudget(cfg);
        pond.step();
        CHECK(pond.phys.budgetStats().buoyancyDeferred == 0);
        CHECK(wetCount(pond, boxes) == 300);
    }
    SUBCASE("the default cap serves 256 and the time is booked")
    {
        Pond pond;
        const std::vector<Entity> boxes = addFloaters(pond);
        pond.start();
        pond.step();
        const HE::PhysicsBudgetStats st = pond.phys.budgetStats();
        CHECK(HE::PhysicsBudgetConfig{}.waterBodiesPerStep == 256);
        CHECK(st.buoyancyDeferred == 44);
        CHECK(st.wetBodies == 256);
        CHECK(st.buoyancyMs > 0.0);
        CHECK(st.usedMs >= st.buoyancyMs);                 // part of what the step spent
    }
}

TEST_CASE("Water buoyancy: what 256 floating bodies cost (reported, not asserted)")
{
    // Timing is not a thing a test can promise on every machine; this prints the number
    // docs/water-physics-plan.md quotes, and checks only that the cost is booked and finite.
    Pond pond;
    const std::vector<Entity> boxes = addFloaters(pond);
    pond.start();
    HE::PhysicsBudgetConfig cfg = pond.phys.budget();
    cfg.waterBodiesPerStep = 256;
    pond.phys.setBudget(cfg);
    pond.step(60);                                         // let them settle onto the surface
    double total = 0.0, worst = 0.0;
    uint32_t served = 0;
    const int steps = 240;
    for (int i = 0; i < steps; ++i)
    {
        pond.step();
        const HE::PhysicsBudgetStats st = pond.phys.budgetStats();
        total += st.buoyancyMs;
        worst  = std::max(worst, st.buoyancyMs);
        served = st.wetBodies;
    }
    const double perStep = total / steps;
    MESSAGE("water: ", served, " bodies served per step, ", perStep, " ms per step on average (",
            served ? perStep * 1000.0 / served : 0.0, " us per body), worst step ", worst, " ms");
    CHECK(std::isfinite(perStep));
    CHECK(perStep > 0.0);
    CHECK(served > 0);
}

TEST_CASE("Water buoyancy: the simulation clock counts the steps it is given")
{
    Pond pond;
    pond.addBox({});
    pond.start();
    CHECK(pond.phys.simTime() == 0.0);
    pond.step(60);
    CHECK(pond.phys.simTime() == doctest::Approx(1.0).epsilon(1.0e-4));
    pond.step(30);
    CHECK(pond.phys.simTime() == doctest::Approx(1.5).epsilon(1.0e-4));
    pond.phys.initialize(pond.world);                      // a fresh world starts its clock over
    CHECK(pond.phys.simTime() == 0.0);
    CHECK(pond.phys.waterPhysics());
}
