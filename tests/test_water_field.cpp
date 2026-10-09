#include "doctest.h"
#include "TestFsUtil.h"
#include <HorizonScene/WaterField.h>
#include <HorizonScene/Components/TerrainComponent.h>
#include <HorizonScene/SplineCurve.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/HorizonWorld.h>
#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;
namespace water = HE::water;

// The water model (WaterField.h): bodies, a coverage + owner raster over the
// terrain's UV range, editing by circle and polygon, the scene format, undo.
// Terrain 64 × 64 m at 64 cells unless a test says otherwise, so a cell is 1 m
// and its edges sit on whole metres: shapes with whole-metre corners rasterise
// to 0 and 255 only, and an area is a cell count.

namespace
{
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

    // A U: 40 × 40 with a notch 20 wide and 30 deep cut from the top (+z). Area 1000.
    std::vector<glm::vec2> uShape()
    {
        return { { -20, -20 }, { 20, -20 }, { 20, 20 }, { 10, 20 },
                 { 10, -10 }, { -10, -10 }, { -10, 20 }, { -20, 20 } };
    }

    double areaOf(const water::Raster& r)
    {
        double sum = 0;
        for (uint8_t c : r.coverage) sum += c;
        return sum / 255.0;
    }

    double shoelace(const std::vector<glm::vec2>& p)
    {
        double a = 0;
        for (size_t i = 0; i < p.size(); ++i)
        {
            const glm::vec2& u = p[i]; const glm::vec2& v = p[(i + 1) % p.size()];
            a += static_cast<double>(u.x) * v.y - static_cast<double>(v.x) * u.y;
        }
        return std::abs(a) * 0.5;
    }

    // The cell a terrain-local point falls in, as an index into the rasters.
    size_t cellIndex(const TerrainComponent& tc, float x, float z)
    {
        const int res = static_cast<int>(tc.water.res);
        const int cx = static_cast<int>(std::floor((x + tc.sizeX * 0.5f) / tc.sizeX * res));
        const int cz = static_cast<int>(std::floor((z + tc.sizeZ * 0.5f) / tc.sizeZ * res));
        return static_cast<size_t>(cz) * res + cx;
    }

    uint8_t covAt(const TerrainComponent& tc, float x, float z) { return tc.water.coverage[cellIndex(tc, x, z)]; }

    TerrainComponent* firstTerrain(HorizonWorld& world)
    {
        auto view = world.registry().view<TerrainComponent>();
        for (auto e : view) return &view.get<TerrainComponent>(e);
        return nullptr;
    }
}

// ── Polygon rasterisation ────────────────────────────────────────────────────

TEST_CASE("Water: a concave U rasterises with the notch dry and both arms wet")
{
    const water::Raster r = water::rasterizePolygon(uShape(), 64.0f, 64.0f, 64);
    REQUIRE_FALSE(r.empty());
    // Corners on whole metres and 1 m cells: the area is exactly the cell count.
    CHECK(areaOf(r) == doctest::Approx(1000.0));
    for (uint8_t c : r.coverage) CHECK((c == 0 || c == 255));

    TerrainComponent tc = makeTerrain();
    const uint16_t lake = tc.water.createBody(3.0f);
    REQUIRE(water::addPolygon(tc, lake, uShape()).ok);
    CHECK(covAt(tc, 0.5f, 0.5f)    == 0);     // in the notch
    CHECK(covAt(tc, 0.5f, 15.5f)   == 0);     // deep in the notch
    CHECK(covAt(tc, -15.5f, 0.5f)  == 255);   // left arm
    CHECK(covAt(tc, 15.5f, 0.5f)   == 255);   // right arm
    CHECK(covAt(tc, 0.5f, -15.5f)  == 255);   // the bar under the notch
    CHECK(water::bodyAt(tc, -15.0f, 0.0f) == lake);
    CHECK(water::bodyAt(tc, 0.0f, 0.0f)   == water::kNoBody);
    CHECK(tc.water.wetCells(lake) == 1000u);
}

TEST_CASE("Water: polygon edges are anti-aliased, the area follows the shape")
{
    SUBCASE("a diagonal")
    {
        const std::vector<glm::vec2> tri = { { -10, -10 }, { 10, -10 }, { -10, 10 } };
        const water::Raster r = water::rasterizePolygon(tri, 64.0f, 64.0f, 64);
        REQUIRE_FALSE(r.empty());
        CHECK(areaOf(r) == doctest::Approx(200.0).epsilon(0.01));
        int partial = 0;
        for (uint8_t c : r.coverage) partial += (c > 0 && c < 255) ? 1 : 0;
        CHECK(partial >= 15);   // the hypotenuse crosses ~20 cells
    }
    SUBCASE("an edge a quarter into a cell")
    {
        const water::Raster r = water::rasterizePolygon(rect(-10.25f, -10.25f, 10.25f, 10.25f), 64.0f, 64.0f, 64);
        CHECK(areaOf(r) == doctest::Approx(420.25).epsilon(0.002));
        // The cell column outside the whole-metre block holds a quarter.
        CHECK(r.coverage[static_cast<size_t>(r.h / 2) * r.w] == 64);
    }
}

