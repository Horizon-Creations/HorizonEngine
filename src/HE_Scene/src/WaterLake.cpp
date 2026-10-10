#include "HorizonScene/WaterLake.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/SplineCurve.h"
#include "HorizonScene/TerrainMeshGenerator.h"   // terrainHeightAt
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/SplineComponent.h"
#include "HorizonScene/Components/TransformComponent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>

namespace HE::water::lake
{

namespace
{
    bool hasArea(const TerrainComponent& tc)
    {
        return std::isfinite(tc.sizeX) && std::isfinite(tc.sizeZ) && tc.sizeX > 0.0f && tc.sizeZ > 0.0f;
    }

    bool finite(float v) { return std::isfinite(v); }

    // At least three numbers-only vertices; whether they enclose anything is the
    // rasteriser's to say.
    bool validPolygon(const std::vector<glm::vec2>& p)
    {
        if (p.size() < 3 || p.size() > kMaxPolygonPoints) return false;
        for (const glm::vec2& v : p) if (!finite(v.x) || !finite(v.y)) return false;
        return true;
    }

    // Coverage of `r` at cell (cx, cz), 0 outside its rectangle.
    uint8_t rasterAt(const Raster& r, int cx, int cz)
    {
        const int k = cx - r.x0, j = cz - r.z0;
        if (r.empty() || k < 0 || j < 0 || k >= r.w || j >= r.h) return 0;
        return r.coverage[static_cast<size_t>(j) * r.w + k];
    }

    // The cells an edit changed: a count and the box, as Touched in WaterField.cpp.
    struct Changes
    {
        uint32_t count = 0;
        int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        void add(int x, int z)
        {
            if (count++ == 0) { x0 = x1 = x; z0 = z1 = z; return; }
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            z0 = std::min(z0, z); z1 = std::max(z1, z);
        }
    };

    // The state of a cell before an edit.
    struct Before { size_t i; uint8_t cov; uint16_t owner; };

    // ── The largest piece of a body ───────────────────────────────────────────
    struct Piece
    {
        bool     ok = false;
        Ring     ring;             // terrain-local XZ, counter-clockwise
        uint32_t pieces = 0, islands = 0;
    };

    Piece largestPiece(const TerrainComponent& tc, uint16_t body)
    {
        Piece out;
        if (!tc.water.findBody(body) || !tc.water.allocated() || !hasArea(tc)) return out;
        const CellRect cells = bodyCells(tc, body);
        if (!cells.valid()) return out;
        Lattice lat;
        if (!buildLattice(tc, body, cells, lat)) return out;
        const std::vector<Polygon> polys = groupContours(extractContours(lat));
        out.pieces = static_cast<uint32_t>(polys.size());
        double best = 0.0;
        const Polygon* pick = nullptr;
        for (const Polygon& p : polys)
        {
            const double a = std::abs(signedArea(p.outer));
            if (a > best) { best = a; pick = &p; }
        }
        if (!pick || pick->outer.size() < 3) return out;
        out.ring = pick->outer;
        if (signedArea(out.ring) < 0.0) std::reverse(out.ring.begin(), out.ring.end());
        out.islands = static_cast<uint32_t>(pick->holes.size());
        out.ok = true;
        return out;
    }

    // ── Douglas–Peucker on a closed ring ─────────────────────────────────────
    double distToSegment(const glm::dvec2& p, const glm::dvec2& a, const glm::dvec2& b)
    {
        const glm::dvec2 ab = b - a;
        const double len2 = ab.x * ab.x + ab.y * ab.y;
        double t = 0.0;
        if (len2 > 0.0) t = std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0, 1.0);
        const glm::dvec2 d = p - (a + ab * t);
        return std::sqrt(d.x * d.x + d.y * d.y);
    }

