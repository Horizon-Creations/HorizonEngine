#include "doctest.h"
#include "TestFsUtil.h"

#include <HorizonScene/SplineGeometry.h>
#include <HorizonScene/RopeTrailSystem.h>
#include <HorizonScene/SceneSerializer.h>
#include <HorizonScene/SceneSystems.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/RopeComponent.h>
#include <HorizonScene/Components/TrailComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/EntityIdComponent.h>
#include <ContentManager/ContentManager.h>
#include <HorizonRendering/RenderExtractor.h>
#include <HorizonRendering/RenderWorld.h>

#include <algorithm>
#include <cmath>
#include <filesystem>

namespace fs = std::filesystem;
using namespace HE::spline;

namespace
{
    // A curve with a genuine inflection point (an S in the XZ plane) plus a dead
    // straight run. Between them they are the two shapes a Frenet frame cannot
    // survive: it flips 180° at the inflection and is undefined where curvature
    // is zero.
    std::vector<glm::vec3> sCurve()
    {
        return { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 1.0f }, { 2.0f, 0.0f, -1.0f },
                 { 3.0f, 0.0f, 1.0f }, { 4.0f, 0.0f, 0.0f } };
    }

    std::vector<glm::vec3> straightLine(int n, const glm::vec3& dir)
    {
        std::vector<glm::vec3> p;
        for (int i = 0; i < n; ++i) p.push_back(dir * static_cast<float>(i));
        return p;
    }
}

// ── Stage 1-3: curve, spacing, frames ────────────────────────────────────────

TEST_CASE("Catmull-Rom passes through every control point")
{
    const std::vector<glm::vec3> cps = sCurve();
    const std::vector<glm::vec3> pts = sampleCatmullRom(cps, 8);

    REQUIRE(pts.size() == (cps.size() - 1) * 8 + 1);
    for (size_t i = 0; i < cps.size(); ++i)
    {
        const glm::vec3 on = pts[i * 8];
        CHECK(glm::length(on - cps[i]) == doctest::Approx(0.0f).epsilon(1e-4));
    }
}

TEST_CASE("Centripetal parametrisation does not overshoot on bunched control points")
{
    // Three points nearly on top of each other and then a jump — the case where
    // a uniform Catmull-Rom loops out and comes back. Every sample has to stay
    // inside a generous box around the control points.
    const std::vector<glm::vec3> cps = { { 0.0f, 0.0f, 0.0f }, { 0.02f, 0.0f, 0.0f },
                                         { 0.04f, 0.0f, 0.0f }, { 2.0f, 0.0f, 0.0f } };
    for (const glm::vec3& p : sampleCatmullRom(cps, 16))
    {
        CHECK(p.x >= -0.05f);
        CHECK(p.x <=  2.05f);
        CHECK(std::fabs(p.z) < 0.05f);
    }
}

