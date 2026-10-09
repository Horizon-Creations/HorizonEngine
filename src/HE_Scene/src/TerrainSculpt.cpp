#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

namespace TerrainSculpt
{

namespace
{
    // The same stable per-vertex hash the interactive Roughen brush uses: a value
    // in [-1, 1] that depends only on the grid coordinate, so repeated dabs pile
    // the SAME bumps up instead of shimmering into noise.
    float vhash(uint32_t xi, uint32_t zi)
    {
        uint32_t n = xi * 73856093u ^ zi * 19349663u;
        n = (n ^ 61u) ^ (n >> 16u); n += n << 3u;
        n ^= n >> 4u; n *= 0x27D4EB2Du; n ^= n >> 15u;
        return static_cast<float>(n & 0x00FFFFFFu)
             / static_cast<float>(0x01000000u) * 2.0f - 1.0f;
    }

    // Only the chunks under the rect have to be rebuilt. `dirty` is left alone:
    // setting it would rebuild all 64+ of them, which is the difference between a
    // brush and a regeneration.
    void markRegionDirty(TerrainComponent& tc, float mnX, float mnZ, float mxX, float mxZ)
    {
        if (tc.regionDirty)
        {
            tc.dirtyMinX = std::min(tc.dirtyMinX, mnX); tc.dirtyMaxX = std::max(tc.dirtyMaxX, mxX);
            tc.dirtyMinZ = std::min(tc.dirtyMinZ, mnZ); tc.dirtyMaxZ = std::max(tc.dirtyMaxZ, mxZ);
        }
        else
        {
            tc.dirtyMinX = mnX; tc.dirtyMaxX = mxX;
            tc.dirtyMinZ = mnZ; tc.dirtyMaxZ = mxZ;
            tc.regionDirty = true;
        }
    }

    // Beyond any landscape; keeps the index arithmetic of excavatePolygon finite.
    constexpr float kMaxCoord = 1.0e7f;

