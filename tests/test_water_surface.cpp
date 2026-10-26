#include "doctest.h"
#include <HorizonScene/WaterMesh.h>
#include <HorizonScene/WaterField.h>
#include <HorizonScene/WaterSurface.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/TerrainSystem.h>
#include <HorizonScene/TerrainSculpt.h>
#include <HorizonScene/TerrainMeshGenerator.h>
#include <HorizonScene/NavigationSystem.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/Components/RigidBodyComponent.h>
#include <HorizonScene/Components/ColliderComponent.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/Components/TerrainChunkComponent.h>
#include <HorizonScene/Components/WaterSurfaceComponent.h>
#include <HorizonScene/Components/MeshComponent.h>
#include <HorizonScene/Components/MaterialComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/NameComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/NavMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/DefaultAssets.h>
#include <Physics/WaterWaves.h>
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>
#include <string>

namespace water = HE::water;

// The water surface (WaterMesh.h, WaterSurface.h): triangulation of concave shapes
// and shapes with holes, contours out of the field, the mesh of a lake, and the
// entities that carry it. Terrain 64 × 64 m at 64 cells unless a test says
// otherwise, so a cell is a metre and a cell-aligned shape rasterises to 0 and 255.

namespace
{
    using Ring = water::Ring;

    Ring ring(std::initializer_list<std::pair<double, double>> pts)
    {
        Ring r;
        for (const auto& p : pts) r.emplace_back(p.first, p.second);
        return r;
    }

    // ── Triangles of a triangulate() result and of a mesh ────────────────────
    struct Tri { glm::dvec2 a, b, c; };

    double triArea(const Tri& t)
    {
        return 0.5 * ((t.b.x - t.a.x) * (t.c.y - t.a.y) - (t.b.y - t.a.y) * (t.c.x - t.a.x));
    }

    std::vector<Tri> trisOf(const std::vector<Ring>& rings, const std::vector<uint32_t>& idx)
    {
        std::vector<glm::dvec2> all;
        for (const Ring& r : rings) all.insert(all.end(), r.begin(), r.end());
        std::vector<Tri> out;
        for (size_t i = 0; i + 2 < idx.size(); i += 3)
            out.push_back({ all[idx[i]], all[idx[i + 1]], all[idx[i + 2]] });
        return out;
    }

    // The XZ triangles of a mesh, in terrain-local space (vertices + centre).
    std::vector<Tri> trisOf(const StaticMeshAsset& m, glm::vec2 center = { 0.0f, 0.0f })
    {
        std::vector<Tri> out;
        auto v = [&](uint32_t i) {
            return glm::dvec2(m.vertices[i * 3] + center.x, m.vertices[i * 3 + 2] + center.y);
        };
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
            out.push_back({ v(m.indices[i]), v(m.indices[i + 1]), v(m.indices[i + 2]) });
        return out;
    }

    // Do the interiors of two triangles overlap? Separating-axis test; touching
    // along an edge or at a point is not an overlap.
    bool interiorsOverlap(const Tri& A, const Tri& B, double eps)
    {
        const glm::dvec2 pa[3] = { A.a, A.b, A.c };
        const glm::dvec2 pb[3] = { B.a, B.b, B.c };
        auto separated = [&](const glm::dvec2 (&p)[3]) {
            for (int e = 0; e < 3; ++e)
            {
                const glm::dvec2 d = p[(e + 1) % 3] - p[e];
                const glm::dvec2 n(-d.y, d.x);
                const double len = std::sqrt(n.x * n.x + n.y * n.y);
                if (len == 0.0) continue;
                double minA = 1e300, maxA = -1e300, minB = 1e300, maxB = -1e300;
                for (const auto& q : pa) { const double s = (n.x * q.x + n.y * q.y) / len; minA = std::min(minA, s); maxA = std::max(maxA, s); }
                for (const auto& q : pb) { const double s = (n.x * q.x + n.y * q.y) / len; minB = std::min(minB, s); maxB = std::max(maxB, s); }
                if (maxA <= minB + eps || maxB <= minA + eps) return true;
            }
            return false;
        };
        return !(separated(pa) || separated(pb));
    }

    bool anyOverlap(const std::vector<Tri>& tris, double eps = 1e-9)
    {
        for (size_t i = 0; i < tris.size(); ++i)
        {
            const double ax0 = std::min({ tris[i].a.x, tris[i].b.x, tris[i].c.x }), ax1 = std::max({ tris[i].a.x, tris[i].b.x, tris[i].c.x });
            const double az0 = std::min({ tris[i].a.y, tris[i].b.y, tris[i].c.y }), az1 = std::max({ tris[i].a.y, tris[i].b.y, tris[i].c.y });
            for (size_t j = i + 1; j < tris.size(); ++j)
            {
                const double bx0 = std::min({ tris[j].a.x, tris[j].b.x, tris[j].c.x }), bx1 = std::max({ tris[j].a.x, tris[j].b.x, tris[j].c.x });
                const double bz0 = std::min({ tris[j].a.y, tris[j].b.y, tris[j].c.y }), bz1 = std::max({ tris[j].a.y, tris[j].b.y, tris[j].c.y });
                if (ax1 <= bx0 + eps || bx1 <= ax0 + eps || az1 <= bz0 + eps || bz1 <= az0 + eps) continue;
                if (interiorsOverlap(tris[i], tris[j], eps)) return true;
            }
        }
        return false;
    }

    double sumArea(const std::vector<Tri>& tris)
    {
        double a = 0.0;
        for (const Tri& t : tris) a += std::abs(triArea(t));
        return a;
    }

    // Every triangle faces +Y (clockwise in x/z with z up, the terrain's winding).
    bool allFaceUp(const StaticMeshAsset& m)
    {
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        {
            const float* a = &m.vertices[m.indices[i] * 3];
            const float* b = &m.vertices[m.indices[i + 1] * 3];
            const float* c = &m.vertices[m.indices[i + 2] * 3];
            const double ny = (double(b[2]) - a[2]) * (double(c[0]) - a[0]) - (double(b[0]) - a[0]) * (double(c[2]) - a[2]);
            if (!(ny > 0.0)) return false;
        }
        return true;
    }

    bool pointInTri(const Tri& t, glm::dvec2 p)
    {
        auto s = [](glm::dvec2 a, glm::dvec2 b, glm::dvec2 c) { return (a.x - c.x) * (b.y - c.y) - (b.x - c.x) * (a.y - c.y); };
        const double d1 = s(p, t.a, t.b), d2 = s(p, t.b, t.c), d3 = s(p, t.c, t.a);
        const bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
        return !(neg && pos);
    }

    bool covered(const std::vector<Tri>& tris, glm::dvec2 p)
    {
        for (const Tri& t : tris) if (pointInTri(t, p)) return true;
        return false;
    }

    // ── Shapes ───────────────────────────────────────────────────────────────
    Ring starRing(int points, double outer, double inner, glm::dvec2 c = { 0, 0 })
    {
        Ring r;
        for (int i = 0; i < points * 2; ++i)
        {
            const double a = 3.14159265358979323846 * i / points + 0.3;
            const double rad = (i & 1) ? inner : outer;
            r.emplace_back(c.x + rad * std::cos(a), c.y + rad * std::sin(a));
        }
        return r;
    }

    Ring lRing() { return ring({ { 0, 0 }, { 6, 0 }, { 6, 2 }, { 2, 2 }, { 2, 6 }, { 0, 6 } }); } // area 20

    // ── Terrain with water ───────────────────────────────────────────────────
    TerrainComponent makeTerrain(float size = 64.0f, uint32_t res = 64)
    {
        TerrainComponent tc;
        tc.sizeX = tc.sizeZ = size;
        tc.water.res = res;
        return tc;
    }

    std::vector<glm::vec2> rect(float x0, float z0, float x1, float z1)
    {
        return { { x0, z0 }, { x1, z0 }, { x1, z1 }, { x0, z1 } };
    }

    std::vector<glm::vec2> toVec2(const Ring& r)
    {
        std::vector<glm::vec2> out;
        for (const auto& p : r) out.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        return out;
    }

    // A lake of `poly` at `level`; returns the body id.
    uint16_t addLake(TerrainComponent& tc, const std::vector<glm::vec2>& poly, float level = 0.0f)
    {
        const uint16_t id = tc.water.createBody(level);
        REQUIRE(water::addPolygon(tc, id, poly).ok);
        return id;
    }

    double polyArea(const std::vector<glm::vec2>& p)
    {
        double a = 0;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const glm::vec2& u = p[i]; const glm::vec2& v = p[(i + 1) % p.size()];
            a += double(u.x) * v.y - double(v.x) * u.y;
        }
        return std::abs(a) * 0.5;
    }

    // The mesh of a body, with its centre applied so the triangles are terrain-local.
    struct Built
    {
        water::SurfaceMesh s;
        std::vector<Tri> tris;
    };
    Built build(const TerrainComponent& tc, uint16_t id, water::SurfaceOptions opt = {})
    {
        Built b;
        REQUIRE(water::buildSurface(tc, id, opt, b.s));
        b.tris = trisOf(b.s.mesh, b.s.center);
        return b;
    }

    // ── A world with one landscape ───────────────────────────────────────────
    struct Rig
    {
        HorizonWorld world;
        ContentManager cm{ "." };
        Entity terrain = entt::null;

        Rig()
        {
            auto& reg = world.registry();
            terrain = world.createEntity("Landscape");
            reg.emplace<TransformComponent>(terrain);
            TerrainComponent tc = makeTerrain();
            tc.resolution = 9;
            tc.dirty = true;
            reg.emplace<TerrainComponent>(terrain, tc);
        }
        TerrainComponent& tc() { return world.registry().get<TerrainComponent>(terrain); }
        void update() { TerrainSystem::updateTerrains(world, cm); }

        std::vector<Entity> surfaces()
        {
            std::vector<Entity> out;
            for (auto e : world.registry().view<WaterSurfaceComponent>()) out.push_back(e);
            return out;
        }
        Entity surfaceOf(uint16_t body)
        {
            for (auto [e, ws] : world.registry().view<WaterSurfaceComponent>().each())
                if (ws.body == body) return e;
            return entt::null;
        }
    };
}

// ═════════════════════════════════════════════════════════════════════════════
// Triangulation
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water triangulate: a square is two triangles that cover it")
{
    const std::vector<Ring> rings = { ring({ { 0, 0 }, { 4, 0 }, { 4, 3 }, { 0, 3 } }) };
    const auto idx = water::triangulate(rings);
    REQUIRE(idx.size() == 6);
    const auto tris = trisOf(rings, idx);
    CHECK(sumArea(tris) == doctest::Approx(12.0));
    CHECK_FALSE(anyOverlap(tris));
}

TEST_CASE("Water triangulate: an L-shaped polygon, either winding")
{
    for (const bool reversed : { false, true })
    {
        Ring l = lRing();
        if (reversed) std::reverse(l.begin(), l.end());
        const std::vector<Ring> rings = { l };
        const auto idx = water::triangulate(rings);
        REQUIRE(idx.size() == (6 - 2) * 3);                    // n − 2 triangles
        const auto tris = trisOf(rings, idx);
        CHECK(sumArea(tris) == doctest::Approx(20.0));         // the mesh is the polygon
        CHECK_FALSE(anyOverlap(tris));
        // Nothing of the notch is covered: its middle is outside the L.
        CHECK_FALSE(covered(tris, { 4.0, 4.0 }));
        CHECK(covered(tris, { 1.0, 4.0 }));
        CHECK(covered(tris, { 4.0, 1.0 }));
    }
}

TEST_CASE("Water triangulate: a star-shaped polygon")
{
    for (const int points : { 5, 7, 12 })
    {
        const Ring star = starRing(points, 10.0, 3.0);
        const std::vector<Ring> rings = { star };
        const auto idx = water::triangulate(rings);
        REQUIRE(idx.size() == (star.size() - 2) * 3);
        const auto tris = trisOf(rings, idx);
        CHECK(sumArea(tris) == doctest::Approx(std::abs(water::signedArea(star))).epsilon(1e-9));
        CHECK_FALSE(anyOverlap(tris));
        for (const Tri& t : tris) CHECK(triArea(t) > 0.0);      // counter-clockwise, none flat
        // The notch between two points: along the first inner vertex's direction,
        // past that vertex it is outside, short of it inside.
        const double a = 3.14159265358979323846 / points + 0.3;
        CHECK_FALSE(covered(tris, { 5.0 * std::cos(a), 5.0 * std::sin(a) }));
        CHECK(covered(tris, { 2.0 * std::cos(a), 2.0 * std::sin(a) }));
    }
}

