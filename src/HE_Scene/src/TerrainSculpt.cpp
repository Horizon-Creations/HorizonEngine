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
    {
        // Only the chunks under the dab have to be rebuilt. `dirty` stays as it
        // was: setting it would rebuild all 64+ of them, which is the difference
        // between a brush and a regeneration.
        const float mnX = localX - outer, mxX = localX + outer;
        const float mnZ = localZ - outer, mxZ = localZ + outer;
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

    if (mn <= mx) { r.minHeight = mn; r.maxHeight = mx; }
    r.centerHeight = terrainHeightAt(tc, localX, localZ);
    return r;
}

} // namespace TerrainSculpt
