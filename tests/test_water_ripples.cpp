// Thema 179, step 3: the rings on the water. Three layers, each tested on its own:
//   * HE::water::RippleField, the bounded list (add, merge, replace the weakest, fade, pack);
//   * what raises a ring in the physics (PhysicsWorld::applyWater): a body going into a lake,
//     one coming out, one cutting the surface, and the scenes that must raise none;
//   * the block the shaders read (HE::FillMaterialWater into the Lighting ABI).
// The shader text and the graph node have their own tests (test_material_graph.cpp,
// test_engine_materials.cpp); the image is the headless witness HE_DUMP_WATERRIPPLE.
//
// The lake is the one of test_water_buoyancy.cpp: 40 x 40 m, surface at y = 3 over flat
// ground at y = 0, inside a 64 x 64 m landscape, with the material's waves (+/-0.24 m).
#include "doctest.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/WaterRipples.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonRendering/LightPacking.h>
#include <material/MaterialShaderLibrary.h>
#include <MaterialGraph/MaterialGraph.h>
#include <Renderer/WaterRipples.h>
#include <glm/glm.hpp>
#if defined(HE_TESTS_HAVE_SHADERC)
#include <spirv_cross.hpp>   // reflection of the block the Vulkan backend would build (as test_material_graph.cpp)
#endif

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <vector>

namespace water = HE::water;
using HE::MaterialGraph;
using HE::MatNodeType;
using HE::water::Ripple;
using HE::water::RippleField;
using HE::water::RippleTriggers;

namespace
{
    constexpr float kDt    = 1.0f / 60.0f;
    constexpr float kLevel = 3.0f;      // the lake's surface; the ground is at 0

    Ripple ring(float x, float z, float strength, int kind = WaterRippleImpact, float radius = 0.5f, float age = 0.0f)
    {
        Ripple r;
        r.pos      = { x, z };
        r.strength = strength;
        r.kind     = kind;
        r.radius   = radius;
        r.age      = age;
        return r;
    }

    bool sameBlock(const WaterRippleBlock& a, const WaterRippleBlock& b)
    {
        return std::memcmp(&a, &b, sizeof(WaterRippleBlock)) == 0;
    }

    // ── The lake and the bodies, as in test_water_buoyancy.cpp ──────────────────
    struct BodySpec
    {
        glm::vec3 at{ 0.0f, 6.0f, 0.0f };
        glm::vec3 halfExtents{ 0.5f, 0.5f, 0.5f };
        float     mass    = 500.0f;
        float     density = 2500.0f;     // heavier than water: it sinks
        bool      sphere  = true;
    };

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    struct Pond
    {
        HorizonWorld world;
        Entity       terrain = entt::null;
        uint16_t     lake    = water::kNoBody;
        PhysicsWorld phys;

        explicit Pond(bool withLake = true)
        {
            terrain = world.createEntity("Landscape");
            world.addComponent(terrain, TransformComponent{});
            TerrainComponent tc;
            tc.sizeX = tc.sizeZ = 64.0f;
            tc.resolution = 9;
            tc.water.res  = 64;
            world.addComponent(terrain, tc);
            if (withLake)
            {
                lake = tcRef().water.createBody(kLevel);
                REQUIRE(water::addPolygon(tcRef(), lake, rect(-20, -20, 20, 20)).ok);
            }
        }

        TerrainComponent& tcRef() { return world.registry().get<TerrainComponent>(terrain); }

        Entity add(const BodySpec& s)
        {
            Entity e = world.createEntity("Body");
            TransformComponent t;
            t.position = s.at;
            world.addComponent(e, t);
            RigidBodyComponent rb;
            rb.type    = RigidBodyType::Dynamic;
            rb.mass    = s.mass;
            rb.density = s.density;
            world.addComponent(e, rb);
            ColliderComponent col;
            col.shape       = s.sphere ? ColliderShape::Sphere : ColliderShape::Box;
            col.halfExtents = s.halfExtents;
            col.radius      = s.halfExtents.x;
            world.addComponent(e, col);
            return e;
        }

        void start(bool waterOn = true)
        {
            phys.initialize(world);
            phys.setThreadCount(1);
            phys.setWaterPhysics(waterOn);
        }
        void step(int n = 1) { for (int i = 0; i < n; ++i) phys.step(world, kDt); }
        const RippleField& rings() const { return phys.waterRipples(); }
        static uint32_t id(Entity e) { return static_cast<uint32_t>(e); }

        // How many rings of this kind are on the water now.
        int count(int kind) const
        {
            int n = 0;
            for (int i = 0; i < rings().count(); ++i)
                if (rings().at(i).kind == kind) ++n;
            return n;
        }
        // The rings raised in the step that has just run (age 0: step() ages the old ones first).
        int fresh() const
        {
            int n = 0;
            for (int i = 0; i < rings().count(); ++i)
                if (rings().at(i).age == 0.0f) ++n;
            return n;
        }
    };

