#include "doctest.h"
#include <HorizonScene/WaterObstacle.h>
#include <HorizonScene/WaterBrush.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterQuery.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/WaterObstacleComponent.h>
#include <HorizonRendering/RenderWorld.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderSorter.h>
#include <HorizonRendering/CommandBuffer.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <ContentManager/Assets.h>
#include <MaterialGraph/MaterialGraph.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <Hpak/ReachFilter.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>

// A WATER OBSTACLE (WaterObstacleComponent, WaterObstacle.h): an entity standing in the landscape water. Where the
// water runs past it, foam gathers along its mesh bounds — a band around the footprint, longer downstream — drawn
// over the water. Still water: none.
//
// A flat 64 x 64 m landscape at y = 10 with a lake 1 m deep painted round the origin.

using namespace HE;

namespace
{
    TerrainComponent lakeLand()
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = 64.0f;
        tc.resolution = 65;
        tc.dirty = false;
        tc.water.res = 128;
        tc.sculptHeights.assign(65u * 65u, 10.0f);
        water::brush::Params p;
        p.radius = 6.0f; p.falloff = 1.0f; p.amount = 1.0f;
        p.levelFromGround = true; p.levelOffset = 1.0f;
        water::brush::Stroke s;
        water::brush::begin(s, tc, 0.0f, 0.0f, p);
        for (int z = -12; z <= 12; z += 3)
            for (int x = -12; x <= 12; x += 3)
                water::brush::dab(s, tc, static_cast<float>(x), static_cast<float>(z), p);
        water::brush::end(s, tc);
        return tc;
    }

    // The world: the lake landscape, flowing at `flow` (m/s, world X/Z) or still.
    void buildLake(HorizonWorld& world, const glm::vec2& flow)
    {
        auto land = world.createEntity("Landscape");
        world.addComponent(land, TransformComponent{});
        TerrainComponent tc = lakeLand();
        for (water::Body& b : tc.water.bodies) b.physics.flow = flow;
        world.addComponent(land, tc);
        HE::propagateTransforms(world);
    }

    // An upright 2 x 4 x 2 m box at (x, 11, z): through the lake's surface (level 11).
    water::ObstacleFootprint boxAt(float x, float z, float halfY = 2.0f)
    {
        const float lo[3] = { -1.0f, -halfY, -1.0f }, hi[3] = { 1.0f, halfY, 1.0f };
        return water::obstacleFootprint(glm::translate(glm::mat4(1.0f), glm::vec3(x, 11.0f, z)), lo, hi);
    }

    float reachAlong(const water::FoamBand& band, const glm::vec2& c, const glm::vec2& d)
    {
        float r = -1e9f;
        for (size_t v = 0; v + 8 <= band.vertices.size(); v += 8)
        {
            const glm::vec2 p(band.vertices[v], band.vertices[v + 2]);
            const float u = band.vertices[v + 6];
            if (u - std::floor(u) > 0.99f) r = std::max(r, glm::dot(p - c, d));   // the outer row
        }
        return r;
    }
}

TEST_CASE("Water obstacle: the footprint is the mesh bounds under the world transform, seen from above")
{
    const float lo[3] = { -0.5f, -0.5f, -0.5f }, hi[3] = { 0.5f, 0.5f, 0.5f };
    glm::mat4 m = glm::translate(glm::mat4(1.0f), glm::vec3(3.0f, 5.0f, -2.0f));
    m = glm::rotate(m, glm::radians(30.0f), glm::vec3(0, 1, 0));
    m = glm::scale(m, glm::vec3(4.0f, 2.0f, 1.0f));
    const water::ObstacleFootprint fp = water::obstacleFootprint(m, lo, hi);
    CHECK(fp.center.x == doctest::Approx(3.0f));
    CHECK(fp.center.y == doctest::Approx(-2.0f));
    CHECK(fp.minY == doctest::Approx(4.0f));
    CHECK(fp.maxY == doctest::Approx(6.0f));
    // The X axis (4 m wide, turned 30 degrees about Y) and the Z axis (1 m) are the footprint; the Y axis is up.
    const float la = glm::length(fp.halfA), lb = glm::length(fp.halfB);
    CHECK(std::max(la, lb) == doctest::Approx(2.0f));
    CHECK(std::min(la, lb) == doctest::Approx(0.5f));
}

