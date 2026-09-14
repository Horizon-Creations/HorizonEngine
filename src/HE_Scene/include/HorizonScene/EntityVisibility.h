#pragma once
#include <entt/entt.hpp>

// ── "Is this entity visible?" for things that have no single answer ──────────
// Visibility is a per-component flag — MeshComponent::visible,
// LightComponent::visible and the rest — read by the render extractor, which
// skips a component whose flag is off. There is no entity-level flag and no
// inheritance down the hierarchy: hiding a parent does nothing to its children.
//
// That is what the runtime wants (zone streaming flips exactly the components
// it loaded), but a person clicking an eye in the Outliner means "this thing,
// and everything under it". These two helpers are that translation, shared by
// the script API (entity.setVisible, scene.setZoneVisible) and the editor so
// the set of components that count as "renderable" is written down once.
namespace HE
{
	// Flip every renderable component the entity itself carries. The caller
	// validates the entity; children are not touched — see subtree below.
	void setEntityVisible(entt::registry& reg, entt::entity e, bool visible);

	// What the flags on the entity's own renderable components add up to.
	enum class Visibility : unsigned char
	{
		None,     // nothing on it that could be drawn
		Hidden,   // it has renderables, and every one of them is off
		Visible,  // at least one renderable is on
	};
	Visibility entityVisibility(const entt::registry& reg, entt::entity e);

	// The same two over the entity AND everything under it (HierarchyComponent
	// children, recursively). The read side answers Visible when anything in
	// the subtree is drawn, Hidden when the subtree has renderables and all are
	// off, None when there is nothing to draw anywhere below — an empty group
	// has no eye to show. `visited`-free on purpose: the hierarchy is a tree,
	// reparentEntity refuses cycles.
	void       setSubtreeVisible(entt::registry& reg, entt::entity e, bool visible);
	Visibility subtreeVisibility(const entt::registry& reg, entt::entity e);
}
