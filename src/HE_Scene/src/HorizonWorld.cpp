#include "HorizonScene/HorizonWorld.h"
#include <HorizonCode/HcCompiledLoader.h>
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/EnvironmentLightComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <algorithm>

HorizonWorld::HorizonWorld()
{
    m_rootEntity = m_registry.create();
    m_registry.emplace<NameComponent>(m_rootEntity, NameComponent{ "World" });
    m_registry.emplace<HierarchyComponent>(m_rootEntity);
    // Sky (EnvironmentComponent) and Weather (WeatherComponent) are NOT created here:
    // a bare world starts empty. The Game/Simulation project templates seed a "Sky"
    // and "Weather" entity into their StartupScene; other projects (and a plain New
    // Scene) start with no sky, and the user adds one via the Environment window.
    reserveComponentStorage();
    // Widgets and the level script share this world's central HorizonCode
    // interpreter (rather than each running its own). Only the OWN widget
    // manager — an app-level one (setWidgetManager) manages its own runtime.
    m_ownWidgets.setRuntime(&scripts());
}

void HorizonWorld::reserveComponentStorage()
{
    // Force-create the core component pools up front so each pool's type-erased
    // operations belong to THIS module (the editor/game executable), never to a
    // hot-loaded native game-logic dylib. entt bakes a pool's destroy/move thunks into
    // whichever module first touches the type; if a dlopen'd IGameLogic is the first to
    // add e.g. a TransformComponent to an otherwise-empty world, that pool dangles the
    // moment the dylib is unloaded and the registry crashes at teardown. These five were
    // previously created implicitly by the constructor's default sky + sun/moon lights;
    // reserving them explicitly keeps a bare world safe now that it starts empty. Pure
    // reservation — adds no components, so the world stays empty.
    (void)m_registry.storage<TransformComponent>();
    (void)m_registry.storage<LightComponent>();
    (void)m_registry.storage<EnvironmentLightComponent>();
    (void)m_registry.storage<EnvironmentComponent>();
    (void)m_registry.storage<WeatherComponent>();
}

bool HorizonWorld::isBuiltin(Entity entity) const
{
    return entity == m_rootEntity || m_registry.all_of<EnvironmentLightComponent>(entity);
}

void HorizonWorld::ensureEnvironmentLights()
{
    // The sun/moon belong to the Sky entity. With no Sky entity there is no sky,
    // so drop any stray lights (a legacy scene, or a scene loaded before migration).
    const Entity sky = environmentEntity();
    if (sky == entt::null)
    {
        std::vector<Entity> strays;
        for (auto [e, elc] : m_registry.view<EnvironmentLightComponent>().each())
            strays.push_back(e);
        for (Entity e : strays)
            if (m_registry.valid(e)) destroyRecursive(e); // built-in → bypass the guard
        m_hierarchyDirty = true;
        return;
    }

    auto attach = [&](Entity e, Entity parent)
    {
        // Detach from the current parent (createEntity parents to root), then attach
        // under `parent` — the lights live UNDER the Sky entity so removeSky() takes
        // them down with it (scene load also rebuilds children from the serialised
        // list, which omits these never-serialised lights, so re-attach every time).
        auto& h = m_registry.get<HierarchyComponent>(e);
        if (h.parent != entt::null && h.parent != parent)
            if (auto* ph = m_registry.try_get<HierarchyComponent>(h.parent))
                ph->children.erase(std::remove(ph->children.begin(), ph->children.end(), e),
                                   ph->children.end());
        h.parent = parent;
        auto& sh = m_registry.get<HierarchyComponent>(parent);
        if (std::find(sh.children.begin(), sh.children.end(), e) == sh.children.end())
            sh.children.push_back(e);
    };

    auto ensure = [&](EnvironmentLightComponent::Role role, const char* name)
    {
        // Find an existing light with this role (e.g. recreated after scene load).
        Entity e = entt::null;
        for (auto [ent, elc] : m_registry.view<EnvironmentLightComponent>().each())
            if (elc.role == role) { e = ent; break; }

        if (e == entt::null)
        {
            e = createEntity(name); // Name + Hierarchy, parented to the root for now
            // A (default) transform so the render extractor's
            // <TransformComponent, LightComponent> view picks the light up; the
            // direction itself is driven by the environment, not this transform.
            m_registry.emplace<TransformComponent>(e, TransformComponent{});
            LightComponent lc;
            lc.type = HE::LightType::Directional;
            m_registry.emplace<LightComponent>(e, lc);
            m_registry.emplace<EnvironmentLightComponent>(e, EnvironmentLightComponent{ role });
        }
        attach(e, sky);
    };
    ensure(EnvironmentLightComponent::Role::Sun,  "Sun");
    ensure(EnvironmentLightComponent::Role::Moon, "Moon");
    m_hierarchyDirty = true;
}

