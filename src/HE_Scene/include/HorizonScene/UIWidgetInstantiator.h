#pragma once
#include <entt/entt.hpp>
#include <vector>

class HorizonWorld;
class ContentManager;
using Entity = entt::entity;

// Expands UI Widget assets (HE::UIWidgetTree JSON) into live UI entities.
// Called at play start (PIE + packaged game), after assets are resident and
// BEFORE scripts are initialised, so widget-spawned ScriptComponents get
// instances like any hand-placed entity. PIE cleanup is free: the play-mode
// snapshot was taken before instantiation, so leaving play mode drops the
// spawned subtree with the restore.
namespace UIWidgetInstantiator
{
    // Expand one host entity's UIWidgetComponent. Adds a UICanvasComponent to
    // the host (sized from the tree) and creates one child entity per widget
    // node. Returns the spawned entities (empty when the asset is missing or
    // the component is inactive).
    std::vector<Entity> instantiate(HorizonWorld& world, ContentManager& content,
                                    Entity host);

    // Expand every active UIWidgetComponent in the world. Returns all spawned
    // entities.
    std::vector<Entity> instantiateAll(HorizonWorld& world, ContentManager& content);
}