    // Marks the points of ring[lo..hi] (inclusive, indices modulo the ring) that
    // stay, between two kept ends.
    void dpMark(const Ring& r, size_t lo, size_t hi, double tol, std::vector<char>& keep)
    {
        // Iterative: a long shore must not recurse thousands deep.
        std::vector<std::pair<size_t, size_t>> stack{ { lo, hi } };
        const size_t n = r.size();
        while (!stack.empty())
        {
            auto [a, b] = stack.back();
            stack.pop_back();
            if (b <= a + 1) continue;
            double worst = -1.0;
            size_t at = a;
            for (size_t k = a + 1; k < b; ++k)
            {
                const double d = distToSegment(r[k % n], r[a % n], r[b % n]);
                if (d > worst) { worst = d; at = k; }
            }
            if (worst > tol)
            {
                keep[at % n] = 1;
                stack.push_back({ a, at });
                stack.push_back({ at, b });
            }
        }
    }

    Ring simplifyRing(const Ring& r, double tol)
    {
        const size_t n = r.size();
        if (n <= 3) return r;
        // Two anchors that cannot both be dropped: the first point and the one
        // farthest from it.
        size_t far0 = 0;
        double dmax = -1.0;
        for (size_t k = 1; k < n; ++k)
        {
            const glm::dvec2 d = r[k] - r[0];
            const double dd = d.x * d.x + d.y * d.y;
            if (dd > dmax) { dmax = dd; far0 = k; }
        }
        std::vector<char> keep(n, 0);
        keep[0] = 1; keep[far0] = 1;
        dpMark(r, 0, far0, tol, keep);
        dpMark(r, far0, n, tol, keep);    // second half wraps through index n % n == 0
        Ring out;
        for (size_t k = 0; k < n; ++k) if (keep[k]) out.push_back(r[k]);
        return out;
    }

