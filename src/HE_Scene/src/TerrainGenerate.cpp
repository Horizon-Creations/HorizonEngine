#include "HorizonScene/TerrainGenerate.h"
#include "HorizonScene/TerrainSculpt.h"
#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <vector>

namespace TerrainGenerate
{

namespace
{
    // Weight of the rim blend at an offset (dx, dz) from the area's centre: 0 on
    // and outside the ellipse, easing up by smoothstep over `falloff` world units
    // measured inwards along the ray from the centre, 1 beyond that.
    //
    // With d the normalised elliptical distance, the rim on the ray through p
    // sits at |p| / d, so the distance left to it is |p| / d − |p|. For a circle
    // that is simply radius − r; for an ellipse it keeps the blend the same
    // width in metres all the way round, instead of squeezing it on the short
    // axis the way a blend in d would.
    float rimWeight(float dx, float dz, float rx, float rz, float falloff)
    {
        const float ex = dx / rx, ez = dz / rz;
        const float d  = std::sqrt(ex * ex + ez * ez);
        if (d >= 1.0f) return 0.0f;
        if (falloff <= 0.0f) return 1.0f;

        // The centre itself has no ray; the nearest rim is the short radius.
        float edge;
        if (d <= 1e-6f)
            edge = std::min(rx, rz);
        else
        {
            const float len = std::sqrt(dx * dx + dz * dz);
            edge = len / d - len;
        }
        const float t = std::clamp(edge / falloff, 0.0f, 1.0f);
        return t * t * (3.0f - 2.0f * t);
    }
}

Result mountain(TerrainComponent& tc, const Area& area, const Params& params)
{
    Result r;
    if (tc.sizeX <= 0.0f || tc.sizeZ <= 0.0f) return r;
    if (!(area.radiusX > 0.0f) || !(area.radiusZ > 0.0f)) return r;

    TerrainSculpt::ensureHeights(tc);
    const uint32_t res = tc.resolution;
    if (tc.sculptHeights.size() != static_cast<size_t>(res) * res) return r;

    r.ok = true;   // legal request from here on; it may still change nothing
    if (params.maxHeight == 0.0f) return r;

    const float halfX = tc.sizeX * 0.5f;
    const float halfZ = tc.sizeZ * 0.5f;
    const float stepX = tc.sizeX / static_cast<float>(res - 1);
    const float stepZ = tc.sizeZ / static_cast<float>(res - 1);

    const float rx = area.radiusX, rz = area.radiusZ;
    const float falloff   = std::max(0.0f, params.falloff);
    const float roughness = std::clamp(params.roughness, 0.0f, 1.0f);
    // Noise coordinates are the offset in units of the larger radius, so
    // `frequency` counts features across the AREA, not across the terrain.
    const float noiseScale = 1.0f / std::max(rx, rz);

    // Vertex rect around the area's bounding box. Everything outside it has
    // weight 0 by construction, so a small mountain never walks the whole grid.
    auto toGrid = [](float v, float lo, float step) { return (v - lo) / step; };
    const int x0 = std::max(0, static_cast<int>(std::floor(toGrid(area.centerX - rx, -halfX, stepX))));
    const int x1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(area.centerX + rx, -halfX, stepX))));
    const int z0 = std::max(0, static_cast<int>(std::floor(toGrid(area.centerZ - rz, -halfZ, stepZ))));
    const int z1 = std::min<int>(static_cast<int>(res) - 1,
                                 static_cast<int>(std::ceil(toGrid(area.centerZ + rz, -halfZ, stepZ))));
    if (x0 > x1 || z0 > z1) return r;

    // ── Pass 1: the raw shape, and its peak ─────────────────────────────────
    // Held until the peak is known, because maxHeight is a promise about the
    // tallest point of the formation — with roughness > 0 the noise decides
    // where that is and how high the unscaled shape gets.
    const int w = x1 - x0 + 1;
    std::vector<float> shape(static_cast<size_t>(w) * (z1 - z0 + 1), 0.0f);
    float shapeMax = 0.0f;
    for (int zi = z0; zi <= z1; ++zi)
    {
        const float dz = -halfZ + static_cast<float>(zi) * stepZ - area.centerZ;
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float dx = -halfX + static_cast<float>(xi) * stepX - area.centerX;
            const float rim = rimWeight(dx, dz, rx, rz, falloff);
            if (rim <= 0.0f) continue;

            float relief = 1.0f;
            if (roughness > 0.0f)
            {
                // Lacunarity and gain are the TerrainComponent defaults, so a
                // formation's relief reads like the seeded landscape around it.
                const float n = terrainFbm(params.seed, dx * noiseScale, dz * noiseScale,
                                           params.octaves, params.frequency, 2.0f, 0.5f);
                relief = (1.0f - roughness) + roughness * n;
            }
            const float s = rim * relief;
            shape[static_cast<size_t>(zi - z0) * w + (xi - x0)] = s;
            shapeMax = std::max(shapeMax, s);
        }
    }
    if (shapeMax <= 0.0f) return r;

    // ── Pass 2: add it, scaled so the peak is exactly maxHeight ─────────────
    const float scale = params.maxHeight / shapeMax;
    float mn = std::numeric_limits<float>::max();
    float mx = std::numeric_limits<float>::lowest();
    float peak = 0.0f;
    for (int zi = z0; zi <= z1; ++zi)
        for (int xi = x0; xi <= x1; ++xi)
        {
            const float s = shape[static_cast<size_t>(zi - z0) * w + (xi - x0)];
            if (s <= 0.0f) continue;
            const float add = s * scale;
            float& h = tc.sculptHeights[static_cast<size_t>(zi) * res + xi];
            const float was = h;
            h += add;
            if (h == was) continue;
            ++r.changed;
            mn = std::min(mn, h);
            mx = std::max(mx, h);
            if (std::fabs(add) > std::fabs(peak)) peak = add;
        }

    if (r.changed > 0)
    {
        // Only the chunks under the area have to be rebuilt; `dirty` would
        // rebuild all of them.
        const float mnX = area.centerX - rx, mxX = area.centerX + rx;
        const float mnZ = area.centerZ - rz, mxZ = area.centerZ + rz;
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
        r.minHeight = mn;
        r.maxHeight = mx;
        r.peakAdded = peak;
    }
    return r;
}

} // namespace TerrainGenerate