TEST_CASE("Arc-length resampling spaces the samples evenly and keeps the ends")
{
    const std::vector<glm::vec3> dense = sampleCatmullRom(sCurve(), 12);
    const std::vector<glm::vec3> even  = resampleByArcLength(dense, 40);

    REQUIRE(even.size() == 40);
    CHECK(glm::length(even.front() - dense.front()) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(glm::length(even.back()  - dense.back())  == doctest::Approx(0.0f).epsilon(1e-4));

    float total = 0.0f;
    for (size_t i = 1; i < even.size(); ++i) total += glm::length(even[i] - even[i - 1]);
    const float step = total / static_cast<float>(even.size() - 1);
    for (size_t i = 1; i < even.size(); ++i)
    {
        const float d = glm::length(even[i] - even[i - 1]);
        CHECK(d == doctest::Approx(step).epsilon(0.05));   // 5 %: the chords cut the curve
    }

    // Arc length is monotonic along the frames built from them.
    const std::vector<Frame> frames = buildFrames(even);
    for (size_t i = 1; i < frames.size(); ++i)
        CHECK(frames[i].arcLength > frames[i - 1].arcLength);
}

TEST_CASE("Rotation-minimising frames never flip — S-curve and straight line")
{
    // THE test for the Frenet mistake: consecutive normals stay on the same side.
    // A Frenet frame flips at the S-curve's inflection point (dot goes to -1) and
    // is not even defined along the straight line.
    for (const std::vector<glm::vec3>& pts : { resampleByArcLength(sampleCatmullRom(sCurve(), 12), 60),
                                               straightLine(20, glm::vec3(0.0f, 0.0f, 0.25f)),
                                               straightLine(20, glm::vec3(0.0f, 0.4f, 0.0f)) })
    {
        const std::vector<Frame> frames = buildFrames(pts);
        REQUIRE(frames.size() == pts.size());
        for (size_t i = 0; i < frames.size(); ++i)
        {
            // Orthonormal at every station.
            CHECK(glm::length(frames[i].normal) == doctest::Approx(1.0f).epsilon(1e-3));
            CHECK(glm::dot(frames[i].normal, frames[i].tangent) == doctest::Approx(0.0f).epsilon(1e-3));
            if (i > 0)
                CHECK(glm::dot(frames[i].normal, frames[i - 1].normal) > 0.0f);
        }
    }
}

TEST_CASE("A vertical curve is not a special case for the caller")
{
    // The default up hint (0,1,0) is parallel to the tangent here, so the seed
    // has to fall back to another axis instead of producing a zero normal.
    const std::vector<Frame> frames = buildFrames(straightLine(8, glm::vec3(0.0f, -0.5f, 0.0f)));
    REQUIRE(frames.size() == 8);
    for (const Frame& f : frames)
    {
        CHECK(glm::length(f.normal) == doctest::Approx(1.0f).epsilon(1e-3));
        CHECK(std::isfinite(f.normal.x));
        CHECK(glm::dot(f.normal, f.tangent) == doctest::Approx(0.0f).epsilon(1e-3));
    }
}

// ── Stage 4: tube and ribbon ─────────────────────────────────────────────────

TEST_CASE("Tube vertex and index counts follow the ring formula")
{
    const std::vector<Frame> frames = buildFrames(straightLine(10, glm::vec3(0.0f, 0.0f, 0.5f)));
    TubeParams tp;
    tp.radius         = 0.1f;
    tp.radialSegments = 6;
    const MeshData m = buildTube(frames, tp);

    // The seam vertex is why it is (segments + 1): u has to reach 1 where it
    // started at 0, and one shared vertex cannot carry both.
    CHECK(m.vertexCount() == frames.size() * 7);
    CHECK(m.indices.size() == (frames.size() - 1) * 6 * 6);
    CHECK(m.normals.size() == m.positions.size());
    CHECK(m.uvs.size() == m.vertexCount() * 2);

    // Every vertex sits `radius` away from its ring's centre, and the normal
    // points straight out.
    for (size_t i = 0; i < m.vertexCount(); ++i)
    {
        const size_t ring = i / 7;
        const glm::vec3 p(m.positions[i * 3], m.positions[i * 3 + 1], m.positions[i * 3 + 2]);
        const glm::vec3 n(m.normals[i * 3],   m.normals[i * 3 + 1],   m.normals[i * 3 + 2]);
        const glm::vec3 radial = p - frames[ring].position;
        CHECK(glm::length(radial) == doctest::Approx(0.1f).epsilon(1e-3));
        CHECK(glm::dot(glm::normalize(radial), n) == doctest::Approx(1.0f).epsilon(1e-3));
    }
}

TEST_CASE("Tube clamps a nonsense radial segment count instead of degenerating")
{
    const std::vector<Frame> frames = buildFrames(straightLine(4, glm::vec3(1.0f, 0.0f, 0.0f)));
    TubeParams tp;
    tp.radialSegments = 0;                       // what an inspector spinner can hand over
    const MeshData m = buildTube(frames, tp);
    CHECK(m.vertexCount() == frames.size() * 4); // clamped to 3 segments + seam
    CHECK(m.triangleCount() > 0);
}

TEST_CASE("Tube UV tiles along the arc length")
{
    const std::vector<Frame> frames = buildFrames(straightLine(9, glm::vec3(0.0f, 0.0f, 0.5f)));
    TubeParams tp;
    tp.radialSegments = 4;
    tp.uvTileLength   = 2.0f;                 // 4 metres of tube ⇒ v runs 0 … 2
    const MeshData m = buildTube(frames, tp);

    CHECK(m.uvs[1] == doctest::Approx(0.0f));
    CHECK(m.uvs[m.uvs.size() - 1] == doctest::Approx(2.0f).epsilon(1e-3));
    // u sweeps 0 … 1 around one ring, seam included.
    CHECK(m.uvs[0] == doctest::Approx(0.0f));
    CHECK(m.uvs[4 * 2] == doctest::Approx(1.0f));
}

TEST_CASE("Ribbon counts, orientation and camera facing")
{
    std::vector<RibbonSection> sections;
    for (int i = 0; i < 5; ++i)
        sections.push_back({ glm::vec3(static_cast<float>(i), 0.0f, 0.0f), 0.5f,
                             static_cast<float>(i) / 4.0f });

    RibbonParams rp;
    rp.cameraAligned = true;
    rp.cameraPos     = glm::vec3(2.0f, 0.0f, 10.0f);
    const MeshData m = buildRibbon(sections, rp);

    CHECK(m.vertexCount() == 10);
    CHECK(m.indices.size() == 4 * 6);

    // Camera-aligned: every normal faces the camera side.
    for (size_t i = 0; i < m.vertexCount(); ++i)
    {
        const glm::vec3 n(m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]);
        CHECK(n.z > 0.9f);
    }
    // Width axis ⊥ the run of the band: the two lanes differ only in Y here.
    CHECK(std::fabs(m.positions[0] - m.positions[3]) == doctest::Approx(0.0f).epsilon(1e-4));
    CHECK(std::fabs(m.positions[1] - m.positions[4]) == doctest::Approx(1.0f).epsilon(1e-3));
}