TEST_CASE("Water obstacle: running water foams round it, longer downstream; still water, a sunk or a lifted box none")
{
    ContentManager cm{ "." };
    HorizonWorld running;
    buildLake(running, glm::vec2(1.5f, 0.0f));           // +X, full strength
    const water::WaterIndex idx = water::WaterIndex::build(running);
    WaterObstacleComponent c;

    water::FoamBand band;
    REQUIRE(water::obstacleFoam(idx, boxAt(0.0f, 0.0f), c, 0.0, band));
    CHECK_FALSE(band.indices.empty());
    CHECK(band.vertices.size() % 8 == 0u);
    CHECK(band.speed == doctest::Approx(1.5f).epsilon(0.05));
    CHECK(band.flowDir.x > 0.99f);
    CHECK(band.strength > 0.5f);
    CHECK(band.level == doctest::Approx(11.0f).epsilon(0.01));
    // On the water: every vertex at the water's level plus the lift.
    for (size_t v = 0; v + 8 <= band.vertices.size(); v += 8)
        CHECK(band.vertices[v + 1] == doctest::Approx(11.0f + water::kObstacleFoamLift).epsilon(0.01));
    // Where the water hits it the foam piles up: upstream (-X) the band reaches further out than at the sides (Z),
    // the pillow; downstream (+X) the wake trails furthest of all.
    const float down = reachAlong(band, glm::vec2(0.0f), glm::vec2(1.0f, 0.0f));
    const float up   = reachAlong(band, glm::vec2(0.0f), glm::vec2(-1.0f, 0.0f));
    const float side = reachAlong(band, glm::vec2(0.0f), glm::vec2(0.0f, 1.0f));
    MESSAGE("foam reaches " << down << " m downstream, " << up << " m upstream, " << side << " m to the side");
    CHECK(up - 1.0f > 1.5f * (side - 1.0f));      // the pillow is the widest part of the collar
    CHECK(up - 1.0f > c.margin);
    CHECK(down > up);                            // and the wake longer still
    CHECK(down > 1.0f + 2.0f);
    // Facing (uv.y): 1 on the upstream face, 0 straight downstream; the band's vertices know where they are.
    float vUp = -1.0f, vDown = 2.0f;
    for (size_t v = 0; v + 8 <= band.vertices.size(); v += 8)
    {
        const float x = band.vertices[v], z = band.vertices[v + 2];
        if (x < -0.9f && std::abs(z) < 0.2f) vUp = std::max(vUp, band.vertices[v + 7]);
        if (x >  0.9f && std::abs(z) < 0.2f) vDown = std::min(vDown, band.vertices[v + 7]);
    }
    CHECK(vUp > 0.95f);
    CHECK(vDown < 0.05f);
    // The strength rides in floor(uv.x).
    const float u0 = band.vertices[6];
    CHECK(std::floor(u0) / water::kObstacleFoamLevels == doctest::Approx(band.strength));
    // The normal leans the way the water runs AT the vertex: close to the upstream face the water is stopped (the
    // pillow's stagnation point, no lean), at the outer edge it is the free current (+X).
    float innerUpLean = 1e9f, outerLeanX = -1e9f;
    for (size_t v = 0; v + 8 <= band.vertices.size(); v += 8)
    {
        const float u = band.vertices[v + 6], sRing = u - std::floor(u);
        const float leanX = band.vertices[v + 3] / band.vertices[v + 4] / water::kObstacleFoamFlowGain;
        const float leanZ = band.vertices[v + 5] / band.vertices[v + 4] / water::kObstacleFoamFlowGain;
        if (sRing < 0.01f && band.vertices[v + 7] > 0.98f) innerUpLean = std::min(innerUpLean, std::hypot(leanX, leanZ));
        if (sRing > 0.99f) outerLeanX = std::max(outerLeanX, leanX);
    }
    CHECK(innerUpLean < 0.15f);
    CHECK(outerLeanX == doctest::Approx(1.0f).epsilon(0.05));

    // Negative controls.
    water::FoamBand none;
    CHECK(water::obstacleFoam(idx, boxAt(0.0f, 0.0f, 0.4f), c, 0.0, none));   // a low box (10.6..11.4) still parts it
    const float lo[3] = { -1.0f, -0.5f, -1.0f }, hi[3] = { 1.0f, 0.5f, 1.0f };
    CHECK_FALSE(water::obstacleFoam(idx, water::obstacleFootprint(glm::translate(glm::mat4(1.0f), glm::vec3(0, 10.4f, 0)), lo, hi), c, 0.0, none));   // under water
    CHECK_FALSE(water::obstacleFoam(idx, water::obstacleFootprint(glm::translate(glm::mat4(1.0f), glm::vec3(0, 12.0f, 0)), lo, hi), c, 0.0, none));   // above it
    CHECK_FALSE(water::obstacleFoam(idx, boxAt(28.0f, 28.0f), c, 0.0, none));          // on dry land
    WaterObstacleComponent off; off.enabled = false;
    CHECK_FALSE(water::obstacleFoam(idx, boxAt(0.0f, 0.0f), off, 0.0, none));
    HorizonWorld still;
    buildLake(still, glm::vec2(0.0f));
    const water::WaterIndex stillIdx = water::WaterIndex::build(still);
    CHECK_FALSE(water::obstacleFoam(stillIdx, boxAt(0.0f, 0.0f), c, 0.0, none));         // still water: no foam
}

