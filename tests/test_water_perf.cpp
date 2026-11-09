#include "doctest.h"
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterObstacle.h>
#include <HorizonScene/WaterSpray.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ParticleSystem.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/ParticleSystemComponent.h>
#include <HorizonScene/Components/WaterObstacleComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/CommandBuffer.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <functional>
#include <vector>

// What the water's spray, its particle bands and the obstacles' foam cost on the CPU (Thema 232). Reported,
// not asserted (like the buoyancy cost case): the numbers are the release build's on the machine that runs it.
// Each figure is the MEDIAN of repeated runs of the same work.
//
//   * the fall's spray: particle simulation per tick, and building its bands in the extractor (vs. the same
//     world with the emitters hidden);
//   * the foam: one extract with 0 / 10 / 50 / 100 obstacles standing in a flowing lake, resting (their bands
//     kept) and all moving (every band built again);
//   * the spray's rebuild when the water changes (finding falls and feet, placing the emitters);
//   * the blended sort with the materials' priorities looked up.

using namespace HE;
namespace water = HE::water;
namespace brush = HE::water::brush;

namespace
{
    double medianMs(int reps, const std::function<void()>& f)
    {
        std::vector<double> t;
        t.reserve(static_cast<size_t>(reps));
        for (int i = 0; i < reps; ++i)
        {
            const auto a = std::chrono::steady_clock::now();
            f();
            t.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        }
        std::sort(t.begin(), t.end());
        return t[t.size() / 2];
    }

    void prepareContent(ContentManager& cm)
    {
#ifdef HE_EDITOR_DEPS_DIR
        cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
        cm.scanContentDirectory();
#else
        (void)cm;
#endif
    }

    // 64 x 64 m: a plateau 10 m up, a 2 m/m face from x = 0, the plain; a stream 8 m wide painted across the edge.
    TerrainComponent fallLand()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = true;
        tc.water.res = 128;
        tc.sculptHeights.resize(65u * 65u);
        for (uint32_t z = 0; z < 65; ++z)
            for (uint32_t x = 0; x < 65; ++x)
            {
                const float wx = -32.0f + static_cast<float>(x);
                tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 20.0f - std::clamp(2.0f * wx, 0.0f, 10.0f);
            }
        brush::Params p;
        p.radius = 4.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 0.3f;
        p.followSlope = true;
        brush::Stroke s;
        brush::begin(s, tc, -14.0f, 0.0f, p);
        for (int i = 0; i <= 120; ++i) brush::dab(s, tc, -14.0f + 40.0f * static_cast<float>(i) / 120.0f, 0.0f, p);
        brush::end(s, tc);
        return tc;
    }

    // 64 x 64 m, flat at 10, a lake 1 m deep over most of it, running at 1.2 m/s along +X.
    TerrainComponent lakeLand()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = true;
        tc.water.res = 128;
        tc.sculptHeights.assign(65u * 65u, 10.0f);
        brush::Params p;
        p.radius = 6.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 1.0f;
        brush::Stroke s;
        brush::begin(s, tc, 0.0f, 0.0f, p);
        for (int z = -24; z <= 24; z += 3)
            for (int x = -24; x <= 24; x += 3)
                brush::dab(s, tc, static_cast<float>(x), static_cast<float>(z), p);
        brush::end(s, tc);
        for (water::Body& b : tc.water.bodies) b.physics.flow = glm::vec2(1.2f, 0.0f);
        return tc;
    }
}

TEST_CASE("Water perf: the fall's spray — particle tick, band building, rebuild on a water change (reported, not asserted)")
{
    ContentManager cm{ "." };
    prepareContent(cm);
    HorizonWorld world;
    auto land = world.createEntity("Landscape");
    world.addComponent(land, TransformComponent{});
    world.addComponent(land, fallLand());
    HE::propagateTransforms(world);
    TerrainSystem::updateTerrains(world, cm);

    auto& reg = world.registry();
    size_t emitters = 0;
    for (auto [e, ws] : reg.view<WaterSurfaceComponent>().each()) emitters += ws.spray ? 1 : 0;
    REQUIRE(emitters > 0);

    // Fill: 3 s of 60 Hz ticks, then the steady state is what a running fall costs.
    for (int i = 0; i < 180; ++i) ParticleSystem::update(world, cm, 1.0f / 60.0f, nullptr);
    size_t particles = 0;
    for (auto [e, ps] : reg.view<ParticleSystemComponent>().each()) particles += ps.particles.size();
    const double tick = medianMs(60, [&] { ParticleSystem::update(world, cm, 1.0f / 60.0f, nullptr); });

    RenderExtractor ex;
    ex.setContentManager(&cm);
    RenderWorld rw;
    const double withSpray = medianMs(40, [&] { ex.extract(world, rw, 16.0f / 9.0f); });
    size_t bandQuads = 0;
    for (const RibbonBatch& rb : rw.ribbonBatches) bandQuads += rb.indices.size() / 6;
    for (auto [e, ps] : reg.view<ParticleSystemComponent>().each()) ps.visible = false;
    const double withoutSpray = medianMs(40, [&] { ex.extract(world, rw, 16.0f / 9.0f); });
    for (auto [e, ps] : reg.view<ParticleSystemComponent>().each()) ps.visible = true;

    // The rebuild: the water changes (a brush dab bumps the revision), the falls and feet are found again.
    TerrainComponent& live = reg.get<TerrainComponent>(land);
    const double rebuild = medianMs(10, [&] { ++live.water.revision; TerrainSystem::updateTerrains(world, cm); });

    MESSAGE("spray: " << emitters << " emitters, " << particles << " particles (" << bandQuads << " band quads)");
    MESSAGE("  particle tick " << tick << " ms  (" << (particles ? tick * 1000.0 / static_cast<double>(particles) : 0.0)
            << " us per particle)");
    MESSAGE("  extract with spray " << withSpray << " ms, without " << withoutSpray << " ms  -> bands "
            << (withSpray - withoutSpray) << " ms");
    MESSAGE("  rebuild on a water change " << rebuild << " ms (surface pass incl. falls/feet)");
    CHECK(particles > 1000u);
}

