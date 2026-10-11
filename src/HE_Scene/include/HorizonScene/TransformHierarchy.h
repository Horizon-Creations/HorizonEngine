#pragma once
#include <glm/glm.hpp>
#include <entt/entt.hpp>   // entt::entity — the per-entity queries below take one
#include <cstddef>

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

    // The same matrix, taken from TransformComponent::localCache when that was
    // built from exactly the current position/rotation/scale, and computed
    // otherwise. Never writes the cache (only propagateTransforms does), so it
    // is as safe to call from anywhere as localMatrix itself.
    glm::mat4 cachedLocalMatrix(const TransformComponent& t);

    // Bring worldMatrix up to date for every entity, and clear their dirty flags.
    // The result is bit-identical to propagateTransformsFull below, but only the
    // subtrees that changed are recomputed.
    //
    // WHAT "CHANGED" MEANS. A world matrix is parent-world * local, and it moves for
    // five reasons, each with its own detector:
    //   * an entity's own position/rotation/scale — found by comparing them with the
    //     values its localCache was built from (TransformComponent::localCache), the
    //     same comparison that already decided whether to pay the sin/cos. Not by the
    //     dirty flag: too many writers (the Inspector, the gizmo) never set it. The flag
    //     is honoured when it IS set, as an extra hint.
    //   * a whole TransformComponent written back (`*tc = saved`): a copy does not carry
    //     the cache's validity (TransformCacheFlag), so it is found the same way.
    //   * the shape of the hierarchy (a reparent, a cell streamed in or out, a scene
    //     load, a spawn) — HorizonWorld::structureEpoch(), compared with the one the last
    //     pass saw. A different one means the next pass walks everything.
    //   * a TransformComponent taken off a node that still has children (they take
    //     the grandparent's matrix) — a registry hook on that component's destruction.
    //   * a world matrix that was written by someone else — only the floating origin
    //     does that; it calls invalidateWorldMatrices below.
    // An entity that changed is recomputed together with everything under it (its
    // children's parent matrix moved); everything else keeps the matrix it has.
    //
    // Two environment switches, for diagnosis: HE_PROPAGATE_FULL=1 makes every call the
    // full walk (the way to tell whether a stale transform is this pass), and
    // HE_PROPAGATE_VERIFY=1 checks every incremental pass against a full walk by bits
    // and aborts on a difference (slow; for runs that must prove the skipping right).
    //
    // THE COST. A pass is a scan over the packed TransformComponent storage (one
    // sequential read per entity, no tree walk, no matrix product) plus the work for
    // the changed subtrees. A world where nothing changed pays the scan only; one
    // where most of it changed (or where the hierarchy changed) pays the full walk
    // it always did, without the scan.
    //
    // Walking from rootEntity() is what makes the full walk work: HorizonWorld
    // parents everything to a root *entity*, so anything keyed on parent ==
    // entt::null would never fire. Entities outside that hierarchy (no
    // HierarchyComponent) are handled separately — for them local IS world.
    //
    // WHAT IT ASSUMES. Every entity is listed in the children of the parent its
    // HierarchyComponent names, and nowhere else (HorizonWorld keeps it so), and a
    // component's world matrix is not written by anything but this function and
    // shiftWorldOrigin. Code that rewires HierarchyComponent itself calls
    // HorizonWorld::noteStructureChanged(), as the scene loaders do; a
    // TransformComponent taken off a live entity is noticed on its own.
    void propagateTransforms(HorizonWorld& world);

    // The whole hierarchy, unconditionally, top-down from the root: the walk
    // propagateTransforms did before it learned to skip. Kept as the reference the
    // differential tests compare against and as the thing to call after writing
    // matrices or links by hand. Leaves the incremental state consistent (the next
    // propagateTransforms scans from here).
    void propagateTransformsFull(HorizonWorld& world);

    // The next propagateTransforms walks everything. For code that writes
    // TransformComponent::worldMatrix itself, or rewires the hierarchy without
    // HorizonWorld's methods: shiftWorldOrigin (it moves every world matrix by hand
    // so a reader before the next pass sees the new place). One flag, no work now.
    void invalidateWorldMatrices(HorizonWorld& world);

    // What the last propagateTransforms of this world did, for the tests and the
    // profiler's counters.
    struct PropagateStats
    {
        size_t transforms = 0;    // TransformComponents in the world
        size_t flagged    = 0;    // entities the scan found changed (0 when it did not run)
        size_t recomputed = 0;    // world matrices written
        bool   scanned    = false;   // the change scan ran
        bool   full       = false;   // the whole hierarchy was walked
    };
    PropagateStats lastPropagateStats(HorizonWorld& world);

    // Switches that turn one detector off, so a test can show that the differential
    // comparison notices when it is missing (a negative control that stays true,
    // unlike a line in a document). All off. Process-wide, set by tests only.
    struct PropagateTestSwitches
    {
        bool ignoreStructureEpoch  = false;   // a different structureEpoch no longer forces the full walk
        bool ignoreInvalidation    = false;   // invalidateWorldMatrices does nothing
        bool ignoreValueCompare    = false;   // the scan trusts the dirty flag only
        bool ignoreTransformRemoval = false;  // a TransformComponent taken off a parent is not noticed
        bool suspendVerify         = false;   // HE_PROPAGATE_VERIFY does not check: the test leaves the world in a state on purpose
    };
    void setPropagateTestSwitches(const PropagateTestSwitches& switches);

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