TEST_CASE("Water triangulate: holes are left open")
{
    // 10 × 10 with a 4 × 4 island-shaped hole, and with two holes.
    const Ring outer = ring({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 0, 10 } });
    const Ring hole1 = ring({ { 3, 3 }, { 3, 7 }, { 7, 7 }, { 7, 3 } });                   // clockwise
    const Ring holeA = ring({ { 1, 1 }, { 3, 1 }, { 3, 3 }, { 1, 3 } });                   // counter-clockwise: winding is the caller's business
    const Ring holeB = ring({ { 6, 6 }, { 9, 6 }, { 9, 9 }, { 6, 9 } });

    {
        const std::vector<Ring> rings = { outer, hole1 };
        const auto idx = water::triangulate(rings);
        REQUIRE(idx.size() == (4 + 4 + 2 * 1 - 2) * 3);         // V + 2H − 2
        const auto tris = trisOf(rings, idx);
        CHECK(sumArea(tris) == doctest::Approx(100.0 - 16.0));
        CHECK_FALSE(anyOverlap(tris));
        CHECK_FALSE(covered(tris, { 5.0, 5.0 }));
        CHECK(covered(tris, { 1.5, 5.0 }));
    }
    {
        const std::vector<Ring> rings = { outer, holeA, holeB };
        const auto idx = water::triangulate(rings);
        REQUIRE(idx.size() == (4 + 4 + 4 + 2 * 2 - 2) * 3);
        const auto tris = trisOf(rings, idx);
        CHECK(sumArea(tris) == doctest::Approx(100.0 - 4.0 - 9.0));
        CHECK_FALSE(anyOverlap(tris));
        CHECK_FALSE(covered(tris, { 2.0, 2.0 }));
        CHECK_FALSE(covered(tris, { 7.5, 7.5 }));
        CHECK(covered(tris, { 5.0, 5.0 }));
    }
}

TEST_CASE("Water triangulate: a hole in one arm of a U needs a bridge that sees it")
{
    // The U's notch is between x = 4 and 6; the hole sits in the left arm.
    const Ring u = ring({ { 0, 0 }, { 10, 0 }, { 10, 10 }, { 6, 10 }, { 6, 3 }, { 4, 3 }, { 4, 10 }, { 0, 10 } });
    const Ring hole = ring({ { 1, 5 }, { 1, 8 }, { 3, 8 }, { 3, 5 } });
    const std::vector<Ring> rings = { u, hole };
    const auto idx = water::triangulate(rings);
    const auto tris = trisOf(rings, idx);
    CHECK(sumArea(tris) == doctest::Approx(std::abs(water::signedArea(u)) - 6.0));
    CHECK_FALSE(anyOverlap(tris));
    CHECK_FALSE(covered(tris, { 2.0, 6.5 }));
    CHECK_FALSE(covered(tris, { 5.0, 6.0 }));       // the notch
    CHECK(covered(tris, { 2.0, 2.0 }));
    CHECK(covered(tris, { 8.0, 6.0 }));
}

TEST_CASE("Water triangulate: random simple polygons with and without a hole")
{
    std::mt19937 rng(20261009);
    std::uniform_real_distribution<double> u01(0.0, 1.0);
    for (int trial = 0; trial < 300; ++trial)
    {
        const int n = 3 + static_cast<int>(u01(rng) * 40);
        const glm::dvec2 c(u01(rng) * 1000.0 - 500.0, u01(rng) * 1000.0 - 500.0);   // far from the origin
        // One vertex per sector of 2π/n, jittered inside it: every gap between
        // neighbours stays under π, so the polygon is star-shaped around c and
        // simple (with random angles, a gap over π makes an edge cut across the middle).
        const double jitter = std::min(0.9, n * 0.5 - 1.05);
        std::vector<double> angles(n);
        for (int i = 0; i < n; ++i) angles[i] = 6.28318530717958647692 * (i + jitter * u01(rng)) / n;
        Ring outer;
        double minR = 1e9;
        for (const double a : angles)
        {
            const double r = 20.0 * (0.3 + 0.7 * u01(rng));
            minR = std::min(minR, r);
            outer.emplace_back(c.x + r * std::cos(a), c.y + r * std::sin(a));
        }
        if (std::abs(water::signedArea(outer)) < 1.0) continue;
        // How much room c has: the shortest distance to an edge. A hole is sized to fit.
        double clearance = 1e9;
        for (size_t i = 0, j = outer.size() - 1; i < outer.size(); j = i++)
        {
            const glm::dvec2 e = outer[i] - outer[j], w = c - outer[j];
            const double t = std::clamp((w.x * e.x + w.y * e.y) / (e.x * e.x + e.y * e.y), 0.0, 1.0);
            clearance = std::min(clearance, glm::length(w - e * t));
        }

        std::vector<Ring> rings = { outer };
        double expected = std::abs(water::signedArea(outer));
        size_t holes = 0;
        if (trial % 2 == 1)
        {
            // A small star-shaped hole well inside every radius of the outer ring.
            const int m = 3 + static_cast<int>(u01(rng) * 8);
            Ring hole;
            for (int i = 0; i < m; ++i)
            {
                const double a = 6.28318530717958647692 * i / m;
                const double r = std::min(minR * 0.4, clearance * 0.8) * (0.3 + 0.7 * u01(rng));
                hole.emplace_back(c.x + r * std::cos(a), c.y + r * std::sin(a));
            }
            if (trial % 4 == 3) std::reverse(hole.begin(), hole.end());
            expected -= std::abs(water::signedArea(hole));
            rings.push_back(hole);
            holes = 1;
        }

        const auto idx = water::triangulate(rings);
        const auto tris = trisOf(rings, idx);
        size_t verts = 0;
        for (const Ring& r : rings) verts += r.size();
        INFO("trial " << trial << " n=" << n << " holes=" << holes);
        CHECK(idx.size() == (verts + 2 * holes - 2) * 3);
        CHECK(sumArea(tris) == doctest::Approx(expected).epsilon(1e-7));
        CHECK_FALSE(anyOverlap(tris, 1e-7));
    }
}

TEST_CASE("Water triangulate: a big ring takes the hashed path and still covers it")
{
    // 600 vertices on a wobbly circle: past the size where the z-order hash is used.
    Ring r;
    const int n = 600;
    for (int i = 0; i < n; ++i)
    {
        const double a = 6.28318530717958647692 * i / n;
        const double rad = 100.0 + 12.0 * std::sin(a * 9.0) + 5.0 * std::cos(a * 23.0);
        r.emplace_back(rad * std::cos(a), rad * std::sin(a));
    }
    const Ring hole = starRing(6, 30.0, 12.0);
    const std::vector<Ring> rings = { r, hole };
    const auto idx = water::triangulate(rings);
    REQUIRE(idx.size() == (n + hole.size() + 2 - 2) * 3);
    const auto tris = trisOf(rings, idx);
    CHECK(sumArea(tris) == doctest::Approx(std::abs(water::signedArea(r)) - std::abs(water::signedArea(hole))).epsilon(1e-9));
    CHECK_FALSE(anyOverlap(tris, 1e-7));
}

TEST_CASE("Water triangulate: degenerate input gives nothing")
{
    CHECK(water::triangulate({}).empty());
    CHECK(water::triangulate({ ring({ { 0, 0 }, { 1, 1 } }) }).empty());
    CHECK(water::triangulate({ ring({ { 0, 0 }, { 1, 0 }, { std::numeric_limits<double>::quiet_NaN(), 1 } }) }).empty());
    // Collinear: no area, no triangle worth keeping.
    const auto flat = water::triangulate({ ring({ { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 } }) });
    CHECK(sumArea(trisOf({ ring({ { 0, 0 }, { 1, 0 }, { 2, 0 }, { 3, 0 } }) }, flat)) == doctest::Approx(0.0));
}

// ═════════════════════════════════════════════════════════════════════════════
// Contours
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water contours: a lattice saddle joins or separates by the mean of its corners")
{
    // Two opposite corners wet. A lattice 4 × 4 with the saddle cell in the middle.
    auto latticeOf = [](float a, float b, float c, float d) {
        water::Lattice l;
        l.w = l.h = 4; l.x0 = l.z0 = 0;
        l.originX = l.originZ = 0.0; l.cellW = l.cellH = 1.0;
        l.minX = l.minZ = -10.0; l.maxX = l.maxZ = 10.0;
        l.v.assign(16, 0.0f);
        l.v[1 * 4 + 1] = a; l.v[1 * 4 + 2] = b;
        l.v[2 * 4 + 2] = c; l.v[2 * 4 + 1] = d;
        return l;
    };
    // (1,1) and (2,2) wet, (2,1) and (1,2) dry. Mean 127.5 → joined: one contour.
    const auto joined = water::extractContours(latticeOf(255, 0, 255, 0));
    CHECK(joined.size() == 1);
    // A weaker second corner pulls the mean under the iso: two separate blobs.
    const auto apart = water::extractContours(latticeOf(255, 0, 200, 0));
    CHECK(apart.size() == 2);
    for (const auto& c : apart) CHECK(c.area > 0.0);
}

TEST_CASE("Water contours: a cell-aligned rectangle is one outer boundary of about its area")
{
    TerrainComponent tc = makeTerrain();
    // At a corner the contour has two crossings in the same place (one per lattice
    // edge that meets there); the one the ring starts on has a twin at its end. Both
    // were once judged redundant by each other and the corner (-9, -8) vanished —
    // the rectangle lost a triangle of 11 m² off its bottom edge.
    const uint16_t id = addLake(tc, rect(-10, -8, 14, 12));       // 24 × 20 = 480
    const water::CellRect box = water::bodyCells(tc, id);
    REQUIRE(box.valid());
    water::Lattice lat;
    REQUIRE(water::buildLattice(tc, id, box, lat));
    const auto cs = water::extractContours(lat);
    REQUIRE(cs.size() == 1);
    CHECK_FALSE(cs[0].hole());
    // The four convex corners are cut across their cell: 4 × 0.5.
    CHECK(cs[0].area == doctest::Approx(480.0 - 2.0).epsilon(1e-6));
    // A straight shore costs no vertex per cell: the rectangle is eight points
    // (each corner cut into two), not a hundred.
    CHECK(cs[0].points.size() == 8);
    // Which eight: the four corners, each cut across its cell.
    auto has = [&](double x, double z) {
        for (const auto& p : cs[0].points) if (std::abs(p.x - x) < 1e-9 && std::abs(p.y - z) < 1e-9) return true;
        return false;
    };
    CHECK(has(-9, -8));
    CHECK(has(-10, -7));
    CHECK(has(13, -8));
    CHECK(has(14, -7));
    CHECK(has(14, 11));
    CHECK(has(13, 12));
    CHECK(has(-9, 12));
    CHECK(has(-10, 11));
    // Counter-clockwise: the first outer boundary has water on its left.
    CHECK(water::signedArea(cs[0].points) > 0.0);
}

TEST_CASE("Water contours: two separate areas of one body are two contours")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = tc.water.createBody(0.0f);
    REQUIRE(water::addPolygon(tc, id, rect(-20, -20, -8, -8)).ok);
    REQUIRE(water::addPolygon(tc, id, rect(6, 4, 22, 18)).ok);
    const water::CellRect box = water::bodyCells(tc, id);
    water::Lattice lat;
    REQUIRE(water::buildLattice(tc, id, box, lat));
    const auto cs = water::extractContours(lat);
    CHECK(cs.size() == 2);
    for (const auto& c : cs) CHECK_FALSE(c.hole());
    CHECK(water::groupContours(cs).size() == 2);

    const Built b = build(tc, id);
    CHECK(b.s.contours == 2);
    CHECK(b.s.polygons == 2);
    CHECK_FALSE(anyOverlap(b.tris));
    // Nothing in the gap between them.
    CHECK_FALSE(covered(b.tris, { -2.0, -2.0 }));
    CHECK(covered(b.tris, { -14.0, -14.0 }));
    CHECK(covered(b.tris, { 14.0, 11.0 }));
}

