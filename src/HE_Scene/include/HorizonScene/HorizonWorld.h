#pragma once
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
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

	// The scene's central HorizonCode interpreter: widgets and the level script
	// register their running scripts here (rather than each running its own), so
	// they share one execution engine + state and can reference each other.
	HorizonCode::Runtime&       scripts()       { return m_scripts; }
	const HorizonCode::Runtime& scripts() const { return m_scripts; }

	// ── Level script ─────────────────────────────────────────────────────────
	// One HorizonCode graph per level (like Unreal's Level Blueprint): intrinsic
	// to the scene, authored in the Level Script editor, serialized with the
	// scene. It reacts to world events — "OnLevelLoaded" fires once when the
	// level starts (play-in-editor / game runtime), "OnLevelUnloaded" once when
	// it ends. Not tied to any entity; holds its own persistent variable store.
	HorizonCode::Graph&       levelScript()       { return m_levelScript; }
	const HorizonCode::Graph& levelScript() const { return m_levelScript; }
	// JSON accessors used by the scene serializer (empty when the graph has no
	// nodes, so empty level scripts don't clutter the scene file).
	std::string levelScriptJson() const;
	void        setLevelScriptJson(const std::string& json);

	// Run the level graph's "OnLevelLoaded" / "OnLevelUnloaded" events. Loaded
	// seeds the variable store from the graph defaults and marks the level
	// running; Unloaded is a no-op unless the level is running, so it fires
	// exactly once per load regardless of which teardown path triggers it
	// (PIE stop, scene switch, runtime shutdown). clear() calls Unloaded.
	void fireLevelLoaded();
	void fireLevelUnloaded();
	bool isLevelRunning() const { return m_levelRunning; }
	// Live level-script variable store (seeded at load, mutated by Set nodes).
	// Read-only view for tooling/tests; backed by the runtime instance.
	const std::unordered_map<std::string, HorizonCode::Value>& levelVariables() const
	{ return m_scripts.variablesOf(m_levelInstance); }


private:
	entt::registry registry_;
	Entity         rootEntity_      = entt::null;
	bool           m_hierarchyDirty = true;
	// Declared before m_widgets so it outlives it (m_widgets points at it).
	HorizonCode::Runtime m_scripts;
	WidgetManager        m_widgets;

	// The level script's authored source graph (edited + serialized). At load a
	// copy is registered in m_scripts as m_levelInstance (the running instance).
	HorizonCode::Graph      m_levelScript;
	HorizonCode::InstanceId m_levelInstance = 0;
	bool                    m_levelRunning  = false;
};
