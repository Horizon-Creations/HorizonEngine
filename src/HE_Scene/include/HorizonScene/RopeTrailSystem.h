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
class DebugDrawBuffer;
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

    // The curve the rope is actually threaded onto, in its LOCAL space: sampled,
    // spaced evenly by arc length, sag applied. Public because the editor's
    // viewport guide draws this line and the mesh builder below threads its
    // rings onto it — two copies of the sag formula would part ways the first
    // time one of them was tuned.
    std::vector<glm::vec3> ropeCenterline(const RopeComponent& rope,
                                          const std::vector<glm::vec3>& localPoints);

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

    // ── The editor's viewport guides ─────────────────────────────────────────
    // A rope you are building has to be visible while it is still wrong: two
    // control points on top of each other make no geometry at all, and without
    // a guide the entity is then simply invisible. These append world-space
    // debug lines the way NavigationSystem::extractNavMeshWireframe does — the
    // curve through the points, and a handle box on every point, so a point
    // that is somewhere unexpected can be found and dragged back.
    //
    // `localPoints` is what resolveControlPoints returned, so an attached end
    // is drawn where the attachment actually put it, not where the authored
    // point sits.
    void appendRopeGuides(const RopeComponent& rope,
                          const std::vector<glm::vec3>& localPoints,
                          const glm::mat4& worldMatrix, DebugDrawBuffer& out);

    // The trail's dropped points, already in world space, plus a box on the tip.
    // This is the half of a trail that is otherwise invisible in the editor: the
    // band needs two points and the entity has to have MOVED to have any.
    void appendTrailGuides(const TrailComponent& trail, DebugDrawBuffer& out);

    // Drop every runtime rope mesh this system registered for `world` and forget
    // the world. Entity-by-entity cleanup happens inside update() (a destroyed
    // rope's mesh is unloaded on the next tick); this is the other half, for a
    // whole world going away while its ContentManager stays.
    void releaseWorld(HorizonWorld& world, ContentManager& cm);
}