TEST_CASE("Water contours: an island is a hole, and a lake in the island is another outer boundary")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = addLake(tc, rect(-28, -28, 28, 28));
    REQUIRE(water::removePolygon(tc, rect(-14, -14, 14, 14)).ok);          // the island
    REQUIRE(water::addPolygon(tc, id, rect(-4, -4, 4, 4)).ok);              // a pond on it

    const water::CellRect box = water::bodyCells(tc, id);
    water::Lattice lat;
    REQUIRE(water::buildLattice(tc, id, box, lat));
    const auto cs = water::extractContours(lat);
    REQUIRE(cs.size() == 3);
    int outers = 0, holes = 0;
    for (const auto& c : cs) (c.hole() ? holes : outers)++;
    CHECK(outers == 2);
    CHECK(holes == 1);

    const auto polys = water::groupContours(cs);
    REQUIRE(polys.size() == 2);
    size_t withHole = 0;
    for (const auto& p : polys) withHole += p.holes.size();
    CHECK(withHole == 1);                           // the hole belongs to the big lake, not the pond

    const Built b = build(tc, id);
    CHECK_FALSE(anyOverlap(b.tris));
    CHECK_FALSE(covered(b.tris, { 10.0, 10.0 }));   // the island
    CHECK(covered(b.tris, { 0.0, 0.0 }));           // the pond
    CHECK(covered(b.tris, { 21.0, 0.0 }));          // the lake
    // Area: ring + pond, within the corner cuts (one per convex corner of each outer boundary,
    // and the same for the island's, which are concave corners of the lake).
    const double expected = 56.0 * 56.0 - 28.0 * 28.0 + 8.0 * 8.0;
    CHECK(sumArea(b.tris) == doctest::Approx(expected).epsilon(0.01));
}

TEST_CASE("Water contours: water at the terrain border reaches the border")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = addLake(tc, rect(-32, -10, 0, 10));          // against the west edge
    const Built b = build(tc, id);
    REQUIRE_FALSE(b.s.empty());
    double minX = 1e9, minZ = 1e9, maxZ = -1e9;
    for (const Tri& t : b.tris)
        for (const glm::dvec2 p : { t.a, t.b, t.c })
        {
            minX = std::min(minX, p.x);
            minZ = std::min(minZ, p.y); maxZ = std::max(maxZ, p.y);
        }
    CHECK(minX == doctest::Approx(-32.0));           // not half a cell short, not outside
    CHECK(minZ >= -10.5);
    CHECK(maxZ <= 10.5);
    // 32 × 20 less the two cut corners at x = 0; the corners on the border are not cut.
    CHECK(sumArea(b.tris) == doctest::Approx(640.0 - 1.0).epsilon(0.002));
    CHECK_FALSE(anyOverlap(b.tris));
}

TEST_CASE("Water contours: the same field always gives the same mesh")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = addLake(tc, toVec2(starRing(5, 22.0, 9.0)));
    const Built a = build(tc, id);
    const Built b = build(tc, id);
    CHECK(a.s.hash == b.s.hash);
    CHECK(a.s.mesh.vertices == b.s.mesh.vertices);
    CHECK(a.s.mesh.indices == b.s.mesh.indices);
}

// ═════════════════════════════════════════════════════════════════════════════
// The mesh of a lake
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water mesh: an L-shaped lake, its area is the polygon's and no triangles overlap")
{
    TerrainComponent tc = makeTerrain();
    // 30 × 15 along the bottom plus 15 × 25 up the left: 825.
    const std::vector<glm::vec2> poly = { { -20, -20 }, { 10, -20 }, { 10, -5 }, { -5, -5 }, { -5, 20 }, { -20, 20 } };
    const uint16_t id = addLake(tc, poly, 1.25f);
    const Built b = build(tc, id);
    REQUIRE_FALSE(b.s.empty());

    // The lattice contour cuts each convex corner of a grid-aligned shape across
    // its cell (half a cell) and rounds the concave one the other way: the mesh is
    // the polygon to within that, here well under one percent.
    CHECK(sumArea(b.tris) == doctest::Approx(polyArea(poly)).epsilon(0.01));
    CHECK_FALSE(anyOverlap(b.tris));
    CHECK(allFaceUp(b.s.mesh));
    CHECK(b.s.level == doctest::Approx(1.25f));
    CHECK(b.s.polygons == 1);
    // About one triangle per contour vertex, far fewer than one per cell.
    CHECK(b.s.triangles < 40);

    // Where the L is not: the notch.
    CHECK_FALSE(covered(b.tris, { 3.0, 8.0 }));
    CHECK(covered(b.tris, { -12.0, 8.0 }));
    CHECK(covered(b.tris, { 3.0, -12.0 }));
}

TEST_CASE("Water mesh: a star-shaped lake off the grid")
{
    TerrainComponent tc = makeTerrain();
    const Ring star = starRing(5, 26.0, 10.5, { 1.3, -2.7 });
    const uint16_t id = addLake(tc, toVec2(star));
    const Built b = build(tc, id);
    REQUIRE_FALSE(b.s.empty());
    // Raster edges are anti-aliased, the contour interpolates them: a few tenths of a percent off.
    CHECK(sumArea(b.tris) == doctest::Approx(std::abs(water::signedArea(star))).epsilon(0.02));
    CHECK_FALSE(anyOverlap(b.tris));
    CHECK(allFaceUp(b.s.mesh));
    // The notch between two points is dry, the middle is wet.
    CHECK(covered(b.tris, { 1.3, -2.7 }));
    const double a = 3.14159265358979323846 / 5 + 0.3;                    // an inner vertex direction
    CHECK_FALSE(covered(b.tris, { 1.3 + 22.0 * std::cos(a), -2.7 + 22.0 * std::sin(a) }));
}

TEST_CASE("Water mesh: flat at the lake's level, normal up, vertices relative to the box centre")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = addLake(tc, rect(8, 4, 24, 20), 3.5f);        // far from the terrain centre
    const Built b = build(tc, id);
    REQUIRE_FALSE(b.s.empty());
    // The entity stands at the box centre; the mesh is centred on it.
    CHECK(b.s.center.x == doctest::Approx(16.0f).epsilon(0.01));
    CHECK(b.s.center.y == doctest::Approx(12.0f).epsilon(0.01));
    const StaticMeshAsset& m = b.s.mesh;
    REQUIRE(m.vertices.size() == m.normals.size());
    REQUIRE(m.vertices.size() / 3 == m.uvs.size() / 2);
    for (size_t i = 0; i < m.vertices.size(); i += 3)
    {
        CHECK(m.vertices[i + 1] == 0.0f);
        CHECK(std::abs(m.vertices[i]) <= 8.5f);
        CHECK(std::abs(m.vertices[i + 2]) <= 8.5f);
        CHECK(m.normals[i] == 0.0f);
        CHECK(m.normals[i + 1] == 1.0f);
        CHECK(m.normals[i + 2] == 0.0f);
    }
    // Bounds are filled (a runtime mesh without them is never culled) and not flat.
    CHECK(m.boundsMax[0] > m.boundsMin[0]);
    CHECK(m.boundsMax[1] > m.boundsMin[1]);
    CHECK(m.boundsMax[2] > m.boundsMin[2]);
    // Indices stay inside the vertex array.
    for (const uint32_t i : m.indices) CHECK(i < m.vertices.size() / 3);
}

TEST_CASE("Water mesh: UVs are world coordinates, so a second terrain beside it continues the pattern")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10));
    water::SurfaceOptions opt;
    opt.uvOrigin = { 100.0f, -50.0f };
    opt.uvMetersPerTile = 4.0f;
    const Built b = build(tc, id, opt);
    const StaticMeshAsset& m = b.s.mesh;
    REQUIRE_FALSE(b.s.empty());
    for (size_t v = 0; v < m.vertices.size() / 3; ++v)
    {
        const double worldX = m.vertices[v * 3] + b.s.center.x + 100.0;
        const double worldZ = m.vertices[v * 3 + 2] + b.s.center.y - 50.0;
        CHECK(m.uvs[v * 2] == doctest::Approx(worldX / 4.0).epsilon(1e-5));
        CHECK(m.uvs[v * 2 + 1] == doctest::Approx(worldZ / 4.0).epsilon(1e-5));
    }
    // The same lake with a different origin is a different mesh (the hash knows).
    const Built c = build(tc, id);
    CHECK(c.s.hash != b.s.hash);
}

TEST_CASE("Water mesh: a body without water is an empty surface, an unknown one an error")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t id = tc.water.createBody(0.0f);                    // no cells
    REQUIRE(water::ensureGrid(tc));
    water::SurfaceMesh s;
    CHECK(water::buildSurface(tc, id, {}, s));
    CHECK(s.empty());
    CHECK_FALSE(water::buildSurface(tc, 999, {}, s));
}

TEST_CASE("Water mesh: a large lake is built through the hashed path in good time")
{
    TerrainComponent tc = makeTerrain(512.0f, 512);
    std::vector<glm::vec2> poly;
    for (int i = 0; i < 400; ++i)
    {
        const double a = 6.28318530717958647692 * i / 400;
        const double r = 190.0 + 9.0 * std::sin(a * 7.0) + 3.0 * std::cos(a * 19.0);
        poly.emplace_back(static_cast<float>(r * std::cos(a)), static_cast<float>(r * std::sin(a)));
    }
    const uint16_t id = addLake(tc, poly);
    const Built b = build(tc, id);
    REQUIRE_FALSE(b.s.empty());
    CHECK(sumArea(b.tris) == doctest::Approx(polyArea(poly)).epsilon(0.005));
    CHECK(allFaceUp(b.s.mesh));
    // Triangles follow the shore, not the 200 000 cells inside it.
    CHECK(b.s.triangles < 3000);
}

// ═════════════════════════════════════════════════════════════════════════════
// The surface in the world
// ═════════════════════════════════════════════════════════════════════════════

TEST_CASE("Water surface: a lake becomes one entity with the engine water material")
{
    Rig rig;
    // An L, in metres on the 64 m terrain.
    const std::vector<glm::vec2> poly = { { -20, -20 }, { 10, -20 }, { 10, -5 }, { -5, -5 }, { -5, 20 }, { -20, 20 } };
    const uint16_t lake = addLake(rig.tc(), poly, 2.0f);

    rig.update();

    const auto surfaces = rig.surfaces();
    REQUIRE(surfaces.size() == 1);
    auto& reg = rig.world.registry();
    const Entity e = surfaces[0];
    const auto& ws = reg.get<WaterSurfaceComponent>(e);
    CHECK((ws.terrain == rig.terrain));
    CHECK(ws.body == lake);

    // An ordinary mesh with an ordinary material: nothing a backend has to know.
    const auto* mat = reg.try_get<MaterialComponent>(e);
    REQUIRE(mat);
    CHECK(mat->materialAssetId == HE::kEngineWaterMaterialId);
    const auto* mc = reg.try_get<MeshComponent>(e);
    REQUIRE(mc);
    CHECK(mc->meshAssetId == ws.meshId);
    CHECK_FALSE(mc->castsShadow);
    CHECK(mc->visible);
    const StaticMeshAsset* mesh = rig.cm.getStaticMesh(ws.meshId);
    REQUIRE(mesh);
    CHECK(mesh->indices.size() == ws.triangles * 3);
    CHECK(allFaceUp(*mesh));

    // Under the landscape, at the surface height, over the middle of the lake.
    const auto* hier = reg.try_get<HierarchyComponent>(e);
    REQUIRE(hier);
    CHECK((hier->parent == rig.terrain));
    const auto& tf = reg.get<TransformComponent>(e);
    CHECK(tf.position.y == doctest::Approx(2.0f));
    CHECK(tf.position.x == doctest::Approx(-5.0f).epsilon(0.05));    // middle of the box x −20..10
    CHECK(std::abs(tf.position.z) <= 0.5f);                          // z −20..20

    // The mesh area is the lake's (with the corner cuts).
    CHECK(sumArea(trisOf(*mesh, { 0.0f, 0.0f })) == doctest::Approx(polyArea(poly)).epsilon(0.01));
    CHECK_FALSE(rig.tc().water.dirty);       // the surface cleared the flag it was told about

    // The landscape's own chunks are untouched by all this.
    CHECK_FALSE(rig.tc().dirty);
    size_t chunks = 0;
    for (auto c : reg.view<TerrainChunkComponent>()) { (void)c; ++chunks; }
    CHECK(chunks > 0);
}