TEST_CASE("twoSidedGeometry doubles the triangles and mirrors the second layer")
{
    std::vector<RibbonSection> sections;
    for (int i = 0; i < 4; ++i)
        sections.push_back({ glm::vec3(0.0f, 0.0f, static_cast<float>(i)), 0.25f, 0.0f });

    RibbonParams rp;
    const MeshData one = buildRibbon(sections, rp);
    rp.twoSided = true;
    const MeshData two = buildRibbon(sections, rp);

    CHECK(two.triangleCount() == one.triangleCount() * 2);
    CHECK(two.vertexCount()   == one.vertexCount()   * 2);

    const size_t half = one.vertexCount();
    for (size_t i = 0; i < half; ++i)
    {
        // Same position, opposite normal.
        for (int k = 0; k < 3; ++k)
        {
            CHECK(two.positions[(half + i) * 3 + k] == doctest::Approx(two.positions[i * 3 + k]));
            CHECK(two.normals  [(half + i) * 3 + k] == doctest::Approx(-two.normals[i * 3 + k]));
        }
    }
}

TEST_CASE("Fewer than two stations produce nothing at all")
{
    CHECK(buildTube(buildFrames({ glm::vec3(0.0f) }), TubeParams{}).empty());
    CHECK(buildRibbon({ { glm::vec3(0.0f), 1.0f, 0.0f } }, RibbonParams{}).empty());
    CHECK(sampleSpline({ glm::vec3(0.0f) }, 8).empty());
}

TEST_CASE("interleave produces pos3 + norm3 + uv2 per vertex")
{
    const MeshData m = buildTube(buildFrames(straightLine(3, glm::vec3(0.0f, 0.0f, 1.0f))),
                                 TubeParams{});
    const std::vector<float> v = interleave(m);
    REQUIRE(v.size() == m.vertexCount() * 8);
    CHECK(v[0] == doctest::Approx(m.positions[0]));
    CHECK(v[3] == doctest::Approx(m.normals[0]));
    CHECK(v[6] == doctest::Approx(m.uvs[0]));
}

// ── Rope geometry ────────────────────────────────────────────────────────────

