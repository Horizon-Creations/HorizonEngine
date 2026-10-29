#include "MeshSimplifier.h"
#include <meshoptimizer.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

namespace {

using MS = MeshSimplifier;

size_t resolveTargetIndexCount(const MS::Settings& s, size_t srcIndexCount)
{
    const size_t srcTriangles = srcIndexCount / 3;
    size_t targetTriangles = static_cast<size_t>(
        std::llround(static_cast<double>(s.targetRatio) * static_cast<double>(srcTriangles)));
    if (s.targetTriangleCount != SIZE_MAX)
        targetTriangles = std::min(targetTriangles, s.targetTriangleCount);
    targetTriangles = std::max(targetTriangles, s.minTriangleCount);
    targetTriangles = std::min(targetTriangles, srcTriangles);
    return targetTriangles * 3;
}

// Two vertices are "the same point" only if their position bits are identical —
// exactly what happens where an importer duplicates a vertex to carry two UVs
// across a seam. Hashing the raw bits (not the floats) keeps this exact and cheap.
struct PosKey {
    uint32_t x, y, z;
    bool operator==(const PosKey& o) const { return x == o.x && y == o.y && z == o.z; }
};
struct PosKeyHash {
    size_t operator()(const PosKey& k) const noexcept
    {
        size_t h = k.x;
        h = h * 1099511628211ull ^ k.y;
        h = h * 1099511628211ull ^ k.z;
        return h;
    }
};
PosKey keyOf(const glm::vec3& p)
{
    PosKey k{};
    std::memcpy(&k.x, &p.x, sizeof(float));
    std::memcpy(&k.y, &p.y, sizeof(float));
    std::memcpy(&k.z, &p.z, sizeof(float));
    return k;
}

// Locks every vertex that sits on a UV seam: same position as another vertex, but
// a different UV. Collapsing one side of a seam without the other tears the UV.
std::vector<unsigned char> buildUvSeamLocks(const std::vector<MS::Vertex>& verts)
{
    std::unordered_map<PosKey, std::vector<uint32_t>, PosKeyHash> byPosition;
    byPosition.reserve(verts.size());
    for (uint32_t i = 0; i < verts.size(); ++i)
        byPosition[keyOf(verts[i].position)].push_back(i);

    std::vector<unsigned char> locks(verts.size(), 0);
    for (const auto& [pos, members] : byPosition)
    {
        if (members.size() < 2)
            continue;
        const glm::vec2 firstUv = verts[members[0]].uv;
        const bool isSeam = std::any_of(members.begin() + 1, members.end(),
            [&](uint32_t idx) { return verts[idx].uv != firstUv; });
        if (!isSeam)
            continue;
        for (uint32_t idx : members)
            locks[idx] = meshopt_SimplifyVertex_Lock;
    }
    return locks;
}

// Builds a compact Mesh containing only the vertices `indices` still reference,
// remapped in first-seen order (deterministic given a deterministic input).
MS::Mesh compact(const std::vector<MS::Vertex>& sourceVerts, const std::vector<uint32_t>& indices)
{
    MS::Mesh out;
    out.indices.reserve(indices.size());
    std::vector<uint32_t> remap(sourceVerts.size(), UINT32_MAX);
    for (uint32_t idx : indices)
    {
        uint32_t& r = remap[idx];
        if (r == UINT32_MAX)
        {
            r = static_cast<uint32_t>(out.vertices.size());
            out.vertices.push_back(sourceVerts[idx]);
        }
        out.indices.push_back(r);
    }
    return out;
}

} // namespace