TEST_CASE("Water surface: nothing changes, nothing is rebuilt")
{
    Rig rig;
    const uint16_t a = addLake(rig.tc(), rect(-28, -28, -8, -8), 1.0f);
    const uint16_t b = addLake(rig.tc(), rect(8, 8, 28, 28), 4.0f);
    rig.update();
    REQUIRE(WaterSurface::lastStats().created == 2);
    REQUIRE(WaterSurface::lastStats().rebuilt == 2);
    const Entity ea = rig.surfaceOf(a), eb = rig.surfaceOf(b);
    REQUIRE((ea != entt::null));
    REQUIRE((eb != entt::null));
    const auto before = rig.world.registry().get<WaterSurfaceComponent>(eb);

    rig.update();
    CHECK_FALSE(WaterSurface::lastStats().worked());
    CHECK((rig.surfaceOf(a) == ea));
    CHECK((rig.surfaceOf(b) == eb));
    CHECK(rig.world.registry().get<WaterSurfaceComponent>(eb).meshId == before.meshId);
    CHECK(rig.surfaces().size() == 2);
}

TEST_CASE("Water surface: an edit rebuilds the body it touches and leaves the others alone")
{
    Rig rig;
    const uint16_t a = addLake(rig.tc(), rect(-28, -28, -8, -8), 1.0f);
    const std::vector<glm::vec2> lPoly = { { 4, 4 }, { 28, 4 }, { 28, 12 }, { 12, 12 }, { 12, 28 }, { 4, 28 } };
    const uint16_t b = addLake(rig.tc(), lPoly, 4.0f);
    rig.update();
    auto& reg = rig.world.registry();
    const HE::UUID meshA = reg.get<WaterSurfaceComponent>(rig.surfaceOf(a)).meshId;
    const HE::UUID meshB = reg.get<WaterSurfaceComponent>(rig.surfaceOf(b)).meshId;
    const uint64_t hashB = reg.get<WaterSurfaceComponent>(rig.surfaceOf(b)).hash;
    const std::vector<float> vertsB = rig.cm.getStaticMesh(meshB)->vertices;

    // Make A bigger.
    REQUIRE(water::addPolygon(rig.tc(), a, rect(-28, -28, 0, 0)).ok);
    rig.update();
    CHECK(WaterSurface::lastStats().rebuilt == 1);
    CHECK(WaterSurface::lastStats().created == 0);
    CHECK(reg.get<WaterSurfaceComponent>(rig.surfaceOf(a)).meshId == meshA);     // replaced in place
    CHECK(reg.get<WaterSurfaceComponent>(rig.surfaceOf(b)).hash == hashB);
    CHECK(rig.cm.getStaticMesh(meshB)->vertices == vertsB);
    const auto grown = trisOf(*rig.cm.getStaticMesh(meshA),
        { reg.get<TransformComponent>(rig.surfaceOf(a)).position.x, reg.get<TransformComponent>(rig.surfaceOf(a)).position.z });
    CHECK(sumArea(grown) == doctest::Approx(28.0 * 28.0).epsilon(0.01));
}

TEST_CASE("Water surface: an edit next to a lake that does not change it does not rebuild it")
{
    Rig rig;
    const std::vector<glm::vec2> lPoly = { { 4, 4 }, { 28, 4 }, { 28, 12 }, { 12, 12 }, { 12, 28 }, { 4, 28 } };
    const uint16_t lake = addLake(rig.tc(), lPoly, 4.0f);
    rig.update();
    const HE::UUID meshLake = rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).meshId;
    const uint64_t hashLake = rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).hash;

    // A pond painted into the L's notch: its rectangle lies inside the lake's box,
    // so the lake is looked at; its own cells are not touched, so it is not rebuilt.
    const uint16_t pond = rig.tc().water.createBody(1.0f);
    REQUIRE(water::addCircle(rig.tc(), pond, 20.0f, 20.0f, 3.0f, 1.0f).ok);
    rig.update();

    CHECK(WaterSurface::lastStats().created == 1);
    CHECK(WaterSurface::lastStats().rebuilt == 1);
    CHECK(WaterSurface::lastStats().kept == 1);
    CHECK((rig.surfaceOf(pond) != entt::null));
    CHECK(rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).meshId == meshLake);
    CHECK(rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).hash == hashLake);
}

TEST_CASE("Water surface: a new level moves the sheet, a removed body takes its entity and mesh with it")
{
    Rig rig;
    const uint16_t a = addLake(rig.tc(), rect(-28, -28, -8, -8), 1.0f);
    const uint16_t b = addLake(rig.tc(), rect(8, 8, 28, 28), 4.0f);
    rig.update();
    auto& reg = rig.world.registry();
    const HE::UUID meshA = reg.get<WaterSurfaceComponent>(rig.surfaceOf(a)).meshId;
    const HE::UUID meshB = reg.get<WaterSurfaceComponent>(rig.surfaceOf(b)).meshId;

    REQUIRE(water::setLevel(rig.tc(), a, 2.5f));
    rig.update();
    CHECK(reg.get<TransformComponent>(rig.surfaceOf(a)).position.y == doctest::Approx(2.5f));
    CHECK(reg.get<TransformComponent>(rig.surfaceOf(b)).position.y == doctest::Approx(4.0f));
    CHECK(reg.get<WaterSurfaceComponent>(rig.surfaceOf(a)).level == doctest::Approx(2.5f));
    CHECK(WaterSurface::lastStats().rebuilt == 1);          // B's lattice did not change

    REQUIRE(water::removeBody(rig.tc(), a).ok);
    rig.update();
    CHECK(WaterSurface::lastStats().removed == 1);
    CHECK((rig.surfaceOf(a) == entt::null));
    CHECK((rig.surfaceOf(b) != entt::null));
    CHECK(rig.cm.getStaticMesh(meshA) == nullptr);          // the mesh went back to the content manager
    CHECK(rig.cm.getStaticMesh(meshB) != nullptr);
}

TEST_CASE("Water surface: a lake dried by reshaping keeps its body and comes back under the same id")
{
    Rig rig;
    const HE::UUID spline{ 0x1234ull, 0x5678ull };
    const uint16_t lake = rig.tc().water.createBody(1.0f, spline);
    REQUIRE(water::addPolygon(rig.tc(), lake, rect(-10, -10, 10, 10)).ok);
    rig.update();
    REQUIRE((rig.surfaceOf(lake) != entt::null));

    REQUIRE(water::clearBody(rig.tc(), lake).ok);            // the lake tool reshapes: dry it up …
    rig.update();
    CHECK((rig.surfaceOf(lake) == entt::null));
    CHECK(rig.tc().water.findBody(lake) != nullptr);

    REQUIRE(water::addPolygon(rig.tc(), lake, rect(-12, -12, 12, 12)).ok);    // … and raster the new outline
    rig.update();
    const Entity e = rig.surfaceOf(lake);
    REQUIRE((e != entt::null));
    const auto* mesh = rig.cm.getStaticMesh(rig.world.registry().get<WaterSurfaceComponent>(e).meshId);
    REQUIRE(mesh);
    CHECK(sumArea(trisOf(*mesh)) == doctest::Approx(24.0 * 24.0).epsilon(0.01));
}

TEST_CASE("Water surface: moving the landscape moves the UVs, the lake stays in place under it")
{
    Rig rig;
    const uint16_t lake = addLake(rig.tc(), rect(-10, -10, 10, 10));
    rig.update();
    auto& reg = rig.world.registry();
    const HE::UUID mesh = reg.get<WaterSurfaceComponent>(rig.surfaceOf(lake)).meshId;
    const std::vector<float> uv0 = rig.cm.getStaticMesh(mesh)->uvs;
    const std::vector<float> pos0 = rig.cm.getStaticMesh(mesh)->vertices;

    reg.get<TransformComponent>(rig.terrain).position = glm::vec3(100.0f, 0.0f, 50.0f);
    rig.update();
    REQUIRE(WaterSurface::lastStats().rebuilt == 1);
    const StaticMeshAsset* moved = rig.cm.getStaticMesh(reg.get<WaterSurfaceComponent>(rig.surfaceOf(lake)).meshId);
    REQUIRE(moved);
    REQUIRE(moved->uvs.size() == uv0.size());
    CHECK(moved->vertices == pos0);                         // geometry is landscape-local
    for (size_t i = 0; i < uv0.size(); i += 2)
    {
        CHECK(moved->uvs[i] == doctest::Approx(uv0[i] + 100.0f));
        CHECK(moved->uvs[i + 1] == doctest::Approx(uv0[i + 1] + 50.0f));
    }
}

TEST_CASE("Water surface: generated entities are not saved, and come back from the field")
{
    Rig rig;
    const uint16_t lake = addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    REQUIRE((rig.surfaceOf(lake) != entt::null));
    const uint32_t trianglesBefore = rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).triangles;

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(rig.world, bytes));
    const std::string text(bytes.begin(), bytes.end());
    CHECK(text.find("WaterSurface") == std::string::npos);
    CHECK(text.find("TerrainChunk") == std::string::npos);
    CHECK(text.find("waterBodies") != std::string::npos);    // the source of truth did travel

    HorizonWorld world2;
    REQUIRE(ser.loadFromMemory(world2, bytes));
    CHECK(world2.registry().view<WaterSurfaceComponent>().size() == 0);
    for (auto [e, n] : world2.registry().view<NameComponent>().each())
        CHECK(n.name != "WaterSurface");

    ContentManager cm2(".");
    TerrainSystem::updateTerrains(world2, cm2);
    const auto surfaces2 = world2.registry().view<WaterSurfaceComponent>();
    REQUIRE(surfaces2.size() == 1);
    for (auto [e, ws] : surfaces2.each())
        CHECK(ws.triangles == trianglesBefore);
}

TEST_CASE("Water surface: a scene rebuild (undo) gives the old meshes back and builds new ones")
{
    Rig rig;
    const uint16_t lake = addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    const HE::UUID oldMesh = rig.world.registry().get<WaterSurfaceComponent>(rig.surfaceOf(lake)).meshId;
    REQUIRE(rig.cm.getStaticMesh(oldMesh) != nullptr);

    // What undo does: every entity is torn down and read back from the snapshot.
    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(rig.world, bytes));
    std::vector<Entity> all;
    for (auto e : rig.world.registry().view<entt::entity>()) all.push_back(e);
    for (Entity e : all)
        if (rig.world.registry().valid(e) && e != rig.world.rootEntity()) rig.world.destroyEntity(e);
    REQUIRE(ser.loadFromMemory(rig.world, bytes));

    rig.update();
    CHECK(rig.cm.getStaticMesh(oldMesh) == nullptr);        // not leaked
    const auto surfaces = rig.world.registry().view<WaterSurfaceComponent>();
    REQUIRE(surfaces.size() == 1);
    for (auto [e, ws] : surfaces.each())
        CHECK(rig.cm.getStaticMesh(ws.meshId) != nullptr);
}

TEST_CASE("Water surface: deleting the landscape takes the water with it")
{
    Rig rig;
    addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    const HE::UUID mesh = rig.world.registry().get<WaterSurfaceComponent>(rig.surfaces()[0]).meshId;
    rig.world.destroyEntity(rig.terrain);
    TerrainSystem::updateTerrains(rig.world, rig.cm);
    CHECK(rig.surfaces().empty());
    CHECK(rig.cm.getStaticMesh(mesh) == nullptr);
}

TEST_CASE("Water surface: it is a generated child like a chunk, and clicking it answers for the landscape")
{
    Rig rig;
    addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    auto& reg = rig.world.registry();
    const Entity surface = rig.surfaces()[0];
    CHECK(HE::isTerrainGenerated(reg, surface));
    CHECK((HE::terrainOwnerOf(reg, surface) == rig.terrain));
    CHECK_FALSE(HE::isTerrainGenerated(reg, rig.terrain));
    CHECK((HE::terrainOwnerOf(reg, rig.terrain) == entt::null));
    for (auto [c, cc] : reg.view<TerrainChunkComponent>().each())
    {
        CHECK(HE::isTerrainGenerated(reg, c));
        CHECK((HE::terrainOwnerOf(reg, c) == rig.terrain));
    }
}

TEST_CASE("Water surface: a stray copy in a scene file is swept out like a stray chunk")
{
    Rig rig;
    addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    // A child named like a surface that lost its component (a subtree copy that was
    // saved before generated children were skipped): not live, so it is a ghost.
    Entity ghost = rig.world.createEntity("WaterSurface");
    rig.world.reparentEntity(ghost, rig.terrain);
    REQUIRE(rig.world.registry().valid(ghost));
    rig.world.purgeOrphanedGeneratedEntities();
    CHECK_FALSE(rig.world.registry().valid(ghost));
    CHECK(rig.surfaces().size() == 1);                       // the live one stays
}