Entity HorizonWorld::environmentEntity() const
{
    auto view = m_registry.view<const EnvironmentComponent>();
    for (auto e : view)
        return e; // at most one; first wins
    return entt::null;
}

Entity HorizonWorld::weatherEntity() const
{
    auto view = m_registry.view<const WeatherComponent>();
    for (auto e : view)
        return e;
    return entt::null;
}

Entity HorizonWorld::addSky()
{
    if (Entity e = environmentEntity(); e != entt::null)
        return e; // already have a sky
    Entity sky = createEntity("Sky");
    m_registry.emplace<EnvironmentComponent>(sky);
    ensureEnvironmentLights(); // sun/moon become children of the Sky entity
    return sky;
}

void HorizonWorld::removeSky()
{
    Entity sky = environmentEntity();
    if (sky == entt::null) return;
    if (sky == m_rootEntity)
    {
        // Legacy/edge case: env sitting on the World root — strip the component and
        // any lights rather than deleting the (non-deletable) root.
        m_registry.remove<EnvironmentComponent>(sky);
        ensureEnvironmentLights(); // sky now gone → tears down the stray lights
        m_hierarchyDirty = true;
        return;
    }
    // Detach from parent, then force-destroy the whole subtree (incl. the built-in
    // sun/moon child lights, which destroyEntity would otherwise skip).
    if (auto* h = m_registry.try_get<HierarchyComponent>(sky); h && h->parent != entt::null)
        if (auto* ph = m_registry.try_get<HierarchyComponent>(h->parent))
            ph->children.erase(std::remove(ph->children.begin(), ph->children.end(), sky),
                               ph->children.end());
    destroyRecursive(sky);
    m_hierarchyDirty = true;
}

Entity HorizonWorld::addWeather()
{
    if (Entity e = weatherEntity(); e != entt::null)
        return e;
    Entity weather = createEntity("Weather");
    m_registry.emplace<WeatherComponent>(weather);
    return weather;
}

void HorizonWorld::removeWeather()
{
    Entity weather = weatherEntity();
    if (weather == entt::null) return;
    if (weather == m_rootEntity) { m_registry.remove<WeatherComponent>(weather); return; }
    destroyEntity(weather); // no hidden children — the normal path is fine
}

void HorizonWorld::migrateLegacyRootEnvironment()
{
    // Legacy scenes stored Environment/Weather on the World root. Move each onto its
    // own dedicated entity so the whole engine sees a single, uniform model.
    if (m_registry.all_of<EnvironmentComponent>(m_rootEntity))
    {
        EnvironmentComponent e = m_registry.get<EnvironmentComponent>(m_rootEntity);
        m_registry.remove<EnvironmentComponent>(m_rootEntity);
        Entity sky = createEntity("Sky");
        m_registry.emplace<EnvironmentComponent>(sky, e);
    }
    if (m_registry.all_of<WeatherComponent>(m_rootEntity))
    {
        WeatherComponent w = m_registry.get<WeatherComponent>(m_rootEntity);
        m_registry.remove<WeatherComponent>(m_rootEntity);
        Entity weather = createEntity("Weather");
        m_registry.emplace<WeatherComponent>(weather, w);
    }
}

