#include "HorizonScene/TerrainMeshGenerator.h"
#include "HorizonScene/Components/TerrainComponent.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace
{
    // Wang hash — maps an integer to a pseudo-random uint32
    uint32_t wangHash(uint32_t n)
    {
        n = (n ^ 61u) ^ (n >> 16u);
        n += n << 3u;
        n ^= n >> 4u;
        n *= 0x27D4EB2Du;
        n ^= n >> 15u;
        return n;
    }

    // Hash two grid coordinates + seed to a float in [0, 1)
    float latticeValue(int seed, int ix, int iz)
    {
        const uint32_t h = wangHash(static_cast<uint32_t>(seed)
                         ^ wangHash(static_cast<uint32_t>(ix + 0x45678901)
                         ^ wangHash(static_cast<uint32_t>(iz + 0x12345678))));
        return static_cast<float>(h & 0x00FFFFFFu) / static_cast<float>(0x01000000u);
    }

    // Smooth value noise — bilinear interpolation with cubic smoothstep
    float valueNoise(int seed, float x, float z)
    {
        const int ix = static_cast<int>(std::floor(x));
        const int iz = static_cast<int>(std::floor(z));
        const float fx = x - static_cast<float>(ix);
        const float fz = z - static_cast<float>(iz);
        const float ux = fx * fx * (3.0f - 2.0f * fx);
        const float uz = fz * fz * (3.0f - 2.0f * fz);
        const float v00 = latticeValue(seed, ix,     iz);
        const float v10 = latticeValue(seed, ix + 1, iz);
        const float v01 = latticeValue(seed, ix,     iz + 1);
        const float v11 = latticeValue(seed, ix + 1, iz + 1);
        return (v00 + (v10 - v00) * ux) + ((v01 - v00) + ((v00 - v10 - v01 + v11) * ux)) * uz;
    }

    // fBm — octave sum, result normalised to [0, 1]
    float fbm(int seed, float x, float z, int octaves, float frequency, float lacunarity, float gain)
    {
        float value     = 0.0f;
        float amplitude = 1.0f;
        float total     = 0.0f;
        float f         = frequency;
        const int oct   = std::clamp(octaves, 1, 12);
        for (int i = 0; i < oct; ++i)
        {
            value += amplitude * valueNoise(seed + i * 31337, x * f, z * f);
            total += amplitude;
            f         *= lacunarity;
            amplitude *= gain;
        }
        return (total > 0.0f) ? (value / total) : 0.0f;
    }
}

