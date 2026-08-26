#pragma once
#include <glm/glm.hpp>
#include <entt/entt.hpp>   // entt::entity — the per-entity queries below take one

class HorizonWorld;
struct TransformComponent;

// ── Transform hierarchy ──────────────────────────────────────────────────────
// World matrices are DERIVED state: TransformComponent stores a local
// position/rotation/scale, and worldMatrix is what falls out of walking the
// parent chain. Something has to do that walk, and for a long time the only
// thing that did was the render extractor, at the top of extraction.
//
// That is fine as long as the only consumer is the renderer. It stops being fine
// the moment gameplay wants a world position DURING the frame — a camera rig
// that follows an entity reads a worldMatrix the extractor has not written yet,
// so it follows where the entity was last frame. Pulling the walk out here lets
// a caller ask for it at the point in the frame where it needs to be true.
namespace HE {

    // The entity's own transform, without its parents: T * R * S, with rotation
    // read as Euler degrees. This is the engine's transform convention and the
    // reason it is shared rather than re-derived — a second copy that composes
    // in a different order is a bug that only shows up on rotated parents.
    glm::mat4 localMatrix(const TransformComponent& t);

    // Recompute worldMatrix for every entity, top-down from the world root, and
    // clear their dirty flags.
    //
    // Walking from rootEntity() is what makes it work: HorizonWorld parents
    // everything to a root *entity*, so anything keyed on parent == entt::null
    // would never fire. Entities outside that hierarchy (no HierarchyComponent)
    // are handled separately — for them local IS world.
    //
    // Recomputing everything is cheap at current scene sizes; dirty-flag pruning
    // can come back with profiling.
    void propagateTransforms(HorizonWorld& world);

    // ONE entity's world matrix, composed on the spot by walking its parent
    // chain upward — NOT read out of worldMatrix.
    //
    // That distinction is the whole point of these two. worldMatrix is only as
    // fresh as the last propagateTransforms, and the callers of that are the
    // render extractor, the camera rig and the NavMesh collector: a script
    // asking mid-frame would get the value from before whatever moved this
    // frame, and an entity spawned this frame would answer with the identity.
    // Walking upward costs the depth of the entity instead of the size of the
    // scene, and it is true at the instant it is asked.
    //
    // Composition goes through localMatrix, deliberately, so this can never
    // drift from what propagateTransforms produces.
    glm::mat4 worldMatrixOf(HorizonWorld& world, entt::entity e);

    // The translation of the above. Identity-safe: an entity without a
    // TransformComponent has no position, and (0,0,0) is the answer everything
    // else in the transform API gives for that case.
    glm::vec3 worldPositionOf(HorizonWorld& world, entt::entity e);

    // The local position an entity needs in order to STAND at `worldPos`, given
    // where its parents are. Without a parent the two are the same; with one it
    // is the parent's world matrix inverted and applied. This is the half that
    // makes "read a world position, offset it, put it back" possible at all.
    glm::vec3 localPositionForWorld(HorizonWorld& world, entt::entity e,
                                    const glm::vec3& worldPos);

}