TEST_CASE("Rope bounds enclose the control points")
{
    RopeComponent rope;
    rope.controlPoints = { { -1.0f, 0.0f, 0.0f }, { 0.0f, 2.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    rope.radius        = 0.05f;
    const MeshData m = RopeTrailSystem::buildRopeGeometry(rope, rope.controlPoints);

    REQUIRE(!m.empty());
    REQUIRE(m.bounds.isValid());
    for (const glm::vec3& p : rope.controlPoints)
    {
        CHECK(m.bounds.min.x <= p.x + 1e-3f);
        CHECK(m.bounds.max.x >= p.x - 1e-3f);
        CHECK(m.bounds.min.y <= p.y + 1e-3f);
        CHECK(m.bounds.max.y >= p.y - 1e-3f);
    }
}

TEST_CASE("Sag bows the rope downwards and leaves the ends alone")
{
    RopeComponent rope;
    rope.controlPoints  = { { 0.0f, 0.0f, 0.0f }, { 4.0f, 0.0f, 0.0f } };
    rope.shape          = RopeShape::Ribbon;
    rope.radius         = 0.01f;
    rope.samplesPerSpan = 16;

    const MeshData taut = RopeTrailSystem::buildRopeGeometry(rope, rope.controlPoints);
    rope.sag = 1.0f;
    const MeshData slack = RopeTrailSystem::buildRopeGeometry(rope, rope.controlPoints);

    CHECK(slack.bounds.min.y < taut.bounds.min.y - 0.5f);
    // The ends are pinned: the first two vertices still sit at the first control point.
    CHECK(slack.positions[0] == doctest::Approx(0.0f).epsilon(1e-3));
    CHECK(slack.positions[1] == doctest::Approx(0.0f).epsilon(1e-3));
}

TEST_CASE("A rope with fewer than two control points builds nothing")
{
    RopeComponent rope;
    rope.controlPoints = { { 0.0f, 0.0f, 0.0f } };
    CHECK(RopeTrailSystem::buildRopeGeometry(rope, rope.controlPoints).empty());
}

TEST_CASE("The geometry hash reacts to everything that changes the geometry")
{
    RopeComponent rope;
    const std::vector<glm::vec3> pts = rope.controlPoints;
    const uint64_t base = RopeTrailSystem::geometryHash(rope, pts);

    CHECK(RopeTrailSystem::geometryHash(rope, pts) == base);        // stable

    RopeComponent other = rope; other.radius += 0.01f;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.radialSegments = 12;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.samplesPerSpan = 4;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.uvTileLength = 2.0f;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.sag = 0.5f;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.shape = RopeShape::Ribbon;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);
    other = rope; other.twoSidedGeometry = true;
    CHECK(RopeTrailSystem::geometryHash(other, pts) != base);

    // Moving an attachment moves a control point, which is how a rope between
    // two moving objects notices that it has to be rebuilt.
    std::vector<glm::vec3> moved = pts;
    moved.back().y -= 0.001f;
    CHECK(RopeTrailSystem::geometryHash(rope, moved) != base);

    // Never the "never built" marker.
    CHECK(base != 0ull);
}

// ── Trail simulation ─────────────────────────────────────────────────────────

TEST_CASE("minVertexDistance suppresses points until the entity has travelled")
{
    TrailComponent t;
    t.minVertexDistance = 1.0f;
    t.lifetime          = 100.0f;

    RopeTrailSystem::stepTrail(t, glm::vec3(0.0f), 0.016f);
    CHECK(t.points.size() == 1);                       // the first one always drops

    for (int i = 0; i < 5; ++i)
        RopeTrailSystem::stepTrail(t, glm::vec3(0.1f * static_cast<float>(i), 0.0f, 0.0f), 0.016f);
    CHECK(t.points.size() == 1);                       // never got a metre away

    RopeTrailSystem::stepTrail(t, glm::vec3(1.5f, 0.0f, 0.0f), 0.016f);
    CHECK(t.points.size() == 2);
}

TEST_CASE("lifetime expires points oldest first")
{
    TrailComponent t;
    t.minVertexDistance = 0.0f;
    t.lifetime          = 0.5f;

    for (int i = 0; i < 4; ++i)
        RopeTrailSystem::stepTrail(t, glm::vec3(static_cast<float>(i), 0.0f, 0.0f), 0.02f);
    REQUIRE(t.points.size() == 4);
    CHECK(t.points.front().age > t.points.back().age);

    // Exactly the points older than the lifetime go, oldest first: after 0.5 s
    // more only the two youngest (ages 0.02 and 0.00 before the step) survive.
    RopeTrailSystem::stepTrail(t, glm::vec3(3.0f, 0.0f, 0.0f), 0.47f);
    REQUIRE(t.points.size() == 3);       // two survivors plus the one just emitted
    CHECK(t.points.front().worldPos.x == doctest::Approx(2.0f));

    // One long step ages everything past the lifetime; only the point emitted in
    // this very step survives.
    RopeTrailSystem::stepTrail(t, glm::vec3(9.0f, 0.0f, 0.0f), 1.0f);
    REQUIRE(t.points.size() == 1);
    CHECK(t.points.front().worldPos.x == doctest::Approx(9.0f));
}

TEST_CASE("maxPoints caps the ring buffer and keeps the newest")
{
    TrailComponent t;
    t.minVertexDistance = 0.0f;
    t.lifetime          = 1000.0f;
    t.maxPoints         = 5;

    for (int i = 0; i < 30; ++i)
        RopeTrailSystem::stepTrail(t, glm::vec3(static_cast<float>(i), 0.0f, 0.0f), 0.001f);

    REQUIRE(t.points.size() == 5);
    CHECK(t.points.back().worldPos.x  == doctest::Approx(29.0f));
    CHECK(t.points.front().worldPos.x == doctest::Approx(25.0f));
}

TEST_CASE("emitting=false adds nothing but lets the band run out")
{
    TrailComponent t;
    t.minVertexDistance = 0.0f;
    t.lifetime          = 0.1f;
    for (int i = 0; i < 3; ++i)
        RopeTrailSystem::stepTrail(t, glm::vec3(static_cast<float>(i), 0.0f, 0.0f), 0.01f);
    REQUIRE(t.points.size() == 3);

    t.emitting = false;
    RopeTrailSystem::stepTrail(t, glm::vec3(50.0f, 0.0f, 0.0f), 0.01f);
    CHECK(t.points.size() == 3);                       // nothing new
    RopeTrailSystem::stepTrail(t, glm::vec3(50.0f, 0.0f, 0.0f), 1.0f);
    CHECK(t.points.empty());                           // and the old ones expired
}

TEST_CASE("Trail uv.v runs from 0 at the tip to 1 at the tail, and the width with it")
{
    TrailComponent t;
    t.lifetime   = 1.0f;
    t.startWidth = 0.4f;
    t.endWidth   = 0.0f;
    t.alignment  = TrailAlignment::Frame;
    // Ages built by hand so the oldest point sits exactly at the lifetime — the
    // only way v reaches 1 exactly, and a runtime that bent to make that true
    // would be the wrong fix.
    t.points = { { glm::vec3(0.0f, 0.0f, 0.0f), 1.0f },
                 { glm::vec3(1.0f, 0.0f, 0.0f), 0.5f },
                 { glm::vec3(2.0f, 0.0f, 0.0f), 0.0f } };

    const MeshData m = RopeTrailSystem::buildTrailGeometry(t, glm::vec3(0.0f, 10.0f, 0.0f));
    REQUIRE(m.vertexCount() == 6);

    // Vertex pairs, tip first: v is monotonic and hits both ends.
    float prev = -1.0f;
    for (size_t i = 0; i < m.vertexCount(); i += 2)
    {
        const float v = m.uvs[i * 2 + 1];
        CHECK(v == doctest::Approx(m.uvs[(i + 1) * 2 + 1]));   // both lanes share it
        CHECK(v >= prev);
        prev = v;
    }
    CHECK(m.uvs[1] == doctest::Approx(0.0f));                              // tip
    CHECK(m.uvs[(m.vertexCount() - 1) * 2 + 1] == doctest::Approx(1.0f));  // tail

    // The tip is startWidth wide, the tail has run down to endWidth.
    const glm::vec3 tipL(m.positions[0], m.positions[1], m.positions[2]);
    const glm::vec3 tipR(m.positions[3], m.positions[4], m.positions[5]);
    CHECK(glm::length(tipR - tipL) == doctest::Approx(0.4f).epsilon(1e-3));
    const size_t last = m.vertexCount() - 2;
    const glm::vec3 tailL(m.positions[last * 3], m.positions[last * 3 + 1], m.positions[last * 3 + 2]);
    const glm::vec3 tailR(m.positions[(last + 1) * 3], m.positions[(last + 1) * 3 + 1],
                          m.positions[(last + 1) * 3 + 2]);
    CHECK(glm::length(tailR - tailL) == doctest::Approx(0.0f).epsilon(1e-4));

    // Tip first also means the strip starts at the youngest point.
    CHECK(tipL.x == doctest::Approx(2.0f));
}

TEST_CASE("A trail with fewer than two points has no geometry")
{
    TrailComponent t;
    CHECK(RopeTrailSystem::buildTrailGeometry(t, glm::vec3(0.0f)).empty());
    t.points.push_back({ glm::vec3(0.0f), 0.0f });
    CHECK(RopeTrailSystem::buildTrailGeometry(t, glm::vec3(0.0f)).empty());
}

// ── The system in a world ────────────────────────────────────────────────────

TEST_CASE("A rope registers its runtime mesh exactly once, however often it changes")
{
    // Regel 1 aus docs/rope-trail-plan.md §6.1: registering per rebuild would
    // leak AND invalidate every ContentManager pointer anything else holds.
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity e = world.createEntity("Rope");
    RopeComponent rope;
    rope.controlPoints = { { 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f }, { 0.0f, -2.0f, 0.5f } };
    reg.emplace<RopeComponent>(e, rope);

    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
    const HE::UUID first = reg.get<RopeComponent>(e).runtimeMeshId;
    REQUIRE(!(first == HE::UUID{}));
    REQUIRE(cm.getStaticMesh(first) != nullptr);

    size_t verticesBefore = cm.getStaticMesh(first)->vertices.size();

    for (int i = 1; i <= 5; ++i)
    {
        reg.get<RopeComponent>(e).radialSegments = 4 + i;   // real geometry change
        RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
        CHECK(reg.get<RopeComponent>(e).runtimeMeshId == first);   // same UUID throughout
        // Re-fetch: any registration could have moved the whole asset pool.
        const StaticMeshAsset* m = cm.getStaticMesh(first);
        REQUIRE(m != nullptr);
        CHECK(m->vertices.size() != verticesBefore);        // it really was replaced
        verticesBefore = m->vertices.size();
    }

    // An unchanged rope does not rebuild at all.
    const uint64_t hash = reg.get<RopeComponent>(e).builtHash;
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
    CHECK(reg.get<RopeComponent>(e).builtHash == hash);
    CHECK(reg.get<RopeComponent>(e).runtimeMeshId == first);

    RopeTrailSystem::releaseWorld(world, cm);
}

TEST_CASE("Replacing the whole component does not register a second mesh")
{
    // A collaboration sync and an undo both put a FRESH component on the same
    // entity (emplace_or_replace), so runtimeMeshId comes back empty. If that
    // were taken at face value the rope would register a second mesh and orphan
    // the first — a leak, and exactly the pool reallocation the register-once
    // rule is there to prevent.
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity e = world.createEntity("Rope");
    reg.emplace<RopeComponent>(e, RopeComponent{});
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);

    const HE::UUID first = reg.get<RopeComponent>(e).runtimeMeshId;
    REQUIRE(cm.getStaticMesh(first) != nullptr);
    const size_t before = cm.getStaticMesh(first)->vertices.size();

    RopeComponent replacement;                       // no runtimeMeshId, no builtHash
    replacement.radialSegments = 16;
    reg.emplace_or_replace<RopeComponent>(e, replacement);
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);

    CHECK(reg.get<RopeComponent>(e).runtimeMeshId == first);
    REQUIRE(cm.getStaticMesh(first) != nullptr);
    CHECK(cm.getStaticMesh(first)->vertices.size() != before);   // rebuilt into the same asset

    RopeTrailSystem::releaseWorld(world, cm);
}

