#include "HorizonScene/SceneGraph.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>

void SceneGraph::setParent(entt::entity child, entt::entity parent, HorizonWorld& world) {
    auto& reg = world.registry();

    // Detach from old parent first
    detach(child, world);

    // Set new parent on child
    auto& childHier = reg.get_or_emplace<HierarchyComponent>(child);
    childHier.parent = parent;

    // Register child on parent
    auto& parentHier = reg.get_or_emplace<HierarchyComponent>(parent);
    parentHier.children.push_back(child);
}

void SceneGraph::detach(entt::entity child, HorizonWorld& world) {
    auto& reg = world.registry();
    auto* hier = reg.try_get<HierarchyComponent>(child);
    if (!hier || hier->parent == entt::null) return;

    auto* parentHier = reg.try_get<HierarchyComponent>(hier->parent);
    if (parentHier) {
        auto& ch = parentHier->children;
        ch.erase(std::remove(ch.begin(), ch.end(), child), ch.end());
    }
    hier->parent = entt::null;
}

void SceneGraph::propagateTransforms(HorizonWorld& world) {
    auto& reg = world.registry();
    auto view = reg.view<TransformComponent, HierarchyComponent>();
    for (auto [e, t, h] : view.each()) {
        if (h.parent == entt::null && t.dirty)
            propagateEntity(e, glm::mat4(1.0f), world);
    }
}

void SceneGraph::propagateEntity(entt::entity entity,
                                  const glm::mat4& parentWorld,
                                  HorizonWorld& world) {
    auto& reg = world.registry();
    auto* t = reg.try_get<TransformComponent>(entity);
    if (!t) return;

    glm::quat q = glm::quat(glm::radians(t->rotation));
    glm::mat4 local = glm::translate(glm::mat4(1.0f), t->position)
                    * glm::mat4_cast(q)
                    * glm::scale(glm::mat4(1.0f), t->scale);

    t->worldMatrix = parentWorld * local;
    t->dirty       = false;

    auto* hier = reg.try_get<HierarchyComponent>(entity);
    if (!hier) return;
    for (entt::entity child : hier->children)
        propagateEntity(child, t->worldMatrix, world);
}