    // The surface height at x, z at simulation time 0.
    float surfaceAtStart(Pond& pond, float x, float z)
    {
        const auto index = water::WaterIndex::build(pond.world);
        water::Surface s;
        REQUIRE(index.surfaceAt(x, z, 0.0, s));
        return s.height;
    }
}

// ═════════════════════════════════════════════════════════════════════════════
// The list
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water ripples: a ring is clamped, and a faded or malformed one is left out")
{
    RippleField f;
    f.add(ring(1.0f, 2.0f, 3.0f));                          // strength is a 0..1 number
    REQUIRE(f.count() == 1);
    CHECK(f.at(0).strength == 1.0f);
    CHECK(f.at(0).pos.x == 1.0f);
    CHECK(f.at(0).pos.y == 2.0f);

    f.add(ring(9.0f, 9.0f, 0.01f));                         // under kMinStrength: nothing to see
    f.add(ring(9.0f, 9.0f, 0.0f));
    f.add(ring(9.0f, 9.0f, -1.0f));
    const float notANumber = std::numeric_limits<float>::quiet_NaN();
    f.add(ring(notANumber, 0.0f, 1.0f));
    f.add(ring(0.0f, 0.0f, notANumber));
    f.add(ring(0.0f, 0.0f, 1.0f, WaterRippleImpact, std::numeric_limits<float>::infinity()));
    CHECK(f.count() == 1);

    RippleField g;
    g.add(ring(0.0f, 0.0f, 1.0f, WaterRippleImpact, 0.0f));          // the radius has a floor and a ceiling
    g.add(ring(20.0f, 0.0f, 1.0f, WaterRippleImpact, 100.0f));
    g.add(ring(40.0f, 0.0f, 1.0f, WaterRippleImpact, 0.5f, -3.0f)); // and an age is never negative
    REQUIRE(g.count() == 3);
    CHECK(g.at(0).radius == doctest::Approx(0.05f));
    CHECK(g.at(1).radius == doctest::Approx(6.0f));
    CHECK(g.at(2).age == 0.0f);
}

