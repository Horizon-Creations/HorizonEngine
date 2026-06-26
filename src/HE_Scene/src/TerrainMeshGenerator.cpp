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

std::vector<float> computeTerrainHeightField(const TerrainComponent& tc)
{
    const uint32_t res       = std::clamp(tc.resolution, 2u, 1024u);
    const uint32_t vertCount = res * res;
    std::vector<float> heights(vertCount, 0.0f);
    if (tc.sculptHeights.size() == static_cast<size_t>(vertCount))
    {
        heights = tc.sculptHeights; // sculpted data overrides everything
    }
    else if (tc.seed != 0)
    {
        for (uint32_t zi = 0; zi < res; ++zi)
            for (uint32_t xi = 0; xi < res; ++xi)
            {
                const float nx = static_cast<float>(xi) / static_cast<float>(res - 1);
                const float nz = static_cast<float>(zi) / static_cast<float>(res - 1);
                heights[zi * res + xi] = tc.heightScale
                    * fbm(tc.seed, nx, nz, tc.octaves, tc.frequency, tc.lacunarity, tc.gain);
            }
    }
    // seed == 0 → heights remain zero → flat terrain
    return heights;
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
    const std::vector<float> heights = computeTerrainHeightField(tc);

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

// ─── Chunked / LOD terrain ──────────────────────────────────────────────────
namespace
{
    // Bilinear sample of the row-major res×res height field at normalized (u,v).
    float sampleField(const std::vector<float>& h, uint32_t res, float u, float v)
    {
        u = std::clamp(u, 0.0f, 1.0f);
        v = std::clamp(v, 0.0f, 1.0f);
        const float fx = u * static_cast<float>(res - 1);
        const float fz = v * static_cast<float>(res - 1);
        const int   ix = static_cast<int>(fx);
        const int   iz = static_cast<int>(fz);
        const int   ix1 = std::min(ix + 1, static_cast<int>(res) - 1);
        const int   iz1 = std::min(iz + 1, static_cast<int>(res) - 1);
        const float tx = fx - static_cast<float>(ix);
        const float tz = fz - static_cast<float>(iz);
        const float h00 = h[static_cast<size_t>(iz)  * res + ix ];
        const float h10 = h[static_cast<size_t>(iz)  * res + ix1];
        const float h01 = h[static_cast<size_t>(iz1) * res + ix ];
        const float h11 = h[static_cast<size_t>(iz1) * res + ix1];
        return (h00 + (h10 - h00) * tx) + ((h01 - h00) + (h00 - h10 - h01 + h11) * tx) * tz;
    }

    // Surface normal at (u,v), from the global field at SOURCE-cell spacing so every
    // chunk/LOD computes the same normal at a shared position → no lighting seams.
    void sampleFieldNormal(const std::vector<float>& h, uint32_t res, float sizeX, float sizeZ,
                           float u, float v, float& nx, float& ny, float& nz)
    {
        const float d  = 1.0f / static_cast<float>(res - 1);
        const float hl = sampleField(h, res, u - d, v);
        const float hr = sampleField(h, res, u + d, v);
        const float hb = sampleField(h, res, u, v - d);
        const float ht = sampleField(h, res, u, v + d);
        const float gx = (hr - hl) / (sizeX * d * 2.0f);   // dh/dx
        const float gz = (ht - hb) / (sizeZ * d * 2.0f);   // dh/dz
        nx = -gx; ny = 1.0f; nz = -gz;
        const float len = std::sqrt(nx * nx + ny * ny + nz * nz);
        if (len > 0.0f) { nx /= len; ny /= len; nz /= len; }
    }
}

std::vector<float> resampleHeightField(const std::vector<float>& src,
                                       uint32_t oldRes, uint32_t newRes)
{
    if (oldRes < 2 || newRes < 2 || src.size() != static_cast<size_t>(oldRes) * oldRes)
        return src;
    std::vector<float> out(static_cast<size_t>(newRes) * newRes);
    for (uint32_t z = 0; z < newRes; ++z)
        for (uint32_t x = 0; x < newRes; ++x)
        {
            const float u = static_cast<float>(x) / static_cast<float>(newRes - 1);
            const float v = static_cast<float>(z) / static_cast<float>(newRes - 1);
            out[static_cast<size_t>(z) * newRes + x] = sampleField(src, oldRes, u, v);
        }
    return out;
}

StaticMeshAsset generateTerrainChunkMesh(
    const std::vector<float>& heights, uint32_t srcRes,
    float sizeX, float sizeZ,
    float u0, float v0, float u1, float v1,
    uint32_t vertsPerSide)
{
    const uint32_t N = std::max(2u, vertsPerSide);
    const float halfX = sizeX * 0.5f;
    const float halfZ = sizeZ * 0.5f;
    // Chunk centre (terrain-local) — the chunk entity is positioned here, and chunk
    // vertices are stored relative to it so per-chunk culling + distance-LOD work.
    const float cxLocal = -halfX + ((u0 + u1) * 0.5f) * sizeX;
    const float czLocal = -halfZ + ((v0 + v1) * 0.5f) * sizeZ;

    StaticMeshAsset mesh;
    mesh.type = HE::AssetType::StaticMesh;
    mesh.name = "terrain_chunk";
    mesh.path = "mem://terrain_chunk";

    const size_t grid = static_cast<size_t>(N) * N;
    mesh.vertices.reserve((grid + static_cast<size_t>(N) * 4) * 3);
    mesh.normals .reserve((grid + static_cast<size_t>(N) * 4) * 3);
    mesh.uvs     .reserve((grid + static_cast<size_t>(N) * 4) * 2);

    float hmin =  1e30f, hmax = -1e30f;
    for (uint32_t j = 0; j < N; ++j)
        for (uint32_t i = 0; i < N; ++i)
        {
            const float u  = u0 + (u1 - u0) * static_cast<float>(i) / static_cast<float>(N - 1);
            const float v  = v0 + (v1 - v0) * static_cast<float>(j) / static_cast<float>(N - 1);
            const float wx = -halfX + u * sizeX;
            const float wz = -halfZ + v * sizeZ;
            const float hy = sampleField(heights, srcRes, u, v);
            hmin = std::min(hmin, hy); hmax = std::max(hmax, hy);

            mesh.vertices.push_back(wx - cxLocal);
            mesh.vertices.push_back(hy);
            mesh.vertices.push_back(wz - czLocal);
            mesh.uvs.push_back(u);
            mesh.uvs.push_back(v);
            float nx, ny, nz; sampleFieldNormal(heights, srcRes, sizeX, sizeZ, u, v, nx, ny, nz);
            mesh.normals.push_back(nx); mesh.normals.push_back(ny); mesh.normals.push_back(nz);
        }

    // Surface triangles (winding matches generateTerrainMesh → normal +Y).
    for (uint32_t j = 0; j < N - 1; ++j)
        for (uint32_t i = 0; i < N - 1; ++i)
        {
            const uint32_t bl = j * N + i;
            const uint32_t br = j * N + (i + 1);
            const uint32_t tl = (j + 1) * N + i;
            const uint32_t tr = (j + 1) * N + (i + 1);
            mesh.indices.push_back(bl); mesh.indices.push_back(tl); mesh.indices.push_back(br);
            mesh.indices.push_back(br); mesh.indices.push_back(tl); mesh.indices.push_back(tr);
        }

    // ── Skirt: a downward wall around the perimeter hides the cracks that appear
    // where this chunk meets a neighbour at a different LOD. Depth adapts to the
    // chunk's slope (height span) and cell size. One-sided — the backends render
    // terrain without backface culling, so a single winding shows from any view.
    const float cellWorld  = std::max(sizeX * (u1 - u0), sizeZ * (v1 - v0)) / static_cast<float>(N - 1);
    const float heightSpan = (hmax > hmin) ? (hmax - hmin) : 0.0f;
    const float skirtDepth = std::max(cellWorld * 0.5f, heightSpan * 0.25f) + 0.01f;

    // Perimeter grid-vertex indices in loop order (bottom→right→top→left).
    std::vector<uint32_t> ring;
    ring.reserve(static_cast<size_t>(N) * 4);
    for (uint32_t i = 0;       i < N;     ++i) ring.push_back(0 * N + i);            // bottom
    for (uint32_t j = 1;       j < N;     ++j) ring.push_back(j * N + (N - 1));      // right
    for (uint32_t i = N - 1;   i-- > 0;      ) ring.push_back((N - 1) * N + i);      // top (reverse)
    for (uint32_t j = N - 1;   j-- > 1;      ) ring.push_back(j * N + 0);            // left (reverse)

    // One skirt vertex below each ring vertex (same XZ + normal + UV, y dropped).
    const uint32_t skirtBase = static_cast<uint32_t>(mesh.vertices.size() / 3);
    for (uint32_t idx : ring)
    {
        mesh.vertices.push_back(mesh.vertices[idx * 3 + 0]);
        mesh.vertices.push_back(mesh.vertices[idx * 3 + 1] - skirtDepth);
        mesh.vertices.push_back(mesh.vertices[idx * 3 + 2]);
        mesh.normals.push_back(mesh.normals[idx * 3 + 0]);
        mesh.normals.push_back(mesh.normals[idx * 3 + 1]);
        mesh.normals.push_back(mesh.normals[idx * 3 + 2]);
        mesh.uvs.push_back(mesh.uvs[idx * 2 + 0]);
        mesh.uvs.push_back(mesh.uvs[idx * 2 + 1]);
    }

    const uint32_t ringN = static_cast<uint32_t>(ring.size());
    for (uint32_t k = 0; k < ringN; ++k)
    {
        const uint32_t a  = ring[k];
        const uint32_t b  = ring[(k + 1) % ringN];
        const uint32_t sa = skirtBase + k;
        const uint32_t sb = skirtBase + (k + 1) % ringN;
        // One-sided wall quad (a, b, sb, sa) → 2 triangles.
        mesh.indices.push_back(a);  mesh.indices.push_back(b);  mesh.indices.push_back(sb);
        mesh.indices.push_back(a);  mesh.indices.push_back(sb); mesh.indices.push_back(sa);
    }

    return mesh;
}
