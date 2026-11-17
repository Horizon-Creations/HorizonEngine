#include "HorizonScene/CookedStatic.h"
#include "HorizonScene/CookAnalysis.h"
#include "HorizonScene/Components/AnimationLayerComponent.h"
#include "HorizonScene/Components/AnimatorBlendComponent.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/AnimatorStateMachineComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/InactiveComponent.h"
#include "HorizonScene/Components/IkComponent.h"
#include "HorizonScene/Components/LODComponent.h"
#include "HorizonScene/Components/MaterialComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MobilityComponent.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/NavAgentComponent.h"
#include "HorizonScene/Components/NetworkComponent.h"
#include "HorizonScene/Components/ParticleSystemComponent.h"
#include "HorizonScene/Components/PropertyAnimatorComponent.h"
#include "HorizonScene/Components/ReplicatedVarsComponent.h"
#include "HorizonScene/Components/RigidBodyComponent.h"
#include "HorizonScene/Components/RootMotionComponent.h"
#include "HorizonScene/Components/RopeComponent.h"
#include "HorizonScene/Components/SaveStateComponent.h"
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/SequencePlayerComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>
#include <algorithm>

namespace HE
{

void CookedStatic::syncContent(const void* manager, uint64_t epoch)
{
	if (manager == content && epoch == contentEpoch) return;
	content      = manager;
	contentEpoch = epoch;
	std::fill(state.begin(), state.end(), State::Pending);
}

void CookedStatic::onVerdictChanged(entt::registry& reg, entt::entity e)
{
	if (stale) return;   // a rebuild is already due: the rest of this frame's changes ride along
	// The entity is a row, or a parent whose new blocker passes down to its children (rule 10).
	// An entity with neither is not in the block and cannot take anything with it.
	// Before the first build there are no rows to look for, so a packed verdict (CookedStatic::seed) is only as
	// good as nothing having changed: any verdict component coming or going on any entity puts the analysis back.
	if (structureEpoch == 0 || rowOf(e) >= 0) { stale = true; return; }
	if (const auto* h = reg.try_get<HierarchyComponent>(e); h && !h->children.empty()) stale = true;
}

namespace
{
	// The components analyzeCookability reads (CookAnalysis.cpp ownBlockers, rules 1 to 12 and the
	// author's mobility): one of them appearing or going on a row's entity changes the verdict.
	// Keep in step with ownBlockers; test_cooked_static adds each of them to a cooked entity.
	using VerdictComponents = entt::type_list<
		ScriptComponent, AnimatorComponent, AnimationLayerComponent, AnimatorBlendComponent,
		AnimatorStateMachineComponent, RootMotionComponent, IkComponent, PropertyAnimatorComponent,
		SequencePlayerComponent, RigidBodyComponent, CharacterControllerComponent, NavAgentComponent,
		MovementComponent, ParticleSystemComponent, NetworkComponent, ReplicatedVarsComponent,
		SaveStateComponent, LODComponent, TrailComponent, RopeComponent,
		InactiveComponent, MobilityComponent>;
	// What a Ready row copies besides the world matrix and the ids it already holds.
	using OutputComponents = entt::type_list<MeshComponent, MaterialComponent>;

	template<auto Handler, typename... T>
	void watch(entt::registry& reg, CookedStatic& c, entt::type_list<T...>)
	{
		(reg.on_construct<T>().template connect<Handler>(c), ...);
		(reg.on_update<T>().template connect<Handler>(c), ...);
		(reg.on_destroy<T>().template connect<Handler>(c), ...);
	}
}

CookedStatic& cookedStaticOf(entt::registry& reg)
{
	if (CookedStatic* c = reg.ctx().find<CookedStatic>()) return *c;
	CookedStatic& c = reg.ctx().emplace<CookedStatic>();
	// Not scoped connections: the block lives exactly as long as the registry (see TransformHierarchy's State).
	watch<&CookedStatic::onVerdictChanged>(reg, c, VerdictComponents{});
	watch<&CookedStatic::onOutputChanged>(reg, c, OutputComponents{});
	return c;
}

void uncookEntity(entt::registry& reg, entt::entity e)
{
	if (CookedStatic* c = reg.ctx().find<CookedStatic>()) c->uncook(e);
}

void refreshCookedStatic(entt::registry& reg, uint64_t structureEpoch)
{
	CookedStatic& c = cookedStaticOf(reg);
	if (c.structureEpoch == structureEpoch && !c.stale) return;
	const bool seeded = !c.stale && !c.seed.empty() && c.seedEpoch == structureEpoch;
	c.structureEpoch = structureEpoch;
	c.stale          = false;

	const size_t slots = reg.storage<entt::entity>().size();
	std::vector<uint8_t> cookable(slots, 0);
	if (seeded)
	{
		for (const entt::entity e : c.seed) cookable[static_cast<size_t>(entt::to_entity(e))] = 1;
		++c.seededBuilds;
		HE_LOG_INFO(Scene, "Cooked block: %zu cookable entity/-ies taken from the packed scene, analysis skipped", c.seed.size());
	}
	else
	{
		for (const CookEntry& ce : analyzeCookability(reg))
			if (ce.verdict.cookable()) cookable[static_cast<size_t>(entt::to_entity(ce.entity))] = 1;
		++c.analysedBuilds;
	}
	// One shot: whatever the seed said holds for this epoch only, and the next rebuild is a new question.
	std::vector<entt::entity>().swap(c.seed);

	c.rowOfIndex.assign(slots, -1);
	c.entity.clear();

	const auto meshes = reg.view<TransformComponent, MeshComponent>();
	c.entity.reserve(meshes.size_hint());
	for (const entt::entity e : meshes)
	{
		const size_t i = static_cast<size_t>(entt::to_entity(e));
		if (!cookable[i]) continue;
		c.rowOfIndex[i] = static_cast<int32_t>(c.entity.size());
		c.entity.push_back(e);
	}
	const size_t rows = c.entity.size();
	// Rows are filled when the walk settles them; sized here so the extract never allocates.
	c.state.assign(rows, CookedStatic::State::Pending);
	c.world.assign(rows, glm::mat4(1.0f));
	c.worldBounds.assign(rows, HE::AABB{});
	c.meshId.assign(rows, HE::UUID{});
	c.materialId.assign(rows, HE::UUID{});
	c.lod.assign(rows, 0);
	c.castsShadow.assign(rows, 1);
	c.receivesShadow.assign(rows, 1);
	c.lodFirst.assign(rows, -1);
	c.lodCount.assign(rows, 0);
	c.levelMeshId.clear();
	c.levelMaterialId.clear();
	c.levelBounds.clear();
}

void seedCookedStatic(entt::registry& reg, std::vector<entt::entity>&& cookable, uint64_t structureEpoch)
{
	CookedStatic& c = cookedStaticOf(reg);
	c.seed           = std::move(cookable);
	c.seedEpoch      = structureEpoch;
	c.structureEpoch = 0;       // never built: the first refresh takes the seed
	c.stale          = false;
	c.enabled        = true;
}

}