TEST_CASE("Water ripples: a young ring of the same kind close by is refreshed, not duplicated")
{
    RippleField f;
    f.add(ring(0.0f, 0.0f, 0.4f, WaterRippleImpact, 0.4f));
    f.advance(0.1f);                                        // young: under kMergeAge
    f.add(ring(0.2f, 0.0f, 0.9f, WaterRippleImpact, 0.6f)); // within kMergeDistance, same kind
    REQUIRE(f.count() == 1);
    CHECK(f.at(0).strength == doctest::Approx(0.9f));      // the stronger of the two
    CHECK(f.at(0).radius == doctest::Approx(0.6f));
    CHECK(f.at(0).pos.x == doctest::Approx(0.2f));         // from the new place
    CHECK(f.at(0).age == 0.0f);                            // starting over

    // The faded old one against a weaker newcomer: the old ring's strength NOW is what counts.
    RippleField h;
    h.add(ring(0.0f, 0.0f, 1.0f));
    h.advance(0.2f);
    const float nowStrength = RippleField::effectiveStrength(h.at(0));
    h.add(ring(0.1f, 0.0f, 0.1f));
    REQUIRE(h.count() == 1);
    CHECK(h.at(0).strength == doctest::Approx(nowStrength));

    // A newcomer that is itself part-faded (age > 0): the ring that is stronger NOW wins, and
    // what is stored must read as that strength at the age the merge leaves (the newcomer's).
    // Old: 0.5 faded 0.1 s = 0.5 * exp(-0.1/1.4) = 0.4656. Newcomer: 0.6 at age 0.5 = 0.6 * exp(-0.5/1.4)
    // = 0.4198, so the old ring is stronger even though 0.6 > 0.4656 raw.
    RippleField a;
    a.add(ring(0.0f, 0.0f, 0.5f));
    a.advance(0.1f);
    const float oldNow = RippleField::effectiveStrength(a.at(0));
    a.add(ring(0.1f, 0.0f, 0.6f, WaterRippleImpact, 0.5f, 0.5f));
    REQUIRE(a.count() == 1);
    CHECK(a.at(0).age == doctest::Approx(0.5f));
    CHECK(RippleField::effectiveStrength(a.at(0)) == doctest::Approx(oldNow));

    // And the other way round: the faded newcomer is the stronger one, and its own strength is kept.
    // Old: 0.2 faded 0.1 s = 0.1862. Newcomer: 0.9 at age 0.2 = 0.9 * exp(-0.2/1.4) = 0.7802.
    RippleField b;
    b.add(ring(0.0f, 0.0f, 0.2f));
    b.advance(0.1f);
    b.add(ring(0.1f, 0.0f, 0.9f, WaterRippleImpact, 0.5f, 0.2f));
    REQUIRE(b.count() == 1);
    CHECK(b.at(0).strength == doctest::Approx(0.9f));
    CHECK(RippleField::effectiveStrength(b.at(0)) == doctest::Approx(0.9f * std::exp(-0.2f / RippleField::kDecayTime)));

    // Each of these is a separate ring: another kind, too far, too old.
    RippleField s;
    s.add(ring(0.0f, 0.0f, 0.5f, WaterRippleImpact));
    s.add(ring(0.1f, 0.0f, 0.5f, WaterRippleExit));
    s.add(ring(RippleField::kMergeDistance + 0.05f, 0.0f, 0.5f, WaterRippleImpact));
    s.advance(RippleField::kMergeAge + 0.1f);
    s.add(ring(0.0f, 0.0f, 0.5f, WaterRippleImpact));
    CHECK(s.count() == 4);

    // A trail never merges: a body moving through the water leaves a line of them.
    RippleField t;
    t.add(ring(0.0f, 0.0f, 0.3f, WaterRippleTrail));
    t.add(ring(0.1f, 0.0f, 0.3f, WaterRippleTrail));
    t.add(ring(0.2f, 0.0f, 0.3f, WaterRippleTrail));
    CHECK(t.count() == 3);
}

TEST_CASE("Water ripples: the list is bounded, and a full list gives way only to a stronger ring")
{
    RippleField f;
    for (int i = 0; i < RippleField::kMax; ++i)
        f.add(ring(5.0f * static_cast<float>(i), 0.0f, 0.2f + 0.04f * static_cast<float>(i)));   // 0.20 .. 0.80
    REQUIRE(f.count() == RippleField::kMax);

    // Stronger than the weakest (0.20): that one goes.
    f.add(ring(500.0f, 0.0f, 0.5f));
    REQUIRE(f.count() == RippleField::kMax);
    bool weakestStillThere = false, newcomerThere = false;
    for (int i = 0; i < f.count(); ++i)
    {
        weakestStillThere |= f.at(i).pos.x == 0.0f;
        newcomerThere     |= f.at(i).pos.x == 500.0f;
    }
    CHECK_FALSE(weakestStillThere);
    CHECK(newcomerThere);

    // Weaker than everything on it (the weakest now is 0.24): it is not worth a ring.
    const WaterRippleBlock before = f.pack();
    f.add(ring(600.0f, 0.0f, 0.05f));
    CHECK(f.count() == RippleField::kMax);
    CHECK(sameBlock(before, f.pack()));

    // And more than kMax adds never grows it.
    for (int i = 0; i < 100; ++i)
        f.add(ring(700.0f + 5.0f * static_cast<float>(i), 1.0f, 0.9f));
    CHECK(f.count() == RippleField::kMax);
}