TEST_CASE("Water: polygon rasterisation, winding, self-crossing, bad and clipped input")
{
    const water::Raster fwd = water::rasterizePolygon(uShape(), 64.0f, 64.0f, 64);
    std::vector<glm::vec2> rev = uShape();
    std::reverse(rev.begin(), rev.end());
    const water::Raster bwd = water::rasterizePolygon(rev, 64.0f, 64.0f, 64);
    CHECK(fwd.coverage == bwd.coverage);   // even-odd: winding does not matter

    SUBCASE("a figure eight fills its two lobes and leaves the rest")
    {
        const std::vector<glm::vec2> bow = { { -10, -10 }, { 10, 10 }, { 10, -10 }, { -10, 10 } };
        TerrainComponent tc = makeTerrain();
        const uint16_t b = tc.water.createBody(0.0f);
        REQUIRE(water::addPolygon(tc, b, bow).ok);
        CHECK(water::bodyAt(tc, -8.0f, 0.5f) == b);
        CHECK(water::bodyAt(tc, 8.0f, 0.5f)  == b);
        CHECK(water::bodyAt(tc, 0.5f, 8.0f)  == water::kNoBody);
        CHECK(water::bodyAt(tc, 0.5f, -8.0f) == water::kNoBody);
    }
    SUBCASE("nothing to draw")
    {
        CHECK(water::rasterizePolygon({}, 64, 64, 64).empty());
        CHECK(water::rasterizePolygon({ { 0, 0 }, { 5, 5 } }, 64, 64, 64).empty());
        CHECK(water::rasterizePolygon({ { 0, 0 }, { 5, 5 }, { 10, 10 } }, 64, 64, 64).empty());      // no area
        CHECK(water::rasterizePolygon(rect(100, 100, 120, 120), 64, 64, 64).empty());                // off the terrain
        const float notANumber = std::nanf("");
        CHECK(water::rasterizePolygon({ { 0, 0 }, { notANumber, 5 }, { 10, 10 } }, 64, 64, 64).empty());
        CHECK(water::rasterizePolygon(uShape(), 0.0f, 64.0f, 64).empty());
        CHECK(water::rasterizePolygon(uShape(), 64.0f, 64.0f, 0).empty());
    }
    SUBCASE("a polygon half off the terrain is clipped")
    {
        // The quarter inside: x, z from -32 to 0.
        const water::Raster r = water::rasterizePolygon(rect(-50, -50, 0, 0), 64.0f, 64.0f, 64);
        REQUIRE_FALSE(r.empty());
        CHECK(areaOf(r) == doctest::Approx(32.0 * 32.0));
        CHECK(r.x0 == 0);
        CHECK(r.z0 == 0);
    }
}

TEST_CASE("Water: a polygon from a real closed spline carries over, without the closing vertex")
{
    // A C: two arms joined on the left, the notch open to the right.
    const std::vector<glm::vec3> ctl = {
        { -15, 0, -15 }, { 15, 0, -15 }, { 15, 0, -5 }, { 0, 0, -5 },
        { 0, 0, 5 }, { 15, 0, 5 }, { 15, 0, 15 }, { -15, 0, 15 } };
    const HE::spline::Curve curve(ctl, true);
    REQUIRE(curve.closed());
    const std::vector<glm::vec3> ring = curve.polyline(0.05f);
    REQUIRE(ring.size() > 8);
    CHECK(glm::length(ring.front() - ring.back()) < 1e-4f);   // the lesson: first == last

    const std::vector<glm::vec2> poly = water::polygonFromPolyline(ring);
    CHECK(poly.size() == ring.size() - 1);
    CHECK(poly.front() == glm::vec2(ring.front().x, ring.front().z));

    TerrainComponent tc = makeTerrain();
    const uint16_t lake = tc.water.createBody(2.0f, HE::UUID::generate());
    REQUIRE(water::addPolygon(tc, lake, poly).ok);
    CHECK(water::bodyAt(tc, -8.0f, 0.0f)  == lake);          // the spine
    CHECK(water::bodyAt(tc, 8.0f, -10.0f) == lake);          // lower arm
    CHECK(water::bodyAt(tc, 8.0f, 10.0f)  == lake);          // upper arm
    CHECK(water::bodyAt(tc, 8.0f, 0.0f)   == water::kNoBody);   // the notch
    CHECK(water::bodyAt(tc, 25.0f, 0.0f)  == water::kNoBody);
    // What the raster holds is the polygon's area.
    const double wet = [&] { double s = 0; for (uint8_t c : tc.water.coverage) s += c; return s / 255.0; }();
    CHECK(wet == doctest::Approx(shoelace(poly)).epsilon(0.01));

    SUBCASE("the caller's transform is applied before the XZ projection")
    {
        const glm::mat4 shift = glm::translate(glm::mat4(1.0f), glm::vec3(10.0f, 99.0f, 5.0f));
        const std::vector<glm::vec2> moved = water::polygonFromPolyline(ring, shift);
        REQUIRE(moved.size() == poly.size());
        CHECK(moved[3].x == doctest::Approx(poly[3].x + 10.0f));
        CHECK(moved[3].y == doctest::Approx(poly[3].y + 5.0f));    // y is dropped, never added to z
    }
}

// ── Circle ───────────────────────────────────────────────────────────────────

TEST_CASE("Water: a circle adds full water inside the radius and a falloff ring outside")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t b = tc.water.createBody(4.0f);
    const water::Result r = water::addCircle(tc, b, 0.0f, 0.0f, 8.0f, 6.0f);
    REQUIRE(r.ok);
    CHECK(r.changed > 200);

    const int res = 64;
    uint8_t prev = 255;
    for (int k = 0; k < 20; ++k)          // along +x from the middle, one cell per step
    {
        const float x = 0.5f + static_cast<float>(k);
        const uint8_t c = covAt(tc, x, 0.5f);
        CHECK(c <= prev);                 // monotone with the distance
        prev = c;
        const float dist = std::sqrt(x * x + 0.25f);
        if (dist <= 8.0f) CHECK(c == 255);
        if (dist > 14.0f) CHECK(c == 0);
    }
    CHECK(covAt(tc, 0.5f, 0.5f) == 255);
    // Linear across the falloff: 11.5 m out is 3.5/6 of the way to the rim.
    CHECK(static_cast<int>(covAt(tc, 11.5f, 0.5f)) == doctest::Approx(106).epsilon(0.03));
    // Whoever is wet, is the brush's body; nothing is wet that has no owner.
    for (size_t i = 0; i < tc.water.coverage.size(); ++i)
        CHECK((tc.water.coverage[i] > 0) == (tc.water.owner[i] == b));
    CHECK(tc.water.coverage.size() == static_cast<size_t>(res) * res);
}

