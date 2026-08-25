#pragma once
#include <HorizonScene/Components/NavMeshComponent.h>
#include <glm/glm.hpp>
#include <cstddef>

class HorizonWorld;
class ContentManager;
class DebugDrawBuffer;

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

    // Advance agent movement along their computed paths.  For each entity
    // with a NavAgentComponent and a TransformComponent, queries the NavMesh
    // held by the first NavMeshComponent found in the world, computes a path
    // if none exists, and moves along it according to speed*dt.
    void update(HorizonWorld& world, float dt);

    // Appends the baked NavMesh's polygon edges (one line loop per polygon,
    // world-space) into `out` for viewport visualization. No-op if `nmc.navMesh`
    // is null (not baked yet).
    void extractNavMeshWireframe(const NavMeshComponent& nmc, DebugDrawBuffer& out,
                                 const glm::vec3& color = glm::vec3(0.2f, 0.9f, 0.7f));
}