TEST_CASE("Water obstacle: the extractor draws the foam as a band on the water layer, and it follows the obstacle")
{
    ContentManager cm{ "." };
    HorizonWorld world;
    buildLake(world, glm::vec2(0.0f, 1.2f));             // +Z
    auto rock = world.createEntity("Rock");
    TransformComponent t;
    t.position = glm::vec3(-3.0f, 11.0f, 0.0f);
    t.scale    = glm::vec3(1.5f, 3.0f, 1.5f);
    world.addComponent(rock, t);
    world.addComponent(rock, MeshComponent{ HE::kDefaultCubeMeshId });
    world.addComponent(rock, WaterObstacleComponent{});
    HE::propagateTransforms(world);
    REQUIRE(cm.getStaticMesh(HE::kDefaultCubeMeshId) != nullptr);

    RenderExtractor extractor;
    extractor.setContentManager(&cm);
    auto foamOf = [&](RenderWorld& rw) -> const RibbonBatch* {
        for (const RibbonBatch& rb : rw.ribbonBatches)
            if (rb.materialAssetId == HE::kEngineWaterFoamMaterialId) return &rb;
        return nullptr;
    };
    RenderWorld rw;
    extractor.extract(world, rw, 16.0f / 9.0f);
    const RibbonBatch* foam = foamOf(rw);
    REQUIRE(foam != nullptr);
    CHECK(foam->entityId == static_cast<uint32_t>(entt::entity(rock)));
    const glm::vec3 c0 = foam->worldBounds.center();
    CHECK(c0.x == doctest::Approx(-3.0f).epsilon(0.1));
    CHECK(c0.z > 0.5f);                                   // the wake drags the band's middle downstream (+Z)

    // Moved: the next extract has the foam where the rock is now, nothing else had to be told.
    world.registry().get<TransformComponent>(rock).position.x = 4.0f;
    world.registry().get<TransformComponent>(rock).dirty = true;
    HE::propagateTransforms(world);
    RenderWorld rw2;
    extractor.extract(world, rw2, 16.0f / 9.0f);
    foam = foamOf(rw2);
    REQUIRE(foam != nullptr);
    CHECK(foam->worldBounds.center().x == doctest::Approx(4.0f).epsilon(0.1));

    // The band is drawn with whatever material the component names: an ordinary asset reference.
    const HE::UUID mine{ 0x0B57AC1EULL, 7 };
    world.registry().get<WaterObstacleComponent>(rock).foamMaterial = mine;
    RenderWorld rwMine;
    extractor.extract(world, rwMine, 16.0f / 9.0f);
    bool drawnWithMine = false;
    for (const RibbonBatch& rb : rwMine.ribbonBatches) drawnWithMine = drawnWithMine || rb.materialAssetId == mine;
    CHECK(drawnWithMine);
    CHECK(foamOf(rwMine) == nullptr);
    world.registry().get<WaterObstacleComponent>(rock).foamMaterial = HE::kEngineWaterFoamMaterialId;

    // Switched off: gone.
    world.registry().get<WaterObstacleComponent>(rock).enabled = false;
    RenderWorld rw3;
    extractor.extract(world, rw3, 16.0f / 9.0f);
    CHECK(foamOf(rw3) == nullptr);
}