TEST_CASE("Water: removing the circle that was added leaves the grid exactly as it was")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t b = tc.water.createBody(4.0f);
    REQUIRE(water::ensureGrid(tc));
    const std::vector<uint8_t> dryCov = tc.water.coverage;
    const std::vector<uint16_t> dryOwn = tc.water.owner;

    SUBCASE("full strength")
    {
        REQUIRE(water::addCircle(tc, b, 3.3f, -7.1f, 8.0f, 6.0f).changed > 0);
        const water::Result r = water::removeCircle(tc, 3.3f, -7.1f, 8.0f, 6.0f);
        CHECK(r.ok);
        CHECK(tc.water.coverage == dryCov);
        CHECK(tc.water.owner == dryOwn);
        CHECK(tc.water.bodies.empty());   // a brush's body with no water left is gone
    }
    SUBCASE("half strength")
    {
        REQUIRE(water::addCircle(tc, b, 0.0f, 0.0f, 8.0f, 6.0f, 0.5f).changed > 0);
        water::removeCircle(tc, 0.0f, 0.0f, 8.0f, 6.0f, 0.5f);
        CHECK(tc.water.coverage == dryCov);
        CHECK(tc.water.owner == dryOwn);
    }
}

TEST_CASE("Water: repeated partial dabs build up toward full without overshooting")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t b = tc.water.createBody(0.0f);
    int prev = 0;
    for (int i = 0; i < 12; ++i)
    {
        water::addCircle(tc, b, 0.0f, 0.0f, 4.0f, 0.0f, 0.4f);
        const int c = covAt(tc, 0.5f, 0.5f);
        CHECK(c >= prev);
        CHECK(c <= 255);
        prev = c;
    }
    CHECK(prev >= 250);
}

TEST_CASE("Water: circle arguments the model refuses, and a circle half off the terrain")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t b = tc.water.createBody(0.0f);
    CHECK_FALSE(water::addCircle(tc, 77, 0, 0, 5, 1).ok);                 // unknown body
    CHECK_FALSE(water::addCircle(tc, water::kNoBody, 0, 0, 5, 1).ok);
    CHECK_FALSE(water::addCircle(tc, b, 0, 0, 0, 0).ok);                  // no extent
    CHECK_FALSE(water::addCircle(tc, b, 0, 0, 5, 1, 0.0f).ok);            // no strength
    CHECK_FALSE(water::addCircle(tc, b, std::nanf(""), 0, 5, 1).ok);
    CHECK_FALSE(water::removeCircle(tc, 0, 0, 5, 1, 1.0f, 99).ok);        // unknown filter body
    TerrainComponent flat = makeTerrain();
    flat.sizeX = 0.0f;
    const uint16_t fb = flat.water.createBody(0.0f);
    CHECK_FALSE(water::addCircle(flat, fb, 0, 0, 5, 1).ok);                // a terrain with no area
    CHECK(tc.water.revision == 0);                                         // nothing above touched it

    // Half off the +x edge: the part on the terrain is painted, the rest ignored.
    const water::Result r = water::addCircle(tc, b, 32.0f, 0.0f, 5.0f, 0.0f);
    CHECK(r.ok);
    CHECK(r.changed > 20);
    CHECK(covAt(tc, 30.5f, 0.5f) == 255);
    CHECK(covAt(tc, 31.5f, 0.5f) == 255);
    // Entirely off: a legal ask that changes nothing.
    const water::Result off = water::addCircle(tc, b, 500.0f, 0.0f, 5.0f, 0.0f);
    CHECK(off.ok);
    CHECK(off.changed == 0);
}

// ── Brush and lake on one model ──────────────────────────────────────────────

TEST_CASE("Water: a brush extends a lake and never takes a cell from it")
{
    TerrainComponent tc = makeTerrain();
    const HE::UUID spline = HE::UUID::generate();
    const uint16_t lake = tc.water.createBody(5.0f, spline);
    const uint16_t pond = tc.water.createBody(1.0f);                     // quellenlos
    REQUIRE(water::addPolygon(tc, lake, rect(-10, -10, 10, 10)).ok);

    CHECK(tc.water.findBySource(spline) != nullptr);
    CHECK(tc.water.findBySource(spline)->id == lake);
    CHECK_FALSE(tc.water.findBody(pond)->fromSpline());

    // A dab straddling the lake's +x shore.
    REQUIRE(water::addCircle(tc, pond, 10.0f, 0.0f, 4.0f, 0.0f).ok);
    CHECK(water::bodyAt(tc, 9.0f, 0.5f)  == lake);    // inside the lake: still its water
    CHECK(water::bodyAt(tc, 12.0f, 0.5f) == pond);    // beyond the shore: the brush's own
    float level = 0.0f;
    REQUIRE(water::levelAt(tc, 9.0f, 0.5f, level));
    CHECK(level == 5.0f);
    REQUIRE(water::levelAt(tc, 12.0f, 0.5f, level));
    CHECK(level == 1.0f);
    CHECK_FALSE(water::levelAt(tc, -25.0f, 0.5f, level));
}