Entity HorizonWorld::createEntity(const std::string& name)
{
    Entity e = m_registry.create();
    m_registry.emplace<NameComponent>(e, NameComponent{ name });
    m_registry.emplace<HierarchyComponent>(e);
    auto& rootHierarchy = m_registry.get<HierarchyComponent>(m_rootEntity);
    rootHierarchy.children.push_back(e);
    m_registry.get<HierarchyComponent>(e).parent = m_rootEntity;
    m_hierarchyDirty = true;
    return e;
}

void HorizonWorld::destroyRecursive(Entity entity)
{
    if (!m_registry.valid(entity)) return;
    // Destroy the subtree bottom-up (children vector is copied — destroying
    // mutates the registry under us). No built-in guard here: the caller vetted the
    // top-level entity, and a Sky entity's built-in sun/moon lights must go with it.
    if (auto* h = m_registry.try_get<HierarchyComponent>(entity))
    {
        const std::vector<Entity> children = h->children;
        for (Entity child : children)
            destroyRecursive(child);
    }
    m_registry.destroy(entity);
}

void HorizonWorld::destroyEntity(Entity entity)
{
    if (!m_registry.valid(entity) || isBuiltin(entity))
        return; // root + the environment sun/moon lights are not deletable

    // Detach from parent first so nothing dangles, then force-destroy the subtree.
    if (auto* h = m_registry.try_get<HierarchyComponent>(entity); h && h->parent != entt::null)
        if (auto* ph = m_registry.try_get<HierarchyComponent>(h->parent))
        {
            auto& ch = ph->children;
            ch.erase(std::remove(ch.begin(), ch.end(), entity), ch.end());
        }

    destroyRecursive(entity);
    m_hierarchyDirty = true;
}

void HorizonWorld::clear()
{
    // A running level ends here (PIE stop / scene switch / shutdown all route
    // through clear()). No-op unless it was actually running, so the edit-time
    // clear() at the start of openScene doesn't spuriously fire OnLevelUnloaded.
    fireLevelUnloaded();

    // Live UI widgets track the world's lifetime (PIE stop / scene load) — but
    // ONLY a world-owned WM. An injected app-level WM (the game's persistent
    // GameInstance UI) outlives the world and is never cleared here.
    if (!m_widgetsPtr) m_ownWidgets.clear();

    // Root children first (handles whole subtrees, incl. a Sky entity's sun/moon),
    // then force-destroy any strays that were never parented into the hierarchy.
    // Everything but the root goes — a cleared world is bare (no sky/weather). New
    // Scene stays empty; a loaded scene recreates its own Sky/Weather from the file.
    if (auto* rh = m_registry.try_get<HierarchyComponent>(m_rootEntity))
    {
        const std::vector<Entity> children = rh->children;
        for (Entity c : children)
            destroyEntity(c);
    }
    std::vector<Entity> strays;
    for (auto e : m_registry.view<entt::entity>())
        if (e != m_rootEntity)
            strays.push_back(e);
    for (Entity e : strays)
        if (m_registry.valid(e))
            m_registry.destroy(e); // direct destroy: bypasses the built-in guard for env lights

    // Drop the level script too (like the environment, a loaded scene restores
    // its own via setLevelScriptJson; a scene without one starts empty).
    // fireLevelUnloaded above fired the event but kept the instance — remove it
    // now so a cleared world holds no level state.
    if (m_levelInstance) { scripts().remove(m_levelInstance); m_levelInstance = 0; }
    m_levelScript = HorizonCode::Graph{};

    // Scope rule: with widgets + the level script gone, drop every Create-Object
    // instance the GameInstance doesn't still hold (reachable via its Ref vars) —
    // only GameInstance-held objects persist across scene switches.
    scripts().retainOnlyReachableFrom(scripts().gameInstance());

    m_hierarchyDirty = true;
}

