#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <cstdint>
#include <vector>

// ─── Rope / chain / cable / grapple line ─────────────────────────────────────
// A spline the engine turns into a tube or a flat band (docs/rope-trail-plan.md).
// Unlike a trail, a rope only changes when SOMEBODY CHANGES IT: an edit in the
// inspector, a script, or an attachment entity that moved. That is what lets it
// live as a runtime StaticMeshAsset and go through the ordinary mesh path — full
// PBR, shadows, GI, material graph, and not one line of backend code.
//
// The counterpart of that choice is the reason a trail must NOT be built this
// way: rebuilding a runtime mesh invalidates the renderer's acceleration
// structures, and a per-frame rebuild would throw away the scene's whole
// software BVH every frame. A rope that hangs between two MOVING attachments
// pays exactly that cost — see RopeTrailSystem.
//
// There is no rope SIMULATION here. Control points come from the editor, from
// attachments or from script; `sag` is a geometric approximation of a catenary,
// not a solver. Verlet ropes belong to PhysicsWorld and are their own topic.
enum class RopeShape : uint8_t { Tube = 0, Ribbon = 1 };

struct RopeComponent
{
    bool visible = true;

    // ── Authoring ───────────────────────────────────────────────────────────
    // Control points in the entity's LOCAL space. Fewer than two produce no
    // geometry at all — a rope needs somewhere to go.
    std::vector<glm::vec3> controlPoints { { 0.0f, 0.0f, 0.0f }, { 0.0f, -1.0f, 0.0f } };

    // Optional attachment entities, referenced by EntityIdComponent UUID the way
    // CameraRigComponent references its target — an entt handle would not survive
    // a save. When one is set, that entity's WORLD position (converted back into
    // this entity's local space) replaces the first / last control point, so a
    // grapple line hangs between two things that move.
    HE::UUID attachStart;
    HE::UUID attachEnd;

    // Sag in metres, added straight down onto the curve as a parabola that is
    // zero at both ends. 0 = taut.
    float sag = 0.0f;

    RopeShape shape             = RopeShape::Tube;
    float     radius            = 0.05f;   // Tube: radius. Ribbon: half width.
    int       radialSegments    = 8;       // Tube only; a ribbon ignores it
    int       samplesPerSpan    = 8;       // resolution along the curve, per span
    float     uvTileLength      = 1.0f;    // metres per UV-V tile (≤ 0 = normalise 0…1)
    bool      twoSidedGeometry  = false;   // ribbon only, see docs §3.3
    bool      castsShadow       = true;

    HE::UUID  materialAssetId;

    // ── Runtime state (never serialised) ────────────────────────────────────
    // The generated mesh. Registered ONCE and replaced from then on: registering
    // per rebuild would both leak and invalidate every ContentManager pointer
    // anything else in the same loop is holding.
    HE::UUID runtimeMeshId;

    // Hash over everything that determines the geometry, including the current
    // world positions of the attachments. Different → rebuild.
    uint64_t builtHash = 0;
};
