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
    registry_.destroy(entity);
    m_hierarchyDirty = true;
}

void HorizonWorld::renameEntity(Entity entity, const std::string& newName)
{
    if (auto* n = registry_.try_get<NameComponent>(entity))
    {
        n->name = newName;
        m_hierarchyDirty = true;
    }
}

