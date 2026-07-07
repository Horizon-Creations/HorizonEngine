#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/Components/EnvironmentComponent.h"
#include "HorizonScene/Components/WeatherComponent.h"
#include "HorizonScene/Components/EnvironmentLightComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <algorithm>

HorizonWorld::HorizonWorld()
{
    rootEntity_ = registry_.create();
    registry_.emplace<NameComponent>(rootEntity_, NameComponent{ "World" });
    registry_.emplace<HierarchyComponent>(rootEntity_);
    // Scene-wide environment / sky settings live on the root ("World") entity so
    // they serialize with the scene and are edited in its Details panel. Weather is a
    // built-in part of the World too (drives the sky's clouds/fog/wind + precipitation).
    registry_.emplace<EnvironmentComponent>(rootEntity_);
    registry_.emplace<WeatherComponent>(rootEntity_);
    ensureEnvironmentLights();
    // Widgets and the level script share this world's central HorizonCode
    // interpreter (rather than each running its own).
    m_widgets.setRuntime(&scripts());
}

bool HorizonWorld::isBuiltin(Entity entity) const
{
    return entity == rootEntity_ || registry_.all_of<EnvironmentLightComponent>(entity);
}

void HorizonWorld::ensureEnvironmentLights()
{
    auto ensure = [&](EnvironmentLightComponent::Role role, const char* name)
    {
        // Find an existing light with this role (e.g. recreated after clear()).
        Entity e = entt::null;
        for (auto [ent, elc] : registry_.view<EnvironmentLightComponent>().each())
            if (elc.role == role) { e = ent; break; }

        if (e == entt::null)
        {
            e = createEntity(name); // Name + Hierarchy, parented to the root
            // A (default) transform so the render extractor's
            // <TransformComponent, LightComponent> view picks the light up; the
            // direction itself is driven by the environment, not this transform.
            registry_.emplace<TransformComponent>(e, TransformComponent{});
            LightComponent lc;
            lc.type = HE::LightType::Directional;
            registry_.emplace<LightComponent>(e, lc);
            registry_.emplace<EnvironmentLightComponent>(e, EnvironmentLightComponent{ role });
        }
        else
        {
            // Make sure it is still attached to the root (scene load rebuilds
            // root.children from the serialised list, which omits these
            // never-serialised lights).
            auto& h = registry_.get<HierarchyComponent>(e);
            h.parent = rootEntity_;
            auto& rh = registry_.get<HierarchyComponent>(rootEntity_);
            if (std::find(rh.children.begin(), rh.children.end(), e) == rh.children.end())
                rh.children.push_back(e);
        }
    };
    ensure(EnvironmentLightComponent::Role::Sun,  "Sun");
    ensure(EnvironmentLightComponent::Role::Moon, "Moon");
    m_hierarchyDirty = true;
}

Entity HorizonWorld::createEntity(const std::string& name)
{
    Entity e = registry_.create();
    registry_.emplace<NameComponent>(e, NameComponent{ name });
    registry_.emplace<HierarchyComponent>(e);
    auto& rootHierarchy = registry_.get<HierarchyComponent>(rootEntity_);
    rootHierarchy.children.push_back(e);
    registry_.get<HierarchyComponent>(e).parent = rootEntity_;
    m_hierarchyDirty = true;
    return e;
}

void HorizonWorld::destroyEntity(Entity entity)
{
    if (!registry_.valid(entity) || isBuiltin(entity))
        return; // root + the environment sun/moon lights are not deletable

    // Detach from parent first so the recursion below never walks back up
    auto* h = registry_.try_get<HierarchyComponent>(entity);
    if (h && h->parent != entt::null)
    {
        auto* ph = registry_.try_get<HierarchyComponent>(h->parent);
        if (ph)
        {
            auto& ch = ph->children;
            ch.erase(std::remove(ch.begin(), ch.end(), entity), ch.end());
        }
    }

    // Destroy the subtree bottom-up (children vector is copied — destroying
    // mutates the registry under us)
    if (h)
    {
        const std::vector<Entity> children = h->children;
        for (Entity child : children)
        {
            if (auto* chh = registry_.try_get<HierarchyComponent>(child))
                chh->parent = entt::null; // already detached via this loop
            destroyEntity(child);
        }
    }

    registry_.destroy(entity);
    m_hierarchyDirty = true;
}

void HorizonWorld::clear()
{
    // A running level ends here (PIE stop / scene switch / shutdown all route
    // through clear()). No-op unless it was actually running, so the edit-time
    // clear() at the start of openScene doesn't spuriously fire OnLevelUnloaded.
    fireLevelUnloaded();

    // Live UI widgets track the world's lifetime (PIE stop / scene load).
    m_widgets.clear();

    // Root children first (handles whole subtrees), then any strays that
    // were never parented into the hierarchy.
    if (auto* rh = registry_.try_get<HierarchyComponent>(rootEntity_))
    {
        const std::vector<Entity> children = rh->children;
        for (Entity c : children)
            destroyEntity(c);
    }
    std::vector<Entity> strays;
    for (auto e : registry_.view<entt::entity>())
        if (e != rootEntity_ && !isBuiltin(e)) // keep the built-in sun/moon lights
            strays.push_back(e);
    for (Entity e : strays)
        if (registry_.valid(e))
            registry_.destroy(e);
    // Reset the root's environment + weather to defaults so New Scene / loading a scene
    // without those blocks starts from a clean sky (a loaded scene that has them
    // overwrites this via the serializer's emplace_or_replace).
    registry_.emplace_or_replace<EnvironmentComponent>(rootEntity_);
    registry_.emplace_or_replace<WeatherComponent>(rootEntity_);
    ensureEnvironmentLights(); // re-attach (or recreate) the built-in sun/moon

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
    while (cur != entt::null && registry_.valid(cur))
    {
        if (cur == ancestor)
            return true;
        const auto* h = registry_.try_get<HierarchyComponent>(cur);
        cur = h ? h->parent : entt::null;
    }
    return false;
}

bool HorizonWorld::reparentEntity(Entity entity, Entity newParent)
{
    if (entity == rootEntity_ || entity == newParent)
        return false;
    // A built-in (the environment sun/moon) can't be reparented, and nothing may be
    // dropped UNDER a built-in — except the World root itself, which is the valid
    // target for un-parenting an entity back to the top level.
    if (isBuiltin(entity) || (isBuiltin(newParent) && newParent != rootEntity_))
        return false;
    if (!registry_.valid(entity) || !registry_.valid(newParent))
        return false;
    if (isAncestorOf(entity, newParent))
        return false; // would create a cycle

    auto* h  = registry_.try_get<HierarchyComponent>(entity);
    auto* nh = registry_.try_get<HierarchyComponent>(newParent);
    if (!h || !nh)
        return false;
    if (h->parent == newParent)
        return true; // already there

    if (h->parent != entt::null)
        if (auto* ph = registry_.try_get<HierarchyComponent>(h->parent))
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
    if (auto* n = registry_.try_get<NameComponent>(entity))
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

