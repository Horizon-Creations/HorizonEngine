#pragma once
#include <entt/entt.hpp>
#include <string>
#include "Components/NameComponent.h"
#include "Components/HierarchyComponent.h"

using Entity = entt::entity;

class HorizonWorld {
public:
	HorizonWorld();
	~HorizonWorld() = default;

	Entity rootEntity() const { return rootEntity_; }

	Entity createEntity(const std::string& name = "Entity");
	void   destroyEntity(Entity entity);
	void   renameEntity(Entity entity, const std::string& newName);

	bool isHierarchyDirty()  const { return m_hierarchyDirty; }
	void clearHierarchyDirty()     { m_hierarchyDirty = false; }
	void markHierarchyDirty()      { m_hierarchyDirty = true;  }

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

private:
	entt::registry registry_;
	Entity         rootEntity_      = entt::null;
	bool           m_hierarchyDirty = true;
};
