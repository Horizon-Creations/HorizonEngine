#include "MeshletCull.h"
#include <meshoptimizer.h>
#include <algorithm>
#include <cmath>

namespace MeshletCull
{

namespace
{
// Where the positions live: SoA `vertices` (12-byte stride) or the cooked interleaved
// buffer (32-byte stride, position first).
struct PositionView
{
    const float* data   = nullptr;
    size_t       stride = 0;   // bytes
    size_t       count  = 0;
};

PositionView positionsOf(const StaticMeshAsset& m)
{
    PositionView v;
    if (m.cooked && !m.interleaved.empty())
    {
        v.data = m.interleaved.data(); v.stride = 8 * sizeof(float); v.count = m.interleaved.size() / 8;
    }
    else
    {
        v.data = m.vertices.data(); v.stride = 3 * sizeof(float); v.count = m.vertices.size() / 3;
    }
    return v;
}
} // namespace

Set build(const StaticMeshAsset& mesh, size_t maxVertices, size_t maxTriangles, float coneWeight)
{
    Set out;
    const PositionView pos = positionsOf(mesh);
    if (pos.count == 0 || mesh.indices.size() < 3)
        return out;

    // meshopt asserts on limits; clamp instead so a bad argument cannot abort a tool.
    maxVertices  = std::clamp<size_t>(maxVertices, 3, 255);
    maxTriangles = std::clamp<size_t>(maxTriangles, 1, 512);
    maxTriangles = (maxTriangles + 3) / 4 * 4; // meshoptimizer wants a multiple of 4

    const size_t indexCount = mesh.indices.size() / 3 * 3;
    out.sourceTriangles = static_cast<uint32_t>(indexCount / 3);

    const size_t bound = meshopt_buildMeshletsBound(indexCount, maxVertices, maxTriangles);
    std::vector<meshopt_Meshlet> raw(bound);
    std::vector<unsigned int>    meshletVertices(indexCount);
    std::vector<unsigned char>   meshletTriangles(indexCount);

    const size_t n = meshopt_buildMeshlets(raw.data(), meshletVertices.data(), meshletTriangles.data(),
                                           mesh.indices.data(), indexCount, pos.data, pos.count, pos.stride,
                                           maxVertices, maxTriangles, coneWeight);
    raw.resize(n);

    out.meshlets.reserve(n);
    out.indices.reserve(indexCount);
    for (const meshopt_Meshlet& r : raw)
    {
        const meshopt_Bounds b = meshopt_computeMeshletBounds(
            meshletVertices.data() + r.vertex_offset, meshletTriangles.data() + r.triangle_offset,
            r.triangle_count, pos.data, pos.count, pos.stride);

        Meshlet m;
        m.center        = { b.center[0], b.center[1], b.center[2] };
        m.radius        = b.radius;
        m.coneApex      = { b.cone_apex[0], b.cone_apex[1], b.cone_apex[2] };
        m.coneAxis      = { b.cone_axis[0], b.cone_axis[1], b.cone_axis[2] };
        m.coneCutoff    = b.cone_cutoff;
        m.firstTriangle = static_cast<uint32_t>(out.indices.size() / 3);
        m.triangleCount = r.triangle_count;
        m.vertexCount   = r.vertex_count;
        for (unsigned t = 0; t < r.triangle_count * 3; ++t)
            out.indices.push_back(meshletVertices[r.vertex_offset + meshletTriangles[r.triangle_offset + t]]);
        out.meshlets.push_back(m);
    }
    return out;
}

Frustum Frustum::fromViewProj(const glm::mat4& m)
{
    // Gribb/Hartmann on a column-major matrix. Depth range is irrelevant for the
    // prototype: the near plane is taken as row3 + row2 (GL, -1..1) which is the more
    // conservative of the two conventions (a point at z in [0,1] clip depth passes both).
    auto row = [&](int r) { return glm::vec4(m[0][r], m[1][r], m[2][r], m[3][r]); };
    Frustum f;
    f.plane[0] = row(3) + row(0);
    f.plane[1] = row(3) - row(0);
    f.plane[2] = row(3) + row(1);
    f.plane[3] = row(3) - row(1);
    f.plane[4] = row(3) + row(2);
    f.plane[5] = row(3) - row(2);
    for (glm::vec4& p : f.plane)
    {
        const float len = glm::length(glm::vec3(p));
        if (len > 0.0f) p /= len;
    }
    return f;
}

bool sphereVisible(const Frustum& f, const glm::vec3& c, float r)
{
    for (const glm::vec4& p : f.plane)
        if (glm::dot(glm::vec3(p), c) + p.w < -r)
            return false;
    return true;
}

bool meshletVisible(const Frustum& f, const Meshlet& m, const glm::mat4& model, float s)
{
    const glm::vec3 c = glm::vec3(model * glm::vec4(m.center, 1.0f));
    return sphereVisible(f, c, m.radius * s);
}

bool meshletBackfacing(const Meshlet& m, const glm::vec3& eye)
{
    if (m.coneCutoff >= 1.0f) return false;
    const glm::vec3 d = m.coneApex - eye;
    const float len = glm::length(d);
    if (len <= 0.0f) return false;
    // meshoptimizer: cull when dot(normalize(apex - eye), axis) >= cutoff
    return glm::dot(d / len, m.coneAxis) >= m.coneCutoff;
}

} // namespace MeshletCull
