#pragma once
#include <HorizonScene/Components/NavMeshComponent.h>
#include <glm/glm.hpp>
#include <entt/entt.hpp>   // entt::entity — the per-agent calls below take one
#include <cstddef>

class HorizonWorld;
class ContentManager;
class DebugDrawBuffer;
class PhysicsWorld;

namespace NavigationSystem {
    // Gather the scene's static, visible mesh geometry — terrain chunks included —
    // into `out` as world-space triangles, replacing whatever it held. This is the
    // missing half of baking: NavMeshComponent::geometry used to have no writer but
    // the serializer, so the editor's Bake button had nothing to build from.
    // Returns the number of triangles collected.
    std::size_t collectStaticGeometry(HorizonWorld& world, ContentManager& content,
                                      NavMeshGeometry& out);

    // Build (or rebuild) the NavMesh stored in the given component.
    // Returns true on success. Must be called after geometry is provided.
    bool bake(NavMeshComponent& nmc);

    // Advance agent movement along their computed paths. For each entity with a
    // NavAgentComponent and a TransformComponent, queries the NavMesh held by
    // the first NavMeshComponent found in the world, computes a path if none
    // exists (or if the target has moved away from the one it was planned for),
    // and follows it at `speed`.
    //
    // `physics` is what makes a nav agent a body rather than a ghost. An agent
    // with a CharacterControllerComponent is steered through
    // PhysicsWorld::setCharacterVelocity — the same way the player is — so it
    // collides, falls and is stopped by walls; writing its transform instead
    // either walks it through geometry or leaves its collider at the spawn
    // point. Pass the world's PhysicsWorld whenever there is one.
    //
    // A null `physics` therefore means "nothing is simulating": the editor
    // passes it in play mode only. That is also the gate on
    // NavAgentComponent::autoStart, so opening a scene for editing does not send
    // every NPC walking off its authored position.
    void update(HorizonWorld& world, float dt, PhysicsWorld* physics = nullptr);

    // ── What a script calls ──────────────────────────────────────────────────
    // Send the agent to `target` (world space) and start it walking. The path is
    // searched HERE, not on the next tick, which is the whole reason this exists
    // rather than the caller writing targetPos and moving itself: a script that
    // asks an NPC to go somewhere has to be told NOW whether it can get there,
    // so it can pick another destination in the same breath. Returns false and
    // leaves the agent stopped when there is no NavMesh, when either end is off
    // it, or when no route connects them.
    //
    // Setting NavAgentComponent::targetPos by hand still works and still
    // re-plans (update() notices the target moved) — this is the answer-giving
    // version of the same gesture.
    bool moveTo(HorizonWorld& world, entt::entity e, const glm::vec3& target);

    // Metres left to walk along the current path, summed waypoint to waypoint
    // from where the agent stands — not the straight line to the target, which
    // is a different number the moment a wall is in the way. -1 when the agent
    // has no path (including "not moving"), so a caller can tell "arrived" from
    // "never started".
    float remainingDistance(HorizonWorld& world, entt::entity e);

    // Appends the baked NavMesh's polygon edges (one line loop per polygon,
    // world-space) into `out` for viewport visualization. No-op if `nmc.navMesh`
    // is null (not baked yet).
    void extractNavMeshWireframe(const NavMeshComponent& nmc, DebugDrawBuffer& out,
                                 const glm::vec3& color = glm::vec3(0.2f, 0.9f, 0.7f));
}