bool HorizonWorld::isAncestorOf(Entity ancestor, Entity entity) const
{
    Entity cur = entity;
    while (cur != entt::null && m_registry.valid(cur))
    {
        if (cur == ancestor)
            return true;
        const auto* h = m_registry.try_get<HierarchyComponent>(cur);
        cur = h ? h->parent : entt::null;
    }
    return false;
}

bool HorizonWorld::reparentEntity(Entity entity, Entity newParent)
{
    if (entity == m_rootEntity || entity == newParent)
        return false;
    // A built-in (the environment sun/moon) can't be reparented, and nothing may be
    // dropped UNDER a built-in — except the World root itself, which is the valid
    // target for un-parenting an entity back to the top level.
    if (isBuiltin(entity) || (isBuiltin(newParent) && newParent != m_rootEntity))
        return false;
    if (!m_registry.valid(entity) || !m_registry.valid(newParent))
        return false;
    if (isAncestorOf(entity, newParent))
        return false; // would create a cycle

    auto* h  = m_registry.try_get<HierarchyComponent>(entity);
    auto* nh = m_registry.try_get<HierarchyComponent>(newParent);
    if (!h || !nh)
        return false;
    if (h->parent == newParent)
        return true; // already there

    if (h->parent != entt::null)
        if (auto* ph = m_registry.try_get<HierarchyComponent>(h->parent))
        {
            auto& ch = ph->children;
            ch.erase(std::remove(ch.begin(), ch.end(), entity), ch.end());
        }

    nh->children.push_back(entity);
    h->parent = newParent;
    m_hierarchyDirty = true;
    return true;
}

void HorizonWorld::renameEntity(Entity entity, const std::string& newName)
{
    if (auto* n = m_registry.try_get<NameComponent>(entity))
    {
        n->name = newName;
        m_hierarchyDirty = true;
    }
}

// ── Level script ─────────────────────────────────────────────────────────────

std::string HorizonWorld::levelScriptJson() const
{
    // Empty graph → empty string, so scenes without a level script stay clean.
    if (m_levelScript.nodes.empty() && m_levelScript.variables.empty())
        return {};
    return HorizonCode::toJson(m_levelScript);
}

void HorizonWorld::setLevelScriptJson(const std::string& json)
{
    m_levelScript = HorizonCode::Graph{};
    if (!json.empty())
        HorizonCode::fromJson(json, m_levelScript); // broken/absent → empty graph
}

void HorizonWorld::fireLevelLoaded()
{
    if (m_levelRunning) return; // already loaded — fire OnLevelLoaded exactly once
    m_levelRunning = true;

    // Drop any instance lingering from a previous unload (a level restart), then
    // register a fresh running copy of the authored graph with the central
    // runtime, which seeds its private variable store from the graph defaults.
    // The level has no host bindings (no widget target); Print goes to the log
    // and variables live in the runtime. Engine-system nodes come later.
    if (m_levelInstance) scripts().remove(m_levelInstance);
    // Packaged builds: the export may have compiled this scene's level script to
    // native C++ — the key was set by the game runtime at scene load; a table
    // miss (editor, dev runs, per-asset fallback) interprets the graph as always.
    if (!m_levelScriptKey.empty())
        if (auto compiled = HorizonCode::compiledClasses().create(m_levelScriptKey))
        {
            m_levelInstance = scripts().addCompiled(std::move(compiled));
            scripts().fireEvent(m_levelInstance, "OnLevelLoaded", 0);
            return;
        }
    m_levelInstance = scripts().add(m_levelScript, {});
    scripts().fireEvent(m_levelInstance, "OnLevelLoaded", 0);
}

void HorizonWorld::fireLevelUnloaded()
{
    if (!m_levelRunning) return; // only fire for a level that actually loaded
    m_levelRunning = false;

    // Fire the event but KEEP the instance so its final variable state stays
    // readable after unload; it is dropped on the next load or on clear().
    scripts().fireEvent(m_levelInstance, "OnLevelUnloaded", 0);
}