TEST_CASE("Water perf: obstacle foam per extract, 0 / 10 / 50 / 100 obstacles in a flowing lake (reported, not asserted)")
{
    ContentManager cm{ "." };
    prepareContent(cm);
    for (int count : { 0, 10, 50, 100 })
    {
        HorizonWorld world;
        auto land = world.createEntity("Landscape");
        world.addComponent(land, TransformComponent{});
        world.addComponent(land, lakeLand());
        // A grid of 1 x 2 x 1 m blocks standing through the surface (level 11), 3.5 m apart.
        for (int i = 0; i < count; ++i)
        {
            auto rock = world.createEntity("Rock");
            TransformComponent t;
            t.position = glm::vec3(-17.5f + 3.5f * static_cast<float>(i % 11), 11.0f, -17.5f + 3.5f * static_cast<float>(i / 11));
            t.scale    = glm::vec3(1.0f, 2.0f, 1.0f);
            world.addComponent(rock, t);
            world.addComponent(rock, MeshComponent{ HE::kDefaultCubeMeshId });
            world.addComponent(rock, WaterObstacleComponent{});
        }
        HE::propagateTransforms(world);
        TerrainSystem::updateTerrains(world, cm);
        RenderExtractor ex;
        ex.setContentManager(&cm);
        RenderWorld rw;
        ex.extract(world, rw, 16.0f / 9.0f);
        // Resting: every band is the one built before (the extractor keeps it while nothing under it moved).
        const double resting = medianMs(40, [&] { ex.extract(world, rw, 16.0f / 9.0f); });
        size_t bands = 0;
        for (const RibbonBatch& rb : rw.ribbonBatches) bands += rb.materialAssetId == HE::kEngineWaterFoamMaterialId ? 1 : 0;
        // Moving: every obstacle nudged before each extract (1 mm back and forth), so every band is built again —
        // what a frame costs while they all are dragged or simulated. Only the extract is timed.
        std::vector<double> moving;
        auto& reg = world.registry();
        for (int rep = 0; rep < 40; ++rep)
        {
            for (auto [e, t, wo] : reg.view<TransformComponent, WaterObstacleComponent>().each())
            {
                t.position.x += (rep % 2 == 0) ? 0.001f : -0.001f;
                t.dirty = true;
            }
            HE::propagateTransforms(world);
            const auto a = std::chrono::steady_clock::now();
            ex.extract(world, rw, 16.0f / 9.0f);
            moving.push_back(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count());
        }
        std::sort(moving.begin(), moving.end());
        // Control: the same blocks with their obstacle switched off — the extract of everything else (their meshes).
        for (auto [e, wo] : reg.view<WaterObstacleComponent>().each()) wo.enabled = false;
        ex.extract(world, rw, 16.0f / 9.0f);
        const double off = medianMs(40, [&] { ex.extract(world, rw, 16.0f / 9.0f); });
        MESSAGE("foam: " << count << " obstacles -> " << bands << " bands, extract resting " << resting
                << " ms, all moving " << moving[moving.size() / 2] << " ms, foam off " << off << " ms");
        CHECK(bands == static_cast<size_t>(count));
    }
}

TEST_CASE("Water perf: the blended sort with material priorities, 2000 draws (reported, not asserted)")
{
    ContentManager cm{ "." };
    MaterialAsset a; a.type = HE::AssetType::Material; a.name = "PerfA"; a.blendMode = 2;
    MaterialAsset b; b.type = HE::AssetType::Material; b.name = "PerfB"; b.blendMode = 2; b.sortPriority = 1;
    const HE::UUID ida = cm.registerMaterial(std::move(a));
    const HE::UUID idb = cm.registerMaterial(std::move(b));
    std::vector<DrawCall> dcs(2000);
    for (size_t i = 0; i < dcs.size(); ++i)
    {
        dcs[i].materialAssetId = (i % 3 == 0) ? idb : ida;
        dcs[i].transform = glm::translate(glm::mat4(1.0f), glm::vec3(static_cast<float>(i % 97), 0.0f, -static_cast<float>(i % 53)));
    }
    std::vector<const DrawCall*> v;
    auto fill = [&] { v.clear(); for (const DrawCall& d : dcs) v.push_back(&d); };
    const double plain = medianMs(50, [&] { fill(); RenderSorter::sortBackToFront(v, glm::vec3(0.0f)); });
    const double prio  = medianMs(50, [&] { fill(); RenderSorter::sortBackToFront(v, glm::vec3(0.0f), &cm); });
    MESSAGE("blended sort, 2000 draws: distance only " << plain << " ms, with material priorities " << prio << " ms");
    CHECK(prio < 50.0);
}
