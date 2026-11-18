#pragma once
#include <entt/entt.hpp>
#include <cstdint>
#include <vector>

// ── Which entities may be cooked ─────────────────────────────────────────────
// Entity cooking (docs/entity-cooking-survey-2026-11-13.md) reduces an entity
// that never changes at run time to flat render/physics data. This is its first
// half: deciding, once, at load or export (never per frame), which entities
// qualify. Nothing reads the answer yet; the freeze is the next step.
//
// One bit per reason, so a test (and later the editor) can say WHY an entity
// stays a full entity. The numbers in the comments are the rule numbers of
// survey §5. An entity is cookable when it has no bit set. The rules are a hard
// veto: MobilityComponent::Mode::Movable adds a bit of its own, and
// MobilityComponent::Mode::Static does not lift any of the others.
//
// Not decided here, on purpose:
//  * Targets of a joint or rope (survey §6): needs the reverse index, built
//    together with the freeze.
//  * Cell membership (survey rule 11, dropped): a cell's entities are created and destroyed
//    with it, and every create or destroy moves HorizonWorld::structureEpoch, which rebuilds
//    the cooked block from the live registry (entt handles do not survive an unload and a
//    load, the block never keeps one past a rebuild). Nothing is left for a verdict to say.
//  * SaveStateComponent (survey rule 7, dropped): applySavedState only writes the transform (the
//    byte compare in propagateTransforms un-cooks the row), visibility (the extract reads it live)
//    and script variables (an entity with those carries a ScriptComponent, rule 1).
// The bit numbers 6 (rule 7) and 10 (rule 11) stay unused: no renumbering.
namespace HE
{
	enum class CookBlocker : uint16_t
	{
		Script          = 1 << 0,   // 1  ScriptComponent
		Animation       = 1 << 1,   // 2  Animator, layers, blend, state machine, root motion, IK
		PropertyPlayback= 1 << 2,   // 3  PropertyAnimator, SequencePlayer
		Physics         = 1 << 3,   // 4  RigidBody that is not Static
		Locomotion      = 1 << 4,   // 5  CharacterController, NavAgent, Movement, ParticleSystem, CameraRig
		Replication     = 1 << 5,   // 6  NetworkComponent::replicates, declared replicated vars
		Lod             = 1 << 7,   // 8  LOD with a refined mesh (several levels cook: CookedStatic holds each)
		Geometry        = 1 << 8,   // 9  Trail, Rope (the entity itself; an ancestor's Geometry does not pass down)
		Ancestor        = 1 << 9,   // 10 an ancestor has a blocker of its own
		Switchable      = 1 << 11,  // 12 InactiveComponent, or a hidden mesh
		MarkedMovable   = 1 << 12,  // the author's MobilityComponent::Mode::Movable
	};

	struct CookVerdict
	{
		uint16_t blockers = 0;   // CookBlocker bits

		bool cookable() const { return blockers == 0; }
		bool has(CookBlocker b) const { return (blockers & static_cast<uint16_t>(b)) != 0; }
	};

	// The verdict for one entity: its own blockers plus, for an ancestor with
	// any, CookBlocker::Ancestor. Walks the parent chain, so it costs the depth.
	CookVerdict analyzeCookability(const entt::registry& reg, entt::entity e);

	struct CookEntry
	{
		entt::entity entity;
		CookVerdict  verdict;
	};

	// The verdict of every entity with a TransformComponent. Entities the author
	// marked Static that nevertheless carry a blocker are logged as a warning
	// (once per entity, here, at analysis time) and stay uncooked.
	std::vector<CookEntry> analyzeCookability(const entt::registry& reg);
}