TEST_CASE("A destroyed rope gives its runtime mesh back")
{
    HorizonWorld world;
    ContentManager cm;

    const Entity e = world.createEntity("Rope");
    world.registry().emplace<RopeComponent>(e, RopeComponent{});
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);

    const HE::UUID mesh = world.registry().get<RopeComponent>(e).runtimeMeshId;
    REQUIRE(cm.getStaticMesh(mesh) != nullptr);

    world.destroyEntity(e);
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
    CHECK(cm.getStaticMesh(mesh) == nullptr);
}

TEST_CASE("An attachment entity's world position replaces the rope's end point")
{
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity anchor = world.createEntity("Anchor");
    TransformComponent at;
    at.position = { 3.0f, 1.0f, 0.0f };
    reg.emplace_or_replace<TransformComponent>(anchor, at);

    const Entity e = world.createEntity("Rope");
    reg.emplace_or_replace<TransformComponent>(e, TransformComponent{});
    RopeComponent rope;
    rope.controlPoints = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 0.0f, 0.0f } };
    rope.attachEnd     = reg.get<EntityIdComponent>(anchor).id;
    reg.emplace<RopeComponent>(e, rope);

    const std::vector<glm::vec3> pts = RopeTrailSystem::resolveControlPoints(world, e, rope);
    REQUIRE(pts.size() == 2);
    CHECK(pts.back().x == doctest::Approx(3.0f));
    CHECK(pts.back().y == doctest::Approx(1.0f));

    // And moving the anchor changes the hash, i.e. forces a rebuild — this is the
    // frame-fresh path: worldPositionOf, never the stale TransformComponent::worldMatrix.
    const uint64_t before = RopeTrailSystem::geometryHash(rope, pts);
    reg.get<TransformComponent>(anchor).position.x = 5.0f;
    const uint64_t after = RopeTrailSystem::geometryHash(
        rope, RopeTrailSystem::resolveControlPoints(world, e, rope));
    CHECK(before != after);

    RopeTrailSystem::releaseWorld(world, cm);
}