    // Squared distance from (px, pz) to the segment a–b.
    float segmentDist2(float px, float pz, const glm::vec2& a, const glm::vec2& b)
    {
        const float abx = b.x - a.x, abz = b.y - a.y;
        const float len2 = abx * abx + abz * abz;
        float t = 0.0f;
        if (len2 > 0.0f)
            t = std::clamp(((px - a.x) * abx + (pz - a.y) * abz) / len2, 0.0f, 1.0f);
        const float dx = px - (a.x + t * abx), dz = pz - (a.y + t * abz);
        return dx * dx + dz * dz;
    }
}

const char* opName(Op op)
{
    switch (op)
    {
    case Op::Raise:   return "raise";
    case Op::Lower:   return "lower";
    case Op::Set:     return "set";
    case Op::Flatten: return "flatten";
    case Op::Smooth:  return "smooth";
    case Op::Roughen: return "roughen";
    }
    return "raise";
}

bool opFromName(const char* name, Op& out)
{
    if (!name) return false;
    const std::string s(name);
    if (s == "raise")   { out = Op::Raise;   return true; }
    if (s == "lower")   { out = Op::Lower;   return true; }
    if (s == "set")     { out = Op::Set;     return true; }
    if (s == "flatten") { out = Op::Flatten; return true; }
    if (s == "smooth")  { out = Op::Smooth;  return true; }
    if (s == "roughen") { out = Op::Roughen; return true; }
    return false;
}

void ensureHeights(TerrainComponent& tc)
{
    // ── The 2ⁿ+1 snap, taken here instead of at the next regen ───────────────
    // TerrainSystem does this on its next pass anyway (so chunk LOD0 vertices land
    // exactly on source grid points). Doing it BEFORE the brush is what keeps a
    // caller's arithmetic honest: at resolution 128 the grid step is size/127,
    // and one regen later it is size/128 with every height resampled.
    const uint32_t r0 = std::clamp(tc.resolution, 2u, 1024u);
    uint32_t cells = r0 - 1, p = 1;
    while (p < cells) p <<= 1;
    const uint32_t snapped = p + 1;
    if (snapped != r0)
    {
        if (tc.sculptHeights.size() == static_cast<size_t>(r0) * r0)
            tc.sculptHeights = resampleHeightField(tc.sculptHeights, r0, snapped);
        tc.resolution = snapped;
        tc.dirty      = true;   // the chunk grid itself changes, so a full rebuild
    }
    else
        tc.resolution = r0;

    const size_t want = static_cast<size_t>(tc.resolution) * tc.resolution;
    if (tc.sculptHeights.size() == want) return;
    tc.sculptHeights = computeTerrainHeightField(tc);
}

Result apply(TerrainComponent& tc, float localX, float localZ, Op op,
             float radius, float falloff, float amount)
{
    Result r;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return r;

    radius  = std::max(0.0f, radius);
    falloff = std::max(0.0f, falloff);
    const float outer = radius + falloff;
    if (outer <= 0.0f) return r;

    ensureHeights(tc);
    const uint32_t res = tc.resolution;
    if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;

    const float halfX = tc.sizeX * 0.5f;
    const float halfZ = tc.sizeZ * 0.5f;
    const float stepX = tc.sizeX / static_cast<float>(res - 1);
    const float stepZ = tc.sizeZ / static_cast<float>(res - 1);

    // The brush centre's own height, sampled BEFORE anything moves — Flatten's
    // target, and the "what is under the cursor" the caller gets back.
    const float centerBefore = terrainHeightAt(tc, localX, localZ);

    // Vertex rect around the dab. Everything outside it has weight 0 by
    // construction, so the loop never walks the whole grid for a small brush.
    auto toGrid = [](float v, float lo, float step) { return (v - lo) / step; };
    const int x0 = std::max(0, static_cast<int>(std::floor(toGrid(localX - outer, -halfX, stepX))));
    const int x1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(localX + outer, -halfX, stepX))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toGrid(localZ - outer, -halfZ, stepZ))));
    const int z1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(localZ + outer, -halfZ, stepZ))));

    r.ok = true;   // legal request; a dab entirely off the terrain simply changes nothing
    if (x0 > x1 || z0 > z1)
    {
        r.centerHeight = centerBefore;
        return r;
    }

    auto weightAt = [&](float dist) -> float
    {
        if (dist >= outer) return 0.0f;
        if (dist <= radius) return 1.0f;
        if (falloff < 0.001f) return 0.0f;
        return 1.0f - (dist - radius) / falloff;
    };

    // Smooth reads its neighbourhood from the state before this dab, so the
    // result does not depend on which vertex the loop reached first.
    const std::vector<float> before = (op == Op::Smooth) ? tc.sculptHeights
                                                         : std::vector<float>{};

    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();

    for (int zi = z0; zi <= z1; ++zi)
    {
        const float wz = -halfZ + static_cast<float>(zi) * stepZ;
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float wx = -halfX + static_cast<float>(xi) * stepX;
            const float dx = wx - localX, dz = wz - localZ;
            const float w  = weightAt(std::sqrt(dx * dx + dz * dz));
            if (w <= 0.0f) continue;

            float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
            const float was = h;

            switch (op)
            {
            case Op::Raise:  h += w * amount; break;
            case Op::Lower:  h -= w * amount; break;
            // Set and Flatten blend by the WEIGHT alone: inside `radius` the
            // vertex lands exactly on the target, across the falloff it eases in.
            // That is what makes "set the height at this position" mean it in one
            // call, rather than converging over an unspecified number of them.
            case Op::Set:     h += w * (amount - h); break;
            case Op::Flatten: h += w * (centerBefore - h); break;
            case Op::Smooth:
            {
                float sum = 0.0f; int cnt = 0;
                for (int dzi = -1; dzi <= 1; ++dzi)
                    for (int dxi = -1; dxi <= 1; ++dxi)
                    {
                        const int nz = zi + dzi, nx = xi + dxi;
                        if (nz < 0 || nx < 0 ||
                            nz >= static_cast<int>(res) || nx >= static_cast<int>(res)) continue;
                        sum += before[static_cast<size_t>(nz) * res + nx];
                        ++cnt;
                    }
                const float avg   = cnt > 0 ? sum / static_cast<float>(cnt) : h;
                const float blend = std::clamp(w * amount, 0.0f, 1.0f);
                h += blend * (avg - h);
                break;
            }
            case Op::Roughen:
                h += vhash(static_cast<uint32_t>(xi), static_cast<uint32_t>(zi)) * w * amount;
                break;
            }

            if (h != was) ++r.changed;
            mn = std::min(mn, h);
            mx = std::max(mx, h);
        }
    }

    if (r.changed > 0)
        markRegionDirty(tc, localX - outer, localZ - outer, localX + outer, localZ + outer);

    if (mn <= mx) { r.minHeight = mn; r.maxHeight = mx; }
    r.centerHeight = terrainHeightAt(tc, localX, localZ);
    return r;
}

