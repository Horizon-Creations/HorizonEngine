#include "HorizonScene/WaterBrush.h"
#include "HorizonScene/TerrainMeshGenerator.h"   // terrainHeightAt
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>

namespace HE::water::brush
{

namespace
{
    bool hasArea(const TerrainComponent& tc) { return tc.sizeX > 0.0f && tc.sizeZ > 0.0f; }
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
        const TerrainSculpt::Result g =
            TerrainSculpt::lowerToFloor(tc, x, z, p.radius, p.falloff, s.level - p.digDepth, p.amount);
        d.ground = g.changed;
        d.ok = d.ok || g.ok;
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