TEST_CASE("The world tick moves a trail's points with the entity")
{
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity e = world.createEntity("Trail");
    TransformComponent t;
    t.position = { 0.0f, 0.0f, 0.0f };
    reg.emplace_or_replace<TransformComponent>(e, t);
    TrailComponent trail;
    trail.minVertexDistance = 0.5f;
    trail.lifetime          = 10.0f;
    reg.emplace<TrailComponent>(e, trail);

    for (int i = 0; i < 4; ++i)
    {
        reg.get<TransformComponent>(e).position.x = static_cast<float>(i);
        RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f, 0.0f, 10.0f), 0.016f);
    }

    const TrailComponent& out = reg.get<TrailComponent>(e);
    REQUIRE(out.points.size() == 4);
    CHECK(out.points.back().worldPos.x == doctest::Approx(3.0f));
    CHECK(out.points.front().age > out.points.back().age);
}

// ── Render extraction ────────────────────────────────────────────────────────
// The split from §2 shows up here as two different destinations: a rope becomes
// an ordinary RenderObject pointing at its runtime mesh, a trail becomes a
// per-frame RibbonBatch that is not an asset at all.

TEST_CASE("A rope extracts as one render object aimed at its runtime mesh")
{
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity e = world.createEntity("Rope");
    TransformComponent t;
    t.position = { 2.0f, 3.0f, 4.0f };
    reg.emplace_or_replace<TransformComponent>(e, t);
    RopeComponent rope;
    rope.controlPoints   = { { 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };
    rope.materialAssetId = HE::UUID::generate();
    rope.castsShadow     = false;
    reg.emplace<RopeComponent>(e, rope);

    RenderExtractor ex;
    RenderWorld     rw;

    // Before the world tick there is no runtime mesh, so there is nothing to
    // point a RenderObject at — and pushing one anyway would draw the default
    // cube at the rope's position.
    ex.extract(world, rw, 1.0f);
    CHECK(rw.objects.empty());

    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.016f);
    const HE::UUID meshId = reg.get<RopeComponent>(e).runtimeMeshId;
    REQUIRE(!(meshId == HE::UUID{}));

    ex.extract(world, rw, 1.0f);
    REQUIRE(rw.objects.size() == 1);
    const RenderObject& obj = rw.objects[0];
    CHECK(obj.meshAssetId     == meshId);
    CHECK(obj.materialAssetId == rope.materialAssetId);
    CHECK(obj.entityId        == static_cast<uint32_t>(e));
    CHECK(obj.castsShadow     == false);
    CHECK(glm::vec3(obj.transform[3]).x == doctest::Approx(2.0f));
    CHECK(glm::vec3(obj.transform[3]).y == doctest::Approx(3.0f));

    // Hidden means hidden, and no ribbon batch was invented on the way.
    reg.get<RopeComponent>(e).visible = false;
    ex.extract(world, rw, 1.0f);
    CHECK(rw.objects.empty());
    CHECK(rw.ribbonBatches.empty());

    RopeTrailSystem::releaseWorld(world, cm);
}