Result excavatePolygon(TerrainComponent& tc, const std::vector<glm::vec2>& polygon,
                       const ExcavateParams& params)
{
    Result r;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return r;
    if (polygon.size() < 3 || !std::isfinite(params.amount) || !std::isfinite(params.falloff)) return r;

    glm::vec2 lo(std::numeric_limits<float>::max()), hi(std::numeric_limits<float>::lowest());
    double area2 = 0.0;
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const glm::vec2& p = polygon[i];
        const glm::vec2& q = polygon[(i + 1) % polygon.size()];
        if (!std::isfinite(p.x) || !std::isfinite(p.y) ||
            std::fabs(p.x) > kMaxCoord || std::fabs(p.y) > kMaxCoord) return r;
        lo = glm::min(lo, p); hi = glm::max(hi, p);
        area2 += static_cast<double>(p.x) * q.y - static_cast<double>(q.x) * p.y;
    }
    if (std::fabs(area2) <= 1e-12) return r;   // a line or a point: nothing to dig

    const float falloff = std::max(0.0f, params.falloff);
    const float depth   = std::max(0.0f, params.amount);

    ensureHeights(tc);
    const uint32_t res = tc.resolution;
    if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;

    const float halfX = tc.sizeX * 0.5f, halfZ = tc.sizeZ * 0.5f;
    const float stepX = tc.sizeX / static_cast<float>(res - 1);
    const float stepZ = tc.sizeZ / static_cast<float>(res - 1);

    // A vertex this close to the outline is on it: weight 1, whatever rounding
    // says about a slanted edge. A fraction of a grid cell, far below anything a
    // caller means by "outside".
    const float onEdge  = 1e-4f * std::min(stepX, stepZ);
    const float reach   = std::max(falloff, onEdge);

    r.ok = true;   // legal request; a polygon entirely off the terrain simply changes nothing

    // A polygon wholly off the terrain (bank included) is a legal request that
    // moves nothing.
    const float centerX = 0.5f * (lo.x + hi.x), centerZ = 0.5f * (lo.y + hi.y);
    r.centerHeight = terrainHeightAt(tc, centerX, centerZ);
    if (hi.x + reach < -halfX || lo.x - reach > halfX ||
        hi.y + reach < -halfZ || lo.y - reach > halfZ)
        return r;

    // Grid index of a terrain-local coordinate, rounded down or up and clamped to
    // the grid in float first: a huge bank must not overflow the cast.
    auto gridIndex = [res](float q, bool up)
    {
        return static_cast<int>(std::clamp(up ? std::ceil(q) : std::floor(q),
                                           0.0f, static_cast<float>(res - 1)));
    };

    // Vertex rect the polygon and its bank can reach. Everything outside it has
    // weight 0, so a small lake on a big landscape never walks the whole grid.
    const int x0 = gridIndex((lo.x - reach + halfX) / stepX, false);
    const int x1 = gridIndex((hi.x + reach + halfX) / stepX, true);
    const int z0 = gridIndex((lo.y - reach + halfZ) / stepZ, false);
    const int z1 = gridIndex((hi.y + reach + halfZ) / stepZ, true);

    const int W = x1 - x0 + 1, H = z1 - z0 + 1;
    const size_t cells = static_cast<size_t>(W) * H;
    auto at = [W, x0, z0](int xi, int zi) { return static_cast<size_t>(zi - z0) * W + (xi - x0); };

    // ── Which vertices are inside: even-odd, one scanline per grid row ──────
    // Edges are half-open in z (a.z <= z < b.z), so a scanline through a polygon
    // vertex counts it once.
    std::vector<uint8_t> inside(cells, 0);
    {
        std::vector<float> xs;
        for (int zi = z0; zi <= z1; ++zi)
        {
            const float wz = -halfZ + static_cast<float>(zi) * stepZ;
            xs.clear();
            for (size_t i = 0; i < polygon.size(); ++i)
            {
                const glm::vec2& a = polygon[i];
                const glm::vec2& b = polygon[(i + 1) % polygon.size()];
                if ((a.y <= wz) == (b.y <= wz)) continue;
                xs.push_back(a.x + (wz - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2)
            {
                // A span wholly beyond the rect is skipped BEFORE the clamp: clamped,
                // one past the border would land on the border column and mark a
                // vertex that is outside the polygon as inside it.
                const float qa = (xs[k] + halfX) / stepX, qb = (xs[k + 1] + halfX) / stepX;
                if (qb < static_cast<float>(x0) || qa > static_cast<float>(x1)) continue;
                const int xa = std::max(x0, gridIndex(qa, true));
                const int xb = std::min(x1, gridIndex(qb, false));
                for (int xi = xa; xi <= xb; ++xi) inside[at(xi, zi)] = 1;
            }
        }
    }

    // ── Distance of the rest to the outline, only where the bank reaches ────
    // Edge by edge, over the vertices within `reach` of that edge's box: a long
    // outline costs its length times the bank width, not the polygon's area.
    constexpr float kFar = std::numeric_limits<float>::max();
    std::vector<float> dist2(cells, kFar);
    for (size_t i = 0; i < polygon.size(); ++i)
    {
        const glm::vec2& a = polygon[i];
        const glm::vec2& b = polygon[(i + 1) % polygon.size()];
        const int ex0 = std::max(x0, gridIndex((std::min(a.x, b.x) - reach + halfX) / stepX, false));
        const int ex1 = std::min(x1, gridIndex((std::max(a.x, b.x) + reach + halfX) / stepX, true));
        const int ez0 = std::max(z0, gridIndex((std::min(a.y, b.y) - reach + halfZ) / stepZ, false));
        const int ez1 = std::min(z1, gridIndex((std::max(a.y, b.y) + reach + halfZ) / stepZ, true));
        for (int zi = ez0; zi <= ez1; ++zi)
        {
            const float wz = -halfZ + static_cast<float>(zi) * stepZ;
            for (int xi = ex0; xi <= ex1; ++xi)
            {
                const size_t c = at(xi, zi);
                if (inside[c]) continue;
                const float wx = -halfX + static_cast<float>(xi) * stepX;
                dist2[c] = std::min(dist2[c], segmentDist2(wx, wz, a, b));
            }
        }
    }

    // 1 on and inside the outline, 0 from `falloff` outward, smoothstep between.
    auto weightAt = [&](size_t c) -> float
    {
        if (inside[c]) return 1.0f;
        const float d2 = dist2[c];
        if (d2 >= kFar) return 0.0f;
        const float d = std::sqrt(d2);
        if (d <= onEdge) return 1.0f;
        if (d >= falloff) return 0.0f;
        const float t = 1.0f - d / falloff;
        return t * t * (3.0f - 2.0f * t);
    };

    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    int cx0 = x1, cx1 = x0, cz0 = z1, cz1 = z0;   // vertices that actually moved

    for (int zi = z0; zi <= z1; ++zi)
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float w = weightAt(at(xi, zi));
            if (w <= 0.0f) continue;

            float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
            const float was = h;

            if (params.mode == ExcavateMode::Floor)
            {
                // w == 1 assigns, it does not blend: h + 1 * (floor - h) is not
                // bit-equal to floor, and "the bottom is at the floor" is the
                // thing a caller checks.
                if (h > params.amount)
                    h = (w >= 1.0f) ? params.amount : h + w * (params.amount - h);
            }
            else
                h = (w >= 1.0f) ? h - depth : h - w * depth;

            if (h != was)
            {
                ++r.changed;
                cx0 = std::min(cx0, xi); cx1 = std::max(cx1, xi);
                cz0 = std::min(cz0, zi); cz1 = std::max(cz1, zi);
            }
            mn = std::min(mn, h);
            mx = std::max(mx, h);
        }

    if (r.changed > 0)
    {
        // The vertices that moved, one grid step wider: a vertex on a chunk border
        // belongs to both chunks, and the dirty test is on the chunk rectangles.
        markRegionDirty(tc,
                        -halfX + static_cast<float>(cx0) * stepX - stepX,
                        -halfZ + static_cast<float>(cz0) * stepZ - stepZ,
                        -halfX + static_cast<float>(cx1) * stepX + stepX,
                        -halfZ + static_cast<float>(cz1) * stepZ + stepZ);
    }

    if (mn <= mx) { r.minHeight = mn; r.maxHeight = mx; }
    r.centerHeight = terrainHeightAt(tc, centerX, centerZ);
    return r;
}

} // namespace TerrainSculpt
