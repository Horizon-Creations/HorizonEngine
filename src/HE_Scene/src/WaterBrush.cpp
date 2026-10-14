#include "HorizonScene/WaterBrush.h"
#include "HorizonScene/TerrainMeshGenerator.h"   // terrainHeightAt
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/WaterMesh.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace HE::water::brush
{

namespace
{
    bool hasArea(const TerrainComponent& tc) { return tc.sizeX > 0.0f && tc.sizeZ > 0.0f; }

    // The terrain vertices a dab of reach `outer` at (x, z) can move, with their
    // heights before it — the rect TerrainSculpt::lowerToFloor walks.
    struct Region
    {
        int x0 = 0, z0 = 0, x1 = -1, z1 = -1;
        std::vector<float> heights;   // (x1 − x0 + 1) · (z1 − z0 + 1), row-major
        bool empty() const { return heights.empty(); }
    };

    Region saveRegion(TerrainComponent& tc, float x, float z, float outer)
    {
        Region r;
        if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z) || !std::isfinite(outer) || outer <= 0.0f)
            return r;
        TerrainSculpt::ensureHeights(tc);    // what lowerToFloor does first; a no-op for it afterwards
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        r.x0 = std::max(0, static_cast<int>(std::floor((x - outer + halfX) / stepX)));
        r.x1 = std::min(res - 1, static_cast<int>(std::ceil((x + outer + halfX) / stepX)));
        r.z0 = std::max(0, static_cast<int>(std::floor((z - outer + halfZ) / stepZ)));
        r.z1 = std::min(res - 1, static_cast<int>(std::ceil((z + outer + halfZ) / stepZ)));
        if (r.x0 > r.x1 || r.z0 > r.z1) return Region{};
        for (int zi = r.z0; zi <= r.z1; ++zi)
            for (int xi = r.x0; xi <= r.x1; ++xi)
                r.heights.push_back(tc.sculptHeights[static_cast<size_t>(zi) * res + xi]);
        return r;
    }

    // Put back every vertex of `was` the dab lowered that `body`'s sheet does not
    // cover — the sheet as the mesh builds it (WaterMesh.h: the lattice of the
    // body's own cells, marching squares, the field's shore clipping at the body's
    // level), asked point by point with insideSheet. Putting a vertex back can
    // raise the ground under a lattice corner and take the sheet off a neighbour,
    // so it repeats until nothing moves (it only ever puts back, so it ends).
    // Returns the vertices still lowered.
    uint32_t keepUnderSheet(TerrainComponent& tc, uint16_t body, float level, const Region& was)
    {
        if (was.empty()) return 0;
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const int   w = was.x1 - was.x0 + 1;
        auto vx = [&](int xi) { return -halfX + static_cast<float>(xi) * stepX; };
        auto vz = [&](int zi) { return -halfZ + static_cast<float>(zi) * stepZ; };
        auto old = [&](int xi, int zi) { return was.heights[static_cast<size_t>(zi - was.z0) * w + (xi - was.x0)]; };
        auto cur = [&](int xi, int zi) -> float& { return tc.sculptHeights[static_cast<size_t>(zi) * res + xi]; };

        const ShoreClip clip{ tc.water.clipToGround, level, tc.water.shoreOvershoot };
        const CellRect cells = cellRect(tc, vx(was.x0), vz(was.z0), vx(was.x1), vz(was.z1));
        for (bool moved = true; moved; )
        {
            moved = false;
            Lattice lattice;
            const bool sheet = cells.valid() && buildLattice(tc, body, cells, lattice, clip);
            for (int zi = was.z0; zi <= was.z1; ++zi)
                for (int xi = was.x0; xi <= was.x1; ++xi)
                {
                    float& h = cur(xi, zi);
                    if (h == old(xi, zi)) continue;
                    if (sheet && insideSheet(lattice, vx(xi), vz(zi))) continue;
                    h = old(xi, zi);
                    moved = true;
                }
        }
        uint32_t kept = 0;
        for (int zi = was.z0; zi <= was.z1; ++zi)
            for (int xi = was.x0; xi <= was.x1; ++xi)
                if (cur(xi, zi) != old(xi, zi)) ++kept;
        return kept;
    }

    // The bank (Thema 188 Schritt 4). keepUnderSheet leaves the ground next to the
    // water where it was, so on rising ground the pit ends in a wall as high as
    // the ground stood above the level (4 m at the end of a 50 m stroke up an 8 %
    // slope), its top a staircase of 1 m vertices against a shore drawn through
    // 4 m water cells. Ground outside the sheet that stands above
    //     bank(d) = foot + kBankSlope · d / (1 − d / reach)
    // is lowered to it, d = the distance to the shoreline (shoreSegments: the
    // contour itself, not the nearest vertex under it, so the staircase is gone).
    // foot = just above where the shore clipping would put the sheet onto the
    // ground (level + overshoot): the bank never sinks below the water's reach, so
    // it is dry land and never a pit, and the sheet never spreads onto it. At the
    // waterline it rises kBankSlope per metre, steepening to vertical at `reach`,
    // so a wall of any height meets the old ground without a step and nothing
    // beyond `reach` moves. A target set by the shore, never a share of the height
    // it lowers from: a dab repeated at one spot changes nothing more.
    constexpr float kBankSlope = 0.5f;    // rise per metre at the waterline
    constexpr float kBankLip   = 0.01f;   // metres above the shore clipping's limit
    constexpr int   kBankPasses = 8;      // see the shore clipping's crossing below
    constexpr float kBankTolerance = 0.001f;   // metres a pass must lower a vertex by to count

    // Returns the vertices lowered.
    uint32_t shapeBank(TerrainComponent& tc, uint16_t body, float level, float x, float z, float outer)
    {
        const int res = static_cast<int>(tc.resolution);
        if (tc.sculptHeights.size() != static_cast<size_t>(res) * res || !tc.water.allocated()) return 0;
        const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
        const float stepX = tc.sizeX / static_cast<float>(res - 1);
        const float stepZ = tc.sizeZ / static_cast<float>(res - 1);
        const float reach = std::max(outer, 2.0f * std::max(stepX, stepZ));
        const float foot  = level + (tc.water.clipToGround ? tc.water.shoreOvershoot : 0.0f) + kBankLip;

        // The vertices within reach of the shore this dab can have moved, and a
        // lattice a reach wider, so every piece of shore they can see is on it.
        const float span = outer + reach;
        const int x0 = std::max(0, static_cast<int>(std::floor((x - span + halfX) / stepX)));
        const int x1 = std::min(res - 1, static_cast<int>(std::ceil((x + span + halfX) / stepX)));
        const int z0 = std::max(0, static_cast<int>(std::floor((z - span + halfZ) / stepZ)));
        const int z1 = std::min(res - 1, static_cast<int>(std::ceil((z + span + halfZ) / stepZ)));
        if (x0 > x1 || z0 > z1) return 0;
        const CellRect cells = cellRect(tc, x - span - reach, z - span - reach, x + span + reach, z + span + reach);
        if (!cells.valid()) return 0;
        auto distance = [](const glm::dvec2& p, const ShoreSegment& s)
        {
            const glm::dvec2 ab = s.b - s.a;
            const double len2 = glm::dot(ab, ab);
            const double t = len2 > 0.0 ? std::clamp(glm::dot(p - s.a, ab) / len2, 0.0, 1.0) : 0.0;
            return glm::length(p - (s.a + ab * t));
        };
        const int w = x1 - x0 + 1;
        std::vector<float> was;
        was.reserve(static_cast<size_t>(w) * (z1 - z0 + 1));
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
                was.push_back(tc.sculptHeights[static_cast<size_t>(zi) * res + xi]);

        // Where the sheet's edge is the shore clipping (the cells are wet further
        // out, the ground stood too high), the edge sits where the ground's depth
        // under the water crosses zero along a lattice edge — and lowering the
        // bank's corner moves that crossing outward. Measured against the shore
        // before it moved, the bank stands too high, so it is shaped again from
        // the new shore until nothing moves by more than kBankTolerance (the
        // crossing settles within a few passes; the sheet only ever grows, so no
        // pass can uncover the pit).
        for (int pass = 0; pass < kBankPasses; ++pass)
        {
            Lattice lattice;
            if (!buildLattice(tc, body, cells, lattice, ShoreClip{ tc.water.clipToGround, level, tc.water.shoreOvershoot }))
                break;
            // Only the shore within this dab's own circle: a wall can only have been
            // left where this dab dug, and the shore of the rest of the body (a lake
            // the stroke continues, its own banks) is not this dab's to reshape. Two
            // water cells wider: a lattice corner averages the four cells around it,
            // so the contour of the cells the circle painted runs up to a cell past
            // it. Leaving pieces out only ever measures a vertex farther from the
            // water, which keeps its bank higher, and a distance to a fixed set of
            // segments is continuous, so it leaves no step either.
            std::vector<ShoreSegment> shore = shoreSegments(lattice);
            const glm::dvec2 centre(x, z);
            const double shoreReach = outer + 2.0 * std::max(lattice.cellW, lattice.cellH);
            shore.erase(std::remove_if(shore.begin(), shore.end(),
                                       [&](const ShoreSegment& s) { return distance(centre, s) > shoreReach; }),
                        shore.end());
            if (shore.empty()) break;

            // Each segment lies within one lattice cell: bucket them by it, so a vertex
            // only looks at the cells its search radius touches.
            const int bw = lattice.w - 1, bh = lattice.h - 1;
            auto bucketOf = [&](double px, double pz, int& bi, int& bj)
            {
                bi = std::clamp(static_cast<int>(std::floor((px - lattice.originX) / lattice.cellW)) - lattice.x0, 0, bw - 1);
                bj = std::clamp(static_cast<int>(std::floor((pz - lattice.originZ) / lattice.cellH)) - lattice.z0, 0, bh - 1);
            };
            std::vector<std::vector<uint32_t>> buckets(static_cast<size_t>(bw) * bh);
            for (uint32_t s = 0; s < shore.size(); ++s)
            {
                int bi, bj;
                bucketOf((shore[s].a.x + shore[s].b.x) * 0.5, (shore[s].a.y + shore[s].b.y) * 0.5, bi, bj);
                buckets[static_cast<size_t>(bj) * bw + bi].push_back(s);
            }

            bool moved = false;
            for (int zi = z0; zi <= z1; ++zi)
                for (int xi = x0; xi <= x1; ++xi)
                {
                    float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
                    const float above = h - foot;
                    if (!(above > 0.0f)) continue;
                    const double px = -halfX + static_cast<double>(xi) * stepX;
                    const double pz = -halfZ + static_cast<double>(zi) * stepZ;
                    if (insideSheet(lattice, px, pz)) continue;     // under the water: the dig's, not the bank's
                    // bank(d) < h only for d below this (bank(d) = h solved for d).
                    const double radius = static_cast<double>(above) * reach / (kBankSlope * reach + above);
                    int bi0, bj0, bi1, bj1;
                    bucketOf(px - radius, pz - radius, bi0, bj0);
                    bucketOf(px + radius, pz + radius, bi1, bj1);
                    double d = radius;
                    const glm::dvec2 p(px, pz);
                    for (int bj = bj0; bj <= bj1; ++bj)
                        for (int bi = bi0; bi <= bi1; ++bi)
                            for (const uint32_t s : buckets[static_cast<size_t>(bj) * bw + bi])
                                d = std::min(d, distance(p, shore[s]));
                    if (!(d < radius)) continue;
                    const float bank = foot + kBankSlope * static_cast<float>(d / (1.0 - d / reach));
                    if (!(bank < h - kBankTolerance)) continue;
                    h = bank;
                    moved = true;
                }
            if (!moved) break;
        }

        uint32_t lowered = 0;
        int cx0 = res, cx1 = -1, cz0 = res, cz1 = -1;
        for (int zi = z0; zi <= z1; ++zi)
            for (int xi = x0; xi <= x1; ++xi)
            {
                if (tc.sculptHeights[static_cast<size_t>(zi) * res + xi] == was[static_cast<size_t>(zi - z0) * w + (xi - x0)])
                    continue;
                ++lowered;
                cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
            }
        if (lowered > 0)
            TerrainSculpt::markRegionDirty(tc,
                                           -halfX + static_cast<float>(cx0) * stepX - stepX,
                                           -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                                           -halfX + static_cast<float>(cx1) * stepX + stepX,
                                           -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
        return lowered;
    }
}

