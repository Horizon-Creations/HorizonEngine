#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace HE {

namespace {

    void propagateFrom(entt::registry& reg, entt::entity e, const glm::mat4& parentWorld)
    {
        glm::mat4 world = parentWorld;
        if (auto* t = reg.try_get<TransformComponent>(e))
        {
            world          = parentWorld * localMatrix(*t);
            t->worldMatrix = world;
            t->dirty       = false;
        }
        if (auto* h = reg.try_get<HierarchyComponent>(e))
            for (entt::entity child : h->children)
                propagateFrom(reg, child, world);
    }

} // namespace

glm::mat4 localMatrix(const TransformComponent& t)
{
    glm::quat q = glm::quat(glm::radians(t.rotation));
    return glm::translate(glm::mat4(1.0f), t.position)
         * glm::mat4_cast(q)
         * glm::scale(glm::mat4(1.0f), t.scale);
}

void propagateTransforms(HorizonWorld& world)
{
    entt::registry& reg = world.registry();

    propagateFrom(reg, world.rootEntity(), glm::mat4(1.0f));

    // Entities outside the root hierarchy (no HierarchyComponent)
    for (auto [e, t] : reg.view<TransformComponent>(entt::exclude<HierarchyComponent>).each())
    {
        t.worldMatrix = localMatrix(t);
        t.dirty       = false;
    }
}

} // namespace HE