TEST_CASE("Water ripples: a ring ages with the time it is given, drifts, and leaves when it has faded")
{
    RippleField f;
    Ripple a = ring(0.0f, 0.0f, 0.3f);
    Ripple b = ring(10.0f, 0.0f, 1.0f);
    b.drift = { 2.0f, -1.0f };
    Ripple c = ring(20.0f, 0.0f, 0.5f);
    f.add(a); f.add(b); f.add(c);
    REQUIRE(f.count() == 3);

    f.advance(1.0f);
    CHECK(f.at(0).age == doctest::Approx(1.0f));
    CHECK(f.at(1).pos.x == doctest::Approx(12.0f));        // the current carries it
    CHECK(f.at(1).pos.y == doctest::Approx(-1.0f));
    CHECK(f.at(2).pos.x == doctest::Approx(20.0f));        // no drift: it stays

    // The strength a ring has: strength * exp(-age / decay).
    CHECK(RippleField::effectiveStrength(f.at(1)) ==
          doctest::Approx(std::exp(-1.0f / RippleField::kDecayTime)));

    // 0.3 fades under kMinStrength first (after decay * ln(15) = 3.8 s), the others stay, in order.
    float t = 1.0f;
    while (f.count() == 3 && t < 10.0f) { f.advance(0.1f); t += 0.1f; }
    REQUIRE(f.count() == 2);
    CHECK(t > 3.6f);
    CHECK(t < 4.0f);
    CHECK(f.at(0).strength == doctest::Approx(1.0f));
    CHECK(f.at(1).strength == doctest::Approx(0.5f));

    // Nothing moves for a dt that is not a positive number.
    const WaterRippleBlock held = f.pack();
    f.advance(0.0f);
    f.advance(-1.0f);
    f.advance(std::numeric_limits<float>::quiet_NaN());
    CHECK(sameBlock(held, f.pack()));

    // Everything fades in the end: the list is empty again.
    for (int i = 0; i < 400; ++i) f.advance(0.1f);
    CHECK(f.count() == 0);
}

TEST_CASE("Water ripples: the block the shaders read is the list, field for field")
{
    RippleField f;
    Ripple a = ring(1.5f, -2.5f, 0.8f, WaterRippleImpact, 0.6f, 0.4f);
    Ripple b = ring(-7.0f, 3.0f, 0.3f, WaterRippleTrail, 0.2f, 0.0f);
    f.add(a); f.add(b);
    const WaterRippleBlock p = f.pack();

    CHECK(p.head[0] == 2.0f);                              // how many
    CHECK(p.head[1] == RippleField::kRingSpeed);           // how fast the front runs
    CHECK(p.head[2] == RippleField::kDecayTime);           // how fast it fades
    CHECK(p.a[0][0] == 1.5f);  CHECK(p.a[0][1] == -2.5f);  CHECK(p.a[0][2] == 0.6f);  CHECK(p.a[0][3] == 0.8f);
    CHECK(p.b[0][0] == 0.4f);  CHECK(p.b[0][1] == static_cast<float>(WaterRippleImpact));
    CHECK(p.a[1][0] == -7.0f); CHECK(p.a[1][1] == 3.0f);   CHECK(p.a[1][2] == 0.2f);  CHECK(p.a[1][3] == 0.3f);
    CHECK(p.b[1][0] == 0.0f);  CHECK(p.b[1][1] == static_cast<float>(WaterRippleTrail));
    // What is not a ring is zero, so a loop that runs too far reads nothing.
    for (int i = 2; i < kWaterRippleMax; ++i)
        for (int k = 0; k < 4; ++k)
        {
            CHECK(p.a[i][k] == 0.0f);
            CHECK(p.b[i][k] == 0.0f);
        }

    // An empty list is an empty block: count 0 is the shader's "no rings" (it never enters the loop).
    RippleField none;
    CHECK(none.pack().head[0] == 0.0f);

    // The same adds, the same bytes.
    RippleField g;
    g.add(a); g.add(b);
    CHECK(sameBlock(p, g.pack()));
}

