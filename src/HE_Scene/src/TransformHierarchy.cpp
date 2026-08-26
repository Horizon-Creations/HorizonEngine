#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

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

glm::mat4 worldMatrixOf(HorizonWorld& world, entt::entity e)
{
    entt::registry& reg = world.registry();
    if (!reg.valid(e)) return glm::mat4(1.0f);

    // Collect the chain first, then compose from the top down. Composing on the
    // way up would need the inverse order and would not match propagateFrom's
    // parentWorld * local — and "the same maths written twice" is exactly the
    // drift localMatrix exists to prevent.
    std::vector<entt::entity> chain;
    for (entt::entity cur = e; cur != entt::null && reg.valid(cur); )
    {
        chain.push_back(cur);
        const auto* h = reg.try_get<HierarchyComponent>(cur);
        entt::entity parent = h ? h->parent : entt::null;
        // The world root carries no transform of its own; stopping here also
        // ends the walk for anything parented to it.
        if (parent == entt::null || parent == world.rootEntity()) break;
        cur = parent;
    }

    glm::mat4 m(1.0f);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it)
        if (const auto* t = reg.try_get<TransformComponent>(*it))
            m = m * localMatrix(*t);
    return m;
}

glm::vec3 worldPositionOf(HorizonWorld& world, entt::entity e)
{
    return glm::vec3(worldMatrixOf(world, e)[3]);
}

glm::vec3 localPositionForWorld(HorizonWorld& world, entt::entity e, const glm::vec3& worldPos)
{
    entt::registry& reg = world.registry();
    if (!reg.valid(e)) return worldPos;

    const auto* h = reg.try_get<HierarchyComponent>(e);
    const entt::entity parent = h ? h->parent : entt::null;
    if (parent == entt::null || parent == world.rootEntity() || !reg.valid(parent))
        return worldPos;   // nothing above it: local IS world

    const glm::mat4 inv = glm::inverse(worldMatrixOf(world, parent));
    return glm::vec3(inv * glm::vec4(worldPos, 1.0f));
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
