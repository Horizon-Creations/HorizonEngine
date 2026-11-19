#pragma once
#include <cstdint>

// ── Whether an entity may ever change at run time ────────────────────────────
// The author's say on entity cooking (CookAnalysis.h): a Static entity is one
// that never moves, never gets a script, is never reparented — so the runtime
// may reduce it to flat render/physics data. Movable says the opposite and
// keeps it a full entity.
//
// Data, not a tag, because there are two answers and the absence of the
// component is a third: no component means "nobody decided", and the answer is
// derived from what the entity carries (CookAnalysis). That keeps every scene
// written before this component existed loading with exactly that default, and
// a save of an undecided entity byte-for-byte what it was. The component is
// written (key "mobility": "static" | "movable") only when present.
//
// The rules stay the veto: Movable always keeps an entity out of the cooked
// set, and Static on an entity that carries a script, an animator, a dynamic
// body and so on does not cook it either (the analysis reports the
// contradiction). Mode::Static is a name of its own, not RigidBodyType::Static:
// that one says how Jolt treats a body, this one says what the entity is.
//
// Default Movable: an Add Component that has not been thought about must not
// freeze anything.
struct MobilityComponent {
	enum class Mode : uint8_t
	{
		Movable = 0,
		Static  = 1,
	};
	Mode mode = Mode::Movable;
};