TEST_CASE("Water surface: navigation does not bake the lake as a floor")
{
    Rig rig;
    rig.update();
    NavMeshGeometry dry;
    const size_t groundTris = NavigationSystem::collectStaticGeometry(rig.world, rig.cm, dry);
    REQUIRE(groundTris > 0);

    addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);
    rig.update();
    REQUIRE(rig.surfaces().size() == 1);
    NavMeshGeometry wet;
    CHECK(NavigationSystem::collectStaticGeometry(rig.world, rig.cm, wet) == groundTris);
}

TEST_CASE("Water surface: physics does not see the sheet, a body dropped over a lake reaches the ground")
{
    Rig rig;
    auto& reg = rig.world.registry();
    // A flat landscape at y = 0 with a lake whose surface stands 3 m above it.
    const uint16_t lake = addLake(rig.tc(), rect(-20, -20, 20, 20), 3.0f);
    rig.update();
    const Entity sheet = rig.surfaceOf(lake);
    REQUIRE((sheet != entt::null));

    const float radius = 0.5f;
    Entity ball = rig.world.createEntity("Ball");
    {
        TransformComponent t;
        t.position = { 0.0f, 15.0f, 0.0f };
        rig.world.addComponent(ball, t);
        RigidBodyComponent rb; rb.type = RigidBodyType::Dynamic; rb.mass = 1.0f;
        // This test is about the SHEET: it has no collider, so what falls through it
        // reaches the ground. The water itself has physics since Thema 179 (a 1 kg
        // ball of this size is about 2 kg/m³ and would float), which is
        // test_water_buoyancy.cpp's business, so the ball opts out of it here.
        rb.affectedByWater = false;
        rig.world.addComponent(ball, rb);
        ColliderComponent col; col.shape = ColliderShape::Sphere; col.radius = radius;
        rig.world.addComponent(ball, col);
    }
    PhysicsWorld phys;
    phys.initialize(rig.world);
    REQUIRE(phys.hasPhysics(static_cast<uint32_t>(rig.terrain)));
    CHECK_FALSE(phys.hasPhysics(static_cast<uint32_t>(sheet)));     // the sheet has no body

    for (int i = 0; i < 240; ++i) phys.step(rig.world, 1.0f / 60.0f);
    const float y = reg.get<TransformComponent>(ball).position.y;
    // On the ground (0 + radius), not on the water (3 + radius).
    CHECK(y < 1.5f);
    CHECK(y > -0.5f);
}

TEST_CASE("Water surface: two landscapes keep their water apart")
{
    Rig rig;
    auto& reg = rig.world.registry();
    const uint16_t lake = addLake(rig.tc(), rect(-12, -12, 12, 12), 1.0f);

    Entity second = rig.world.createEntity("Landscape2");
    TransformComponent tf2;
    tf2.position = glm::vec3(200.0f, 0.0f, 0.0f);
    reg.emplace<TransformComponent>(second, tf2);
    TerrainComponent tc2 = makeTerrain();
    tc2.resolution = 9;
    tc2.dirty = true;
    reg.emplace<TerrainComponent>(second, tc2);
    const uint16_t pond = addLake(reg.get<TerrainComponent>(second), rect(0, 0, 10, 10), 7.0f);

    rig.update();
    REQUIRE(rig.surfaces().size() == 2);
    for (auto [e, ws] : reg.view<WaterSurfaceComponent>().each())
    {
        if (ws.terrain == rig.terrain) { CHECK(ws.body == lake); }
        else                           { CHECK((ws.terrain == second)); CHECK(ws.body == pond); }
    }
    // Editing the second one rebuilds only its surface.
    REQUIRE(water::addPolygon(reg.get<TerrainComponent>(second), pond, rect(0, 0, 14, 14)).ok);
    rig.update();
    CHECK(WaterSurface::lastStats().rebuilt == 1);
}


// ═════════════════════════════════════════════════════════════════════════════
// Shore clipping: the sheet stops where the ground comes up (Thema 174 Schritt 5)
// ═════════════════════════════════════════════════════════════════════════════

namespace
{
    // A height field of res × res vertices over the terrain, from height(x, z) in
    // terrain-local metres. res must be 2ⁿ+1 or TerrainSystem would resample it.
    template <class F> void shapeGround(TerrainComponent& tc, uint32_t res, F height)
    {
        tc.resolution = res;
        tc.sculptHeights.assign(static_cast<size_t>(res) * res, 0.0f);
        for (uint32_t z = 0; z < res; ++z)
            for (uint32_t x = 0; x < res; ++x)
            {
                const float px = -tc.sizeX * 0.5f + tc.sizeX * static_cast<float>(x) / static_cast<float>(res - 1);
                const float pz = -tc.sizeZ * 0.5f + tc.sizeZ * static_cast<float>(z) / static_cast<float>(res - 1);
                tc.sculptHeights[static_cast<size_t>(z) * res + x] = height(px, pz);
            }
    }

    water::SurfaceOptions clipped(float overshoot = water::kDefaultShoreOvershoot)
    {
        water::SurfaceOptions o;
        o.clipToGround = true;
        o.shoreOvershoot = overshoot;
        return o;
    }

    // How far the ground stands above the sheet's level, worst over every vertex and
    // every triangle centroid of the mesh. A clipped sheet has none beyond the overshoot.
    float groundOverSheet(const TerrainComponent& tc, const Built& b)
    {
        float worst = -1e30f;
        for (const Tri& t : b.tris)
        {
            const glm::dvec2 pts[4] = { t.a, t.b, t.c, (t.a + t.b + t.c) / 3.0 };
            for (const glm::dvec2& p : pts)
                worst = std::max(worst, terrainHeightAt(tc, static_cast<float>(p.x), static_cast<float>(p.y)) - b.s.level);
        }
        return worst;
    }

    double maxX(const std::vector<Tri>& tris)
    {
        double m = -1e300;
        for (const Tri& t : tris) m = std::max({ m, t.a.x, t.b.x, t.c.x });
        return m;
    }

    // The triangles of a body's sheet in a world, terrain-local; empty without a sheet.
    std::vector<Tri> sheetOf(Rig& rig, uint16_t body)
    {
        const Entity e = rig.surfaceOf(body);
        if (e == entt::null) return {};
        auto& reg = rig.world.registry();
        const auto& ws = reg.get<WaterSurfaceComponent>(e);
        const auto& tf = reg.get<TransformComponent>(e);
        const StaticMeshAsset* m = rig.cm.getStaticMesh(ws.meshId);
        REQUIRE(m);
        return trisOf(*m, { tf.position.x, tf.position.z });
    }
}

TEST_CASE("Water shore: a clipped lifted lattice cuts where the ground comes up, by interpolation")
{
    // Corners 2..5 are water; the depth of the water over the ground falls one metre
    // per corner and passes zero three quarters of the way from corner 3 (0.75 m)
    // to corner 4 (−0.25 m), i.e. at x = 3.75.
    water::Lattice l;
    l.w = l.h = 8; l.x0 = l.z0 = 0;
    l.originX = l.originZ = 0.0; l.cellW = l.cellH = 1.0;
    l.minX = l.minZ = -20.0; l.maxX = l.maxZ = 20.0;
    l.v.assign(64, 0.0f);
    l.g.assign(64, 1.0f);
    for (int j = 0; j < 8; ++j)
        for (int i = 0; i < 8; ++i)
        {
            if (i >= 2 && i <= 5 && j >= 2 && j <= 5) l.v[static_cast<size_t>(j) * 8 + i] = 255.0f;
            l.g[static_cast<size_t>(j) * 8 + i] = 3.75f - static_cast<float>(i);
        }

    const auto cs = water::extractContours(l);
    REQUIRE(cs.size() == 1);
    CHECK_FALSE(cs[0].hole());
    double lo = 1e300, hi = -1e300;
    for (const auto& p : cs[0].points) { lo = std::min(lo, p.x); hi = std::max(hi, p.x); }
    CHECK(lo == doctest::Approx(1.5));          // the coverage limit, half-way between a wet and a dry corner
    CHECK(hi == doctest::Approx(3.75));         // the ground limit, three quarters of the way
    // 2.25 × 4, less the two coverage-side corners (0.125 each) and the two corners
    // where both limits meet (0.1875 each): the lattice cuts a corner across its cell.
    CHECK(cs[0].area == doctest::Approx(9.0 - 0.25 - 0.375).epsilon(1e-9));

    // Without the ground array the same lattice is the coverage alone: [1.5, 5.5].
    water::Lattice plain = l;
    plain.g.clear();
    const auto cp = water::extractContours(plain);
    REQUIRE(cp.size() == 1);
    double hiPlain = -1e300;
    for (const auto& p : cp[0].points) hiPlain = std::max(hiPlain, p.x);
    CHECK(hiPlain == doctest::Approx(5.5));
}

TEST_CASE("Water shore: a lake over a slope ends at the line where the ground meets the water")
{
    TerrainComponent tc = makeTerrain();                         // 64 m, a metre per cell
    shapeGround(tc, 65, [](float x, float) { return 0.5f * x; });   // rises 0.5 m per metre to the east
    const uint16_t id = addLake(tc, rect(-20, -20, 20, 20), 3.0f);

    const Built flat = build(tc, id);                            // the footprint alone
    const Built b = build(tc, id, clipped());
    REQUIRE_FALSE(b.s.empty());
    REQUIRE_FALSE(flat.s.empty());

    // The footprint is the 40 × 40 square, over ground that stands 10 m up at its east edge.
    CHECK(sumArea(flat.tris) == doctest::Approx(1600.0).epsilon(0.002));
    CHECK(groundOverSheet(tc, flat) > 5.0f);

    // The shore: 3.05 m of water over a 0.5 slope reaches x = 6.1. Exact, not "about":
    // the lattice edge from x = 6 to x = 7 has depths 0.05 and −0.45.
    const double shore = (3.0 + water::kDefaultShoreOvershoot) / 0.5;
    CHECK(maxX(b.tris) == doctest::Approx(shore).epsilon(1e-5));
    // 26.1 × 40 less the two cut corners on the west side (half a cell each).
    CHECK(sumArea(b.tris) == doctest::Approx(26.1 * 40.0 - 1.0).epsilon(0.002));
    CHECK_FALSE(anyOverlap(b.tris));
    CHECK(allFaceUp(b.s.mesh));
    CHECK(b.s.polygons == 1);

    // Wet below the bank, dry over it.
    CHECK(covered(b.tris, { -10.0, 0.0 }));
    CHECK(covered(b.tris, { 5.9, 0.0 }));
    CHECK_FALSE(covered(b.tris, { 6.3, 0.0 }));
    CHECK_FALSE(covered(b.tris, { 15.0, 0.0 }));
    // Nothing of the sheet is over the ground (beyond the overshoot).
    CHECK(groundOverSheet(tc, b) <= water::kDefaultShoreOvershoot + 1e-3f);
    // The ground is part of the sheet's identity: same cells, another mesh.
    CHECK(b.s.hash != flat.s.hash);
    CHECK(b.s.triangles < 20);                                   // still a polygon, not a triangle per cell
}

TEST_CASE("Water shore: the overshoot moves the line up the bank by its height over the slope")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float x, float) { return 0.5f * x; });
    const uint16_t id = addLake(tc, rect(-20, -20, 20, 20), 3.0f);
    CHECK(maxX(build(tc, id, clipped(0.0f)).tris)  == doctest::Approx(6.0).epsilon(1e-5));
    CHECK(maxX(build(tc, id, clipped(0.05f)).tris) == doctest::Approx(6.1).epsilon(1e-5));
    CHECK(maxX(build(tc, id, clipped(0.25f)).tris) == doctest::Approx(6.5).epsilon(1e-5));
    CHECK(maxX(build(tc, id, clipped(1.0f)).tris)  == doctest::Approx(8.0).epsilon(1e-5));
}

TEST_CASE("Water shore: a bowl gives a round lake with the shore where the water meets the rim")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 129, [](float x, float z) { return 0.01f * (x * x + z * z); });
    // Level 4 and 0.05 overshoot: the ground is under the sheet out to r² = 405.
    const uint16_t id = addLake(tc, rect(-28, -28, 28, 28), 4.0f);
    const Built b = build(tc, id, clipped());
    REQUIRE_FALSE(b.s.empty());

    const double radius = std::sqrt((4.0 + water::kDefaultShoreOvershoot) / 0.01);
    CHECK(sumArea(b.tris) == doctest::Approx(3.14159265358979323846 * radius * radius).epsilon(0.01));
    CHECK(b.s.contours == 1);
    CHECK(b.s.polygons == 1);
    CHECK_FALSE(anyOverlap(b.tris));
    // Every vertex of the mesh is on the shore, and the shore is a circle.
    for (size_t i = 0; i < b.s.mesh.vertices.size(); i += 3)
    {
        const double x = b.s.mesh.vertices[i] + b.s.center.x;
        const double z = b.s.mesh.vertices[i + 2] + b.s.center.y;
        CHECK(std::sqrt(x * x + z * z) == doctest::Approx(radius).epsilon(0.005));
    }
    CHECK(covered(b.tris, { 0.0, 0.0 }));
    CHECK(covered(b.tris, { radius - 0.5, 0.0 }));
    CHECK_FALSE(covered(b.tris, { radius + 0.5, 0.0 }));
    CHECK_FALSE(covered(b.tris, { 22.0, 22.0 }));                // inside the lake's square, up the rim
    CHECK(groundOverSheet(tc, b) <= water::kDefaultShoreOvershoot + 1e-3f);
}

