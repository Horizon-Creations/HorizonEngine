#pragma once
#include <entt/entt.hpp>
#include <Math/Math.h>

class HorizonWorld;

// Manages parent/child relationships and propagates world transforms.
// Call propagateTransforms() once per frame before RenderExtractor::extract().
class SceneGraph {
public:
    // Attach child to parent. Updates HierarchyComponent on both entities.
    void setParent(entt::entity child, entt::entity parent, HorizonWorld& world);

    // Detach from parent. Child becomes a root entity.
    void detach(entt::entity child, HorizonWorld& world);

    // Recompute worldMatrix for every TransformComponent in parent-first order.
    // Only processes entities where dirty == true (or whose parent chain is dirty).
    void propagateTransforms(HorizonWorld& world);

private:
    void propagateEntity(entt::entity entity,
                         const glm::mat4& parentWorld,
                         HorizonWorld& world);
};