TEST_CASE("Water obstacle: the extractor keeps a resting obstacle's band and builds it again when anything under it changes")
{
    // The extractor remembers each band with what it was built from (the obstacle's place, bounds and settings, the
    // water's revision and place). Whatever changes, the band it draws must be the one an extractor that remembers
    // nothing draws — and differ from the band before, or the change was not one that matters.
    ContentManager cm{ "." };
    HorizonWorld world;
    buildLake(world, glm::vec2(1.2f, 0.0f));             // +X
    auto& reg = world.registry();
    auto rock = world.createEntity("Rock");
    TransformComponent t;
    t.position = glm::vec3(0.0f, 11.0f, 0.0f);
    t.scale    = glm::vec3(1.5f, 3.0f, 1.5f);
    world.addComponent(rock, t);
    world.addComponent(rock, MeshComponent{ HE::kDefaultCubeMeshId });
    world.addComponent(rock, WaterObstacleComponent{});
    HE::propagateTransforms(world);

    auto foamOf = [&](RenderExtractor& ex) {
        RenderWorld rw;
        ex.extract(world, rw, 16.0f / 9.0f);
        for (const RibbonBatch& rb : rw.ribbonBatches)
            if (rb.materialAssetId == HE::kEngineWaterFoamMaterialId) return rb.vertices;
        return std::vector<float>{};
    };
    auto fresh = [&] { RenderExtractor ex; ex.setContentManager(&cm); return foamOf(ex); };
    RenderExtractor kept;
    kept.setContentManager(&cm);
    std::vector<float> last = foamOf(kept);
    REQUIRE_FALSE(last.empty());
    CHECK(foamOf(kept) == last);                          // resting: the same band
    auto changed = [&](const char* what) {
        const std::vector<float> now = foamOf(kept);
        const bool sameAsFresh = now == fresh();
        const bool differs     = !(now == last);
        CHECK_MESSAGE(sameAsFresh, std::string(what));
        CHECK_MESSAGE(differs, std::string(what));
        last = now;
    };

    reg.get<TransformComponent>(rock).position.z = 1.0f;
    reg.get<TransformComponent>(rock).dirty = true;
    HE::propagateTransforms(world);
    changed("moved");

    reg.get<WaterObstacleComponent>(rock).margin = 1.4f;
    changed("margin");
    reg.get<WaterObstacleComponent>(rock).foam = 0.5f;
    changed("foam");

    entt::entity land = entt::null;
    for (auto [e, tc] : reg.view<TerrainComponent>().each()) land = e;
    REQUIRE(reg.valid(land));
    for (const water::Body& b : std::vector<water::Body>(reg.get<TerrainComponent>(land).water.bodies))
    {
        water::BodyPhysics p = b.physics;
        p.flow = glm::vec2(0.0f, 1.2f);                  // now along +Z
        CHECK(water::setPhysics(reg.get<TerrainComponent>(land), b.id, p));
    }
    changed("current turned");

    // The ground raised out of the water under the wake (z 2..10 m): the band's vertices there are over dry land now.
    {
        TerrainComponent& tc = reg.get<TerrainComponent>(land);
        for (uint32_t z = 34; z <= 42; ++z)
            for (uint32_t x = 0; x < 65; ++x) tc.sculptHeights[static_cast<size_t>(z) * 65 + x] = 13.0f;
        tc.dirty = true;
    }
    TerrainSystem::updateTerrains(world, cm);
    changed("ground raised");
}