// ═════════════════════════════════════════════════════════════════════════════
// The block in the lighting ABI
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water ripples: FillMaterialWater copies the block into the lighting tail and touches nothing else")
{
    using Lighting = HE::MaterialShaderLibrary::Lighting;
    RippleField f;
    f.add(ring(1.0f, 2.0f, 0.7f, WaterRippleImpact, 0.5f, 0.25f));
    f.add(ring(30.0f, -4.0f, 0.4f, WaterRippleExit, 0.9f, 1.0f));
    const WaterRippleBlock block = f.pack();

    Lighting lit;
    const Lighting untouched;
    HE::FillMaterialWater(block, lit);
    CHECK(std::memcmp(lit.waterRipples, block.head, sizeof(block.head)) == 0);
    CHECK(std::memcmp(lit.waterRippleA, block.a, sizeof(block.a)) == 0);
    CHECK(std::memcmp(lit.waterRippleB, block.b, sizeof(block.b)) == 0);
    CHECK(lit.waterRipples[0] == 2.0f);

    // The fields before the tail keep what they had (compare the whole struct up to it).
    CHECK(std::memcmp(&lit, &untouched, offsetof(Lighting, waterRipples)) == 0);

    // A fill site that never calls it leaves "no rings" in the shader's block.
    CHECK(untouched.waterRipples[0] == 0.0f);

    // A block that claims more rings than the shader declares is held to its arrays.
    WaterRippleBlock bad = block;
    bad.head[0] = 99.0f;
    Lighting over;
    HE::FillMaterialWater(bad, over);
    CHECK(over.waterRipples[0] == static_cast<float>(kWaterRippleMax));
    bad.head[0] = -3.0f;
    HE::FillMaterialWater(bad, over);
    CHECK(over.waterRipples[0] == 0.0f);

    // The size the Metal path hands over with setFragmentBytes (4096-byte limit; LightPacking.cpp
    // static_asserts the same).
    CHECK(sizeof(Lighting) <= 4096);
}

#if defined(HE_TESTS_HAVE_SHADERC)
// The size of the translated block is pinned by test_engine_materials.cpp (it must equal
// sizeof(Lighting)); the OFFSETS are what a shear would change without changing the size, so
// they are pinned here, member by member, on the SPIR-V the Vulkan backend would build.
TEST_CASE("Water ripples: the shader's HeLighting block puts the ring arrays where the C++ struct does")
{
    using Lighting = HE::MaterialShaderLibrary::Lighting;
    MaterialGraph g;
    const int out = g.addNode(MatNodeType::Output);
    const int rip = g.addNode(MatNodeType::WaterRipple);
    REQUIRE(g.connect(rip, 2, out, HE::kMatOutputEmissivePin));            // Foam
    const HE::MatShaderGen gen = HE::generateFragment(g);
    REQUIRE_FALSE(gen.glsl.empty());

    HE::MaterialShaderLibrary lib;
    const auto& sv = lib.fragment(std::hash<std::string>{}(gen.glsl), gen.glsl,
                                  HE::MaterialShaderLibrary::Backend::SpirV);
    REQUIRE_MESSAGE(sv.ok, sv.log);

    spirv_cross::Compiler comp(sv.spirv);
    const spirv_cross::ShaderResources res = comp.get_shader_resources(comp.get_active_interface_variables());
    bool foundBlock = false;
    for (const spirv_cross::Resource& ub : res.uniform_buffers)
    {
        if (comp.get_name(ub.base_type_id) != "HeLighting")
            continue;
        foundBlock = true;
        const spirv_cross::SPIRType& t = comp.get_type(ub.base_type_id);
        int seen = 0;
        for (uint32_t i = 0; i < t.member_types.size(); ++i)
        {
            const std::string& name = comp.get_member_name(ub.base_type_id, i);
            const size_t offset = comp.type_struct_member_offset(t, i);
            if (name == "weather2")
            {
                ++seen;
                CHECK(offset == offsetof(Lighting, weather2));             // the tail starts right after it
            }
            else if (name == "waterRipples")
            {
                ++seen;
                CHECK(offset == offsetof(Lighting, waterRipples));
            }
            else if (name == "waterRippleA" || name == "waterRippleB")
            {
                ++seen;
                CHECK(offset == (name == "waterRippleA" ? offsetof(Lighting, waterRippleA)
                                                         : offsetof(Lighting, waterRippleB)));
                // One vec4 per ring, which is what the C++ arrays of four floats are.
                CHECK(comp.type_struct_member_array_stride(t, i) == 4u * sizeof(float));
                const spirv_cross::SPIRType& mt = comp.get_type(t.member_types[i]);
                REQUIRE(mt.array.size() == 1);
                CHECK(mt.array[0] == static_cast<uint32_t>(kWaterRippleMax));
            }
        }
        CHECK(seen == 4);
        CHECK(comp.get_declared_struct_size(t) == sizeof(Lighting));
    }
    CHECK(foundBlock);
}
#endif