TEST_CASE("Water: a lake polygon is authoritative over painted water, but only where it really covers")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t pond = tc.water.createBody(1.0f);
    const uint16_t lake = tc.water.createBody(5.0f, HE::UUID::generate());
    water::addCircle(tc, pond, 0.0f, 0.0f, 5.0f, 0.0f);          // in the middle of where the lake goes
    const uint16_t farPond = tc.water.createBody(2.0f);
    water::addCircle(tc, farPond, 13.0f, 0.0f, 3.0f, 0.0f);          // reaches 10.5..15.5: grazes the lake's shore cell

    REQUIRE(water::addPolygon(tc, lake, rect(-10.25f, -10.25f, 10.25f, 10.25f)).ok);
    CHECK(water::bodyAt(tc, 0.0f, 0.0f) == lake);                 // taken over
    CHECK(tc.water.wetCells(pond) == 0u);
    CHECK(water::bodyAt(tc, 10.5f, 0.5f) == farPond);                 // a quarter cell of polygon: not enough
    CHECK(covAt(tc, 10.5f, 0.5f) == 255);                         // and its water was left alone
    CHECK(tc.water.owner[cellIndex(tc, -10.5f, 0.5f)] == lake);   // a dry shore cell takes the grazed quarter
    CHECK(covAt(tc, -10.5f, 0.5f) == 64);

    CHECK(water::pruneEmptyBodies(tc) == 1u);                     // pond: no water, no source
    CHECK(tc.water.findBody(pond) == nullptr);
    CHECK(tc.water.findBody(lake) != nullptr);
}

TEST_CASE("Water: reshaping a lake keeps its id, level and source")
{
    TerrainComponent tc = makeTerrain();
    const HE::UUID spline = HE::UUID::generate();
    const uint16_t lake = tc.water.createBody(7.5f, spline);
    REQUIRE(water::addPolygon(tc, lake, rect(-10, -10, 10, 10)).ok);
    CHECK(tc.water.wetCells(lake) == 400u);

    const water::Result cleared = water::clearBody(tc, lake);
    CHECK(cleared.ok);
    CHECK(cleared.changed == 400u);
    CHECK(tc.water.wetCells() == 0u);
    REQUIRE(tc.water.findBody(lake) != nullptr);                // the lake stays, its spline can bring it back

    REQUIRE(water::addPolygon(tc, lake, rect(-5, -5, 5, 5)).ok);
    CHECK(tc.water.wetCells(lake) == 100u);
    CHECK(tc.water.findBySource(spline)->id == lake);
    CHECK(tc.water.findBody(lake)->level == 7.5f);

    CHECK(water::setLevel(tc, lake, 9.0f));
    CHECK(tc.water.findBody(lake)->level == 9.0f);
    CHECK_FALSE(water::setLevel(tc, lake, std::nanf("")));
    CHECK_FALSE(water::setLevel(tc, 999, 1.0f));
}

TEST_CASE("Water: carving a polygon or a circle out of a lake")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t lake = tc.water.createBody(5.0f, HE::UUID::generate());
    const uint16_t pond = tc.water.createBody(1.0f);
    water::addPolygon(tc, lake, rect(-10, -10, 10, 10));
    water::addCircle(tc, pond, 20.0f, 20.0f, 3.0f, 0.0f);

    SUBCASE("polygon")
    {
        const water::Result r = water::removePolygon(tc, rect(-2, -2, 2, 2));
        CHECK(r.changed == 16u);
        CHECK(water::bodyAt(tc, 0.5f, 0.5f) == water::kNoBody);
        CHECK(water::bodyAt(tc, 5.0f, 5.0f) == lake);
        CHECK(tc.water.wetCells(lake) == 400u - 16u);
    }
    SUBCASE("a body filter leaves the others alone")
    {
        const water::Result r = water::removePolygon(tc, rect(-30, -30, 30, 30), pond);
        CHECK(r.changed > 0);
        CHECK(tc.water.wetCells(lake) == 400u);
        CHECK(tc.water.findBody(pond) == nullptr);              // emptied and sourceless: pruned
        const water::Result none = water::removeCircle(tc, 0.0f, 0.0f, 50.0f, 0.0f, 1.0f, pond + 5);
        CHECK_FALSE(none.ok);
    }
    SUBCASE("a lake emptied by the eraser keeps its body")
    {
        water::removeCircle(tc, 0.0f, 0.0f, 20.0f, 0.0f);
        CHECK(tc.water.wetCells(lake) == 0u);
        CHECK(tc.water.findBody(lake) != nullptr);
    }
}

TEST_CASE("Water: body ids are stable and do not come back")
{
    water::Field f;
    const uint16_t a = f.createBody(1.0f);
    const uint16_t b = f.createBody(2.0f);
    const uint16_t c = f.createBody(3.0f);
    CHECK((a != b && b != c && a != c));

    TerrainComponent tc = makeTerrain();
    tc.water = f;
    REQUIRE(water::removeBody(tc, b).ok);
    CHECK(tc.water.findBody(a)->level == 1.0f);                 // neighbours untouched
    CHECK(tc.water.findBody(c)->level == 3.0f);
    const uint16_t d = tc.water.createBody(4.0f);
    CHECK(d != b);                                              // a stale handle to b never means d
    CHECK_FALSE(water::removeBody(tc, 999).ok);

    // After the counter wraps it walks past ids that are in use.
    water::Field w;
    w.nextBodyId = 65535;
    const uint16_t last = w.createBody(0.0f);
    CHECK(last == 65535);
    const uint16_t wrapped = w.createBody(0.0f);
    CHECK(wrapped == 1);
    CHECK(w.createBody(0.0f) == 2);
}

TEST_CASE("Water: resolution changes keep the shape")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t lake = tc.water.createBody(2.0f);
    water::addPolygon(tc, lake, rect(-10, -10, 10, 10));
    REQUIRE(tc.water.wetCells(lake) == 400u);

    water::setResolution(tc, 32);                               // 2 m cells, still aligned
    CHECK(tc.water.res == 32u);
    CHECK(tc.water.coverage.size() == 32u * 32u);
    CHECK(tc.water.wetCells(lake) == 100u);
    CHECK(water::bodyAt(tc, 5.0f, 5.0f) == lake);
    CHECK(water::bodyAt(tc, 15.0f, 5.0f) == water::kNoBody);

    water::setResolution(tc, 128);                              // 0.5 m cells
    CHECK(tc.water.wetCells(lake) == 1600u);

    // Out-of-range asks are clamped, not refused (on an empty field, so no 4096² grid is built).
    TerrainComponent bare = makeTerrain();
    water::setResolution(bare, 99999);
    CHECK(bare.water.res == water::kMaxRes);
    CHECK(bare.water.coverage.empty());
    water::setResolution(bare, 0);                              // 0 reads as "the default"
    CHECK(bare.water.res == water::kDefaultRes);
}