TEST_CASE("Blended sort: a material's sort priority orders the blended draws first, then back to front")
{
    // Two ordinary materials: one at the default priority (the lake), one asking to come later (foam on it).
    ContentManager cm{ "." };
    MaterialAsset water; water.type = HE::AssetType::Material; water.name = "SortWater"; water.blendMode = 2;
    MaterialAsset foamM; foamM.type = HE::AssetType::Material; foamM.name = "SortFoam";  foamM.blendMode = 2;
    foamM.sortPriority = 1;
    const HE::UUID waterId = cm.registerMaterial(std::move(water));
    const HE::UUID foamId  = cm.registerMaterial(std::move(foamM));
    CHECK(RenderSorter::materialSortPriority(&cm, foamId) == 1);
    CHECK(RenderSorter::materialSortPriority(&cm, waterId) == 0);
    CHECK(RenderSorter::materialSortPriority(&cm, HE::UUID{}) == 0);

    DrawCall lake, foamFar, foamNear, glass;
    lake.materialAssetId = waterId;
    lake.transform     = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -2));    // the lake's centre: near the camera
    foamFar.materialAssetId = foamId;
    foamFar.transform  = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -80));   // a rock at the far shore
    foamNear.materialAssetId = foamId;
    foamNear.transform = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -10));
    glass.transform    = glm::translate(glm::mat4(1.0f), glm::vec3(0, 0, -40));   // no material: priority 0
    std::vector<const DrawCall*> v = { &foamNear, &lake, &foamFar, &glass };
    RenderSorter::sortBackToFront(v, glm::vec3(0.0f), &cm);
    REQUIRE(v.size() == 4u);
    CHECK(v[0] == &glass);      // priority 0, far to near
    CHECK(v[1] == &lake);
    CHECK(v[2] == &foamFar);    // then priority 1, far to near
    CHECK(v[3] == &foamNear);
    // Without a content manager it is the plain distance order (every backend's old behaviour).
    std::vector<const DrawCall*> w = { &foamNear, &lake, &foamFar, &glass };
    RenderSorter::sortBackToFront(w, glm::vec3(0.0f));
    CHECK(w[0] == &foamFar);
    CHECK(w[3] == &lake);
}

TEST_CASE("Material nodes for generated surfaces: Water Surface, Particle and Foam Band decode in the codegen")
{
    // Each node wired to the Output and generated: the shader reads the varyings and nothing the graph had to spell
    // out — the conventions live in one place (MaterialGraph.cpp), with the numbers the meshes are written with.
    auto glslOf = [](MatNodeType t, int pin, int outPin)
    {
        MaterialGraph g;
        const int out = g.addNode(MatNodeType::Output);
        const int n   = g.addNode(t);
        g.findNode(out)->p[1] = static_cast<float>(MatBlendMode::Translucent);
        REQUIRE(g.connect(n, pin, out, outPin));
        return generateFragment(g).glsl;
    };
    const std::string ws = glslOf(MatNodeType::WaterSurfaceData, 0, kMatOutputOpacityPin);
    CHECK(ws.find("normalize(vNormal)") != std::string::npos);
    const std::string pa = glslOf(MatNodeType::ParticleData, 1, kMatOutputOpacityPin);
    CHECK(pa.find("fract(vUV)") != std::string::npos);
    CHECK(pa.find("64.0") != std::string::npos);             // kParticleBandLevels
    const std::string fb = glslOf(MatNodeType::FoamBandData, 2, kMatOutputOpacityPin);
    CHECK(fb.find("vUV.y * 2.0 - 1.0") != std::string::npos);
    // The engine materials read them.
    for (const char* name : { "Water Surface", "Particle", "Foam Band" })
        CHECK(matNodeDescByName(name) != nullptr);
}

TEST_CASE("Material sort priority: set on the Output node, baked by the codegen, kept by the .hasset")
{
    MaterialGraph g;
    const int out = g.addNode(MatNodeType::Output);
    const int col = g.addNode(MatNodeType::ConstColor);
    g.connect(col, 0, out, kMatOutputBaseColorPin);
    g.findNode(out)->p[1] = static_cast<float>(MatBlendMode::Translucent);
    g.findNode(out)->sortPriority = 3;
    MaterialGraph back;
    REQUIRE(materialGraphFromJson(materialGraphToJson(g), back));
    CHECK(back.findNode(out)->sortPriority == 3);
    CHECK(generateFragment(back).sortPriority == 3);
    // Out of range is clamped, and a graph saved without the key reads 0.
    g.findNode(out)->sortPriority = 99;
    REQUIRE(materialGraphFromJson(materialGraphToJson(g), back));
    CHECK(back.findNode(out)->sortPriority == kMatMaxSortPriority);
    g.findNode(out)->sortPriority = 0;
    CHECK(materialGraphToJson(g).find("sortPriority") == std::string::npos);

    // Through a file.
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "he_sortprio_test";
    std::filesystem::create_directories(dir);
    {
        ContentManager cm{ dir.string() };
        MaterialAsset a;
        a.type = HE::AssetType::Material; a.name = "Prio"; a.path = "Prio.hasset";
        a.id = HE::UUID{ 0x5021ULL, 1 };
        a.blendMode = 2; a.sortPriority = -2;
        REQUIRE(cm.saveAsset(a));
    }
    {
        ContentManager cm{ dir.string() };
        const HE::UUID id = cm.loadAsset("Prio.hasset");
        const MaterialAsset* m = cm.getMaterial(id);
        REQUIRE(m != nullptr);
        CHECK(m->sortPriority == -2);
    }
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
}