TEST_CASE("Water shore: a mound in the lake is an island, a ridge between two hollows makes two pools")
{
    SUBCASE("a mound")
    {
        TerrainComponent tc = makeTerrain();
        shapeGround(tc, 129, [](float x, float z) { return 6.0f - 0.03f * (x * x + z * z); });
        const uint16_t id = addLake(tc, rect(-25, -25, 25, 25), 1.0f);
        const Built b = build(tc, id, clipped());
        REQUIRE_FALSE(b.s.empty());
        // The ground is above the sheet inside r² = (6 − 1.05) / 0.03 = 165: a hole.
        CHECK(b.s.contours == 2);
        CHECK(b.s.polygons == 1);
        CHECK(sumArea(b.tris) == doctest::Approx(2500.0 - 3.14159265358979323846 * 165.0).epsilon(0.01));
        CHECK_FALSE(covered(b.tris, { 0.0, 0.0 }));
        CHECK_FALSE(covered(b.tris, { 12.0, 0.0 }));
        CHECK(covered(b.tris, { 14.0, 0.0 }));
        CHECK(covered(b.tris, { -20.0, 15.0 }));
        CHECK_FALSE(anyOverlap(b.tris));
        // The hole is a polygon inscribed in the island's rim, so between two of its
        // vertices a sliver of sheet reaches under the rim by the chord's sagitta
        // times the slope (a centimetre or so here): out of sight under the opaque
        // ground, and the reason this bound is looser than the bowl's.
        CHECK(groundOverSheet(tc, b) <= water::kDefaultShoreOvershoot + 0.02f);
    }
    SUBCASE("two hollows")
    {
        TerrainComponent tc = makeTerrain();
        shapeGround(tc, 129, [](float x, float z) {
            const float a = (x + 16.0f) * (x + 16.0f) + z * z;
            const float c = (x - 16.0f) * (x - 16.0f) + z * z;
            return 0.02f * std::min(a, c);
        });
        const uint16_t id = addLake(tc, rect(-30, -12, 30, 12), 2.0f);     // one body over both
        const Built b = build(tc, id, clipped());
        REQUIRE_FALSE(b.s.empty());
        // Two pools of r² = 2.05 / 0.02 = 102.5, with the ridge between them dry.
        CHECK(b.s.polygons == 2);
        CHECK(b.s.contours == 2);
        CHECK(sumArea(b.tris) == doctest::Approx(2.0 * 3.14159265358979323846 * 102.5).epsilon(0.01));
        CHECK(covered(b.tris, { -16.0, 0.0 }));
        CHECK(covered(b.tris, { 16.0, 0.0 }));
        CHECK_FALSE(covered(b.tris, { 0.0, 0.0 }));
        CHECK_FALSE(anyOverlap(b.tris));
    }
}

TEST_CASE("Water shore: brushed water is clipped by the same ground as a lake drawn from a polygon")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float x, float) { return 0.5f * x; });

    // A brush dab: radius 12 with a falloff, centred on the slope.
    const uint16_t painted = tc.water.createBody(3.0f);
    REQUIRE(water::addCircle(tc, painted, 0.0f, 0.0f, 12.0f, 4.0f).ok);
    const Built brush = build(tc, painted, clipped());
    REQUIRE_FALSE(brush.s.empty());

    const double shore = (3.0 + water::kDefaultShoreOvershoot) / 0.5;
    CHECK(maxX(brush.tris) == doctest::Approx(shore).epsilon(1e-5));
    CHECK(covered(brush.tris, { -8.0, 0.0 }));
    CHECK_FALSE(covered(brush.tris, { 6.5, 0.0 }));
    CHECK(groundOverSheet(tc, brush) <= water::kDefaultShoreOvershoot + 1e-3f);

    // The lake of a closed spline, on the same ground at the same level: the same line.
    TerrainComponent tc2 = tc;
    water::clearAll(tc2);
    tc2.water.res = tc.water.res;
    const uint16_t lake = addLake(tc2, rect(-15, -15, 15, 15), 3.0f);
    CHECK(maxX(build(tc2, lake, clipped()).tris) == doctest::Approx(maxX(brush.tris)).epsilon(1e-5));
}

TEST_CASE("Water shore: ground under the sheet that is lower than the level changes nothing")
{
    // The usual lake: the ground is below the sheet everywhere. Clipped or not,
    // the mesh is the same bit for bit.
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float x, float z) { return -4.0f - 0.01f * (x * x + z * z) * 0.1f; });
    const uint16_t id = addLake(tc, toVec2(starRing(5, 26.0, 10.5, { 1.3, -2.7 })), 0.5f);
    const Built a = build(tc, id);
    const Built b = build(tc, id, clipped());
    REQUIRE_FALSE(a.s.empty());
    CHECK(a.s.mesh.vertices == b.s.mesh.vertices);
    CHECK(a.s.mesh.indices == b.s.mesh.indices);
}

TEST_CASE("Water shore: ground above the level everywhere is no sheet at all")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float, float) { return 2.0f; });
    const uint16_t id = addLake(tc, rect(-10, -10, 10, 10), 0.5f);
    water::SurfaceMesh s;
    REQUIRE(water::buildSurface(tc, id, clipped(), s));
    CHECK(s.empty());
    CHECK(s.polygons == 0);
    // The footprint alone is still there when asked for.
    CHECK_FALSE(build(tc, id).s.empty());
}

TEST_CASE("Water shore: sculpting a hill into a lake takes the water off the hill, digging it away brings it back")
{
    Rig rig;
    const uint16_t lake = addLake(rig.tc(), rect(-20, -20, 20, 20), 1.0f);
    rig.update();
    REQUIRE((rig.surfaceOf(lake) != entt::null));
    {
        const auto tris = sheetOf(rig, lake);
        CHECK(sumArea(tris) == doctest::Approx(1600.0).epsilon(0.01));
        CHECK(covered(tris, { 0.0, 0.0 }));
    }

    // A hill: 4 m high in the middle, 6 m of falloff around a 6 m radius.
    REQUIRE(TerrainSculpt::apply(rig.tc(), 0.0f, 0.0f, TerrainSculpt::Op::Raise, 6.0f, 6.0f, 4.0f).ok);
    rig.update();
    CHECK(WaterSurface::lastStats().rebuilt == 1);               // the ground moved, the sheet followed
    CHECK_FALSE(rig.tc().water.dirty);
    {
        const auto tris = sheetOf(rig, lake);
        REQUIRE_FALSE(tris.empty());
        CHECK_FALSE(covered(tris, { 0.0, 0.0 }));                // the top of the hill is dry
        CHECK(covered(tris, { -18.0, -18.0 }));                  // the rest of the lake is not
        CHECK(covered(tris, { 17.0, 3.0 }));
        CHECK(sumArea(tris) < 1550.0);                           // a hill of about 13 m radius is out
        CHECK(sumArea(tris) > 800.0);
        // The field did not change: the cells are still the whole 40 × 40.
        CHECK(rig.tc().water.wetCells(lake) == 1600u);
    }
    // The landscape's chunks did their own thing and the water did not make them redo it.
    CHECK_FALSE(rig.tc().dirty);
    CHECK_FALSE(rig.tc().regionDirty);

    // Level the hill again: the water is back, over the same cells.
    REQUIRE(TerrainSculpt::apply(rig.tc(), 0.0f, 0.0f, TerrainSculpt::Op::Set, 40.0f, 0.0f, 0.0f).ok);
    rig.update();
    {
        const auto tris = sheetOf(rig, lake);
        REQUIRE_FALSE(tris.empty());
        CHECK(covered(tris, { 0.0, 0.0 }));
        CHECK(sumArea(tris) == doctest::Approx(1600.0).epsilon(0.01));
    }
}

TEST_CASE("Water shore: a sculpt far from every lake does not rebuild it, and with clipping off no ground edit does")
{
    Rig rig;
    const uint16_t lake = addLake(rig.tc(), rect(8, 8, 24, 24), 1.0f);
    rig.update();
    REQUIRE((rig.surfaceOf(lake) != entt::null));

    REQUIRE(TerrainSculpt::apply(rig.tc(), -24.0f, -24.0f, TerrainSculpt::Op::Raise, 3.0f, 2.0f, 2.0f).ok);
    rig.update();
    CHECK_FALSE(WaterSurface::lastStats().worked());
    CHECK(WaterSurface::lastStats().kept == 1);

    water::setShoreClip(rig.tc(), false, water::kDefaultShoreOvershoot);
    rig.update();                                                // the setting itself rebuilds
    CHECK(WaterSurface::lastStats().rebuilt == 1);
    REQUIRE(TerrainSculpt::apply(rig.tc(), 16.0f, 16.0f, TerrainSculpt::Op::Raise, 4.0f, 2.0f, 5.0f).ok);
    rig.update();
    CHECK_FALSE(WaterSurface::lastStats().worked());             // under the lake, but nobody listens
    CHECK(covered(sheetOf(rig, lake), { 16.0, 16.0 }));          // the footprint stands over the new hill
}

TEST_CASE("Water shore: clipping on and off, and a sheet clipped away entirely comes back when it is switched off")
{
    Rig rig;
    // Under the ground everywhere: the flat landscape stands at 0, the lake at −5.
    const uint16_t lake = addLake(rig.tc(), rect(-10, -10, 10, 10), -5.0f);
    rig.update();
    CHECK((rig.surfaceOf(lake) == entt::null));                  // nothing to draw: no entity
    CHECK(rig.tc().water.wetCells(lake) == 400u);               // the water is still there as intent

    water::setShoreClip(rig.tc(), false, water::kDefaultShoreOvershoot);
    rig.update();
    REQUIRE((rig.surfaceOf(lake) != entt::null));
    CHECK(sumArea(sheetOf(rig, lake)) == doctest::Approx(400.0).epsilon(0.01));

    water::setShoreClip(rig.tc(), true, water::kDefaultShoreOvershoot);
    rig.update();
    CHECK((rig.surfaceOf(lake) == entt::null));                  // clipped away again, entity and mesh gone

    // Raising the lake out of the ground brings it back.
    REQUIRE(water::setLevel(rig.tc(), lake, 0.5f));
    rig.update();
    CHECK((rig.surfaceOf(lake) != entt::null));
}

TEST_CASE("Water shore: the settings are clamped, and a bad one keeps the old")
{
    TerrainComponent tc = makeTerrain();
    water::setShoreClip(tc, true, -3.0f);
    CHECK(tc.water.shoreOvershoot == 0.0f);
    water::setShoreClip(tc, true, 1.0e9f);
    CHECK(tc.water.shoreOvershoot == water::kMaxShoreOvershoot);
    water::setShoreClip(tc, true, 0.4f);
    water::setShoreClip(tc, false, std::numeric_limits<float>::quiet_NaN());
    CHECK(tc.water.shoreOvershoot == 0.4f);
    CHECK_FALSE(tc.water.clipToGround);

    // sanitize (what a loaded file goes through) does the same for a hand-edited value.
    tc.water.shoreOvershoot = std::numeric_limits<float>::infinity();
    water::sanitize(tc);
    CHECK(tc.water.shoreOvershoot == water::kDefaultShoreOvershoot);
    tc.water.shoreOvershoot = -1.0f;
    water::sanitize(tc);
    CHECK(tc.water.shoreOvershoot == 0.0f);
}

// ── Soft shore (Thema 206 Schritt 2): the mesh carries the water's depth ────────────
namespace
{
    // The shore proximity vertex i of a mesh carries (normal.x / gain; see WaterWaves.h).
    float proximityOf(const StaticMeshAsset& m, uint32_t i)
    {
        return m.normals[i * 3] / HE::water::kShoreNormalGain;
    }

