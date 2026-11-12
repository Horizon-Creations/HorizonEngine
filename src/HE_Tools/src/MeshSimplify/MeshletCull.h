#pragma once
#include <cstdint>
#include <vector>
#include <glm/glm.hpp>
#include "ContentManager/Assets.h"

// Meshlet (cluster) culling PROTOTYPE (Thema 331, step 6). Tool/test side only, nothing
// in the renderer calls it: it exists to put numbers on the question "does culling
// pieces of one plant pay on top of LOD + per-instance culling?" (answer and the
// measurements: docs/auto-lod-meshlet-culling-2026-11-08.md).
//
// A meshlet is a run of <= maxTriangles triangles over <= maxVertices vertices with a
// bounding sphere and a normal cone (meshoptimizer builds both). The three tests a
// GPU-driven pipeline runs per meshlet are here as CPU functions so a unit test can
// check them against brute force:
//   * frustum      sphere vs six planes                      (conservative)
//   * back face    cone vs eye, SINGLE-SIDED surfaces only  (a double-sided leaf is
//                  never back-facing as a whole, so foliage must not use this test)
//   * distance     not here: the per-instance / per-bucket test already covers it
namespace MeshletCull
{

struct Meshlet
{
    glm::vec3 center{ 0.0f };   // object space
    float     radius = 0.0f;
    glm::vec3 coneApex{ 0.0f }; // object space
    glm::vec3 coneAxis{ 0.0f }; // unit, object space; (0,0,0) with coneCutoff 1 = no cone
    float     coneCutoff = 1.0f; // cos(half angle) + margin; >= 1 means "never cull"
    uint32_t  firstTriangle = 0; // into Set::triangles (3 vertex indices each)
    uint32_t  triangleCount = 0;
    uint32_t  vertexCount   = 0;
};

struct Set
{
    std::vector<Meshlet>  meshlets;
    // Re-ordered copy of the index buffer: meshlet k owns triangles
    // [firstTriangle, firstTriangle + triangleCount), indices into the ORIGINAL vertices.
    // This is the index buffer a draw per surviving meshlet range would read.
    std::vector<uint32_t> indices;
    uint32_t              sourceTriangles = 0;
};

// maxTriangles up to 512, maxVertices up to 255 (meshoptimizer limits). 64/124 is the
// NVIDIA/AMD-friendly mesh-shader shape; coneWeight 0 favours spatial compactness (the
// frustum test), 1 favours normal coherence (the back-face test).
Set build(const StaticMeshAsset& mesh, size_t maxVertices = 64, size_t maxTriangles = 124,
          float coneWeight = 0.0f);

struct Frustum
{
    glm::vec4 plane[6]; // xyz = inward unit normal, w = offset: inside when dot(n, p) + w >= 0
    static Frustum fromViewProj(const glm::mat4& viewProj);
};

// True when the sphere may touch the frustum (false = certainly outside).
bool sphereVisible(const Frustum& f, const glm::vec3& center, float radius);

// Meshlet of an instance placed by `model` (uniform scale assumed): bounding sphere in world space.
bool meshletVisible(const Frustum& f, const Meshlet& m, const glm::mat4& model, float uniformScale);

// True when every triangle of the meshlet faces away from `eyeObject` (the eye in the
// instance's object space). Valid only for a single-sided surface.
bool meshletBackfacing(const Meshlet& m, const glm::vec3& eyeObject);

} // namespace MeshletCull