#ifdef HE_EDITOR_DEPS_DIR
TEST_CASE("Export: an untouched landscape water and obstacle reach their engine assets by reference, no fixed list")
{
    // The scene as the export ships it (re-serialized), with nothing set by hand: the defaults are references too.
    HorizonWorld world;
    buildLake(world, glm::vec2(1.2f, 0.0f));
    auto rock = world.createEntity("Rock");
    world.addComponent(rock, TransformComponent{});
    world.addComponent(rock, WaterObstacleComponent{});
    SceneSerializer ser;
    std::vector<uint8_t> scene;
    REQUIRE(ser.saveToMemory(world, scene));

    // The engine's files as nodes, plus one nothing names.
    const std::filesystem::path root = std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent";
    struct File { const char* path; HE::UUID id; uint16_t type; std::vector<uint8_t> bytes; };
    std::vector<File> files = {
        { "Materials/Water.hasset",        HE::kEngineWaterMaterialId,       static_cast<uint16_t>(HE::AssetType::Material), {} },
        { "Materials/WaterFoam.hasset",    HE::kEngineWaterFoamMaterialId,   static_cast<uint16_t>(HE::AssetType::Material), {} },
        { "Materials/WaterSpray.hasset",   HE::kEngineWaterSprayMaterialId,  static_cast<uint16_t>(HE::AssetType::Material), {} },
        { "Particles/WaterCurtain.hasset", HE::kEngineWaterCurtainParticlesId, static_cast<uint16_t>(HE::AssetType::ParticleSystem), {} },
        { "Particles/WaterDrops.hasset",   HE::kEngineWaterDropsParticlesId,   static_cast<uint16_t>(HE::AssetType::ParticleSystem), {} },
        { "Particles/WaterMist.hasset",    HE::kEngineWaterMistParticlesId,    static_cast<uint16_t>(HE::AssetType::ParticleSystem), {} },
        { "Materials/DisplacementDemo.hasset", HE::UUID{ 0x601, 1 },         static_cast<uint16_t>(HE::AssetType::Material), {} },
    };
    std::vector<reach::Node> nodes;
    for (File& f : files)
    {
        std::ifstream in(root / f.path, std::ios::binary);
        REQUIRE(in.good());
        f.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        reach::Node n;
        n.id = f.id; n.path = std::string("Engine/") + f.path; n.type = f.type;
        n.data = f.bytes.data(); n.size = f.bytes.size();
        nodes.push_back(n);
    }
    reach::Roots roots = reach::engineImplicitRoots();
    roots.blobs.push_back({ scene.data(), scene.size() });
    const reach::Result r = reach::computeReachable(nodes, roots);
    for (size_t i = 0; i + 1 < files.size(); ++i)
    {
        INFO(files[i].path);
        CHECK(r.reachable[i]);   // the water material, the foam, the falls' particle graphs and through them the spray
    }
    CHECK_FALSE(r.reachable.back());   // the control: an engine asset nothing names stays out
}
#endif

TEST_CASE("Water obstacle: the component is saved and loaded with the scene, an old scene without it still loads")
{
    HorizonWorld a;
    auto e = a.createEntity("Rock");
    a.addComponent(e, TransformComponent{});
    WaterObstacleComponent wo;
    wo.enabled = false; wo.foam = 2.5f; wo.margin = 1.25f; wo.wake = 7.5f;
    a.addComponent(e, wo);
    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(a, bytes));
    HorizonWorld b;
    REQUIRE(ser.loadFromMemory(b, bytes));
    bool found = false;
    for (auto [be, bwo] : b.registry().view<WaterObstacleComponent>().each())
    {
        found = true;
        CHECK_FALSE(bwo.enabled);
        CHECK(bwo.foam == doctest::Approx(2.5f));
        CHECK(bwo.margin == doctest::Approx(1.25f));
        CHECK(bwo.wake == doctest::Approx(7.5f));
    }
    CHECK(found);
    CHECK(SceneSerializer::isKnownComponentKey("waterObstacle"));
}