// ═════════════════════════════════════════════════════════════════════════════
// What raises a ring
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water ripples: a ball that lands in a lake raises one impact ring where it landed, and none before")
{
    Pond pond;
    BodySpec spec;
    spec.at = { 4.0f, 6.0f, -3.0f };                       // 2.5 m above the surface, centre to the water
    const Entity ball = pond.add(spec);
    pond.start();

    int landed = -1;
    for (int i = 1; i <= 240 && landed < 0; ++i)
    {
        pond.step();
        if (pond.rings().count() > 0)
            landed = i;
        else
            CHECK(pond.phys.submergedFraction(Pond::id(ball)) < RippleTriggers::kWetShare);   // dry up to then
    }
    REQUIRE(landed > 0);
    REQUIRE(pond.rings().count() >= 1);
    const Ripple& r = pond.rings().at(0);
    CHECK(r.kind == WaterRippleImpact);
    CHECK(r.pos.x == doctest::Approx(4.0f).epsilon(0.02));
    CHECK(r.pos.y == doctest::Approx(-3.0f).epsilon(0.02));
    CHECK(r.radius == doctest::Approx(0.5f));              // the ball's size
    CHECK(r.strength > 0.9f);                              // about 7 m/s: a full ring
    CHECK(r.age == 0.0f);                                  // raised in the step that just ran
    CHECK(pond.phys.submergedFraction(Pond::id(ball)) >= RippleTriggers::kWetShare);

    // Sinking through the surface and on to the bed raises no second impact: one body, one splash.
    pond.step(180);
    CHECK(pond.count(WaterRippleImpact) == 1);
    CHECK(pond.rings().count() <= RippleField::kMax);
}

TEST_CASE("Water ripples: the same drop raises nothing where the water is off, has no splash, or is not there")
{
    BodySpec spec;
    spec.at = { 4.0f, 6.0f, -3.0f };

    // The control the others are measured against: with water, a ring.
    {
        Pond pond;
        pond.add(spec);
        pond.start();
        pond.step(240);
        CHECK(pond.rings().count() > 0);
    }
    // The water physics off (HE_PHYSICS_WATER=0): no buoyancy, no ring.
    {
        Pond pond;
        pond.add(spec);
        pond.start(false);
        pond.step(240);
        CHECK(pond.rings().count() == 0);
    }
    // This water makes no splash (splash 0): the ball is still wet, there is no ring.
    {
        Pond pond;
        const Entity ball = pond.add(spec);
        water::BodyPhysics p = pond.tcRef().water.bodies.front().physics;
        p.splash = 0.0f;
        water::setPhysics(pond.tcRef(), pond.lake, p);
        pond.start();
        float wettest = 0.0f;
        for (int i = 0; i < 240; ++i)
        {
            pond.step();
            wettest = std::max(wettest, pond.phys.submergedFraction(Pond::id(ball)));
        }
        CHECK(wettest > 0.5f);
        CHECK(pond.rings().count() == 0);
    }
    // No lake in the scene at all.
    {
        Pond pond(false);
        pond.add(spec);
        pond.start();
        pond.step(240);
        CHECK(pond.rings().count() == 0);
    }
}

