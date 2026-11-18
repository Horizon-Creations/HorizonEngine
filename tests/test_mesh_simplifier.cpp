#include "doctest.h"
#include "MeshSimplifier.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

using Vertex = MeshSimplifier::Vertex;
using Mesh   = MeshSimplifier::Mesh;

Mesh makeGrid(int n, float size)
{
    Mesh mesh;
    const int verts = n + 1;
    mesh.vertices.resize(static_cast<size_t>(verts) * verts);
    for (int z = 0; z <= n; ++z)
    {
        for (int x = 0; x <= n; ++x)
        {
            Vertex v;
            v.position = glm::vec3(static_cast<float>(x) / n * size, 0.0f, static_cast<float>(z) / n * size);
            v.normal   = glm::vec3(0.0f, 1.0f, 0.0f);
            v.uv       = glm::vec2(static_cast<float>(x) / n, static_cast<float>(z) / n);
            mesh.vertices[static_cast<size_t>(z) * verts + x] = v;
        }
    }
    for (int z = 0; z < n; ++z)
    {
        for (int x = 0; x < n; ++x)
        {
            const uint32_t i0 = static_cast<uint32_t>(z * verts + x);
            const uint32_t i1 = i0 + 1;
            const uint32_t i2 = i0 + static_cast<uint32_t>(verts);
            const uint32_t i3 = i2 + 1;
            mesh.indices.insert(mesh.indices.end(), {i0, i2, i1, i1, i2, i3});
        }
    }
    return mesh;
}

Mesh makeQuad()
{
    Mesh m;
    m.vertices = {
        {{0, 0, 0}, {0, 1, 0}, {0, 0}},
        {{1, 0, 0}, {0, 1, 0}, {1, 0}},
        {{1, 0, 1}, {0, 1, 0}, {1, 1}},
        {{0, 0, 1}, {0, 1, 0}, {0, 1}},
    };
    m.indices = {0, 1, 2, 0, 2, 3};
    return m;
}

// Two triangles that occupy the exact same three positions but carry two
// completely disjoint UV sets — every one of the 6 vertices sits on a UV seam.
Mesh makeFullyLockedSeamPair()
{
    const glm::vec3 p0(0, 0, 0), p1(1, 0, 0), p2(0, 0, 1), n(0, 1, 0);
    Mesh m;
    m.vertices = {
        {p0, n, {0, 0}}, {p1, n, {1, 0}}, {p2, n, {0, 1}},  // triangle A, UV set A
        {p0, n, {5, 5}}, {p1, n, {6, 5}}, {p2, n, {5, 6}},  // triangle B, same positions, UV set B
    };
    m.indices = {0, 1, 2, 3, 4, 5};
    return m;
}

// `count` fully disconnected unit quads along X, each UV-mapped 0..1 — stand-ins
// for independent alpha-tested leaf/blade cards.
Mesh makeFoliageCards(int count)
{
    const glm::vec3 n(0, 1, 0);
    Mesh m;
    for (int i = 0; i < count; ++i)
    {
        const float ox = static_cast<float>(i) * 2.0f;
        const uint32_t base = static_cast<uint32_t>(m.vertices.size());
        m.vertices.push_back({{ox + 0, 0, 0}, n, {0, 0}});
        m.vertices.push_back({{ox + 1, 0, 0}, n, {1, 0}});
        m.vertices.push_back({{ox + 1, 0, 1}, n, {1, 1}});
        m.vertices.push_back({{ox + 0, 0, 1}, n, {0, 1}});
        m.indices.insert(m.indices.end(), {base + 0, base + 1, base + 2, base + 0, base + 2, base + 3});
    }
    return m;
}

void boundsOf(const Mesh& m, glm::vec3& outMin, glm::vec3& outMax)
{
    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(std::numeric_limits<float>::lowest());
    for (const auto& v : m.vertices)
    {
        outMin = glm::min(outMin, v.position);
        outMax = glm::max(outMax, v.position);
    }
}

} // namespace