Target targetAt(const TerrainComponent& tc, float x, float z, const Params& p)
{
    Target t;
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return t;
    if (!p.erase)
    {
        const uint16_t under = bodyAt(tc, x, z);
        if (const Body* b = tc.water.findBody(under))
        {
            t.body      = b->id;
            t.level     = b->level;
            t.continues = true;
            return t;
        }
    }
    const float level = p.levelFromGround ? terrainHeightAt(tc, x, z) + p.levelOffset : p.level;
    t.level = std::isfinite(level) ? level : 0.0f;
    return t;
}

void begin(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p)
{
    if (s.active) end(s, tc);
    s = Stroke{};
    if (!hasArea(tc) || !std::isfinite(x) || !std::isfinite(z)) return;

    s.erase = p.erase;
    if (p.erase)
    {
        s.active = true;
        return;
    }
    const Target t = targetAt(tc, x, z, p);
    s.level = t.level;
    if (t.continues)
        s.body = t.body;
    else
    {
        s.body = tc.water.createBody(t.level);
        s.created = s.body != kNoBody;
    }
    s.active = s.body != kNoBody;
}

Dab dab(Stroke& s, TerrainComponent& tc, float x, float z, const Params& p)
{
    Dab d;
    if (!s.active || !hasArea(tc)) return d;

    if (s.erase)
    {
        // Nothing painted yet: removeCircle would allocate an all-dry grid just to
        // take nothing out of it, and that grid makes the scene non-pristine.
        if (!tc.water.allocated()) { d.ok = true; ++s.dabs; return d; }
        const Result r = removeCircle(tc, x, z, p.radius, p.falloff, p.amount);
        d.ok = r.ok;
        d.cells = r.changed;
        ++s.dabs;
        return d;
    }

    // Undo (or anything else that replaced the field) can take the body away
    // mid-stroke; painting into a body that is gone would have no owner to give.
    if (!tc.water.findBody(s.body)) return d;
    const Result r = addCircle(tc, s.body, x, z, p.radius, p.falloff, p.amount);
    d.ok    = r.ok;
    d.cells = r.changed;
    if (p.dig && p.digDepth > 0.0f && std::isfinite(p.digDepth))
    {
        // wetLevel = the stroke's own water level (Thema 174 Schritt 13): the bed
        // eases from the level at half weight to the floor at full weight, and
        // ground above the level goes to it at once.
        // The reach lowerToFloor itself walks: it clamps both to ≥ 0 first.
        const float outer = std::max(0.0f, p.radius) + std::max(0.0f, p.falloff);
        const Region was = saveRegion(tc, x, z, outer);
        const TerrainSculpt::Result g =
            TerrainSculpt::lowerToFloor(tc, x, z, p.radius, p.falloff, s.level - p.digDepth, p.amount, s.level);
        d.ok = d.ok || g.ok;
        // ...and then only where the water is (Thema 188). The weight says nothing
        // about where the sheet ends: at the Landscape tool's pacing a cell needs
        // several dabs to cross the shoreline while the dig starts with the first,
        // and ground lowered to the level next to the water is a shelf it never
        // covers. Every vertex this dab lowered that the body's own sheet does not
        // cover goes back to where it was: the pit is the water's shape, never more.
        const uint32_t kept = g.changed > 0 ? keepUnderSheet(tc, s.body, s.level, was) : 0u;
        // Where that left a wall of ground above the water, it becomes a bank down
        // to the waterline (Schritt 4), dry land that never reaches the water.
        d.bank   = kept > 0 ? shapeBank(tc, s.body, s.level, x, z, outer) : 0u;
        d.ground = kept + d.bank;
    }
    ++s.dabs;
    return d;
}