TEST_CASE("Water ripples: a body that starts in the water makes no splash, however it is moving, unless it is fast")
{
    // A box floating half in the water: it was never "going in" (nothing at the first step, it
    // starts there) and, once the start-up wobble from the swell has settled, it bobs slower
    // than any trail needs, so six seconds more on the swell raise nothing.
    {
        Pond pond;
        BodySpec spec;
        spec.at = { 0.0f, kLevel, 0.0f };
        spec.density = 500.0f;
        spec.sphere = false;
        pond.add(spec);
        pond.start();
        pond.step(1);
        CHECK(pond.rings().count() == 0);                  // no splash for being there at the start
        pond.step(239);                                    // four seconds: the first wobble dies down
        pond.phys.waterRipples().clear();
        pond.step(360);
        CHECK(pond.rings().count() == 0);
    }
    // A heavy ball put in the surface (half under it) and sent down: the speed it arrives with
    // is the line. At 0.5 m/s, under kImpactSpeed, there is no impact (a trail is another thing);
    // at 4 m/s there is exactly one, and a body that sinks raises no second.
    for (const float down : { 0.5f, 4.0f })
    {
        CAPTURE(down);
        Pond pond;
        BodySpec spec;
        spec.at = { 6.0f, surfaceAtStart(pond, 6.0f, 6.0f), 6.0f };
        const Entity ball = pond.add(spec);
        pond.start();
        pond.phys.setVelocity(Pond::id(ball), { 0.0f, -down, 0.0f });
        pond.step(180);
        CHECK(pond.count(WaterRippleImpact) == (down > RippleTriggers::kImpactSpeed ? 1 : 0));
    }
}

TEST_CASE("Water ripples: a body that leaves the water fast raises an exit ring, a body that cuts the surface a trail")
{
    // Out: a ball that floats is thrown up through the surface at 5 m/s.
    {
        Pond pond;
        BodySpec spec;
        spec.at = { -5.0f, kLevel, 2.0f };
        spec.density = 500.0f;
        const Entity ball = pond.add(spec);
        pond.start();
        pond.phys.setVelocity(Pond::id(ball), { 0.0f, 5.0f, 0.0f });
        pond.step(60);
        REQUIRE(pond.count(WaterRippleExit) == 1);
        for (int i = 0; i < pond.rings().count(); ++i)
            if (pond.rings().at(i).kind == WaterRippleExit)
            {
                CHECK(pond.rings().at(i).strength > 0.3f);
                CHECK(pond.rings().at(i).strength <= 0.6f);
                CHECK(pond.rings().at(i).pos.x == doctest::Approx(-5.0f).epsilon(0.05));
            }
    }
    // The cut: a floating box held at 2 m/s along the surface leaves rings behind it, a line of
    // them, no more often than kTrailInterval, and none of them is an impact.
    {
        Pond pond;
        BodySpec spec;
        spec.at = { -8.0f, kLevel, 0.0f };
        spec.density = 500.0f;
        spec.sphere = false;
        const Entity box = pond.add(spec);
        pond.start();
        pond.step(120);                                    // settle on the swell
        pond.phys.waterRipples().clear();
        for (int i = 0; i < 60; ++i)                       // one second at 2 m/s along +x
        {
            pond.phys.setVelocity(Pond::id(box), { 2.0f, pond.phys.getVelocity(Pond::id(box)).y, 0.0f });
            pond.step();
        }
        const int trails = pond.count(WaterRippleTrail);
        CHECK(trails >= 5);                                // 8 in a second at 0.12 s
        CHECK(trails <= 9);
        CHECK(pond.count(WaterRippleImpact) == 0);
        float lastX = -1.0e9f;
        for (int i = 0; i < pond.rings().count(); ++i)
        {
            CHECK(pond.rings().at(i).pos.x > lastX);       // a line along its way, in order
            lastX = pond.rings().at(i).pos.x;
        }
    }
}

