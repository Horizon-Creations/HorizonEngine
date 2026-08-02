#pragma once
#include <entt/entt.hpp>
#include <vector>

// Stores the parent/child relationship for the scene graph.
// HierarchyComponent alone does NOT update transforms — RenderExtractor::extract()
// walks this hierarchy top-down from HorizonWorld::rootEntity() each frame and
// writes TransformComponent::worldMatrix. Use HorizonWorld::reparentEntity() to
// edit the links; it guards against cycles and builtin entities.
struct HierarchyComponent {
    entt::entity              parent = entt::null;
    std::vector<entt::entity> children;
};
