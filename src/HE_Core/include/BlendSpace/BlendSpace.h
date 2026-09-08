// A blend space: N animation clips placed in a 1D or 2D parameter space, mixed
// by where the parameters currently stand. Speed → walk/jog/run on one axis;
// direction × speed → a whole strafe set on two.
//
// Lives in HE_Core (like HE::BoneMask and HE::AnimatorStateMachineGraph) rather
// than next to the animation systems (HE_Scene), because ContentManager
// (HE_Core) needs a type it is allowed to hold: HE_Core cannot depend on
// HE_Scene.
//
// ── Why a flat sample list and not a node graph ──────────────────────────────
// AnimatorStateMachineGraph.h says it outright: the animation half of the editor
// deliberately does NOT use the GraphEditor node model, the panel edits these
// vectors directly. A blend space as a node graph would turn that decision
// around in the same panel. And a blend space is not a graph in the first place:
// it is a point cloud in a parameter space, whose natural editor is a diagram
// with draggable points, not a mesh of nodes and edges.
//
// ── Why ONE struct for 1D and 2D ────────────────────────────────────────────
// 1D is the 2D case that ignores y. Two structs would be two samplers, two
// serializers and two editors for one idea.
#pragma once

#include <Types/Defines.h> // HE_API
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <vector>

namespace HE
{

enum class BlendSpaceKind : uint8_t
{
    OneD = 0, // only `x` and `paramX` are read
    TwoD = 1,
};

// A saved kind is a raw int, and a file from a newer editor — or a hand-edit —
// can name a value this build does not have. Casting it blind makes an enum with
// no enumerator, and the branch that reads it falls through to whichever arm
// happens to be last. Same guard, same reason, as HE::transitionOpFromInt.
constexpr BlendSpaceKind blendSpaceKindFromInt(int v)
{
    switch (v)
    {
        case (int)BlendSpaceKind::OneD: return BlendSpaceKind::OneD;
        case (int)BlendSpaceKind::TwoD: return BlendSpaceKind::TwoD;
        default:                        return BlendSpaceKind::OneD;
    }
}

struct BlendSpaceSample
{
    HE::UUID clipId;
    float    x = 0.0f;
    float    y = 0.0f;
    // A per-sample time scale for clips whose author got the tempo wrong. It does
    // NOT scale the sample's own playhead directly — every sample of a blend
    // space runs on one shared PHASE — it scales that sample's contribution to
    // the weighted duration, which is the same thing expressed once for all of
    // them (see blendSpaceWeightedDuration).
    float    speedScale = 1.0f;
};

struct HE_API BlendSpace
{
    std::string    name; // display only, for the inspector
    BlendSpaceKind kind = BlendSpaceKind::OneD;

    // Parameter names, looked up in whatever param map the caller has (the state
    // machine's `params`). Empty = the axis reads 0.
    std::string paramX, paramY;

    // Editor axis ranges only. The sampler never clamps to these — it clamps to
    // the outermost SAMPLE, which is the thing that actually exists.
    float minX = 0.0f, maxX = 1.0f, minY = 0.0f, maxY = 1.0f;

    std::vector<BlendSpaceSample> samples;

    // Whether the shared phase wraps at 1 or stops there.
    bool looping = true;
};

HE_API std::string blendSpaceToJson(const BlendSpace& s);
HE_API bool        blendSpaceFromJson(const std::string& json, BlendSpace& out);

} // namespace HE
