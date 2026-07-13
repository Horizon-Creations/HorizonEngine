// The authored data behind an AnimatorStateMachine asset — states + transitions
// + default param values. Lives in HE_Core (like HE::ParticleGraph) rather than
// alongside AnimatorStateMachineComponent (HE_Scene), because ContentManager
// (HE_Core) needs a type it's allowed to hold: HE_Core cannot depend on HE_Scene.
//
// Deliberately simpler than HE::ParticleGraph/MaterialGraph: there is no
// evaluate() step. A state machine has no per-instance randomization or
// procedural inputs — the parsed graph IS what AnimationStateMachineSystem
// (HE_Scene) consumes directly, node-for-node, not a resolved-config
// indirection. This also means no GraphEditor node-graph model is needed for
// states — the editor just edits this struct's vectors directly (states get
// canvas positions via x/y, same as before this became an asset).
#pragma once

#include <Types/Defines.h> // HE_API
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace HE
{

enum class TransitionOp : uint8_t { Greater = 0, Less = 1, Equal = 2 };

struct AnimationState
{
    int         id = 0; // stable id for the GraphEditor canvas (0 = unassigned)
    std::string name;
    HE::UUID    clipId;
    bool        looping = true;
    float       x = 0.0f, y = 0.0f; // persisted canvas position
};

struct AnimationTransition
{
    std::string  fromState;
    std::string  toState;
    std::string  paramName;
    TransitionOp op        = TransitionOp::Greater;
    float        threshold = 0.5f;
    float        duration  = 0.2f; // crossfade length, seconds
};

struct HE_API AnimatorStateMachineGraph
{
    std::vector<AnimationState>            states;
    std::vector<AnimationTransition>       transitions;
    std::unordered_map<std::string, float> defaultParams;
    // Initial current-state name when an entity first resolves this asset.
    // Empty → the first entry in `states` (if any).
    std::string startState;
};

HE_API std::string animatorStateMachineToJson(const AnimatorStateMachineGraph& g);
// A saved state entirely missing "id" means the WHOLE array predates ids/x/y
// (see AnimatorStateMachineEditorPanel) — the caller decides what to do about
// that (SceneSerializer's old inline-component format already had this same
// migration need and knows how to grid-auto-layout; this parser itself just
// reports whatever it finds, id 0 included).
HE_API bool animatorStateMachineFromJson(const std::string& json, AnimatorStateMachineGraph& out);

} // namespace HE
