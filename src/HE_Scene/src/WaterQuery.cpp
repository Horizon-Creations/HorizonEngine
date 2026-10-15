#include "HorizonScene/WaterQuery.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace HE::water
{

namespace
{
    // The most the physics waves lift the surface above its level, metres.
    float maxWaveHeight()
    {
        float h = 0.0f;
        for (int i = 0; i < kPhysicsWaveCount; ++i) h += std::abs(waveAmplitude(kPhysicsWaves[i]));
        return h;
    }

    bool finiteMat(const glm::mat4& m)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r])) return false;
        return true;
    }
}

WaterIndex WaterIndex::build(HorizonWorld& world)
{
    WaterIndex index;
    index.refresh(world);
    return index;
}

void WaterIndex::refresh(HorizonWorld& world)
{
    auto& reg = world.registry();
    std::vector<Entry> next;
    for (auto [entity, tc] : reg.view<TerrainComponent>().each())
    {
        const Field& f = tc.water;
        // A field with bodies but no grid has no water to speak of; nothing to ask.
        if (f.bodies.empty() || !f.allocated()) continue;

        Entry e;
        e.entity       = entity;
        e.tc           = &tc;
        e.localToWorld = HE::worldMatrixOf(world, entity);
        if (!finiteMat(e.localToWorld)) continue;
        // x/z of the local space must be reachable from the world's x/z alone: the
        // 2×2 block of the map that takes local (x, z) to world (x, z) has to invert.
        const float det = e.localToWorld[0][0] * e.localToWorld[2][2] - e.localToWorld[2][0] * e.localToWorld[0][2];
        if (!(std::abs(det) > 1.0e-12f)) continue;
        e.worldToLocal = glm::inverse(e.localToWorld);

        // The turn of the landscape without its scale, for carrying the current into
        // world space (a current is a velocity: scaling the landscape must not speed it).
        for (int c = 0; c < 3; ++c)
        {
            const glm::vec3 col(e.localToWorld[c]);
            const float len = glm::length(col);
            e.rotation[c] = len > 1.0e-12f ? col / len : glm::vec3(c == 0, c == 1, c == 2);
        }

        // The cache of the wet rectangle survives a refresh as long as its key does.
        for (const Entry& old : m_entries)
            if (old.entity == entity)
            {
                e.boundsValid = old.boundsValid;  e.anyWet = old.anyWet;
                e.minX = old.minX;  e.minZ = old.minZ;  e.maxX = old.maxX;  e.maxZ = old.maxZ;
                e.minLevel = old.minLevel;  e.maxLevel = old.maxLevel;
                e.keyRevision = old.keyRevision;  e.keyRes = old.keyRes;
                e.keyGrid = old.keyGrid;  e.keyBodies = old.keyBodies;
                break;
            }
        next.push_back(e);
    }
    std::sort(next.begin(), next.end(),
              [](const Entry& a, const Entry& b) { return static_cast<uint32_t>(a.entity) < static_cast<uint32_t>(b.entity); });
    m_entries = std::move(next);
}

void WaterIndex::ensureBounds(Entry& e)
{
    const Field& f = e.tc->water;
    if (e.boundsValid && e.keyRevision == f.revision && e.keyRes == f.res &&
        e.keyGrid == static_cast<const void*>(f.coverage.data()) && e.keyBodies == f.bodies.size())
        return;

    const int res = static_cast<int>(f.res);
    int x0 = res, z0 = res, x1 = -1, z1 = -1;
    std::vector<uint8_t> used(65536, 0);
    for (int z = 0; z < res; ++z)
        for (int x = 0; x < res; ++x)
        {
            const size_t i = static_cast<size_t>(z) * res + x;
            if (f.coverage[i] < kWet) continue;
            used[f.owner[i]] = 1;
            x0 = std::min(x0, x); x1 = std::max(x1, x);
            z0 = std::min(z0, z); z1 = std::max(z1, z);
        }

    e.anyWet = x1 >= x0 && z1 >= z0;
    if (e.anyWet)
    {
        const TerrainComponent& tc = *e.tc;
        const float cw = tc.sizeX / static_cast<float>(res);
        const float ch = tc.sizeZ / static_cast<float>(res);
        e.minX = -tc.sizeX * 0.5f + static_cast<float>(x0) * cw;
        e.maxX = -tc.sizeX * 0.5f + static_cast<float>(x1 + 1) * cw;
        e.minZ = -tc.sizeZ * 0.5f + static_cast<float>(z0) * ch;
        e.maxZ = -tc.sizeZ * 0.5f + static_cast<float>(z1 + 1) * ch;
        bool first = true;
        for (const Body& b : f.bodies)
        {
            if (!used[b.id]) continue;
            e.minLevel = first ? b.level : std::min(e.minLevel, b.level);
            e.maxLevel = first ? b.level : std::max(e.maxLevel, b.level);
            first = false;
        }
    }
    e.boundsValid = true;
    e.keyRevision = f.revision;
    e.keyRes      = f.res;
    e.keyGrid     = f.coverage.data();
    e.keyBodies   = f.bodies.size();
}

