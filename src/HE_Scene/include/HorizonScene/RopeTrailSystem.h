#pragma once
#include <Math/Math.h>
#include <Types/UUID.h>
#include <HorizonScene/SplineGeometry.h>
#include <entt/entt.hpp>
#include <cstdint>
#include <vector>

class HorizonWorld;
class ContentManager;
class IRenderer;
struct RopeComponent;
struct TrailComponent;

// ─── Ropes and trails, once per frame ────────────────────────────────────────
// Two components, two very different data rates, and the split runs right
// through this system (docs/rope-trail-plan.md §2):
//
//   RopeComponent  — rebuild only when something actually changed, into a
//                    runtime StaticMeshAsset that the normal mesh path draws.
//   TrailComponent — no asset at all. This system only ages and emits the
//                    points; the extractor triangulates them into a per-frame
//                    ribbon batch.
namespace RopeTrailSystem
{
    // Age trails, emit new trail points, and rebuild the ropes whose geometry
    // changed. `renderer` may be null (tests, headless): the meshes are still
    // built, they simply are not invalidated in a renderer that does not exist.
    void update(HorizonWorld& world, ContentManager& cm, IRenderer* renderer,
                const glm::vec3& cameraPos, float dt);

    // The rope's control points in its LOCAL space, with the attachments
    // substituted in. Attachment world positions come from HE::worldPositionOf,
    // never from TransformComponent::worldMatrix — nothing has propagated the
    // hierarchy at this point in the frame, so the stored matrix is a frame old
    // and plain identity for anything created this frame.
    std::vector<glm::vec3> resolveControlPoints(HorizonWorld& world, entt::entity entity,
                                                const RopeComponent& rope);

    // Tube or ribbon for one rope, in local space, sag applied. Split out of
    // update() because the editor preview and the tests want the geometry
    // without a ContentManager.
    HE::spline::MeshData buildRopeGeometry(const RopeComponent& rope,
                                           const std::vector<glm::vec3>& localPoints);

    // The trail's band in WORLD space: tip first, uv.v running 0 (tip) → 1
    // (tail) and the width interpolating startWidth → endWidth along it. Called
    // by the render extractor every frame.
    HE::spline::MeshData buildTrailGeometry(const TrailComponent& trail,
                                            const glm::vec3& cameraPos);

    // Advance one trail by dt at `worldPos`: age the points, drop the expired
    // ones, and append a new one once the entity has travelled far enough.
    // Public so anything driving a trail outside the world tick behaves the same.
    void stepTrail(TrailComponent& trail, const glm::vec3& worldPos, float dt);

    // Everything that decides what the geometry looks like, in one number. The
    // attachment positions are folded in by resolveControlPoints, so a rope
    // hanging between two moving objects rebuilds — and a rope nobody touched
    // costs one hash per frame.
    uint64_t geometryHash(const RopeComponent& rope, const std::vector<glm::vec3>& localPoints);

    // Drop every runtime rope mesh this system registered for `world` and forget
    // the world. Entity-by-entity cleanup happens inside update() (a destroyed
    // rope's mesh is unloaded on the next tick); this is the other half, for a
    // whole world going away while its ContentManager stays.
    void releaseWorld(HorizonWorld& world, ContentManager& cm);
}
