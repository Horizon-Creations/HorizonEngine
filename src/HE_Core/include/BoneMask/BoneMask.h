// A bone mask: which joints of a skeleton an animation layer is allowed to
// touch, and how strongly. Lives in HE_Core (like HE::AnimatorStateMachineGraph)
// rather than next to AnimationLayerComponent (HE_Scene), because ContentManager
// (HE_Core) needs a type it is allowed to hold: HE_Core cannot depend on HE_Scene.
//
// An asset and not a field on the layer, because "UpperBody" is used by the aim
// layer, the hit-reaction layer and the gesture layer of every humanoid in the
// project. The same name list copied onto three layers of forty characters is
// exactly what moving the state machine from an inline component field to an
// asset got rid of.
//
// Joints are named, not indexed: a re-import can change a skeleton's joint order,
// and a mask that then means the fingers instead of the shoulder is a bug nobody
// would look for. Resolution to indices happens per (mask, skeleton) pair and is
// cached on the component — see HE::resolveBoneMask in HorizonScene/AnimationPose.h.
#pragma once

#include <Types/Defines.h> // HE_API
#include <string>
#include <vector>

namespace HE
{

struct BoneMaskEntry
{
    std::string joint;          // joint name as it appears in the skeleton
    float       weight = 1.0f;  // 0..1, multiplied onto the layer's own weight
};

// Deliberately a flat list of explicit joint names, with no "and its children"
// flag. A subtree flag would have to be resolved against a hierarchy the ASSET
// does not know (it holds names, not a skeleton), and two masks authored against
// two skeletons would then mean different things under the same name. The editor
// panel expands a subtree into explicit names at authoring time instead, which is
// the same convenience with none of the ambiguity.
struct HE_API BoneMask
{
    std::string                name;     // display only, for the inspector
    std::vector<BoneMaskEntry> entries;
};

HE_API std::string boneMaskToJson(const BoneMask& m);
HE_API bool        boneMaskFromJson(const std::string& json, BoneMask& out);

} // namespace HE