TEST_CASE("MeshSimplifier reduces a flat grid and keeps result bounds inside the input's")
{
    const Mesh grid = makeGrid(8, 10.0f); // 128 triangles
    MeshSimplifier::Settings s;
    s.targetRatio    = 0.25f;
    s.lockMeshBorder = true;

    const auto result = MeshSimplifier::simplify(grid, s);
    const size_t triCount = result.mesh.indices.size() / 3;

    CHECK(result.mesh.indices.size() % 3 == 0);
    CHECK(triCount < 128);
    CHECK(triCount > 0);

    glm::vec3 srcMin, srcMax, dstMin, dstMax;
    boundsOf(grid, srcMin, srcMax);
    boundsOf(result.mesh, dstMin, dstMax);
    CHECK(dstMin.x >= srcMin.x);
    CHECK(dstMin.y >= srcMin.y);
    CHECK(dstMin.z >= srcMin.z);
    CHECK(dstMax.x <= srcMax.x);
    CHECK(dstMax.y <= srcMax.y);
    CHECK(dstMax.z <= srcMax.z);

    // Plain simplify never moves a vertex — every surviving one must be a verbatim
    // copy (never an interpolation) of one of the grid's uniform attributes.
    for (const auto& v : result.mesh.vertices)
    {
        CHECK(v.normal == glm::vec3(0.0f, 1.0f, 0.0f));
    }
}

TEST_CASE("MeshSimplifier is deterministic for the same input and settings")
{
    const Mesh grid = makeGrid(8, 10.0f);
    MeshSimplifier::Settings s;
    s.targetRatio = 0.4f;

    const auto a = MeshSimplifier::simplify(grid, s);
    const auto b = MeshSimplifier::simplify(grid, s);

    REQUIRE(a.mesh.vertices.size() == b.mesh.vertices.size());
    REQUIRE(a.mesh.indices.size() == b.mesh.indices.size());
    CHECK(a.mesh.vertices == b.mesh.vertices);
    CHECK(a.mesh.indices == b.mesh.indices);
    CHECK(a.resultError == b.resultError);
}

TEST_CASE("MeshSimplifier never asks below the minimum-triangle floor")
{
    const Mesh grid = makeGrid(8, 10.0f); // 128 triangles
    MeshSimplifier::Settings s;
    s.targetRatio       = 0.0f;  // ask for (almost) nothing
    s.minTriangleCount  = 10;

    const auto result = MeshSimplifier::simplify(grid, s);
    const size_t triCount = result.mesh.indices.size() / 3;

    CHECK(triCount >= 10);
    CHECK(triCount < 128); // the floor still let real reduction happen
}

TEST_CASE("MeshSimplifier border lock keeps a single quad fully intact")
{
    // Every vertex of a lone quad sits on the mesh border — with lockMeshBorder on,
    // nothing is eligible to move, so no edge can collapse, however hard the ask.
    const Mesh quad = makeQuad();
    MeshSimplifier::Settings s;
    s.targetRatio    = 0.0f;
    s.targetError    = 1.0f;
    s.lockMeshBorder = true;

    const auto result = MeshSimplifier::simplify(quad, s);
    CHECK(result.mesh.indices.size() == 6u);
    CHECK(result.mesh.vertices.size() == 4u);
}

TEST_CASE("MeshSimplifier locks UV-seam vertices: a fully-seamed pair cannot collapse")
{
    // Both triangles have all three vertices duplicated at the same positions with a
    // disjoint UV set each — every edge of both triangles has two locked endpoints,
    // so no collapse is legal regardless of the (aggressive) target. lockMeshBorder
    // is off so only the UV-seam lock is exercised.
    const Mesh seam = makeFullyLockedSeamPair();
    MeshSimplifier::Settings s;
    s.targetRatio    = 0.0f;
    s.targetError    = 1.0f;
    s.lockMeshBorder = false;

    const auto result = MeshSimplifier::simplify(seam, s);
    CHECK(result.mesh.indices.size() == 6u);
    CHECK(result.mesh.vertices.size() == 6u);

    bool hasSetA = false, hasSetB = false;
    for (const auto& v : result.mesh.vertices)
    {
        if (v.uv == glm::vec2(0, 0)) hasSetA = true;
        if (v.uv == glm::vec2(5, 5)) hasSetB = true;
    }
    CHECK(hasSetA);
    CHECK(hasSetB);
}