// ── Chunks and dirtiness ─────────────────────────────────────────────────────

TEST_CASE("Water: chunk cell rectangles partition the grid, whatever the resolution")
{
    for (uint32_t res : { 64u, 10u, 37u, 256u })
        for (uint32_t chunks : { 1u, 2u, 4u, 8u })
        {
            TerrainComponent tc = makeTerrain(64.0f, res);
            REQUIRE(water::ensureGrid(tc));
            std::vector<int> seen(static_cast<size_t>(res) * res, 0);
            for (uint32_t cz = 0; cz < chunks; ++cz)
                for (uint32_t cx = 0; cx < chunks; ++cx)
                {
                    const water::CellRect r = water::chunkCellRect(tc, chunks, cx, cz);
                    if (!r.valid()) continue;               // a chunk smaller than a cell
                    for (int z = r.z0; z <= r.z1; ++z)
                        for (int x = r.x0; x <= r.x1; ++x) ++seen[static_cast<size_t>(z) * res + x];
                }
            for (int n : seen) CHECK(n == 1);
        }

    TerrainComponent tc = makeTerrain();
    REQUIRE(water::ensureGrid(tc));
    const water::CellRect r = water::chunkCellRect(tc, 4, 1, 2);
    CHECK(r.x0 == 16); CHECK(r.x1 == 31); CHECK(r.z0 == 32); CHECK(r.z1 == 47);
    CHECK_FALSE(water::chunkCellRect(tc, 4, 4, 0).valid());
    CHECK_FALSE(water::chunkCellRect(tc, 0, 0, 0).valid());
}

TEST_CASE("Water: a chunk can ask whether it has any water")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t b = tc.water.createBody(0.0f);
    water::addCircle(tc, b, -20.0f, -20.0f, 3.0f, 0.0f);       // chunk (0, 0) of 4 × 4
    CHECK(water::anyWater(tc, water::chunkCellRect(tc, 4, 0, 0)));
    CHECK(water::anyWater(tc, water::chunkCellRect(tc, 4, 0, 0), true));
    CHECK_FALSE(water::anyWater(tc, water::chunkCellRect(tc, 4, 3, 3)));
    CHECK_FALSE(water::anyWater(tc, water::chunkCellRect(tc, 4, 1, 0)));

    const water::CellRect box = water::cellRect(tc, -25.0f, -25.0f, -15.0f, -15.0f);
    CHECK(box.valid());
    CHECK(water::anyWater(tc, box));
    CHECK_FALSE(water::cellRect(tc, 100.0f, 100.0f, 200.0f, 200.0f).valid());
    CHECK(water::cellRect(tc, 0.0f, 0.0f, 0.0f, 0.0f).valid());   // a point is one cell
}

TEST_CASE("Water: an edit marks only the water dirty, over the rectangle it touched")
{
    TerrainComponent tc = makeTerrain();
    tc.dirty = false;
    tc.regionDirty = false;
    const uint16_t b = tc.water.createBody(0.0f);
    CHECK_FALSE(tc.water.dirty);

    REQUIRE(water::addCircle(tc, b, 10.0f, -5.0f, 3.0f, 0.0f).changed > 0);
    CHECK(tc.water.dirty);
    CHECK(tc.water.revision == 1u);
    CHECK(tc.water.dirtyMinX == doctest::Approx(7.0f));     // the cells whose centre is within 3 m
    CHECK(tc.water.dirtyMaxX == doctest::Approx(13.0f));
    CHECK(tc.water.dirtyMinZ == doctest::Approx(-8.0f));
    CHECK(tc.water.dirtyMaxZ == doctest::Approx(-2.0f));
    // Terrain chunks are not rebuilt for it.
    CHECK_FALSE(tc.dirty);
    CHECK_FALSE(tc.regionDirty);

    // A second edit widens the rectangle.
    water::addCircle(tc, b, -20.0f, 20.0f, 3.0f, 0.0f);
    CHECK(tc.water.dirtyMinX < -16.0f);
    CHECK(tc.water.dirtyMaxX > 12.0f);
    CHECK(tc.water.dirtyMaxZ > 22.0f);
    CHECK(tc.water.revision == 2u);

    // Edits that change nothing leave the revision alone.
    const uint64_t rev = tc.water.revision;
    water::removeCircle(tc, 0.0f, 0.0f, 2.0f, 0.0f);           // dry ground
    water::addCircle(tc, b, 10.0f, -5.0f, 3.0f, 0.0f);         // already full
    CHECK(tc.water.revision == rev);
}

// ── Scene format ─────────────────────────────────────────────────────────────

namespace
{
    water::Field authoredField(TerrainComponent& tc)
    {
        const uint16_t lake  = tc.water.createBody(6.25f, HE::UUID::generate());
        const uint16_t pond  = tc.water.createBody(-1.5f);
        const uint16_t gone  = tc.water.createBody(0.0f);
        water::addPolygon(tc, lake, uShape());
        water::addCircle(tc, pond, 25.0f, 25.0f, 4.0f, 3.0f, 0.8f);
        water::removeBody(tc, gone);                              // a hole in the id sequence
        return tc.water;
    }

    HorizonWorld* makeWorld(HorizonWorld& world, TerrainComponent tc)
    {
        const auto e = world.createEntity("Land");
        world.registry().emplace<TerrainComponent>(e, tc);
        return &world;
    }
}

