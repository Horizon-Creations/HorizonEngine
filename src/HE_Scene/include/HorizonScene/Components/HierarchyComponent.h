#pragma once
#include <entt/entt.hpp>
#include <vector>

// Stores the parent/child relationship for the scene graph.
// HierarchyComponent alone does NOT update transforms —
// SceneGraph::propagateTransforms() does that each frame.
struct HierarchyComponent {
    entt::entity              parent = entt::null;
    std::vector<entt::entity> children;
};
