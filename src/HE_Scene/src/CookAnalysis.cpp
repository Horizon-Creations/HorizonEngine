#include "HorizonScene/CookAnalysis.h"
#include "HorizonScene/Components/AnimationLayerComponent.h"
#include "HorizonScene/Components/AnimatorBlendComponent.h"
#include "HorizonScene/Components/AnimatorComponent.h"
#include "HorizonScene/Components/AnimatorStateMachineComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
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
#include "HorizonScene/Components/ScriptComponent.h"
#include "HorizonScene/Components/SequencePlayerComponent.h"
#include "HorizonScene/Components/TrailComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include <Diagnostics/Log.h>

#include <iterator>
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
		    reg.all_of<MovementComponent>(e) || reg.all_of<ParticleSystemComponent>(e) ||
			    reg.all_of<CameraRigComponent>(e))   // CameraRigController writes the camera entity's own transform every frame
			b |= bit(CookBlocker::Locomotion);
		// The component alone is not the switch: NetworkComponent::replicates is,
		// and declared variables are what a ReplicatedVarsComponent adds.
		if (const auto* n = reg.try_get<NetworkComponent>(e); n && n->replicates)
			b |= bit(CookBlocker::Replication);
		if (const auto* v = reg.try_get<ReplicatedVarsComponent>(e); v && !v->entries.empty())
			b |= bit(CookBlocker::Replication);
		// LODSystem::update swaps the mesh id by distance among the levels, which the cooked row holds
		// one by one (CookedStatic::lodFirst), so several levels no longer block. A refined mesh does:
		// TerrainSystem hooks it in and out at runtime (terrain tessellation), nothing a row can hold.
		if (const auto* lod = reg.try_get<LODComponent>(e); lod && lod->refinedMeshId != HE::UUID{})
			b |= bit(CookBlocker::Lod);
		if (reg.all_of<TrailComponent>(e) || reg.all_of<RopeComponent>(e)) b |= bit(CookBlocker::Geometry);
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

	// Geometry (trail, rope) rebuilds the entity's OWN render content and never writes its
	// TransformComponent (RopeTrailSystem), so it says nothing about a child's world matrix.
	constexpr uint16_t kAncestorIgnored = bit(CookBlocker::Geometry);

	bool lineBlocked(const entt::registry& reg, entt::entity e, AncestorMemo* memo)
	{
		if (e == entt::null || !reg.valid(e)) return false;
		if (memo)
			if (const auto it = memo->find(e); it != memo->end()) return it->second;
		bool blocked = (ownBlockers(reg, e) & ~kAncestorIgnored) != 0;
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

namespace
{
	// Every CookBlocker bit that exists today, in rule order (bits 6 and 10 are the
	// retired rules 7 and 11, CookAnalysis.h:20-28, and stay out of this list).
	constexpr CookBlocker kAllBlockers[] = {
		CookBlocker::Script, CookBlocker::Animation, CookBlocker::PropertyPlayback,
		CookBlocker::Physics, CookBlocker::Locomotion, CookBlocker::Replication,
		CookBlocker::Lod, CookBlocker::Geometry, CookBlocker::Ancestor,
		CookBlocker::Switchable, CookBlocker::MarkedMovable,
	};

	const char* blockerLabel(CookBlocker b)
	{
		switch (b)
		{
		case CookBlocker::Script:           return "Script";
		case CookBlocker::Animation:        return "Animation";
		case CookBlocker::PropertyPlayback: return "Property playback";
		case CookBlocker::Physics:          return "Physics";
		case CookBlocker::Locomotion:       return "Locomotion";
		case CookBlocker::Replication:      return "Replication";
		case CookBlocker::Lod:              return "LOD";
		case CookBlocker::Geometry:         return "Geometry";
		case CookBlocker::Ancestor:         return "Parent blocked";
		case CookBlocker::Switchable:       return "Switchable";
		case CookBlocker::MarkedMovable:    return "Movable";
		}
		return "?";
	}
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

CookSummary summarizeCookability(const std::vector<CookEntry>& entries, const entt::registry& reg, size_t maxExamples)
{
	CookSummary::Reason slots[std::size(kAllBlockers)];
	for (size_t i = 0; i < std::size(kAllBlockers); ++i)
		slots[i].blocker = kAllBlockers[i];

	CookSummary out;
	for (const CookEntry& ce : entries)
	{
		if (ce.verdict.cookable()) { ++out.cookable; continue; }
		++out.blocked;
		for (size_t i = 0; i < std::size(kAllBlockers); ++i)
		{
			if (!ce.verdict.has(kAllBlockers[i])) continue;
			++slots[i].count;
			if (slots[i].examples.size() < maxExamples)
				if (const auto* name = reg.try_get<NameComponent>(ce.entity); name && !name->name.empty())
					slots[i].examples.push_back(name->name);
		}
	}
	out.reasons.reserve(std::size(kAllBlockers));
	for (CookSummary::Reason& r : slots)
		if (r.count) out.reasons.push_back(std::move(r));
	return out;
}

std::string formatCookSummary(const CookSummary& summary)
{
	std::string line = std::to_string(summary.cookable) + " cookable, "
	                  + std::to_string(summary.blocked) + " not";
	if (summary.reasons.empty()) return line;
	line += " (";
	for (size_t i = 0; i < summary.reasons.size(); ++i)
	{
		const CookSummary::Reason& r = summary.reasons[i];
		if (i) line += ", ";
		line += std::string(blockerLabel(r.blocker)) + ": " + std::to_string(r.count);
		if (!r.examples.empty())
		{
			line += " [";
			for (size_t j = 0; j < r.examples.size(); ++j)
			{
				if (j) line += ", ";
				line += r.examples[j];
			}
			line += "]";
		}
	}
	line += ")";
	// A blocked entity can carry several bits at once (e.g. Script AND Physics), so
	// the reasons do not partition `blocked` and their counts can sum past it.
	line += " — reasons can overlap";
	return line;
}

}
