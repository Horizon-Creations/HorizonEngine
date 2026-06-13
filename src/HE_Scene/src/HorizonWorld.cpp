#include "HorizonScene/HorizonWorld.h"
#include <algorithm>

HorizonWorld::HorizonWorld()
{
    rootEntity_ = registry_.create();
    registry_.emplace<NameComponent>(rootEntity_, NameComponent{ "World" });
    registry_.emplace<HierarchyComponent>(rootEntity_);
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
    if (entity == rootEntity_ || !registry_.valid(entity))
        return;

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
        if (e != rootEntity_)
            strays.push_back(e);
    for (Entity e : strays)
        if (registry_.valid(e))
            registry_.destroy(e);
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