    // The triangles that cover something. The triangulator's ears (and so the pieces a
    // split cuts them into) can be slivers of no area at all, a corner on the line of
    // the other two; they draw nothing and a float cannot say which way they face. A run of contour
    // points that is straight to within a float (a bank that is a plane) leaves them a little over
    // 1e-5 m² (a corner 10 to 20 µm off the chord of a 3 m edge): a square centimetre is nothing to see.
    std::vector<uint32_t> solidIndices(const StaticMeshAsset& m, double minArea = 1e-4)
    {
        std::vector<uint32_t> out;
        for (size_t i = 0; i + 2 < m.indices.size(); i += 3)
        {
            const float* a = &m.vertices[m.indices[i] * 3];
            const float* b = &m.vertices[m.indices[i + 1] * 3];
            const float* c = &m.vertices[m.indices[i + 2] * 3];
            const double twice = (double(b[2]) - a[2]) * (double(c[0]) - a[0]) - (double(b[0]) - a[0]) * (double(c[2]) - a[2]);
            if (std::abs(twice) < 2.0 * minArea) continue;
            out.insert(out.end(), { m.indices[i], m.indices[i + 1], m.indices[i + 2] });
        }
        return out;
    }

    // Is a vertex of the (solid) mesh strictly inside an edge of one of its triangles? That
    // is a T-junction: the two sides of the edge would be rasterised differently and a pixel
    // of the blended sheet could drop out or draw twice.
    bool hasTJunction(const StaticMeshAsset& m)
    {
        const std::vector<uint32_t> idx = solidIndices(m);
        std::vector<uint32_t> used = idx;
        std::sort(used.begin(), used.end());
        used.erase(std::unique(used.begin(), used.end()), used.end());
        auto P = [&](uint32_t i) { return glm::dvec2(m.vertices[i * 3], m.vertices[i * 3 + 2]); };
        for (size_t t = 0; t + 2 < idx.size(); t += 3)
            for (int k = 0; k < 3; ++k)
            {
                const glm::dvec2 a = P(idx[t + k]), b = P(idx[t + (k + 1) % 3]);
                const glm::dvec2 ab = b - a;
                const double len2 = glm::dot(ab, ab);
                if (len2 < 1e-12) continue;
                for (const uint32_t v : used)
                {
                    const glm::dvec2 p = P(v);
                    const double u = glm::dot(p - a, ab) / len2;
                    if (u <= 1e-6 || u >= 1.0 - 1e-6) continue;
                    const glm::dvec2 q = a + ab * u;
                    // A float holds a coordinate of 25 m to 3 µm: a vertex that close to the line is on it.
                    if (glm::length(p - q) < 1e-5) return true;
                }
            }
        return false;
    }

    // Every covering triangle faces up, and the covering ones overlap nowhere.
    StaticMeshAsset solidOf(const StaticMeshAsset& m)
    {
        StaticMeshAsset s = m;
        s.indices = solidIndices(m);
        return s;
    }

    water::SurfaceOptions softShore(float overshoot = water::kDefaultShoreOvershoot)
    {
        water::SurfaceOptions o = clipped(overshoot);
        o.shoreFade = true;
        return o;
    }
}

TEST_CASE("Water soft shore: off, the mesh is the plain sheet; without the ground it does nothing")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float x, float) { return 0.04f * x; });
    const uint16_t id = addLake(tc, rect(-20, -20, 20, 20), 0.3f);
    const Built plain = build(tc, id, clipped());
    REQUIRE_FALSE(plain.s.empty());
    CHECK(plain.s.shoreVertices == 0);
    for (size_t i = 0; i < plain.s.mesh.normals.size(); i += 3) CHECK(plain.s.mesh.normals[i] == 0.0f);

    // No ground to read the depth from: shoreFade asks for nothing, the mesh and its hash stay.
    water::SurfaceOptions noGround;
    noGround.shoreFade = true;
    const Built a = build(tc, id, noGround), b = build(tc, id, water::SurfaceOptions{});
    CHECK(a.s.hash == b.s.hash);
    CHECK(a.s.mesh.vertices == b.s.mesh.vertices);
    CHECK(a.s.mesh.indices == b.s.mesh.indices);
    CHECK(a.s.mesh.normals == b.s.mesh.normals);
    CHECK(a.s.shoreVertices == 0);
}

TEST_CASE("Water soft shore: on a plane bank the depth is carried linearly, the sheet is the same sheet")
{
    TerrainComponent tc = makeTerrain();                                  // 64 m, a metre per cell
    shapeGround(tc, 65, [](float x, float) { return 0.04f * x; });       // 4 cm per metre, rising east
    const uint16_t id = addLake(tc, rect(-20, -20, 20, 20), 0.3f);
    const Built plain = build(tc, id, clipped());
    const Built soft  = build(tc, id, softShore());
    REQUIRE_FALSE(soft.s.empty());

    // The same sheet: the area, the outline, nothing overlapping, every triangle up.
    CHECK(sumArea(soft.tris) == doctest::Approx(sumArea(plain.tris)).epsilon(1e-6));
    CHECK_FALSE(anyOverlap(soft.tris));
    CHECK(allFaceUp(soft.s.mesh));
    CHECK_FALSE(hasTJunction(soft.s.mesh));
    CHECK(soft.s.hash != plain.s.hash);                                   // another mesh with the same cells
    // Refined near the shore, and not into a grid: tens of triangles, not a thousand.
    CHECK(soft.s.triangles > plain.s.triangles);
    CHECK(soft.s.triangles < 400);
    CHECK(soft.s.shoreVertices > 0);

    // Every vertex: normal (gain · e, 1, 0), e in 0..1.
    const StaticMeshAsset& m = soft.s.mesh;
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
    {
        CHECK(m.normals[i * 3 + 1] == 1.0f);
        CHECK(m.normals[i * 3 + 2] == 0.0f);
        const float e = proximityOf(m, i);
        CHECK(e >= 0.0f);
        CHECK(e <= 1.0f + 1e-6f);
    }

    // e = 1 at the waterline, 0 beyond kShoreFadeDepth of water; in between it is
    // 1 − depth / kShoreFadeDepth, depth being 0.3 − 0.04 x over the drawn sheet.
    auto expected = [](double x)
    {
        return std::clamp(1.0 - (0.3 - 0.04 * x) / double(water::kShoreFadeDepth), 0.0, 1.0);
    };
    float seenOne = 0.0f;
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
    {
        const double x = m.vertices[i * 3] + soft.s.center.x;
        CHECK(proximityOf(m, i) == doctest::Approx(expected(x)).epsilon(0.04));
        seenOne = std::max(seenOne, proximityOf(m, i));
    }
    CHECK(seenOne == doctest::Approx(1.0f));
    // And between the vertices: the centre of every triangle carries the mean of its corners,
    // which is what the rasteriser interpolates to there. It follows the real depth.
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
    {
        const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        const double x = (m.vertices[a * 3] + m.vertices[b * 3] + m.vertices[c * 3]) / 3.0 + soft.s.center.x;
        const float e = (proximityOf(m, a) + proximityOf(m, b) + proximityOf(m, c)) / 3.0f;
        CHECK(e == doctest::Approx(expected(x)).epsilon(0.05));
    }
    // The far end of this lake is 1.1 m deep, which the depth range now covers (the test below
    // has a bank steep enough to leave it): the channel still follows the real depth there.
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
        if (m.vertices[i * 3] + soft.s.center.x < -19.0f)
            CHECK(proximityOf(m, i) == doctest::Approx(expected(-20.0)).epsilon(0.04));

    // The same field always gives the same mesh.
    const Built again = build(tc, id, softShore());
    CHECK(again.s.hash == soft.s.hash);
    CHECK(again.s.mesh.vertices == soft.s.mesh.vertices);
    CHECK(again.s.mesh.normals == soft.s.mesh.normals);
    CHECK(again.s.mesh.indices == soft.s.mesh.indices);
}

// Thema 320: the material has no scene depth, so the depth the mesh carries is all it knows of the
// shore. With a range of one metre the water went from clear to its deep colour inside a single
// contour, which is the hard line where the water meets the landscape; kShoreFadeDepth is wider now,
// and what the mesh carries has to follow the real depth over the whole range, with no step and no
// plateau on the way, so the fade the material builds from it has nothing to jump over.
TEST_CASE("Water soft shore: the depth runs on to kShoreFadeDepth with no plateau on the way (Thema 320)")
{
    CHECK(water::kShoreFadeDepth > 1.5f);                                 // deeper than the old one metre

    TerrainComponent tc = makeTerrain();                                  // 64 m, a metre per cell
    shapeGround(tc, 65, [](float x, float) { return 0.1f * x; });        // 10 cm per metre, rising east
    const uint16_t id = addLake(tc, rect(-30, -20, 20, 20), 0.3f);        // 0.3 over the origin: waterline at x = 3
    const Built soft = build(tc, id, softShore());
    REQUIRE_FALSE(soft.s.empty());
    const StaticMeshAsset& m = soft.s.mesh;

    // depth(x) = 0.3 - 0.1 x on the drawn sheet, e = 1 - depth / kShoreFadeDepth, clamped.
    auto expected = [](double x)
    {
        return std::clamp(1.0 - (0.3 - 0.1 * x) / double(water::kShoreFadeDepth), 0.0, 1.0);
    };

    // Every vertex follows the real depth, also where it is deeper than the old range.
    uint32_t beyondOldRange = 0;
    float lowestInRange = 1.0f;
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
    {
        const double x = m.vertices[i * 3] + soft.s.center.x;
        const float e = proximityOf(m, i);
        CHECK(e == doctest::Approx(expected(x)).epsilon(0.04));
        const double depth = 0.3 - 0.1 * x;
        if (depth > 1.2 && depth < water::kShoreFadeDepth - 0.2)
        {
            ++beyondOldRange;
            CHECK(e > 0.05f);                                             // one metre ago this read 0: open water
            lowestInRange = std::min(lowestInRange, e);
        }
    }
    CHECK(beyondOldRange > 0);
    CHECK(lowestInRange < 0.6f);                                          // and it keeps falling out there

    // Between the vertices too: the centre of a triangle carries the mean of its corners (what the
    // rasteriser interpolates to there) and it follows the real depth, so the ramp has no kink in it.
    for (size_t t = 0; t + 2 < m.indices.size(); t += 3)
    {
        const uint32_t a = m.indices[t], b = m.indices[t + 1], c = m.indices[t + 2];
        const double x = (m.vertices[a * 3] + m.vertices[b * 3] + m.vertices[c * 3]) / 3.0 + soft.s.center.x;
        const float e = (proximityOf(m, a) + proximityOf(m, b) + proximityOf(m, c)) / 3.0f;
        CHECK(e == doctest::Approx(expected(x)).epsilon(0.05));
    }

    // Deeper than the range there is no shore in it: open water, exactly 0.
    const double openX = 0.3 / 0.1 - water::kShoreFadeDepth / 0.1;        // where depth = kShoreFadeDepth
    uint32_t openVertices = 0;
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
        if (m.vertices[i * 3] + soft.s.center.x < openX - 1.0)
        {
            CHECK(proximityOf(m, i) == 0.0f);
            ++openVertices;
        }
    CHECK(openVertices > 0);
}

TEST_CASE("Water soft shore: the shore-proximity readback survives a landscape's own Y-rotation (Thema 206 Schritt 8)")
{
    // The bake above puts (gain*e, 1, 0) in the mesh normal; the material reads it back from
    // the WORLD normal mat3(uModel) * n, normalized (vNormal in the vertex shader, NormalWS
    // normalizes it in MaterialGraph.cpp). A landscape (or its WaterSurface child) rotated
    // about Y moves that tilt out of x and into z. This reimplements both the OLD decode
    // (e = n.x / (n.y*gain), MatGen/main.cpp before Schritt 8) and the NEW one
    // (e = len(n.xz) / (n.y*gain)) in C++ to check the formula itself against a world normal
    // built the same way the renderer builds it — it does not run the generated shader.
    const float gain = HE::water::kShoreNormalGain;
    auto oldDecode = [&](const glm::vec3& n) { return n.x / (n.y * gain); };
    auto newDecode = [&](const glm::vec3& n) { return std::sqrt(n.x * n.x + n.z * n.z) / (n.y * gain); };

    for (const float e : { 0.0f, 0.25f, 0.6f, 1.0f })
    {
        const glm::vec3 local(gain * e, 1.0f, 0.0f);
        for (const float deg : { 0.0f, 30.0f, 60.0f, 90.0f, 135.0f, 180.0f, 270.0f })
        {
            const glm::mat3 r(glm::rotate(glm::mat4(1.0f), glm::radians(deg), glm::vec3(0, 1, 0)));
            const glm::vec3 world = glm::normalize(r * local);
            CHECK(newDecode(world) == doctest::Approx(e).epsilon(1e-4));              // holds at every heading
            if (deg == 0.0f || e == 0.0f)
                CHECK(oldDecode(world) == doctest::Approx(e).epsilon(1e-4));
            else
                CHECK_FALSE(oldDecode(world) == doctest::Approx(e).epsilon(0.02));    // the bug: wrong past 0 deg
        }

        // A non-uniform scale on the landscape (sx != sy, e.g. 2:1): the SHADER FORMULA
        // alone has no way to see the object's scale, so both decodes are off by that
        // ratio here. SurfaceOptions::shoreTiltScale (WaterMesh.h) closes this at bake
        // time instead — see the end-to-end test below.
        if (e > 0.0f)
        {
            const glm::mat3 s(2.0f, 0, 0,  0, 1.0f, 0,  0, 0, 3.0f);
            const glm::vec3 scaled = glm::normalize(s * local);
            CHECK(newDecode(scaled) == doctest::Approx(2.0f * e).epsilon(1e-3));
        }
    }
}

