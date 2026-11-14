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
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/MobilityComponent.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/NameComponent.h"
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
#include "HorizonScene/Components/StreamingComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>

#include <unordered_map>

namespace HE
{

namespace
{
	constexpr uint16_t bit(CookBlocker b) { return static_cast<uint16_t>(b); }

	// What the entity itself carries, without its ancestors (rule 10 is the walk
	// in analyze below). Rules in survey §5 order; each is a reason a system, the
	// script API or a savegame can write to the entity after load.
	uint16_t ownBlockers(const entt::registry& reg, entt::entity e)
	{
		uint16_t b = 0;
		if (reg.all_of<ScriptComponent>(e)) b |= bit(CookBlocker::Script);
		if (reg.all_of<AnimatorComponent>(e) || reg.all_of<AnimationLayerComponent>(e) ||
		    reg.all_of<AnimatorBlendComponent>(e) || reg.all_of<AnimatorStateMachineComponent>(e) ||
		    reg.all_of<RootMotionComponent>(e) || reg.all_of<IkComponent>(e))
			b |= bit(CookBlocker::Animation);
		if (reg.all_of<PropertyAnimatorComponent>(e) || reg.all_of<SequencePlayerComponent>(e))
			b |= bit(CookBlocker::PropertyPlayback);
		if (const auto* rb = reg.try_get<RigidBodyComponent>(e); rb && rb->type != RigidBodyType::Static)
			b |= bit(CookBlocker::Physics);
		if (reg.all_of<CharacterControllerComponent>(e) || reg.all_of<NavAgentComponent>(e) ||
		    reg.all_of<MovementComponent>(e) || reg.all_of<ParticleSystemComponent>(e))
			b |= bit(CookBlocker::Locomotion);
		// The component alone is not the switch: NetworkComponent::replicates is,
		// and declared variables are what a ReplicatedVarsComponent adds.
		if (const auto* n = reg.try_get<NetworkComponent>(e); n && n->replicates)
			b |= bit(CookBlocker::Replication);
		if (const auto* v = reg.try_get<ReplicatedVarsComponent>(e); v && !v->entries.empty())
			b |= bit(CookBlocker::Replication);
		if (reg.all_of<SaveStateComponent>(e)) b |= bit(CookBlocker::SaveState);
		// LODSystem::update rewrites the mesh id every frame for more than one
		// level, and for a refined mesh even with one level (terrain tessellation).
		if (const auto* lod = reg.try_get<LODComponent>(e);
		    lod && (lod->levels.size() > 1 || lod->refinedMeshId != HE::UUID{}))
			b |= bit(CookBlocker::Lod);
		if (reg.all_of<TrailComponent>(e) || reg.all_of<RopeComponent>(e)) b |= bit(CookBlocker::Geometry);
		if (const auto* s = reg.try_get<StreamingComponent>(e); s && s->mode == StreamingComponent::Mode::Auto)
			b |= bit(CookBlocker::Streaming);
		if (reg.all_of<InactiveComponent>(e)) b |= bit(CookBlocker::Switchable);
		if (const auto* m = reg.try_get<MeshComponent>(e); m && !m->visible)
			b |= bit(CookBlocker::Switchable);
		if (const auto* mob = reg.try_get<MobilityComponent>(e); mob && mob->mode == MobilityComponent::Mode::Movable)
			b |= bit(CookBlocker::MarkedMovable);
		return b;
	}

	// An ancestor is "blocked" when it has a blocker of its own or one of ITS
	// ancestors does: a cooked child under it would hold a world matrix that goes
	// stale the moment the parent moves. `memo` is the whole-world pass's cache,
	// so a group of a hundred children is looked at once; null for a single query.
	using AncestorMemo = std::unordered_map<entt::entity, bool>;

	bool lineBlocked(const entt::registry& reg, entt::entity e, AncestorMemo* memo)
	{
		if (e == entt::null || !reg.valid(e)) return false;
		if (memo)
			if (const auto it = memo->find(e); it != memo->end()) return it->second;
		bool blocked = ownBlockers(reg, e) != 0;
		if (!blocked)
		{
			const auto* h = reg.try_get<HierarchyComponent>(e);
			blocked = h && lineBlocked(reg, h->parent, memo);
		}
		if (memo) (*memo)[e] = blocked;
		return blocked;
	}

	CookVerdict analyze(const entt::registry& reg, entt::entity e, AncestorMemo* memo)
	{
		CookVerdict v;
		v.blockers = ownBlockers(reg, e);
		if (const auto* h = reg.try_get<HierarchyComponent>(e); h && lineBlocked(reg, h->parent, memo))
			v.blockers |= bit(CookBlocker::Ancestor);
		return v;
	}
}

CookVerdict analyzeCookability(const entt::registry& reg, entt::entity e)
{
	return analyze(reg, e, nullptr);
}

std::vector<CookEntry> analyzeCookability(const entt::registry& reg)
{
	const auto view = reg.view<const TransformComponent>();
	std::vector<CookEntry> out;
	out.reserve(view.size());
	AncestorMemo memo;
	for (const entt::entity e : view)
	{
		const CookVerdict v = analyze(reg, e, &memo);
		out.push_back({ e, v });
		// "Static" is the author's claim, and the rules are the veto: report the
		// contradiction instead of freezing an entity a system will write to.
		// MarkedMovable cannot be set here (the mode is Static), so any bit is a
		// rule that the claim runs into.
		const auto* mob = reg.try_get<MobilityComponent>(e);
		if (mob && mob->mode == MobilityComponent::Mode::Static && !v.cookable())
		{
			const auto* name = reg.try_get<NameComponent>(e);
			HE_LOG_WARN(Scene, "Entity '%s' is marked Static but cannot be cooked (blockers 0x%x); it stays a normal entity",
			            name ? name->name.c_str() : "?", static_cast<unsigned>(v.blockers));
		}
	}
	return out;
}

}
