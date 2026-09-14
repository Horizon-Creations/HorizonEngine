#include "HorizonScene/EntityVisibility.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include "HorizonScene/Components/LightComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/FoliageComponent.h"
#include "HorizonScene/Components/RopeComponent.h"
#include "HorizonScene/Components/TrailComponent.h"

namespace HE
{

// One list for both directions. A component type added to the extractor's
// "skip when !visible" set belongs in both functions below, or the eye would
// show a thing hidden that the picture still draws.
void setEntityVisible(entt::registry& reg, entt::entity e, bool visible)
{
	if (auto* m  = reg.try_get<MeshComponent>(e))           m->visible  = visible;
	if (auto* sm = reg.try_get<SkeletalMeshComponent>(e))   sm->visible = visible;
	if (auto* l  = reg.try_get<LightComponent>(e))          l->visible  = visible;
	if (auto* ps = reg.try_get<ParticleSystemComponent>(e)) ps->visible = visible;
	if (auto* f  = reg.try_get<FoliageComponent>(e))        f->visible  = visible;
	if (auto* r  = reg.try_get<RopeComponent>(e))           r->visible  = visible;
	if (auto* t  = reg.try_get<TrailComponent>(e))          t->visible  = visible;
}

Visibility entityVisibility(const entt::registry& reg, entt::entity e)
{
	bool any = false, shown = false;
	auto note = [&](const auto* c) { if (c) { any = true; shown = shown || c->visible; } };
	note(reg.try_get<MeshComponent>(e));
	note(reg.try_get<SkeletalMeshComponent>(e));
	note(reg.try_get<LightComponent>(e));
	note(reg.try_get<ParticleSystemComponent>(e));
	note(reg.try_get<FoliageComponent>(e));
	note(reg.try_get<RopeComponent>(e));
	note(reg.try_get<TrailComponent>(e));
	return !any ? Visibility::None : shown ? Visibility::Visible : Visibility::Hidden;
}

void setSubtreeVisible(entt::registry& reg, entt::entity e, bool visible)
{
	if (!reg.valid(e)) return;
	setEntityVisible(reg, e, visible);
	if (const auto* h = reg.try_get<HierarchyComponent>(e))
		for (entt::entity child : h->children)
			setSubtreeVisible(reg, child, visible);
}

Visibility subtreeVisibility(const entt::registry& reg, entt::entity e)
{
	if (!reg.valid(e)) return Visibility::None;
	Visibility v = entityVisibility(reg, e);
	if (v == Visibility::Visible) return v;
	if (const auto* h = reg.try_get<HierarchyComponent>(e))
		for (entt::entity child : h->children)
		{
			const Visibility cv = subtreeVisibility(reg, child);
			if (cv == Visibility::Visible) return cv;
			if (cv == Visibility::Hidden)  v = Visibility::Hidden;
		}
	return v;
}

} // namespace HE
