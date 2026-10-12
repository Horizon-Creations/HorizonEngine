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
        const Region was = saveRegion(tc, x, z, p.radius + p.falloff);
        const TerrainSculpt::Result g =
            TerrainSculpt::lowerToFloor(tc, x, z, p.radius, p.falloff, s.level - p.digDepth, p.amount, s.level);
        d.ok = d.ok || g.ok;
        // ...and then only where the water is (Thema 188). The weight says nothing
        // about where the sheet ends: at the Landscape tool's pacing a cell needs
        // several dabs to cross the shoreline while the dig starts with the first,
        // and ground lowered to the level next to the water is a shelf it never
        // covers. Every vertex this dab lowered that the body's own sheet does not
        // cover goes back to where it was: the pit is the water's shape, never more.
        d.ground = g.changed > 0 ? keepUnderSheet(tc, s.body, s.level, was) : 0u;
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