TEST_CASE("A trail extracts as a ribbon batch, not as an object")
{
    HorizonWorld world;
    ContentManager cm;
    auto& reg = world.registry();

    const Entity e = world.createEntity("Trail");
    TransformComponent t;
    t.position = { 0.0f, 0.0f, 5.0f };
    reg.emplace_or_replace<TransformComponent>(e, t);
    TrailComponent trail;
    trail.minVertexDistance = 0.5f;
    // Matched to the four ticks below, so the oldest point really is at the end
    // of its life. A lifetime much longer than the run would pin uv.v near 0 for
    // the whole band — the gradient a material graph reads would be a flat line.
    trail.lifetime          = 0.4f;
    trail.alignment         = TrailAlignment::Frame;   // no camera dependency
    trail.materialAssetId   = HE::UUID::generate();
    reg.emplace<TrailComponent>(e, trail);

    RenderExtractor ex;
    RenderWorld     rw;

    // One point is not a band.
    RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.1f);
    ex.extract(world, rw, 1.0f);
    CHECK(rw.ribbonBatches.empty());

    for (int i = 1; i < 5; ++i)
    {
        reg.get<TransformComponent>(e).position.x = static_cast<float>(i);
        RopeTrailSystem::update(world, cm, nullptr, glm::vec3(0.0f), 0.1f);
    }
    const size_t points = reg.get<TrailComponent>(e).points.size();
    REQUIRE(points >= 2);

    ex.extract(world, rw, 1.0f);
    REQUIRE(rw.ribbonBatches.size() == 1);
    CHECK(rw.objects.empty());   // a trail is NOT a mesh draw

    const RibbonBatch& rb = rw.ribbonBatches[0];
    CHECK(rb.materialAssetId == trail.materialAssetId);
    CHECK(rb.entityId        == static_cast<uint32_t>(e));
    // pos3 + norm3 + uv2 per vertex, two vertices per station.
    CHECK(rb.vertices.size() == points * 2 * 8);
    CHECK(rb.indices.size()  == (points - 1) * 6);
    CHECK(rb.worldBounds.isValid());
    for (uint32_t i : rb.indices) CHECK(i < points * 2);

    // The age travels in uv.v — the tip at 0, the tail at 1 (docs §3.2). That is
    // the only channel a material graph has to fade a trail out.
    float minV = 1e9f, maxV = -1e9f;
    for (size_t v = 0; v < rb.vertices.size() / 8; ++v)
    {
        const float vv = rb.vertices[v * 8 + 7];
        minV = std::min(minV, vv);
        maxV = std::max(maxV, vv);
    }
    CHECK(minV == doctest::Approx(0.0f));
    CHECK(maxV > 0.7f);   // the tail really reaches the far end of the gradient

    // The vertices are WORLD space: the band sits where the entity has been, not
    // at the origin, and the backends draw it with the identity model matrix.
    CHECK(rb.worldBounds.min.z == doctest::Approx(5.0f).epsilon(0.5));

    reg.get<TrailComponent>(e).visible = false;
    ex.extract(world, rw, 1.0f);
    CHECK(rw.ribbonBatches.empty());
}

TEST_CASE("Ribbon batches do not survive into the next frame's extraction")
{
    // RenderWorld::clear has to drop them: they are per-frame geometry, and a
    // stale batch would draw a trail that no longer exists.
    RenderWorld rw;
    rw.ribbonBatches.push_back(RibbonBatch{});
    rw.clear();
    CHECK(rw.ribbonBatches.empty());
}

// ── Serialisation ────────────────────────────────────────────────────────────