MeshSimplifier::Result MeshSimplifier::simplify(const Mesh& input, const Settings& settings)
{
    Result result;
    const size_t vertexCount = input.vertices.size();
    const size_t indexCount  = input.indices.size();
    if (vertexCount == 0 || indexCount < 3)
    {
        result.mesh             = input;
        result.hitTargetExactly = true;
        return result;
    }

    const size_t targetIndexCount = resolveTargetIndexCount(settings, indexCount);

    // Attribute buffer for the error metric: normal (3) + UV (2) per vertex, packed
    // separately from position because meshoptimizer takes its own stride for each.
    std::vector<float> attrs(vertexCount * 5);
    for (size_t i = 0; i < vertexCount; ++i)
    {
        const Vertex& v = input.vertices[i];
        attrs[i * 5 + 0] = v.normal.x;
        attrs[i * 5 + 1] = v.normal.y;
        attrs[i * 5 + 2] = v.normal.z;
        attrs[i * 5 + 3] = v.uv.x;
        attrs[i * 5 + 4] = v.uv.y;
    }
    const float attrWeights[5] = {
        settings.normalWeight, settings.normalWeight, settings.normalWeight,
        settings.uvWeight, settings.uvWeight,
    };

    const std::vector<unsigned char> vertexLock = buildUvSeamLocks(input.vertices);

    // pruneComponents and lockMeshBorder are mutually exclusive in meshoptimizer's
    // own option set; pruneComponents wins (foliage mode always sets it, see
    // simplifyFoliage, and needs whole disconnected cards to be removable).
    unsigned int options = 0;
    if (settings.pruneComponents)
        options |= meshopt_SimplifyPrune;
    else if (settings.lockMeshBorder)
        options |= meshopt_SimplifyLockBorder;
    if (settings.absoluteError)
        options |= meshopt_SimplifyErrorAbsolute;

    std::vector<uint32_t> destination(indexCount);
    float resultError = 0.0f;
    const size_t newIndexCount = meshopt_simplifyWithAttributes(
        destination.data(), input.indices.data(), indexCount,
        &input.vertices[0].position.x, vertexCount, sizeof(Vertex),
        attrs.data(), 5 * sizeof(float),
        attrWeights, 5,
        vertexLock.data(),
        targetIndexCount, settings.targetError, options, &resultError);
    destination.resize(newIndexCount);

    result.mesh             = compact(input.vertices, destination);
    result.resultError      = resultError;
    result.hitTargetExactly = (newIndexCount <= targetIndexCount);
    return result;
}

float MeshSimplifier::computeAlphaWeightedArea(const Mesh& mesh, const uint8_t* alphaMask, int width, int height)
{
    if (!alphaMask || width <= 0 || height <= 0)
        return 0.0f;

    double total = 0.0;
    for (size_t t = 0; t + 2 < mesh.indices.size(); t += 3)
    {
        const Vertex& a = mesh.vertices[mesh.indices[t + 0]];
        const Vertex& b = mesh.vertices[mesh.indices[t + 1]];
        const Vertex& c = mesh.vertices[mesh.indices[t + 2]];

        const float area = 0.5f * glm::length(glm::cross(b.position - a.position, c.position - a.position));
        const glm::vec2 centroidUv = (a.uv + b.uv + c.uv) / 3.0f;
        const int px = std::clamp(static_cast<int>(centroidUv.x * static_cast<float>(width)), 0, width - 1);
        const int py = std::clamp(static_cast<int>(centroidUv.y * static_cast<float>(height)), 0, height - 1);
        const float alpha = static_cast<float>(alphaMask[py * width + px]) / 255.0f;

        total += static_cast<double>(area) * alpha;
    }
    return static_cast<float>(total);
}

MeshSimplifier::FoliageResult MeshSimplifier::simplifyFoliage(const Mesh& lod0, const FoliageSettings& settings)
{
    FoliageResult result;
    result.lod0AlphaArea = computeAlphaWeightedArea(lod0, settings.alphaMask, settings.alphaWidth, settings.alphaHeight);

    Settings s = settings.base;
    s.pruneComponents  = true;
    s.lockMeshBorder   = false;
    s.minTriangleCount = std::max(s.minTriangleCount, settings.minCardTriangleCount);

    float ratio       = s.targetRatio;
    float bestCoverage = -1.0f;
    Result best;
    int retries = 0;
    for (;; ++retries)
    {
        s.targetRatio = ratio;
        Result candidate = simplify(lod0, s);
        const float area = computeAlphaWeightedArea(candidate.mesh, settings.alphaMask, settings.alphaWidth, settings.alphaHeight);
        const float coverage = (result.lod0AlphaArea > 0.0f) ? (area / result.lod0AlphaArea) : 1.0f;

        if (coverage > bestCoverage)
        {
            bestCoverage           = coverage;
            best                   = candidate;
            result.resultAlphaArea = area;
            result.coverageRatio   = coverage;
        }
        if (coverage >= settings.minSilhouetteCoverage || retries >= settings.maxRetries)
            break;
        ratio = ratio + (1.0f - ratio) * 0.5f; // back off toward LOD0
    }

    result.mesh             = std::move(best.mesh);
    result.resultError      = best.resultError;
    result.hitTargetExactly = best.hitTargetExactly;
    result.retriesUsed      = retries;
    result.metCoverageGoal  = (result.coverageRatio >= settings.minSilhouetteCoverage);
    return result;
}
