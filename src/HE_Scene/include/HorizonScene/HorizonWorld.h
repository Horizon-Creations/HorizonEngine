#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Components/NameComponent.h"
#include "Components/HierarchyComponent.h"
#include "WidgetManager.h"

using Entity = entt::entity;

class HorizonWorld {
public:
	HorizonWorld();
	~HorizonWorld() = default;

	Entity rootEntity() const { return rootEntity_; }

	Entity createEntity(const std::string& name = "Entity");
	// Destroys the entity and its entire subtree.
	void   destroyEntity(Entity entity);
	void   renameEntity(Entity entity, const std::string& newName);

	// Moves `entity` under `newParent`. Fails (returns false) when the move
	// would create a cycle, target the root itself, or parent is invalid.
	bool   reparentEntity(Entity entity, Entity newParent);
	// Destroys every entity except the root (used by scene load / play-mode
	// restore).
	void   clear();
	// True when `ancestor` appears on `entity`'s parent chain (or is equal).
	bool   isAncestorOf(Entity ancestor, Entity entity) const;

	bool isHierarchyDirty()  const { return m_hierarchyDirty; }
	void clearHierarchyDirty()     { m_hierarchyDirty = false; }
	void markHierarchyDirty()      { m_hierarchyDirty = true;  }

	// Built-in entities (the root and the environment sun/moon lights) cannot be
	// deleted or have arbitrary components managed; they belong to the World.
	bool isBuiltin(Entity entity) const;
	// Ensures the hidden, built-in sun + moon directional lights exist on the root
	// (creating them if missing). Idempotent; called on construction, clear() and
	// after scene load (the lights are never serialised, so they are recreated).
	void ensureEnvironmentLights();

	// Template helpers — must stay in header
	void addComponent(Entity entity, auto&& component)
	{
		registry_.emplace<std::decay_t<decltype(component)>>(entity, std::forward<decltype(component)>(component));
	}
	bool hasComponent(Entity entity, auto&& componentType)
	{
		return registry_.all_of<std::decay_t<decltype(componentType)>>(entity);
	}
	void removeComponent(Entity entity, auto&& componentType)
	{
		registry_.remove<std::decay_t<decltype(componentType)>>(entity);
	}

	entt::registry& registry() { return registry_; }

	// Live UI widgets (UMG-style) — NOT entities; they exist outside the scene
	// graph and render directly. Owned here so their lifetime tracks the world
	// (clear() drops them: PIE stop / scene load discards play-created widgets).
	WidgetManager& widgets() { return m_widgets; }


private:
	entt::registry registry_;
	Entity         rootEntity_      = entt::null;
	bool           m_hierarchyDirty = true;
	WidgetManager  m_widgets;
};
