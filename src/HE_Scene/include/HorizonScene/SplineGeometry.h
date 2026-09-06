#pragma once
#include <Math/Math.h>
#include <Math/AABB.h>
#include <cstdint>
#include <vector>

// ── Spline → tube / ribbon geometry ──────────────────────────────────────────
// The CPU half of rope & trail rendering (docs/rope-trail-plan.md): control
// points in, triangulated bands out. Deliberately free of ContentManager,
// registry and renderer — a rope, a trail and the editor's preview all want the
// same four stages, and only this way are they testable without a world.
//
// The four stages, in order, are: centripetal Catmull-Rom through the control
// points → resample by arc length → rotation-minimising frames → thread the
// profile onto the frames.
namespace HE::spline
{
    // One station along the curve: where it is, where it goes, and the two axes
    // perpendicular to it. `normal` and `binormal` come from a rotation-
    // minimising frame — NOT from Frenet, which is undefined on a straight
    // stretch (curvature 0) and flips through 180° at an inflection point. That
    // flip is exactly what a rope with a single twist in the middle looks like.
    struct Frame
    {
        glm::vec3 position { 0.0f };
        glm::vec3 tangent  { 0.0f, 0.0f, 1.0f };
        glm::vec3 normal   { 0.0f, 1.0f, 0.0f };   // ⊥ tangent
        glm::vec3 binormal { 1.0f, 0.0f, 0.0f };   // = cross(tangent, normal)
        float     arcLength = 0.0f;                // distance from the first frame
    };

    // Triangulated output in the same SoA shape StaticMeshAsset uses, so a rope
    // becomes a runtime mesh by copying four vectors, and a trail becomes a
    // ribbon batch through interleave() below.
    struct MeshData
    {
        std::vector<float>    positions;   // 3 per vertex
        std::vector<float>    normals;     // 3 per vertex
        std::vector<float>    uvs;         // 2 per vertex
        std::vector<uint32_t> indices;
        HE::AABB              bounds;

        size_t vertexCount()   const { return positions.size() / 3; }
        size_t triangleCount() const { return indices.size() / 3; }
        bool   empty()         const { return indices.empty(); }
    };

    // ── Stage 1: the curve ───────────────────────────────────────────────────
    // Centripetal Catmull-Rom (α = 0.5) through every control point, with the
    // end segments defined by mirrored phantom points. Centripetal rather than
    // uniform on purpose: the uniform parametrisation loops and overshoots where
    // control points bunch up, which is precisely what happens to a rope somebody
    // drags together in the editor.
    //
    // `samplesPerSpan` (clamped to ≥ 1) counts the NEW points added per span, so
    // the result has (n-1) * samplesPerSpan + 1 points and passes through every
    // control point. Fewer than two control points → the input, verbatim.
    std::vector<glm::vec3> sampleCatmullRom(const std::vector<glm::vec3>& controlPoints,
                                            int samplesPerSpan);

    // ── Stage 2: even spacing ────────────────────────────────────────────────
    // Resample a polyline into `count` points of equal arc-length spacing.
    // Without it the rings sit dense where the curve bends, and any UV tiling
    // along the length visibly drifts out of step. Returns the input when there
    // is nothing to resample (< 2 points, count < 2, or zero total length).
    std::vector<glm::vec3> resampleByArcLength(const std::vector<glm::vec3>& points, int count);

    // ── Stage 3: frames ──────────────────────────────────────────────────────
    // Parallel-transport (double-reflection, Wang et al.) frames along the
    // polyline. `upHint` seeds the first frame's normal, orthogonalised against
    // the first tangent; if it is parallel to the tangent the world axis with the
    // smallest |dot| takes over, so a vertical rope is not a special case for the
    // caller. Consecutive points closer than ~1e-6 reuse the previous tangent
    // rather than producing a NaN.
    std::vector<Frame> buildFrames(const std::vector<glm::vec3>& points,
                                   const glm::vec3& upHint = glm::vec3(0.0f, 1.0f, 0.0f));

    // Stages 1-3 in one call.
    std::vector<Frame> sampleSpline(const std::vector<glm::vec3>& controlPoints,
                                    int samplesPerSpan,
                                    const glm::vec3& upHint = glm::vec3(0.0f, 1.0f, 0.0f));

    // ── Stage 4a: tube ───────────────────────────────────────────────────────
    struct TubeParams
    {
        float radius         = 0.05f;
        int   radialSegments = 8;      // clamped to ≥ 3
        // Metres per UV-V tile, so a rope texture repeats along the length.
        // ≤ 0 normalises V to 0…1 over the whole tube instead.
        float uvTileLength   = 1.0f;
    };

    // A closed ring per frame, consecutive rings stitched into quads. The ring
    // carries a SEAM vertex (radialSegments + 1 per ring): u has to reach 1.0
    // where it started at 0.0, and one shared vertex cannot hold both. Open at
    // both ends — the components have no cap switch, so nothing here invents one.
    //
    // Vertices  = frames * (radialSegments + 1)
    // Indices   = (frames - 1) * radialSegments * 6
    MeshData buildTube(const std::vector<Frame>& frames, const TubeParams& params);

    // ── Stage 4b: ribbon ─────────────────────────────────────────────────────
    // One station of a flat band. The caller supplies `v` rather than having it
    // derived, because the two users mean different things by it: a rope tiles
    // arc length, a trail carries normalised AGE (0 at the tip, 1 at the tail).
    // See docs/rope-trail-plan.md §3.2 — the vertex format has no colour
    // attribute, so age travels in the UV and a material graph reads it there.
    struct RibbonSection
    {
        glm::vec3 position  { 0.0f };
        float     halfWidth = 0.1f;
        float     v         = 0.0f;
    };

    struct RibbonParams
    {
        // Camera alignment turns the band to face the viewer (the usual choice
        // for a weapon trail); otherwise the band lies in the frame's binormal
        // axis and keeps a fixed orientation in the world (a strap, a flag).
        bool      cameraAligned = false;
        glm::vec3 cameraPos     { 0.0f };
        glm::vec3 upHint        { 0.0f, 1.0f, 0.0f };
        // Emit the band a second time with reversed winding and mirrored
        // normals, so a LIT band is shaded correctly from behind. Unnecessary
        // for the emissive default — see docs/rope-trail-plan.md §3.3.
        bool      twoSided      = false;
    };

    // Vertices = sections * 2 (× 2 with twoSided), indices = (sections-1) * 6
    // (× 2 with twoSided). Fewer than two sections → an empty mesh.
    MeshData buildRibbon(const std::vector<RibbonSection>& sections, const RibbonParams& params);

    // pos3 + norm3 + uv2 per vertex — the exact layout the backends upload, and
    // the form a RibbonBatch hands to the renderer.
    std::vector<float> interleave(const MeshData& mesh);
}