void end(Stroke& s, TerrainComponent& tc)
{
    if (s.active && s.created && s.body != kNoBody && tc.water.findBody(s.body))
    {
        // A press that landed nowhere (off the landscape, a brush of no size)
        // leaves a body with no cell and no source: nothing would ever clean it up.
        bool owns = false;
        const Field& f = tc.water;
        if (f.allocated())
            for (size_t i = 0; i < f.owner.size() && !owns; ++i)
                owns = f.owner[i] == s.body && f.coverage[i] > 0;
        if (!owns) removeBody(tc, s.body);
    }
    // A press that never wet a cell (or an eraser that dried the last one) leaves
    // an allocated all-dry grid behind, and a grid makes the scene non-pristine:
    // hand it back, so such a stroke costs the file nothing. Only when there is no
    // body at all; a lake keeps its (possibly dry) cells' grid for its spline.
    Field& f = tc.water;
    if (s.active && f.bodies.empty() && f.allocated() &&
        std::none_of(f.coverage.begin(), f.coverage.end(), [](uint8_t c) { return c != 0; }))
    {
        f.coverage.clear(); f.coverage.shrink_to_fit();
        f.owner.clear();    f.owner.shrink_to_fit();
    }
    s = Stroke{};
}

} // namespace HE::water::brush