TEST_CASE("Water soft shore: a non-uniform landscape scale is pre-compensated at bake time (Thema 206 Schritt 8)")
{
    // End to end: bake with shoreTiltScale set the way WaterSurface.cpp sets it
    // (shoreTiltScaleOf = sy/sx of the landscape's own world matrix), then put the baked
    // normal through the SAME transform the renderer's vertex stage applies
    // (mat3(uModel) * n, normalized) and decode with the new (rotation-robust) formula.
    // The recovered e matches the UNSCALED e: the bake-time compensation and the shader's
    // sx/sy error cancel exactly, for any heading.
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 65, [](float x, float) { return 0.04f * x; });
    const uint16_t id = addLake(tc, rect(-20, -20, 20, 20), 0.3f);

    const float gain = HE::water::kShoreNormalGain;
    auto newDecode = [&](const glm::vec3& n) { return std::sqrt(n.x * n.x + n.z * n.z) / (n.y * gain); };

    for (const float sx : { 0.5f, 1.0f, 2.0f })
        for (const float sy : { 0.5f, 1.0f, 3.0f })
            for (const float deg : { 0.0f, 50.0f, 90.0f, 170.0f })
            {
                water::SurfaceOptions opt = softShore();
                opt.shoreTiltScale = sy / sx;
                const Built scaled = build(tc, id, opt);
                REQUIRE(scaled.s.shoreVertices > 0);

                const glm::mat3 r(glm::rotate(glm::mat4(1.0f), glm::radians(deg), glm::vec3(0, 1, 0)));
                const glm::mat3 s(sx, 0, 0,  0, sy, 0,  0, 0, sx);   // Z follows X: no rotation/Z-scale coupling to track
                const glm::mat3 m = r * s;

                const StaticMeshAsset& bm = scaled.s.mesh;
                size_t checked = 0;
                for (uint32_t i = 0; i < bm.vertices.size() / 3; ++i)
                {
                    const glm::vec3 baked(bm.normals[i * 3], bm.normals[i * 3 + 1], bm.normals[i * 3 + 2]);
                    const float eBaked = proximityOf(scaled.s.mesh, i) / opt.shoreTiltScale;   // what the mesh carries, undone
                    if (eBaked <= 0.0f) continue;                                               // open water: nothing to recover
                    const glm::vec3 world = glm::normalize(m * baked);
                    CHECK(newDecode(world) == doctest::Approx(eBaked).epsilon(0.02));
                    ++checked;
                }
                CHECK(checked > 0);
            }
}

TEST_CASE("Water soft shore: an island's bridged hole and a round bank stay watertight")
{
    TerrainComponent tc = makeTerrain();
    shapeGround(tc, 129, [](float x, float z) { return 6.0f - 0.03f * (x * x + z * z); });   // a mound, as above
    const uint16_t id = addLake(tc, rect(-25, -25, 25, 25), 1.0f);
    const Built plain = build(tc, id, clipped());
    const Built soft  = build(tc, id, softShore());
    REQUIRE_FALSE(soft.s.empty());
    CHECK(soft.s.contours == 2);                                          // an outer boundary and the island's hole
    CHECK(sumArea(soft.tris) == doctest::Approx(sumArea(plain.tris)).epsilon(1e-6));
    const StaticMeshAsset solid = solidOf(soft.s.mesh);
    CHECK_FALSE(anyOverlap(trisOf(solid, soft.s.center), 1e-7));
    CHECK_FALSE(hasTJunction(soft.s.mesh));
    CHECK(allFaceUp(solid));
    CHECK(soft.s.shoreVertices > 0);
    // Water over the ground's low rim is shallow, the lake's outer edge (the square) is no shore.
    const StaticMeshAsset& m = soft.s.mesh;
    for (uint32_t i = 0; i < m.vertices.size() / 3; ++i)
    {
        const float x = m.vertices[i * 3] + soft.s.center.x, z = m.vertices[i * 3 + 2] + soft.s.center.y;
        if (std::max(std::abs(x), std::abs(z)) > 24.9f) CHECK(proximityOf(m, i) < 1.0f);
    }
}

TEST_CASE("Water soft shore: a large lake with a gentle bank is built in good time and stays a shore mesh")
{
    TerrainComponent tc = makeTerrain(512.0f, 512);
    shapeGround(tc, 513, [](float x, float z) { return 4.0f * (x * x + z * z) / (190.0f * 190.0f) - 2.0f; });
    std::vector<glm::vec2> poly;
    for (int i = 0; i < 400; ++i)
    {
        const double a = 6.28318530717958647692 * i / 400;
        const double r = 200.0 + 9.0 * std::sin(a * 7.0) + 3.0 * std::cos(a * 19.0);
        poly.emplace_back(static_cast<float>(r * std::cos(a)), static_cast<float>(r * std::sin(a)));
    }
    const uint16_t id = addLake(tc, poly);
    const Built plain = build(tc, id, clipped());
    const auto t0 = std::chrono::steady_clock::now();
    const Built soft = build(tc, id, softShore());
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    REQUIRE_FALSE(soft.s.empty());
    MESSAGE("soft shore: " << soft.s.triangles << " triangles (plain " << plain.s.triangles << "), " << ms << " ms");
    CHECK(sumArea(soft.tris) == doctest::Approx(sumArea(plain.tris)).epsilon(1e-6));
    CHECK(soft.s.triangles < 20000);
    CHECK(ms < 2000.0);
}

#ifdef HE_EDITOR_DEPS_DIR
// ── The material stays bound (Thema 206 Schritt 9c) ──────────────────────────
// ensureWaterMaterial and the MaterialComponent were only done when a surface was
// CREATED. A surface that lives on (a scene that is re-opened and adopts one, a
// brush stroke after the material was unloaded, a component lost to an undo
// snapshot) drew with the plain fallback until it was rebuilt.
TEST_CASE("Water surface: a surface that already exists is given its material back, loaded and bound")
{
    Rig rig;
    rig.cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());
    const std::vector<glm::vec2> poly = { { -20, -20 }, { 10, -20 }, { 10, 20 }, { -20, 20 } };
    addLake(rig.tc(), poly, 2.0f);
    rig.update();
    auto& reg = rig.world.registry();
    REQUIRE(rig.surfaces().size() == 1);
    const Entity e = rig.surfaces()[0];
    CHECK(rig.cm.isLoaded(HE::kEngineWaterMaterialId));
    CHECK(reg.get<MaterialComponent>(e).materialAssetId == HE::kEngineWaterMaterialId);

    // The material goes away (a project switch, an unload sweep) and nothing was edited.
    REQUIRE(rig.cm.unloadAsset(HE::kEngineWaterMaterialId));
    REQUIRE_FALSE(rig.cm.isLoaded(HE::kEngineWaterMaterialId));
    rig.update();
    CHECK(rig.cm.isLoaded(HE::kEngineWaterMaterialId));

    // The binding is lost (a snapshot that did not carry it, a script) and so is the material.
    reg.remove<MaterialComponent>(e);
    REQUIRE(rig.cm.unloadAsset(HE::kEngineWaterMaterialId));
    rig.update();
    const auto* mat = reg.try_get<MaterialComponent>(e);
    REQUIRE(mat);
    CHECK(mat->materialAssetId == HE::kEngineWaterMaterialId);
    CHECK(rig.cm.isLoaded(HE::kEngineWaterMaterialId));

    // Bound to something else: a scene that was saved with another material on it.
    reg.get<MaterialComponent>(e).materialAssetId = HE::UUID{};
    rig.update();
    CHECK(reg.get<MaterialComponent>(e).materialAssetId == HE::kEngineWaterMaterialId);
}

// ── The water material has an id of its own (Thema 230 Schritt 2) ────────────
// The water material was {0x400, 1}, the id of T_Landscape_Grass_Albedo, and the
// ContentManager keeps ONE entry per id whatever the type. With that texture
// resident (any project that has the landscape textures) ensureResident(water)
// said yes, getMaterial(water) said null and the surface drew the white default,
// with nothing in the log.
TEST_CASE("Water surface: a landscape texture on the water material's old id does not take the material away")
{
    Rig rig;
    rig.cm.setEngineContentRoot((std::filesystem::path(HE_EDITOR_DEPS_DIR) / "EngineContent").string());

    TextureAsset grass;
    grass.id   = HE::kLegacyEngineWaterMaterialId;   // T_Landscape_Grass_Albedo's real id
    grass.name = "T_Landscape_Grass_Albedo";
    REQUIRE(rig.cm.registerTexture(grass) == HE::kLegacyEngineWaterMaterialId);

    const std::vector<glm::vec2> poly = { { -20, -20 }, { 10, -20 }, { 10, 20 }, { -20, 20 } };
    addLake(rig.tc(), poly, 2.0f);
    rig.update();
    REQUIRE(rig.surfaces().size() == 1);

    // A material, not a texture, answers to the water's id ...
    const MaterialAsset* water = rig.cm.getMaterial(HE::kEngineWaterMaterialId);
    REQUIRE(water);
    CHECK_FALSE(water->customShaderFragGlsl.empty());   // what the renderer draws with; empty = the white default
    CHECK(rig.world.registry().get<MaterialComponent>(rig.surfaces()[0]).materialAssetId == HE::kEngineWaterMaterialId);
    // ... and the texture keeps its own.
    CHECK(rig.cm.getTexture(HE::kLegacyEngineWaterMaterialId) != nullptr);
    CHECK(rig.cm.getMaterial(HE::kLegacyEngineWaterMaterialId) == nullptr);
}
#endif

TEST_CASE("Water material id: a scene saved under the old id still gets the water material")
{
    CHECK(HE::kEngineWaterMaterialId != HE::kLegacyEngineWaterMaterialId);
    // 0x400..0x41F is landscape_tex_gen's block (textures, M_AutoLandscape): one id, one asset.
    CHECK((HE::kEngineWaterMaterialId.hi < 0x400 || HE::kEngineWaterMaterialId.hi > 0x41F));

    HorizonWorld world;
    auto& reg = world.registry();
    const HE::UUID other{ 0x1234, 0x5678 };
    const Entity e = world.createEntity("Plane");
    MaterialComponent m;
    m.materialAssetId = HE::kLegacyEngineWaterMaterialId;
    m.slotOverrides   = { HE::kLegacyEngineWaterMaterialId, other, HE::UUID{} };
    reg.emplace<MaterialComponent>(e, m);
    const Entity f = world.createEntity("Other");
    MaterialComponent mo;
    mo.materialAssetId = other;
    reg.emplace<MaterialComponent>(f, mo);

    SceneSerializer ser;
    std::vector<uint8_t> bytes;
    REQUIRE(ser.saveToMemory(world, bytes));
    HorizonWorld world2;
    REQUIRE(ser.loadFromMemory(world2, bytes));

    int seen = 0;
    for (auto [en, mat] : world2.registry().view<MaterialComponent>().each())
    {
        if (mat.materialAssetId == HE::kEngineWaterMaterialId)
        {
            ++seen;
            REQUIRE(mat.slotOverrides.size() >= 2);
            CHECK(mat.slotOverrides[0] == HE::kEngineWaterMaterialId);
            CHECK(mat.slotOverrides[1] == other);   // the negative control: other ids are not touched
        }
        else
            CHECK(mat.materialAssetId == other);
    }
    CHECK(seen == 1);
}