void WaterIndex::regions(std::vector<Region>& out)
{
    out.clear();
    const float lift = maxWaveHeight() + 1.0f;
    for (Entry& e : m_entries)
    {
        ensureBounds(e);
        if (!e.anyWet) continue;
        glm::vec3 lo(std::numeric_limits<float>::max());
        glm::vec3 hi(std::numeric_limits<float>::lowest());
        const float ys[2] = { e.minLevel - kRegionDepth, e.maxLevel };
        const float xs[2] = { e.minX, e.maxX };
        const float zs[2] = { e.minZ, e.maxZ };
        for (float lx : xs)
            for (float lz : zs)
                for (float ly : ys)
                {
                    const glm::vec3 p(e.localToWorld * glm::vec4(lx, ly, lz, 1.0f));
                    lo = glm::min(lo, p);
                    hi = glm::max(hi, p);
                }
        hi.y += lift;
        Region r;
        r.terrain = e.entity;
        r.min = lo;
        r.max = hi;
        out.push_back(r);
    }
}

uint64_t WaterIndex::revisionOf(entt::entity terrain) const
{
    for (const Entry& e : m_entries)
        if (e.entity == terrain) return e.tc->water.revision;
    return 0;
}

bool WaterIndex::entryAt(const Entry& e, float x, float z, double t, Surface& out) const
{
    const TerrainComponent& tc = *e.tc;
    // Local XZ from world XZ. The y of the probe is irrelevant for a landscape that is
    // not tilted, which is the only kind this supports.
    const glm::vec4 lp = e.worldToLocal * glm::vec4(x, 0.0f, z, 1.0f);
    const float lx = lp.x, lz = lp.z;
    const uint16_t id = bodyAt(tc, lx, lz);
    if (id == kNoBody) return false;
    const Body* body = tc.water.findBody(id);
    if (!body) return false;

    // The two things the sheet is clipped by (WaterMesh.h): the ground may not stand
    // above the level plus the overshoot. Where it does, no water is drawn, so none
    // carries a body either.
    const float ground = terrainHeightAt(tc, lx, lz);
    if (tc.water.clipToGround && ground > body->level + tc.water.shoreOvershoot)
        return false;

    out.wet     = true;
    out.terrain = e.entity;
    out.body    = id;
    out.level   = (e.localToWorld * glm::vec4(lx, body->level, lz, 1.0f)).y;
    const WaveSample w = sampleWaves(kPhysicsWaves, kPhysicsWaveCount, x, z, t);
    out.height  = out.level + w.height;
    out.normal  = glm::normalize(glm::vec3(-w.dhdx, 1.0f, -w.dhdz));
    const float groundY = (e.localToWorld * glm::vec4(lx, ground, lz, 1.0f)).y;
    out.depth   = std::max(0.0f, out.height - groundY);
    out.flow    = e.rotation * glm::vec3(body->physics.flow.x, 0.0f, body->physics.flow.y);
    out.physics = body->physics;
    out.underwaterFog = body->underwaterFog;
    out.underwaterCaustics = body->underwaterCaustics;
    return true;
}

bool WaterIndex::surfaceAt(float x, float z, double t, Surface& out) const
{
    out = Surface{};
    if (!std::isfinite(x) || !std::isfinite(z)) return false;
    bool found = false;
    Surface best;
    // m_entries is sorted by entity id, so on a tie the first (smaller id) stays.
    for (const Entry& e : m_entries)
    {
        Surface s;
        if (!entryAt(e, x, z, t, s)) continue;
        if (!found || s.height > best.height)
        {
            best  = s;
            found = true;
        }
    }
    if (found) out = best;
    return found;
}

float WaterIndex::heightAt(float x, float z, double t) const
{
    Surface s;
    return surfaceAt(x, z, t, s) ? s.height : std::numeric_limits<float>::quiet_NaN();
}

} // namespace HE::water