TEST_CASE("Water: save and load round-trip the field, as a file and as an undo snapshot")
{
    TerrainComponent tc = makeTerrain(64.0f, 48);                 // not the default resolution
    const water::Field authored = authoredField(tc);
    REQUIRE(authored.wetCells() > 0);
    REQUIRE(authored.bodies.size() == 2);

    HorizonWorld world;
    makeWorld(world, tc);
    SceneSerializer ser;

    SUBCASE("JSON file")
    {
        const fs::path file = fs::temp_directory_path() / "he_test_water_field.hescene";
        REQUIRE(ser.save(world, file, SerializeFormat::JSON));
        HorizonWorld loaded;
        REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
        he_test::removeQuiet(file);
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        CHECK(water::sameContent(t->water, authored));
        CHECK(t->water.res == 48u);
        CHECK(t->water.bodies[0].sourceSpline == authored.bodies[0].sourceSpline);
        CHECK(t->water.bodies[0].level == 6.25f);
        CHECK(t->water.bodies[1].level == -1.5f);
        CHECK_FALSE(t->water.bodies[1].fromSpline());
        // The gap in the ids and the counter survive: id 3 stays retired.
        CHECK(t->water.nextBodyId == authored.nextBodyId);
        // Loaded water is dirty over the whole terrain: the surface is rebuilt, never saved.
        CHECK(t->water.dirty);
        CHECK(t->water.dirtyMinX == doctest::Approx(-32.0f));
        CHECK(t->water.dirtyMaxZ == doctest::Approx(32.0f));
    }
    SUBCASE("binary / CBOR, the undo snapshot path")
    {
        std::vector<uint8_t> blob;
        REQUIRE(ser.saveToMemory(world, blob));
        HorizonWorld loaded;
        REQUIRE(ser.loadFromMemory(loaded, blob));
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        CHECK(water::sameContent(t->water, authored));
    }
}

TEST_CASE("Water: a landscape without water saves exactly as it did before there was water")
{
    HorizonWorld world;
    TerrainComponent plain;                       // the default resolution: nothing about water to say
    plain.sizeX = plain.sizeZ = 64.0f;
    makeWorld(world, plain);
    const fs::path file = fs::temp_directory_path() / "he_test_water_none.hescene";
    SceneSerializer ser;
    REQUIRE(ser.save(world, file, SerializeFormat::JSON));
    std::ifstream in(file);
    std::stringstream text; text << in.rdbuf();
    he_test::removeQuiet(file);
    CHECK(text.str().find("water") == std::string::npos);
    CHECK(text.str().find("\"terrain\"") != std::string::npos);

    // And an old file with no water keys loads with a pristine field.
    HorizonWorld loaded;
    REQUIRE(ser.loadFromMemory(loaded, [&] { std::vector<uint8_t> b; ser.saveToMemory(world, b); return b; }()));
    const TerrainComponent* t = firstTerrain(loaded);
    REQUIRE(t != nullptr);
    CHECK(t->water.pristine());
    CHECK_FALSE(t->water.dirty);
}

namespace
{
    // The "terrain" object of the first entity that has one, in a parsed scene.
    nlohmann::json* findTerrainJson(nlohmann::json& j)
    {
        if (j.is_object())
        {
            auto it = j.find("terrain");
            if (it != j.end() && it->is_object()) return &*it;
            for (auto& kv : j.items())
                if (auto* r = findTerrainJson(kv.value())) return r;
        }
        else if (j.is_array())
            for (auto& v : j)
                if (auto* r = findTerrainJson(v)) return r;
        return nullptr;
    }

    // Save `tc` as a scene file, let `damage` edit its terrain block, load it back.
    template <class F>
    TerrainComponent loadDamaged(const TerrainComponent& tc, F damage)
    {
        HorizonWorld world;
        makeWorld(world, tc);
        const fs::path file = fs::temp_directory_path() / "he_test_water_damaged.hescene";
        SceneSerializer ser;
        REQUIRE(ser.save(world, file, SerializeFormat::JSON));
        nlohmann::json j;
        { std::ifstream in(file); in >> j; }
        nlohmann::json* terrain = findTerrainJson(j);
        REQUIRE(terrain != nullptr);
        damage(*terrain);
        { std::ofstream out(file, std::ios::trunc); out << j.dump(1); }
        HorizonWorld loaded;
        REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
        he_test::removeQuiet(file);
        const TerrainComponent* t = firstTerrain(loaded);
        REQUIRE(t != nullptr);
        return *t;
    }
}