TEST_CASE("Rope and trail survive a scene round trip, runtime state excluded")
{
    HorizonWorld world;
    auto& reg = world.registry();
    const Entity e = world.createEntity("Rigged");

    RopeComponent rope;
    rope.visible          = false;
    rope.controlPoints    = { { 0.0f, 0.0f, 0.0f }, { 1.0f, 2.0f, 3.0f }, { -1.0f, 0.5f, 0.0f } };
    rope.attachStart      = HE::UUID{ 0x11ull, 0x22ull };
    rope.attachEnd        = HE::UUID{ 0x33ull, 0x44ull };
    rope.sag              = 0.75f;
    rope.shape            = RopeShape::Ribbon;
    rope.radius           = 0.125f;
    rope.radialSegments   = 12;
    rope.samplesPerSpan   = 5;
    rope.uvTileLength     = 2.5f;
    rope.twoSidedGeometry = true;
    rope.castsShadow      = false;
    rope.materialAssetId  = HE::UUID{ 0x55ull, 0x66ull };
    rope.runtimeMeshId    = HE::UUID{ 0x77ull, 0x88ull };   // runtime — must NOT survive
    rope.builtHash        = 4711ull;
    reg.emplace<RopeComponent>(e, rope);

    TrailComponent trail;
    trail.visible           = false;
    trail.emitting          = false;
    trail.lifetime          = 1.25f;
    trail.minVertexDistance = 0.2f;
    trail.maxPoints         = 96;
    trail.startWidth        = 0.35f;
    trail.endWidth          = 0.05f;
    trail.alignment         = TrailAlignment::Frame;
    trail.materialAssetId   = HE::UUID{ 0x99ull, 0xAAull };
    trail.points            = { { glm::vec3(1.0f), 0.1f } };   // runtime — must NOT survive
    trail.hasLastEmit       = true;
    reg.emplace<TrailComponent>(e, trail);

    const fs::path file = fs::temp_directory_path() / "he_test_rope_trail.hescene";
    SceneSerializer ser;
    REQUIRE(ser.save(world, file, SerializeFormat::JSON));

    HorizonWorld loaded;
    REQUIRE(ser.load(loaded, file, SerializeFormat::JSON));
    he_test::removeQuiet(file);

    const RopeComponent* r = nullptr;
    const TrailComponent* tr = nullptr;
    for (auto [le, rc] : loaded.registry().view<RopeComponent>().each())  { r  = &rc; break; }
    for (auto [le, tc] : loaded.registry().view<TrailComponent>().each()) { tr = &tc; break; }
    REQUIRE(r  != nullptr);
    REQUIRE(tr != nullptr);

    CHECK(r->visible == false);
    REQUIRE(r->controlPoints.size() == 3);
    CHECK(r->controlPoints[1].y == doctest::Approx(2.0f));
    CHECK(r->controlPoints[2].x == doctest::Approx(-1.0f));
    CHECK(r->attachStart == HE::UUID{ 0x11ull, 0x22ull });
    CHECK(r->attachEnd   == HE::UUID{ 0x33ull, 0x44ull });
    CHECK(r->sag == doctest::Approx(0.75f));
    CHECK(r->shape == RopeShape::Ribbon);
    CHECK(r->radius == doctest::Approx(0.125f));
    CHECK(r->radialSegments == 12);
    CHECK(r->samplesPerSpan == 5);
    CHECK(r->uvTileLength == doctest::Approx(2.5f));
    CHECK(r->twoSidedGeometry == true);
    CHECK(r->castsShadow == false);
    CHECK(r->materialAssetId == HE::UUID{ 0x55ull, 0x66ull });
    // Runtime state stays behind: a procedural mesh's UUID means nothing in a
    // second session, and a restored point buffer would hang a stale band in the air.
    CHECK(r->runtimeMeshId == HE::UUID{});
    CHECK(r->builtHash == 0ull);

    CHECK(tr->visible  == false);
    CHECK(tr->emitting == false);
    CHECK(tr->lifetime == doctest::Approx(1.25f));
    CHECK(tr->minVertexDistance == doctest::Approx(0.2f));
    CHECK(tr->maxPoints  == 96);
    CHECK(tr->startWidth == doctest::Approx(0.35f));
    CHECK(tr->endWidth   == doctest::Approx(0.05f));
    CHECK(tr->alignment  == TrailAlignment::Frame);
    CHECK(tr->materialAssetId == HE::UUID{ 0x99ull, 0xAAull });
    CHECK(tr->points.empty());
    CHECK(tr->hasLastEmit == false);
}

TEST_CASE("The loader admits to knowing the two new component keys")
{
    // Without this the loader warns that it is DROPPING a component it loaded
    // perfectly — a false alarm on the one message that is supposed to flag real
    // data loss.
    CHECK(SceneSerializer::isKnownComponentKey("rope"));
    CHECK(SceneSerializer::isKnownComponentKey("trail"));
}

TEST_CASE("collectAssetRefs takes the materials and leaves the runtime mesh alone")
{
    HorizonWorld world;
    auto& reg = world.registry();

    const HE::UUID ropeMat  = HE::UUID::generate();
    const HE::UUID trailMat = HE::UUID::generate();
    const HE::UUID runtime  = HE::UUID::generate();

    const Entity e = world.createEntity("Both");
    RopeComponent rope;
    rope.materialAssetId = ropeMat;
    rope.runtimeMeshId   = runtime;
    reg.emplace<RopeComponent>(e, rope);
    TrailComponent trail;
    trail.materialAssetId = trailMat;
    reg.emplace<TrailComponent>(e, trail);

    const std::vector<HE::UUID> refs = SceneSystems::collectAssetRefs(world);
    const auto has = [&](const HE::UUID& id)
    { return std::find(refs.begin(), refs.end(), id) != refs.end(); };

    CHECK(has(ropeMat));
    CHECK(has(trailMat));
    // The packer would go looking for a file that was never written.
    CHECK(!has(runtime));
}