    // ── Fingerprint of a spline and its landscape ─────────────────────────────
    uint64_t hashBytes(uint64_t h, const void* data, size_t n)
    {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    uint64_t fingerprint(HorizonWorld& world, Entity spline, Entity terrain, const SplineComponent& sc)
    {
        uint64_t h = 1469598103934665603ull;
        const glm::mat4 a = HE::worldMatrixOf(world, spline);
        const glm::mat4 b = HE::worldMatrixOf(world, terrain);
        h = hashBytes(h, &a, sizeof(a));
        h = hashBytes(h, &b, sizeof(b));
        const uint8_t closed = sc.closed ? 1 : 0;
        h = hashBytes(h, &closed, 1);
        if (!sc.controlPoints.empty())
            h = hashBytes(h, sc.controlPoints.data(), sc.controlPoints.size() * sizeof(glm::vec3));
        return h == 0 ? 1 : h;    // 0 is "not looked at yet"
    }
}

float polylineTolerance(const TerrainComponent& tc)
{
    const float cell = tc.sizeX / static_cast<float>(std::max<uint32_t>(1, tc.water.res));
    return std::clamp(cell * 0.1f, 0.02f, 0.5f);
}

// ── Pure ─────────────────────────────────────────────────────────────────────

float lowestGround(const TerrainComponent& tc, const std::vector<glm::vec2>& polygon)
{
    float lowest = std::numeric_limits<float>::max();
    for (const glm::vec2& p : polygon)
    {
        const float h = terrainHeightAt(tc, p.x, p.y);
        if (finite(h)) lowest = std::min(lowest, h);
    }
    return lowest == std::numeric_limits<float>::max() ? 0.0f : lowest;
}

Result reshape(TerrainComponent& tc, uint16_t id, const std::vector<glm::vec2>& polygon)
{
    Field& f = tc.water;
    Body* b = f.findBody(id);
    if (!b || !hasArea(tc) || !validPolygon(polygon)) return {};
    const Raster newR = rasterizePolygon(polygon, tc.sizeX, tc.sizeZ, f.res);
    if (newR.empty()) return {};
    if (!ensureGrid(tc)) return {};
    const Raster oldR = b->polygon.empty() ? Raster{}
                                           : rasterizePolygon(b->polygon, tc.sizeX, tc.sizeZ, f.res);

    const int res = static_cast<int>(f.res);
    const size_t n = f.coverage.size();
    std::vector<Before> before;                       // every cell this edit may move, once
    std::vector<uint8_t> seen(n, 0);
    auto note = [&](size_t i)
    {
        if (seen[i]) return;
        seen[i] = 1;
        before.push_back({ i, f.coverage[i], f.owner[i] });
    };

    // The brush share, read off the cells against the outline they were laid from.
    struct Keep { size_t i; uint8_t cov; };
    std::vector<Keep> added, erased;
    for (size_t i = 0; i < n; ++i)
    {
        if (f.owner[i] != id) continue;
        note(i);
        const uint8_t o = rasterAt(oldR, static_cast<int>(i % res), static_cast<int>(i / res));
        if (f.coverage[i] > o)      added.push_back({ i, f.coverage[i] });
        else if (f.coverage[i] < o) erased.push_back({ i, f.coverage[i] });
    }
    // Dry cells the old outline covered: a full erase. (A cell another body holds
    // is that body's and says nothing about this brush.)
    for (int j = 0; j < oldR.h; ++j)
        for (int k = 0; k < oldR.w; ++k)
        {
            if (oldR.coverage[static_cast<size_t>(j) * oldR.w + k] == 0) continue;
            const size_t i = static_cast<size_t>(oldR.z0 + j) * res + (oldR.x0 + k);
            if (f.owner[i] == kNoBody) erased.push_back({ i, 0 });
        }

    // The body's cells go, the new outline is laid (the polygon is authoritative
    // over a cell it covers properly), the brush share goes back on top.
    for (const Before& c : before) { f.coverage[c.i] = 0; f.owner[c.i] = kNoBody; }
    for (int j = 0; j < newR.h; ++j)
        for (int k = 0; k < newR.w; ++k)
        {
            const uint8_t p = newR.coverage[static_cast<size_t>(j) * newR.w + k];
            if (p == 0) continue;
            const size_t i = static_cast<size_t>(newR.z0 + j) * res + (newR.x0 + k);
            const uint16_t ow = f.owner[i];
            if (ow != kNoBody && ow != id && p < kWet) continue;
            note(i);
            f.coverage[i] = std::max(f.coverage[i], p);
            f.owner[i] = id;
        }
    for (const Keep& a : added)
    {
        if (f.owner[a.i] != kNoBody && f.owner[a.i] != id) continue;
        note(a.i);
        f.coverage[a.i] = std::max(f.coverage[a.i], a.cov);
        f.owner[a.i] = id;
    }
    for (const Keep& e : erased)
    {
        if (f.owner[e.i] != id) continue;
        f.coverage[e.i] = std::min(f.coverage[e.i], e.cov);
        if (f.coverage[e.i] == 0) f.owner[e.i] = kNoBody;
    }

    Changes ch;
    for (const Before& c : before)
        if (f.coverage[c.i] != c.cov || f.owner[c.i] != c.owner)
            ch.add(static_cast<int>(c.i % res), static_cast<int>(c.i / res));
    if (ch.count) markCellsDirty(tc, ch.x0, ch.z0, ch.x1, ch.z1);

    b->polygon = polygon;
    Result r;
    r.ok = true;
    r.changed = ch.count;
    return r;
}

uint16_t create(TerrainComponent& tc, const HE::UUID& spline, const std::vector<glm::vec2>& polygon, float level)
{
    if (spline == HE::UUID{} || !validPolygon(polygon) || !finite(level) || !hasArea(tc)) return kNoBody;
    if (const Body* have = tc.water.findBySource(spline))
    {
        const uint16_t id = have->id;
        if (!reshape(tc, id, polygon).ok) return kNoBody;
        HE::water::setLevel(tc, id, level);     // qualified: this namespace has a setLevel of its own
        return id;
    }
    if (rasterizePolygon(polygon, tc.sizeX, tc.sizeZ, std::max<uint32_t>(1, tc.water.res)).empty()) return kNoBody;
    const uint16_t id = tc.water.createBody(level, spline);
    if (id == kNoBody) return kNoBody;
    if (!reshape(tc, id, polygon).ok)
    {
        removeBody(tc, id);
        return kNoBody;
    }
    return id;
}

TerrainSculpt::Result dig(TerrainComponent& tc, uint16_t id, float depth, float bank)
{
    const Body* b = tc.water.findBody(id);
    if (!b || b->polygon.size() < 3 || !finite(depth) || !finite(bank)) return {};
    TerrainSculpt::ExcavateParams ep;
    ep.mode    = TerrainSculpt::ExcavateMode::Floor;
    ep.amount  = b->level - std::max(0.0f, depth);
    ep.falloff = std::max(0.0f, bank);
    return TerrainSculpt::excavatePolygon(tc, b->polygon, ep);
}

bool detach(TerrainComponent& tc, uint16_t id)
{
    Body* b = tc.water.findBody(id);
    if (!b) return false;
    b->sourceSpline = HE::UUID{};
    b->polygon.clear();
    b->syncKey = 0;
    return true;
}

Outline extractOutline(const TerrainComponent& tc, uint16_t body, float tolerance, uint32_t maxPoints)
{
    Outline out;
    const Piece piece = largestPiece(tc, body);
    out.pieces = piece.pieces;
    if (!piece.ok) return out;
    out.islands = piece.islands;

    const glm::vec2 cell = cellSize(tc);
    double tol = tolerance > 0.0f ? tolerance : std::max(cell.x, cell.y);
    maxPoints = std::clamp<uint32_t>(maxPoints, 3, 256);
    Ring ring = simplifyRing(piece.ring, tol);
    for (int guard = 0; ring.size() > maxPoints && guard < 64; ++guard)
    {
        tol *= 1.35;
        ring = simplifyRing(piece.ring, tol);
    }
    if (ring.size() < 3 || ring.size() > maxPoints) return out;
    out.tolerance = static_cast<float>(tol);
    out.points.reserve(ring.size());
    for (const glm::dvec2& p : ring) out.points.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
    out.ok = true;
    return out;
}

Result adopt(TerrainComponent& tc, uint16_t id, const HE::UUID& spline, const std::vector<glm::vec2>& polygon)
{
    Field& f = tc.water;
    Body* b = f.findBody(id);
    if (!b || b->fromSpline() || spline == HE::UUID{} || !validPolygon(polygon) || !hasArea(tc)) return {};
    if (rasterizePolygon(polygon, tc.sizeX, tc.sizeZ, f.res).empty()) return {};

    uint32_t dried = 0;
    const Piece piece = largestPiece(tc, id);
    if (piece.ok && f.allocated())
    {
        // The main piece, grown by a cell so its soft edge goes with it, is dried;
        // anything farther away is another piece and stays.
        std::vector<glm::vec2> raw;
        raw.reserve(piece.ring.size());
        for (const glm::dvec2& p : piece.ring) raw.emplace_back(static_cast<float>(p.x), static_cast<float>(p.y));
        const Raster mr = rasterizePolygon(raw, tc.sizeX, tc.sizeZ, f.res);
        const int res = static_cast<int>(f.res);
        Changes ch;
        for (int cz = std::max(0, mr.z0 - 1); !mr.empty() && cz <= std::min(res - 1, mr.z0 + mr.h); ++cz)
            for (int cx = std::max(0, mr.x0 - 1); cx <= std::min(res - 1, mr.x0 + mr.w); ++cx)
            {
                const size_t i = static_cast<size_t>(cz) * res + cx;
                if (f.owner[i] != id) continue;
                bool near = false;
                for (int dz = -1; dz <= 1 && !near; ++dz)
                    for (int dx = -1; dx <= 1 && !near; ++dx)
                        near = rasterAt(mr, cx + dx, cz + dz) != 0;
                if (!near) continue;
                f.coverage[i] = 0; f.owner[i] = kNoBody;
                ch.add(cx, cz);
            }
        dried = ch.count;
        if (ch.count) markCellsDirty(tc, ch.x0, ch.z0, ch.x1, ch.z1);
    }

    b->sourceSpline = spline;
    b->polygon.clear();       // nothing to compare against: the polygon is laid whole
    b->syncKey = 0;
    Result r = reshape(tc, id, polygon);
    r.changed += dried;
    return r;
}

// ── Entity layer ─────────────────────────────────────────────────────────────

bool polygonOf(HorizonWorld& world, Entity spline, Entity terrain, std::vector<glm::vec2>& out)
{
    auto& reg = world.registry();
    if (!reg.valid(spline) || !reg.valid(terrain)) return false;
    const auto* sc = reg.try_get<SplineComponent>(spline);
    const auto* tc = reg.try_get<TerrainComponent>(terrain);
    if (!sc || !tc) return false;
    const HE::spline::Curve curve(*sc);
    if (!curve.closed()) return false;
    const glm::mat4 toLocal = glm::inverse(HE::worldMatrixOf(world, terrain)) * HE::worldMatrixOf(world, spline);
    out = polygonFromPolyline(curve.polyline(polylineTolerance(*tc)), toLocal);
    return validPolygon(out);
}

Link linkOf(HorizonWorld& world, Entity spline)
{
    Link l;
    auto& reg = world.registry();
    if (!reg.valid(spline)) return l;
    const HE::UUID id = world.entityId(spline);
    if (id == HE::UUID{}) return l;
    for (auto [te, tc] : reg.view<TerrainComponent>().each())
        if (const Body* b = tc.water.findBySource(id))
        {
            l.terrain = te;
            l.body = b->id;
            return l;
        }
    return l;
}

Created create(HorizonWorld& world, Entity terrain, Entity spline, const Params& p)
{
    Created out;
    auto& reg = world.registry();
    auto* tc = reg.valid(terrain) ? reg.try_get<TerrainComponent>(terrain) : nullptr;
    if (!tc) { out.error = "That is not a landscape."; return out; }
    const auto* sc = reg.valid(spline) ? reg.try_get<SplineComponent>(spline) : nullptr;
    if (!sc) { out.error = "That is not a spline."; return out; }
    if (!hasArea(*tc)) { out.error = "The landscape has no area."; return out; }
    if (!HE::spline::Curve(*sc).closed())
    {
        out.error = "A lake needs a closed spline of at least three points. Close the shape first.";
        return out;
    }
    if (linkOf(world, spline).body != kNoBody)
    {
        out.error = "This spline already is a lake. Move its points to reshape it, or use Dig Again.";
        return out;
    }
    std::vector<glm::vec2> poly;
    if (!polygonOf(world, spline, terrain, poly))
    {
        out.error = "The outline has no area.";
        return out;
    }
    if (rasterizePolygon(poly, tc->sizeX, tc->sizeZ, std::max<uint32_t>(1, tc->water.res)).empty())
    {
        out.error = "The outline lies outside the landscape.";
        return out;
    }

    // The level is read from the ground as it is NOW, before the excavation.
    const float level = p.levelFromGround ? lowestGround(*tc, poly) + p.levelOffset : p.level;
    if (!finite(level)) { out.error = "The water level is not a number."; return out; }

    if (p.dig)
    {
        TerrainSculpt::ExcavateParams ep;
        ep.mode    = TerrainSculpt::ExcavateMode::Floor;
        ep.amount  = level - std::max(0.0f, p.depth);
        ep.falloff = std::max(0.0f, p.bank);
        const TerrainSculpt::Result sr = TerrainSculpt::excavatePolygon(*tc, poly, ep);
        if (!sr.ok) { out.error = "The landscape could not be dug here."; return out; }
        out.ground = sr.changed;
    }
    const uint16_t id = create(*tc, world.entityId(spline), poly, level);
    if (id == kNoBody) { out.error = "The water could not be laid."; return out; }
    if (Body* b = tc->water.findBody(id)) b->syncKey = fingerprint(world, spline, terrain, *sc);
    out.ok = true;
    out.body = id;
    out.level = level;
    out.cells = tc->water.wetCells(id);
    return out;
}

Result reshape(HorizonWorld& world, Entity spline)
{
    const Link l = linkOf(world, spline);
    if (l.body == kNoBody) return {};
    auto& reg = world.registry();
    auto& tc = reg.get<TerrainComponent>(l.terrain);
    std::vector<glm::vec2> poly;
    if (!polygonOf(world, spline, l.terrain, poly)) return {};
    const Result r = reshape(tc, l.body, poly);
    if (r.ok)
        if (Body* b = tc.water.findBody(l.body))
            b->syncKey = fingerprint(world, spline, l.terrain, reg.get<SplineComponent>(spline));
    return r;
}

TerrainSculpt::Result dig(HorizonWorld& world, Entity spline, float depth, float bank)
{
    const Link l = linkOf(world, spline);
    if (l.body == kNoBody) return {};
    return dig(world.registry().get<TerrainComponent>(l.terrain), l.body, depth, bank);
}

bool setLevel(HorizonWorld& world, Entity spline, float level)
{
    const Link l = linkOf(world, spline);
    if (l.body == kNoBody) return false;
    return HE::water::setLevel(world.registry().get<TerrainComponent>(l.terrain), l.body, level);
}

bool remove(HorizonWorld& world, Entity spline)
{
    const Link l = linkOf(world, spline);
    if (l.body == kNoBody) return false;
    return removeBody(world.registry().get<TerrainComponent>(l.terrain), l.body).ok;
}

Converted convertBody(HorizonWorld& world, Entity terrain, uint16_t body, const std::string& name)
{
    Converted out;
    auto& reg = world.registry();
    auto* tc = reg.valid(terrain) ? reg.try_get<TerrainComponent>(terrain) : nullptr;
    if (!tc) { out.error = "That is not a landscape."; return out; }
    const Body* b = tc->water.findBody(body);
    if (!b) { out.error = "There is no such body of water."; return out; }
    if (b->fromSpline()) { out.error = "This water already is a lake."; return out; }

    const Outline o = extractOutline(*tc, body);
    out.pieces = o.pieces;
    out.islands = o.islands;
    if (!o.ok) { out.error = "This water is too small or too thin to outline."; return out; }
    out.points = static_cast<uint32_t>(o.points.size());

    const float level = b->level;
    const Entity e = world.createEntity(name);
    TransformComponent tf;                  // identity, in the landscape's space
    tf.dirty = true;
    reg.emplace_or_replace<TransformComponent>(e, tf);
    SplineComponent sc;
    sc.closed = true;
    sc.controlPoints.reserve(o.points.size());
    for (const glm::vec2& p : o.points) sc.controlPoints.emplace_back(p.x, level, p.y);
    reg.emplace<SplineComponent>(e, std::move(sc));
    world.reparentEntity(e, terrain);

    std::vector<glm::vec2> poly;
    if (!polygonOf(world, e, terrain, poly) ||
        !adopt(*tc, body, world.entityId(e), poly).ok)
    {
        world.destroyEntity(e);
        out.error = "The outline could not be turned into a spline.";
        return out;
    }
    if (Body* nb = tc->water.findBody(body))
        nb->syncKey = fingerprint(world, e, terrain, reg.get<SplineComponent>(e));
    out.spline = e;
    return out;
}

uint32_t syncSplines(HorizonWorld& world)
{
    auto& reg = world.registry();
    bool any = false;
    for (auto [te, tc] : reg.view<TerrainComponent>().each())
    {
        for (const Body& b : tc.water.bodies) any = any || b.fromSpline();
        if (any) break;
    }
    if (!any) return 0;

    // A handful of splines against a handful of lakes: look each spline's id up
    // once per call instead of searching every entity for every lake.
    std::vector<std::pair<HE::UUID, Entity>> splines;
    for (auto [se, sc] : reg.view<SplineComponent>().each())
        splines.emplace_back(world.entityId(se), se);

    uint32_t reshaped = 0;
    for (auto [te, tc] : reg.view<TerrainComponent>().each())
        for (Body& b : tc.water.bodies)
        {
            if (!b.fromSpline()) continue;
            Entity se = entt::null;
            for (const auto& s : splines) if (s.first == b.sourceSpline) { se = s.second; break; }
            if (se == entt::null) continue;                     // its spline is gone: the water stays
            const auto& sc = reg.get<SplineComponent>(se);
            const uint64_t key = fingerprint(world, se, te, sc);
            if (b.syncKey == 0 || b.syncKey == key) { b.syncKey = key; continue; }
            b.syncKey = key;
            std::vector<glm::vec2> poly;
            if (!polygonOf(world, se, te, poly)) continue;     // not a closed shape right now: wait for one
            if (poly == b.polygon) continue;
            if (reshape(tc, b.id, poly).ok) ++reshaped;
        }
    return reshaped;
}

} // namespace HE::water::lake