TEST_CASE("Water: a damaged water block loads into something safe")
{
    TerrainComponent tc = makeTerrain();
    authoredField(tc);
    const uint32_t wet = tc.water.wetCells();
    REQUIRE(wet > 0);

    SUBCASE("a cell stream cut short drops the cells and keeps the bodies")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) {
            std::string b64 = terrain["waterCellsB64"].get<std::string>();
            terrain["waterCellsB64"] = b64.substr(0, b64.size() / 2);
        });
        CHECK(t.water.wetCells() == 0u);
        CHECK(t.water.bodies.size() == 2);          // the lake can be re-rasterised from its spline
        CHECK(t.water.coverage.empty());
    }
    SUBCASE("cells that name a body that is not there are dried")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) {
            nlohmann::json bodies = nlohmann::json::array();
            bodies.push_back(terrain["waterBodies"][1]);          // keep only the brush body
            terrain["waterBodies"] = bodies;
        });
        REQUIRE(t.water.bodies.size() == 1);
        for (size_t i = 0; i < t.water.coverage.size(); ++i)
            CHECK((t.water.coverage[i] > 0) == (t.water.owner[i] != 0));
        CHECK(t.water.wetCells(tc.water.bodies[0].id) == 0u);
        CHECK(t.water.wetCells() > 0);                            // the pond is still there
    }
    SUBCASE("bad bodies are dropped, good ones kept")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) {
            auto& bodies = terrain["waterBodies"];
            bodies.push_back({ { "id", 0 }, { "level", 1.0f } });                       // id 0
            bodies.push_back({ { "id", 70000 }, { "level", 1.0f } });                   // out of range
            bodies.push_back({ { "id", bodies[0]["id"] }, { "level", 9.0f } });          // a repeated id
            bodies.push_back({ { "id", 41 }, { "level", "deep" } });                    // level not a number: 0
            bodies.push_back("not a body");
        });
        REQUIRE(t.water.bodies.size() == 3);
        CHECK(t.water.bodies[0].level == 6.25f);                  // first of the duplicates wins
        CHECK(t.water.findBody(41) != nullptr);
        CHECK(t.water.nextBodyId > 41);
    }
    SUBCASE("a resolution that does not fit the cells drops the cells")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) { terrain["waterRes"] = 31; });
        CHECK(t.water.res == 31u);
        CHECK(t.water.wetCells() == 0u);
        CHECK(t.water.bodies.size() == 2);
    }
    SUBCASE("a resolution out of range is clamped")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) {
            terrain["waterRes"] = 1000000;
            terrain.erase("waterCellsB64");
        });
        CHECK(t.water.res == water::kMaxRes);
        CHECK(t.water.coverage.empty());
    }
    SUBCASE("rubbish where numbers belong does not take the scene down")
    {
        const TerrainComponent t = loadDamaged(tc, [](nlohmann::json& terrain) {
            terrain["waterBodies"] = "nope";
            terrain["waterCellsB64"] = "@@@@";
        });
        CHECK(t.water.bodies.empty());
        CHECK(t.water.wetCells() == 0u);
    }
}

TEST_CASE("Water: the cell stream is small for a mostly dry grid and exact for a busy one")
{
    TerrainComponent tc = makeTerrain(64.0f, 256);
    const uint16_t lake = tc.water.createBody(1.0f);
    water::addPolygon(tc, lake, uShape());
    const std::vector<uint8_t> bytes = water::encodeCells(tc.water);
    CHECK_FALSE(bytes.empty());
    CHECK(bytes.size() < tc.water.coverage.size() / 8);           // a fraction of the 64 KiB raster alone

    water::Field back;
    back.res = 256;
    REQUIRE(water::decodeCells(back, bytes.data(), bytes.size()));
    CHECK(back.coverage == tc.water.coverage);
    CHECK(back.owner == tc.water.owner);

    // Noise: every cell different, still exact.
    water::Field noisy;
    noisy.res = 16;
    noisy.coverage.resize(256); noisy.owner.resize(256);
    for (size_t i = 0; i < 256; ++i) { noisy.coverage[i] = static_cast<uint8_t>(i * 37 + 1); noisy.owner[i] = static_cast<uint16_t>(i % 5 + 1); }
    const std::vector<uint8_t> nb = water::encodeCells(noisy);
    water::Field nback;
    nback.res = 16;
    REQUIRE(water::decodeCells(nback, nb.data(), nb.size()));
    CHECK(nback.coverage == noisy.coverage);
    CHECK(nback.owner == noisy.owner);

    SUBCASE("a dry or unallocated grid has no stream")
    {
        water::Field dry;
        CHECK(water::encodeCells(dry).empty());
        dry.res = 8; dry.coverage.assign(64, 0); dry.owner.assign(64, 0);
        CHECK(water::encodeCells(dry).empty());
    }
    SUBCASE("streams that do not fill the grid are refused")
    {
        water::Field f; f.res = 16;
        CHECK_FALSE(water::decodeCells(f, nb.data(), nb.size() - 1));          // truncated
        CHECK(f.coverage.empty());
        std::vector<uint8_t> longer = nb;
        longer.push_back(1); longer.push_back(0); longer.push_back(0); longer.push_back(255);
        CHECK_FALSE(water::decodeCells(f, longer.data(), longer.size()));      // one cell too many
        const std::vector<uint8_t> huge = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x01, 0, 0, 0 };   // a count past 2³²
        CHECK_FALSE(water::decodeCells(f, huge.data(), huge.size()));
        const std::vector<uint8_t> zero = { 0x00, 0, 0, 0 };                   // a run of zero cells
        CHECK_FALSE(water::decodeCells(f, zero.data(), zero.size()));
    }
}

// ── Undo ─────────────────────────────────────────────────────────────────────

TEST_CASE("Water: undo, which is a world snapshot, restores the water exactly")
{
    // EditorUndo stores the CBOR of the whole world before each operation and
    // loads it back on Ctrl+Z. The water rides along through the serializer.
    HorizonWorld world;
    TerrainComponent tc = makeTerrain();
    makeWorld(world, tc);
    SceneSerializer ser;

    std::vector<uint8_t> s0, s1, s2;
    REQUIRE(ser.saveToMemory(world, s0));                         // before any water

    TerrainComponent* live = firstTerrain(world);
    REQUIRE(live != nullptr);
    const uint16_t lake = live->water.createBody(5.0f, HE::UUID::generate());
    water::addPolygon(*live, lake, uShape());
    const water::Field afterLake = live->water;
    REQUIRE(ser.saveToMemory(world, s1));                         // after the lake

    const uint16_t pond = live->water.createBody(1.0f);
    water::addCircle(*live, pond, 25.0f, 0.0f, 5.0f, 2.0f);
    water::removeCircle(*live, -15.0f, 0.0f, 3.0f, 0.0f);
    const water::Field afterBrush = live->water;
    REQUIRE(ser.saveToMemory(world, s2));
    CHECK_FALSE(water::sameContent(afterLake, afterBrush));

    auto restore = [&](const std::vector<uint8_t>& snap) {
        HorizonWorld w;
        REQUIRE(ser.loadFromMemory(w, snap));
        const TerrainComponent* t = firstTerrain(w);
        REQUIRE(t != nullptr);
        return t->water;
    };
    CHECK(water::sameContent(restore(s1), afterLake));            // undo the brush
    {
        const water::Field none = restore(s0);                    // undo the lake
        CHECK(none.bodies.empty());
        CHECK(none.coverage.empty());
        CHECK(none.wetCells() == 0u);
    }
    CHECK(water::sameContent(restore(s2), afterBrush));           // redo
    CHECK(restore(s1).bodies.size() == 1);
}

