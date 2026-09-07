#pragma once
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <HorizonCode/HorizonCode.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include "Components/NameComponent.h"
#include "Components/HierarchyComponent.h"
#include "Components/EntityIdComponent.h"
#include <UIWidget/WidgetManager.h>

using Entity = entt::entity;

class HorizonWorld {
public:
	HorizonWorld();
	~HorizonWorld() = default;

	Entity rootEntity() const { return m_rootEntity; }

	Entity createEntity(const std::string& name = "Entity");

	// ── Stable entity identity ───────────────────────────────────────────────
	// Every entity carries an EntityIdComponent minted at creation. These are the
	// accessors for it; see the component header for why handles are not enough.
	//
	// A default-constructed UUID (both halves zero) means "no id", which only
	// happens for an invalid handle — generate() never produces it, since it
	// always sets the RFC 4122 version and variant bits.
	HE::UUID entityId(Entity entity) const;
	// The entity carrying `id`, or entt::null. Linear over the id pool: fine for
	// load-time resolution and tests, not for a per-frame lookup.
	Entity   findByEntityId(const HE::UUID& id) const;
	// Replace an entity's identity. Used by scene loading to restore the value
	// from the file — deliberately NOT used by prefab instantiation, which keeps
	// the freshly minted id so one prefab inserted twice yields two identities.
	void     setEntityId(Entity entity, const HE::UUID& id);
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
	// Ensures the hidden, built-in sun + moon directional lights exist as children
	// of the Sky entity (creating them if missing) — or, when the scene has no Sky
	// entity, destroys any stray lights. Idempotent; called from addSky() and after
	// scene load (the lights are never serialised, so they are recreated). Also
	// repairs scenes that accumulated ORDINARY "Sun"/"Moon" copies under the Sky
	// (see the .cpp) by adopting one per role and deleting the rest.
	void ensureEnvironmentLights();
	// Copies the Sky's authored sun/moon colour + brightness onto the built-in
	// lights' LightComponent so the component data matches the Sky panel instead of
	// showing a stale default. Cheap; call it once per frame alongside the
	// environment push (the renderer reads the environment directly either way).
	void syncEnvironmentLights();
	// Logs a warning when the scene carries more than one Sky or Weather entity.
	// The extras are inert (every consumer takes the first), but they are authored
	// data — the user removes them in the Outliner, the engine does not guess.
	void warnOnDuplicateEnvironmentEntities() const;
	// Deletes engine-generated entities that a scene file should never have
	// contained: terrain chunks (TerrainSystem regenerates them from the
	// TerrainComponent) that came back from disk WITHOUT their TerrainChunkComponent
	// and are therefore invisible to that regeneration. Called by the scene loader.
	void purgeOrphanedGeneratedEntities();

	// ── Sky & Weather scene entities ───────────────────────────────────────────
	// Sky (EnvironmentComponent) and Weather (WeatherComponent) are ordinary,
	// deletable scene entities — added/removed via the editor's Environment window
	// (View menu) and visible in the Outliner. There is at most one of each. A bare
	// world has neither: the Game/Simulation project templates seed them, everything
	// else starts empty.
	//   * environmentEntity() / weatherEntity(): the entity carrying the component,
	//     or entt::null when the scene has none (scans the registry).
	//   * addSky() creates the "Sky" entity (+ the built-in sun/moon child lights)
	//     if absent and returns it; removeSky() destroys it (and its lights).
	//     addWeather()/removeWeather() do the same for the "Weather" entity.
	Entity environmentEntity() const;
	Entity weatherEntity() const;
	Entity addSky();
	void   removeSky();
	Entity addWeather();
	void   removeWeather();
	// Legacy scenes serialised Environment/Weather on the World root; move them onto
	// dedicated "Sky"/"Weather" entities so every scene uses the same model. Called
	// by the scene loader after applying components. No-op when nothing is on root.
	void   migrateLegacyRootEnvironment();