StaticMeshAsset generateTerrainMesh(const TerrainComponent& tc)
{
    const uint32_t res   = std::clamp(tc.resolution, 2u, 1024u);
    const float    halfX = tc.sizeX * 0.5f;
    const float    halfZ = tc.sizeZ * 0.5f;
    const float    stepX = tc.sizeX / static_cast<float>(res - 1);
    const float    stepZ = tc.sizeZ / static_cast<float>(res - 1);

    // Pre-compute all heights so normal calculation can sample neighbours cheaply
    const uint32_t vertCount = res * res;
    std::vector<float> heights(vertCount);
    if (tc.sculptHeights.size() == static_cast<size_t>(vertCount))
    {
        heights = tc.sculptHeights; // sculpted data overrides everything
    }
    else if (tc.seed != 0)
    {
        for (uint32_t zi = 0; zi < res; ++zi)
        {
            for (uint32_t xi = 0; xi < res; ++xi)
            {
                const float nx = static_cast<float>(xi) / static_cast<float>(res - 1);
                const float nz = static_cast<float>(zi) / static_cast<float>(res - 1);
                heights[zi * res + xi] = tc.heightScale
                    * fbm(tc.seed, nx, nz, tc.octaves, tc.frequency, tc.lacunarity, tc.gain);
            }
        }
    }
    // seed == 0 → heights remain zero → flat terrain

    StaticMeshAsset mesh;
    mesh.type = HE::AssetType::StaticMesh;
    mesh.name = "terrain";
    mesh.path = "mem://terrain";

    mesh.vertices.reserve(static_cast<size_t>(vertCount) * 3);
    mesh.normals .reserve(static_cast<size_t>(vertCount) * 3);
    mesh.uvs     .reserve(static_cast<size_t>(vertCount) * 2);

    for (uint32_t zi = 0; zi < res; ++zi)
    {
        for (uint32_t xi = 0; xi < res; ++xi)
        {
            const float wx = -halfX + static_cast<float>(xi) * stepX;
            const float wy = heights[zi * res + xi];
            const float wz = -halfZ + static_cast<float>(zi) * stepZ;

            mesh.vertices.push_back(wx);
            mesh.vertices.push_back(wy);
            mesh.vertices.push_back(wz);

            mesh.uvs.push_back(static_cast<float>(xi) / static_cast<float>(res - 1));
            mesh.uvs.push_back(static_cast<float>(zi) / static_cast<float>(res - 1));

            // Central differences — one-sided at grid borders
            const float yL = (xi > 0)       ? heights[zi * res + (xi - 1)] : wy;
            const float yR = (xi < res - 1) ? heights[zi * res + (xi + 1)] : wy;
            const float yB = (zi > 0)       ? heights[(zi - 1) * res + xi] : wy;
            const float yT = (zi < res - 1) ? heights[(zi + 1) * res + xi] : wy;
            // tangentX = (sx, yR-yL, 0), tangentZ = (0, yT-yB, sz)
            // normal = cross(tangentZ, tangentX) → (+Y for flat terrain)
            const float sx = (xi > 0 && xi < res - 1) ? 2.0f * stepX : stepX;
            const float sz = (zi > 0 && zi < res - 1) ? 2.0f * stepZ : stepZ;
            float nx2 = -sz * (yR - yL);
            float ny2 =  sx *  sz;
            float nz2 = -sx * (yT - yB);
            const float len = std::sqrt(nx2 * nx2 + ny2 * ny2 + nz2 * nz2);
            if (len > 0.0f) { nx2 /= len; ny2 /= len; nz2 /= len; }

            mesh.normals.push_back(nx2);
            mesh.normals.push_back(ny2);
            mesh.normals.push_back(nz2);
        }
    }

    // Two triangles per cell, winding: (BL, TL, BR) and (BR, TL, TR) → normal +Y
    const size_t cells = static_cast<size_t>(res - 1) * static_cast<size_t>(res - 1);
    mesh.indices.reserve(cells * 6);
    for (uint32_t zi = 0; zi < res - 1; ++zi)
    {
        for (uint32_t xi = 0; xi < res - 1; ++xi)
        {
            const uint32_t bl = zi * res + xi;
            const uint32_t br = zi * res + (xi + 1);
            const uint32_t tl = (zi + 1) * res + xi;
            const uint32_t tr = (zi + 1) * res + (xi + 1);
            mesh.indices.push_back(bl);
            mesh.indices.push_back(tl);
            mesh.indices.push_back(br);
            mesh.indices.push_back(br);
            mesh.indices.push_back(tl);
            mesh.indices.push_back(tr);
        }
    }

    return mesh;
}

float terrainHeightAt(const TerrainComponent& tc, float localX, float localZ)
{
    const float nx = (localX + tc.sizeX * 0.5f) / tc.sizeX;
    const float nz = (localZ + tc.sizeZ * 0.5f) / tc.sizeZ;

    const uint32_t res = std::clamp(tc.resolution, 2u, 1024u);
    if (tc.sculptHeights.size() == static_cast<size_t>(res * res))
    {
        // Bilinear sample from sculpted heights
        const float fx = std::clamp(nx, 0.f, 1.f) * static_cast<float>(res - 1);
        const float fz = std::clamp(nz, 0.f, 1.f) * static_cast<float>(res - 1);
        const int   ix = static_cast<int>(fx);
        const int   iz = static_cast<int>(fz);
        const float tx = fx - static_cast<float>(ix);
        const float tz = fz - static_cast<float>(iz);
        const int   ix1 = std::min(ix + 1, static_cast<int>(res) - 1);
        const int   iz1 = std::min(iz + 1, static_cast<int>(res) - 1);
        const float h00 = tc.sculptHeights[static_cast<size_t>(iz  * static_cast<int>(res) + ix )];
        const float h10 = tc.sculptHeights[static_cast<size_t>(iz  * static_cast<int>(res) + ix1)];
        const float h01 = tc.sculptHeights[static_cast<size_t>(iz1 * static_cast<int>(res) + ix )];
        const float h11 = tc.sculptHeights[static_cast<size_t>(iz1 * static_cast<int>(res) + ix1)];
        return (h00 + (h10 - h00) * tx) + ((h01 - h00) + (h00 - h10 - h01 + h11) * tx) * tz;
    }
    if (tc.seed != 0)
        return tc.heightScale * fbm(tc.seed, nx, nz, tc.octaves, tc.frequency, tc.lacunarity, tc.gain);
    return 0.f;
}