TEST_CASE("Water: a delta takes a stroke back and forward without the world")
{
    TerrainComponent tc = makeTerrain();
    const uint16_t lake = tc.water.createBody(5.0f, HE::UUID::generate());
    water::addPolygon(tc, lake, rect(-10, -10, 10, 10));
    const water::Field before = tc.water;

    // One stroke: a new body, a dab that crosses the lake, an erase, a level change.
    const uint16_t pond = tc.water.createBody(1.0f);
    water::addCircle(tc, pond, 14.0f, 0.0f, 5.0f, 2.0f);
    water::removeCircle(tc, -10.0f, -10.0f, 4.0f, 0.0f);
    water::setLevel(tc, lake, 6.0f);
    const water::Field after = tc.water;
    REQUIRE_FALSE(water::sameContent(before, after));

    water::Delta d;
    REQUIRE(water::makeDelta(before, after, d));
    CHECK_FALSE(d.empty());
    size_t differing = 0;
    for (size_t i = 0; i < before.coverage.size(); ++i)
        differing += (before.coverage[i] != after.coverage[i] || before.owner[i] != after.owner[i]) ? 1 : 0;
    CHECK(d.cells.size() == differing);                           // sparse: only what moved
    CHECK(d.cells.size() < before.coverage.size() / 4);

    REQUIRE(water::revertDelta(tc, d));
    CHECK(water::sameContent(tc.water, before));
    CHECK(tc.water.findBody(pond) == nullptr);
    CHECK(tc.water.findBody(lake)->level == 5.0f);
    CHECK(tc.water.dirty);

    REQUIRE(water::applyDelta(tc, d));
    CHECK(water::sameContent(tc.water, after));

    SUBCASE("a stroke that changed nothing is an empty delta")
    {
        water::Delta none;
        REQUIRE(water::makeDelta(after, after, none));
        CHECK(none.empty());
        CHECK(none.cells.empty());
    }
    SUBCASE("a delta onto a field it was not made for is refused untouched")
    {
        TerrainComponent other = makeTerrain(64.0f, 32);
        other.water.createBody(0.0f);
        const water::Field copy = other.water;
        CHECK_FALSE(water::applyDelta(other, d));
        CHECK(water::sameContent(other.water, copy));

        water::Delta bad = d;
        bad.cells.push_back({ 64u * 64u + 5u, 0, 1, 0, 255 });     // a cell outside the grid
        const water::Field now = tc.water;
        CHECK_FALSE(water::revertDelta(tc, bad));
        CHECK(water::sameContent(tc.water, now));
    }
    SUBCASE("a resample is not a stroke")
    {
        water::Field coarse = before;
        TerrainComponent t2 = makeTerrain();
        t2.water = coarse;
        water::setResolution(t2, 32);
        water::Delta none;
        CHECK_FALSE(water::makeDelta(before, t2.water, none));
    }
    SUBCASE("onto an unallocated grid")
    {
        TerrainComponent empty = makeTerrain();
        water::Delta fromNothing;
        REQUIRE(water::makeDelta(empty.water, before, fromNothing));
        REQUIRE(water::applyDelta(empty, fromNothing));
        CHECK(water::sameContent(empty.water, before));
        REQUIRE(water::revertDelta(empty, fromNothing));
        CHECK(empty.water.res == 64u);
        CHECK(empty.water.wetCells() == 0u);
        CHECK(empty.water.bodies.empty());
    }
}

TEST_CASE("Water: sampling the field")
{
    TerrainComponent tc = makeTerrain();
    CHECK(water::coverageAt(tc, 0.0f, 0.0f) == 0.0f);             // no grid yet
    CHECK(water::bodyAt(tc, 0.0f, 0.0f) == water::kNoBody);
    const uint16_t b = tc.water.createBody(2.0f);
    water::addPolygon(tc, b, rect(-10, -10, 10, 10));
    CHECK(water::coverageAt(tc, 0.0f, 0.0f) == doctest::Approx(1.0f));
    CHECK(water::coverageAt(tc, 30.0f, 0.0f) == 0.0f);
    CHECK(water::coverageAt(tc, 500.0f, 0.0f) == 0.0f);            // off the terrain
    // Bilinear: halfway between a wet and a dry cell centre.
    CHECK(water::coverageAt(tc, 10.0f, 0.0f) == doctest::Approx(0.5f).epsilon(0.02));
    CHECK(water::coverageAt(tc, std::nanf(""), 0.0f) == 0.0f);
    CHECK(water::cellCenter(tc, 0, 0) == glm::vec2(-31.5f, -31.5f));
    CHECK(water::cellSize(tc) == glm::vec2(1.0f, 1.0f));
}

TEST_CASE("Water: clearAll puts a terrain back to no water")
{
    TerrainComponent tc = makeTerrain(64.0f, 32);
    const uint16_t b = tc.water.createBody(2.0f);
    water::addCircle(tc, b, 0.0f, 0.0f, 5.0f, 0.0f);
    REQUIRE_FALSE(tc.water.pristine());
    water::clearAll(tc);
    CHECK(tc.water.pristine());
    CHECK(tc.water.res == water::kDefaultRes);
    CHECK(tc.water.dirty);                                         // the surface has to go too
}