TEST_CASE("Water ripples: a crowd falling in at once is throttled per step and bounded in the list")
{
    Pond pond;
    for (int layer = 0; layer < 4; ++layer)
        for (int z = 0; z < 8; ++z)
            for (int x = 0; x < 8; ++x)
            {
                BodySpec s;
                s.at = { -14.0f + 4.0f * static_cast<float>(x), 5.0f + 2.5f * static_cast<float>(layer),
                         -14.0f + 4.0f * static_cast<float>(z) };
                s.halfExtents = { 0.4f, 0.4f, 0.4f };
                s.mass = 200.0f;
                pond.add(s);
            }
    pond.start();

    int mostInOneStep = 0, mostOnTheWater = 0;
    for (int i = 0; i < 300; ++i)
    {
        pond.step();
        mostInOneStep  = std::max(mostInOneStep, pond.fresh());
        mostOnTheWater = std::max(mostOnTheWater, pond.rings().count());
    }
    CHECK(mostOnTheWater <= RippleField::kMax);
    CHECK(mostOnTheWater >= 4);                            // the rings did come, the test is not vacuous
    CHECK(mostInOneStep <= RippleTriggers::kMaxImpactsPerStep + RippleTriggers::kMaxTrailsPerStep);
    CHECK(mostInOneStep >= 2);                             // and several arrived in one step
}

TEST_CASE("Water ripples: a ring ages on the simulation's clock, in the physics step, water or not")
{
    // Raised from outside (a script's water.disturb will do exactly this) and aged by step().
    Pond pond(false);
    pond.start();
    pond.phys.waterRipples().add(ring(1.0f, 1.0f, 1.0f, WaterRippleScript));
    pond.step(60);
    REQUIRE(pond.rings().count() == 1);
    CHECK(pond.rings().at(0).age == doctest::Approx(1.0f).epsilon(0.001));
    CHECK(pond.phys.simTime() == doctest::Approx(1.0).epsilon(0.001));

    // The ring a landing raises ages the same way: one second of steps, one second of age.
    Pond lake;
    BodySpec spec;
    spec.at = { 4.0f, 6.0f, -3.0f };
    lake.add(spec);
    lake.start();
    int landed = -1;
    for (int i = 1; i <= 240 && landed < 0; ++i) { lake.step(); if (lake.rings().count() > 0) landed = i; }
    REQUIRE(landed > 0);
    lake.step(60);
    REQUIRE(lake.rings().count() >= 1);
    CHECK(lake.rings().at(0).age == doctest::Approx(1.0f).epsilon(0.001));

    // And a world that is cleared (a scene stopped or swapped) leaves none behind.
    lake.phys.clear();
    CHECK(lake.rings().count() == 0);
}

TEST_CASE("Water ripples: two runs of the same scene raise the same rings, byte for byte")
{
    const auto run = [] {
        Pond pond;
        for (int i = 0; i < 6; ++i)
        {
            BodySpec s;
            s.at = { -6.0f + 2.5f * static_cast<float>(i), 5.0f + 0.7f * static_cast<float>(i), 1.0f * static_cast<float>(i % 3) };
            s.mass = 150.0f + 20.0f * static_cast<float>(i);
            pond.add(s);
        }
        pond.start();
        pond.step(200);
        return pond.rings().pack();
    };
    const WaterRippleBlock a = run();
    const WaterRippleBlock b = run();
    CHECK(a.head[0] > 0.0f);                               // there is something to compare
    CHECK(sameBlock(a, b));
}