TEST_CASE("MeshSimplifier alpha-weighted area matches a known fixture")
{
    const Mesh quad = makeFoliageCards(1); // unit quad, uv (0,0)-(1,1)
    std::vector<uint8_t> opaque(4 * 4, 255);
    std::vector<uint8_t> leftHalfOnly(4 * 4, 0);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 2; ++x)
            leftHalfOnly[static_cast<size_t>(y) * 4 + x] = 255;

    const float fullArea = MeshSimplifier::computeAlphaWeightedArea(quad, opaque.data(), 4, 4);
    const float halfArea = MeshSimplifier::computeAlphaWeightedArea(quad, leftHalfOnly.data(), 4, 4);

    CHECK(fullArea == doctest::Approx(1.0f)); // unit quad, full area
    CHECK(halfArea < fullArea);
    CHECK(halfArea > 0.0f);
}

TEST_CASE("MeshSimplifier foliage mode backs off when coverage would drop too far")
{
    const Mesh cards = makeFoliageCards(5); // 5 equal-area, disconnected cards
    const std::vector<uint8_t> mask(4 * 4, 255);
    MeshSimplifier::FoliageSettings fs;
    fs.base.targetRatio       = 0.3f; // would need at least one whole card gone
    fs.base.targetError       = 1.0f; // let the ratio bind, not the (tight) default error budget
    fs.alphaMask              = mask.data();
    fs.alphaWidth             = 4;
    fs.alphaHeight            = 4;
    fs.minSilhouetteCoverage  = 0.95f;
    fs.maxRetries             = 8;

    const auto result = MeshSimplifier::simplifyFoliage(cards, fs);
    CHECK(result.metCoverageGoal);
    CHECK(result.coverageRatio >= 0.95f);
    CHECK(result.retriesUsed >= 1); // the first, aggressive attempt cannot meet 95%
}

TEST_CASE("MeshSimplifier foliage mode accepts reduction when the coverage goal allows it")
{
    const Mesh cards = makeFoliageCards(5);
    const std::vector<uint8_t> mask(4 * 4, 255);
    MeshSimplifier::FoliageSettings fs;
    fs.base.targetRatio      = 0.3f;
    fs.base.targetError      = 1.0f; // let the ratio bind, not the (tight) default error budget
    fs.alphaMask             = mask.data();
    fs.alphaWidth            = 4;
    fs.alphaHeight           = 4;
    fs.minSilhouetteCoverage = 0.15f; // low enough that even one surviving card clears it
    fs.maxRetries            = 8;

    const auto result = MeshSimplifier::simplifyFoliage(cards, fs);
    CHECK(result.metCoverageGoal);
    CHECK(result.coverageRatio >= 0.15f);
    CHECK(result.mesh.indices.size() < cards.indices.size());
}

TEST_CASE("MeshSimplifier foliage mode never prunes below the card-count floor")
{
    const Mesh cards = makeFoliageCards(5);
    const std::vector<uint8_t> mask(4 * 4, 255);
    MeshSimplifier::FoliageSettings fs;
    fs.base.targetRatio        = 0.0f; // ask for nothing
    fs.base.targetError        = 1.0f; // let the ratio (and the floor) actually bind
    fs.alphaMask               = mask.data();
    fs.alphaWidth              = 4;
    fs.alphaHeight             = 4;
    fs.minSilhouetteCoverage   = 0.0f; // accept anything — isolate the floor itself
    fs.minCardTriangleCount    = 4;
    fs.maxRetries              = 0;

    const auto result = MeshSimplifier::simplifyFoliage(cards, fs);
    const size_t triCount = result.mesh.indices.size() / 3;
    CHECK(triCount >= 4u);     // the floor held
    CHECK(triCount < 10u);     // and real reduction actually happened below it
    CHECK(result.retriesUsed == 0); // coverage goal of 0 is met on the first attempt
}
