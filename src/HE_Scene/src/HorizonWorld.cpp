#include "HorizonScene/HorizonWorld.h"
#include <HorizonCode/HcCompiledLoader.h>
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/TerrainComponent.h"
#include "HorizonScene/Components/TerrainChunkComponent.h"
#include "HorizonScene/Components/EnvironmentLightComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/EntityIdComponent.h"
#include "HorizonScene/Components/RopeComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include <Diagnostics/Log.h>
#include <algorithm>

HorizonWorld::HorizonWorld()
{
    m_rootEntity = m_registry.create();
    m_registry.emplace<NameComponent>(m_rootEntity, NameComponent{ "World" });
    m_registry.emplace<HierarchyComponent>(m_rootEntity);
    // The root gets an identity like everything else. It is the one entity a
    // scene load maps onto an EXISTING entity rather than creating (see
    // applySceneJson), so without this its id would differ between the world
    // that saved a scene and the world that loaded it.
    m_registry.emplace<EntityIdComponent>(m_rootEntity, EntityIdComponent{ HE::UUID::generate() });
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
    // Every entity carries one of these, so it is guaranteed to be touched — and
    // therefore guaranteed to be the kind of pool a hot-loaded game-logic dylib
    // must never create first.
    (void)m_registry.storage<EntityIdComponent>();
    // Ropes and trails are reserved for a narrower reason than the pools above:
    // they are the component types a GAME is most likely to add first at runtime
    // (a grapple line, a weapon trail) rather than the editor placing one in a
    // scene — and "added first by the dlopen'd game logic" is exactly the case
    // that dangles the pool when that library unloads. Every other component
    // type reaches the registry through the editor or a scene load long before
    // any game code runs, which is why the list is not simply all of them.
    (void)m_registry.storage<RopeComponent>();
    (void)m_registry.storage<TrailComponent>();
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

    // ── Pass 1: exactly one tagged light per role ────────────────────────────
    // More than one is corruption (see the adoption pass below for how a scene
    // ends up with copies); keep the first, destroy the rest.
    Entity tagged[2] = { entt::null, entt::null };
    {
        std::vector<Entity> extra;
        for (auto [e, elc] : m_registry.view<EnvironmentLightComponent>().each())
        {
            Entity& slot = tagged[static_cast<size_t>(elc.role)];
            if (slot == entt::null) slot = e;
            else                    extra.push_back(e);
        }
        for (Entity e : extra)
            if (m_registry.valid(e)) destroyRecursive(e);
    }

    // ── Pass 2: adopt orphaned copies, then create what is still missing ─────
    // A copy is an untagged directional light named "Sun"/"Moon" sitting directly
    // under a Sky entity. These exist because subtree serialisation (Save as
    // Prefab, collaboration's create-replication) used to walk the Sky's children
    // without skipping the built-ins: the copy came back as an ORDINARY light —
    // full intensity, invisible to the day-night pass, visible in the Outliner,
    // and written into the scene file, where it multiplied on every round-trip.
    // Adopting rather than deleting is what heals those scenes in place: the copy
    // becomes the role's one built-in light instead of a second one.
    //
    // Every EnvironmentComponent entity is searched, not just the one
    // environmentEntity() picked: a scene damaged this way often carries a second
    // Sky as well, and that is exactly where the copies hang.
    auto candidates = [&]()
    {
        std::vector<Entity> out;
        for (auto [se, ec] : m_registry.view<EnvironmentComponent>().each())
        {
            (void)ec;
            const auto* h = m_registry.try_get<HierarchyComponent>(se);
            if (!h) continue;
            for (Entity child : h->children)
            {
                if (!m_registry.valid(child)) continue;
                if (m_registry.all_of<EnvironmentLightComponent>(child)) continue;
                const auto* lc = m_registry.try_get<LightComponent>(child);
                const auto* n  = m_registry.try_get<NameComponent>(child);
                if (lc && lc->type == HE::LightType::Directional && n &&
                    (n->name == "Sun" || n->name == "Moon"))
                    out.push_back(child);
            }
        }
        return out;
    };

    auto adopt = [&](EnvironmentLightComponent::Role role, const char* name) -> Entity
    {
        for (Entity child : candidates())
        {
            const auto* n = m_registry.try_get<NameComponent>(child);
            if (!n || n->name != name) continue;
            m_registry.emplace<EnvironmentLightComponent>(child, EnvironmentLightComponent{ role });
            if (!m_registry.all_of<TransformComponent>(child))
                m_registry.emplace<TransformComponent>(child, TransformComponent{});
            HE_LOG_WARN(Scene, "Scene repair: adopted a stray '%s' light as the built-in "
                               "environment light (scene file carried a duplicate).", name);
            return child;
        }
        return entt::null;
    };

    auto ensure = [&](EnvironmentLightComponent::Role role, const char* name)
    {
        Entity e = tagged[static_cast<size_t>(role)];
        if (e == entt::null) e = adopt(role, name);
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

    // ── Pass 3: any REMAINING untagged Sun/Moon under a Sky is a leftover ────
    // (adoption only takes one per role, so a scene that accumulated several
    // copies still has the others hanging around lighting the scene).
    for (Entity e : candidates())
    {
        if (!m_registry.valid(e)) continue;
        HE_LOG_WARN(Scene, "Scene repair: removed a leftover duplicate environment light.");
        destroyRecursive(e);
    }

    // A second Sky/Weather does nothing at all — every consumer takes the first
    // one — but it is authored data, so say so loudly instead of deleting it and
    // let the user remove the one they don't want in the Outliner.
    warnOnDuplicateEnvironmentEntities();

    syncEnvironmentLights();
    m_hierarchyDirty = true;
}

void HorizonWorld::warnOnDuplicateEnvironmentEntities() const
{
    size_t skies = 0, weathers = 0;
    for (auto e : m_registry.view<EnvironmentComponent>()) { (void)e; ++skies; }
    for (auto e : m_registry.view<WeatherComponent>())     { (void)e; ++weathers; }
    if (skies > 1)
        HE_LOG_WARN(Scene, "Scene has %zu Sky entities — only one is used (the others are "
                           "inert). Delete the extras in the Outliner.", skies);
    if (weathers > 1)
        HE_LOG_WARN(Scene, "Scene has %zu Weather entities — only one is used (the others "
                           "are inert). Delete the extras in the Outliner.", weathers);
}

void HorizonWorld::syncEnvironmentLights()
{
    // Mirror the Sky's authored sun/moon colour + brightness onto the built-in
    // lights' LightComponent. The RenderExtractor overrides both at render time
    // anyway (it also folds in the day-night arc and cloud cover), but without
    // this the ECS data reads a flat default 1.0 that has nothing to do with the
    // Sky panel — a lie for anything that inspects the component (Details panel,
    // scripts, tools).
    const Entity sky = environmentEntity();
    if (sky == entt::null) return;
    const auto* env = m_registry.try_get<EnvironmentComponent>(sky);
    if (!env) return;

    for (auto [e, elc, lc] : m_registry.view<EnvironmentLightComponent, LightComponent>().each())
    {
        const bool isSun = (elc.role == EnvironmentLightComponent::Role::Sun);
        lc.color     = isSun ? env->sunColor     : env->moonColor;
        lc.intensity = isSun ? env->sunIntensity : env->moonIntensity;
    }
}

void HorizonWorld::purgeOrphanedGeneratedEntities()
{
    // Terrain chunks are runtime output: the TerrainSystem finds them through
    // TerrainChunkComponent, which is never serialised. A chunk that came back from
    // a scene file has lost that component, so the system neither updates nor
    // deletes it — it just sits there as a frozen second copy of the landscape
    // (stale mesh, stale heights) on top of the real, regenerated one. They only
    // ever got into a file via subtree serialisation walking a terrain's children;
    // delete them so the terrain rebuilds clean.
    std::vector<Entity> ghosts;
    for (auto [te, tc] : m_registry.view<TerrainComponent>().each())
    {
        (void)tc;
        const auto* hier = m_registry.try_get<HierarchyComponent>(te);
        if (!hier) continue;
        for (Entity child : hier->children)
        {
            if (!m_registry.valid(child)) continue;
            if (m_registry.all_of<TerrainChunkComponent>(child)) continue; // live chunk
            const auto* n = m_registry.try_get<NameComponent>(child);
            if (n && n->name == "TerrainChunk")
                ghosts.push_back(child);
        }
    }
    if (ghosts.empty()) return;
    HE_LOG_WARN(Scene, "Scene repair: removed %zu orphaned terrain chunk(s) from the "
                       "scene file (they are regenerated from the TerrainComponent).",
                ghosts.size());
    for (Entity e : ghosts)
        if (m_registry.valid(e)) destroyRecursive(e);
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
    // Minted here rather than by callers, so no creation path can forget it.
    // Scene loading overwrites this with the stored value; prefab instantiation
    // deliberately keeps it, which is what makes one prefab inserted twice
    // produce two distinct identities.
    m_registry.emplace<EntityIdComponent>(e, EntityIdComponent{ HE::UUID::generate() });
    auto& rootHierarchy = m_registry.get<HierarchyComponent>(m_rootEntity);
    rootHierarchy.children.push_back(e);
    m_registry.get<HierarchyComponent>(e).parent = m_rootEntity;
    m_hierarchyDirty = true;
    return e;
}

HE::UUID HorizonWorld::entityId(Entity entity) const
{
    if (!m_registry.valid(entity)) return {};
    if (const auto* c = m_registry.try_get<EntityIdComponent>(entity)) return c->id;
    return {};
}

Entity HorizonWorld::findByEntityId(const HE::UUID& id) const
{
    // A zero id is the "no identity" sentinel, never a real one — matching it
    // would return an arbitrary entity that merely lacks the component.
    if (id == HE::UUID{}) return entt::null;
    for (auto [e, c] : m_registry.view<const EntityIdComponent>().each())
        if (c.id == id) return e;
    return entt::null;
}

void HorizonWorld::setEntityId(Entity entity, const HE::UUID& id)
{
    if (!m_registry.valid(entity)) return;
    m_registry.emplace_or_replace<EntityIdComponent>(entity, EntityIdComponent{ id });
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
    HE_LOG_INFO(World, "Clearing world (%zu entity/-ies)",
                static_cast<size_t>(m_registry.storage<entt::entity>().in_use()));

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
    {
        HE_LOG_WARN(World, "Reparent of entity %u under %u rejected: would create a cycle",
                    static_cast<uint32_t>(entity), static_cast<uint32_t>(newParent));
        return false;
    }

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
    if (json.empty()) return;
    if (!HorizonCode::fromJson(json, m_levelScript))
        // A level script that fails to parse leaves an empty graph, and the level
        // then simply does nothing — previously with no explanation anywhere.
        HE_LOG_ERROR(HorizonCode, "Level script could not be parsed (%zu bytes of JSON) — "
                                  "the level will run without it", json.size());
    else
        HE_LOG_DEBUG(HorizonCode, "Level script loaded: %zu node(s)", m_levelScript.nodes.size());
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
    // A level script's class key is its "level:<uuid>" host key, not a content
    // path — it lives inside the .hescene, not in an asset of its own. It stays
    // a plain Object: a level is not a scene entity and owns none.
    const HorizonCode::ClassIdentity levelCls{ m_levelScriptKey, "Object" };
    if (!m_levelScriptKey.empty())
        if (auto compiled = HorizonCode::compiledClasses().create(m_levelScriptKey))
        {
            m_levelInstance = scripts().addCompiled(std::move(compiled), {}, levelCls);
            HE_LOG_INFO(HorizonCode, "Level script '%s' running (compiled native class)",
                        m_levelScriptKey.c_str());
            scripts().fireOnLevelLoaded(m_levelInstance);
            return;
        }
    m_levelInstance = scripts().add(m_levelScript, {}, levelCls);
    HE_LOG_INFO(HorizonCode, "Level script running (interpreted, %zu node(s)) — OnLevelLoaded",
                m_levelScript.nodes.size());
    scripts().fireOnLevelLoaded(m_levelInstance);
}

void HorizonWorld::fireLevelUnloaded()
{
    if (!m_levelRunning) return; // only fire for a level that actually loaded
    m_levelRunning = false;

    // Fire the event but KEEP the instance so its final variable state stays
    // readable after unload; it is dropped on the next load or on clear().
    HE_LOG_INFO(HorizonCode, "%s", "Level script OnLevelUnloaded");
    scripts().fireOnLevelUnloaded(m_levelInstance);
}