	// Template helpers — must stay in header
	void addComponent(Entity entity, auto&& component)
	{
		m_registry.emplace<std::decay_t<decltype(component)>>(entity, std::forward<decltype(component)>(component));
	}
	bool hasComponent(Entity entity, auto&& componentType)
	{
		return m_registry.all_of<std::decay_t<decltype(componentType)>>(entity);
	}
	void removeComponent(Entity entity, auto&& componentType)
	{
		m_registry.remove<std::decay_t<decltype(componentType)>>(entity);
	}

	entt::registry& registry() { return m_registry; }

	// Live UI widgets (UMG-style) — NOT entities; they exist outside the scene
	// graph and render directly. By default a world owns its widgets (clear()
	// drops them: PIE stop / scene load discards play-created widgets). The game
	// injects an APP-LEVEL WidgetManager (setWidgetManager) so the GameInstance's
	// UI lives above any single world and survives scene switches; an external WM
	// is never cleared by the world.
	WidgetManager& widgets() { return m_widgetsPtr ? *m_widgetsPtr : m_ownWidgets; }
	void setWidgetManager(WidgetManager* wm)
	{ m_widgetsPtr = wm; if (m_widgetsPtr) m_widgetsPtr->setRuntime(&scripts());
	  else m_ownWidgets.setRuntime(&scripts()); }

	// The central HorizonCode interpreter widgets and the level script run on.
	// By default a world owns its own, but the application injects an app-wide
	// runtime (setScriptRuntime, right after construction) so the GameInstance
	// and its state survive scene switches and can be referenced from any scene.
	HorizonCode::Runtime&       scripts()       { return m_scriptsPtr ? *m_scriptsPtr : m_ownScripts; }
	const HorizonCode::Runtime& scripts() const { return m_scriptsPtr ? *m_scriptsPtr : m_ownScripts; }
	void setScriptRuntime(HorizonCode::Runtime* r)
	{ m_scriptsPtr = r; widgets().setRuntime(&scripts()); }

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
	// The scene's compiled-class key ("level:<uuid>", levelScriptKeyForUuid).
	// Set by the game runtime after loading a packed scene; when the process's
	// CompiledClassTable has an entry for it, fireLevelLoaded runs the COMPILED
	// level script instead of interpreting m_levelScript. Empty (the editor,
	// loose-scene dev runs) → always interpreted.
	void setLevelScriptKey(std::string key) { m_levelScriptKey = std::move(key); }

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
	{ return scripts().variablesOf(m_levelInstance); }


private:
	// Destroy an entire subtree (children first), ignoring the built-in guard — the
	// caller has already vetted the top-level entity. Lets removeSky() take its
	// hidden sun/moon child lights down with it.
	void destroyRecursive(Entity entity);

	// Force-create the core component pools so they belong to this module, not a
	// hot-loaded native game-logic dylib (which would dangle them on unload). Called
	// once from the constructor. See the .cpp for the full rationale.
	void reserveComponentStorage();

	entt::registry m_registry;
	Entity         m_rootEntity     = entt::null;
	bool           m_hierarchyDirty = true;
	// Declared before the widget managers so it outlives them (they point at it).
	HorizonCode::Runtime  m_ownScripts;          // used unless an app runtime is injected
	HorizonCode::Runtime* m_scriptsPtr = nullptr;
	WidgetManager         m_ownWidgets;          // used unless an app-level WM is injected
	WidgetManager*        m_widgetsPtr = nullptr; // set → app-level UI (persists across worlds)

	// The level script's authored source graph (edited + serialized). At load a
	// copy is registered in m_scripts as m_levelInstance (the running instance).
	HorizonCode::Graph      m_levelScript;
	HorizonCode::InstanceId m_levelInstance = 0;
	bool                    m_levelRunning  = false;
	std::string             m_levelScriptKey;   // compiled lookup key (packaged builds)
};
