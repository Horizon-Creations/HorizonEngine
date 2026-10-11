#pragma once
#include <entt/entt.hpp>
#include <vector>

// Stores the parent/child relationship for the scene graph.
// HierarchyComponent alone does NOT update transforms — HE::propagateTransforms()
// (TransformHierarchy.h; the render extractor, the camera rig and the NavMesh
// collector call it) walks this hierarchy top-down from HorizonWorld::rootEntity()
// and writes TransformComponent::worldMatrix, skipping the subtrees that did not
// change. Use HorizonWorld::reparentEntity() to edit the links; it guards against
// cycles and builtin entities, and it moves HorizonWorld::structureEpoch(), which
// is how propagateTransforms learns that the links changed. Code that edits the
// links by hand calls HorizonWorld::noteStructureChanged() afterwards.
struct HierarchyComponent {
    entt::entity              parent = entt::null;
    std::vector<entt::entity> children;
};
