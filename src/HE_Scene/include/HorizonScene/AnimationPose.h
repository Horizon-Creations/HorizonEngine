#pragma once
#include <BoneMask/BoneMask.h>
#include <ContentManager/Assets.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <vector>

// ── The pose, and what a layer does to it ────────────────────────────────────
// A pose is one local TRS per joint, before forward kinematics and before the
// inverse bind matrix. The type and the two-way blend used to live in the
// internal AnimationEval.h; they are public for the same reason RootMotion.h is:
// the systems, the editor's preview and the tests all need the same arithmetic
// and none of them share an ECS. The ECS half — playheads, masks resolved per
// entity, notifies — stays internal (PoseFinalize.h).

// Per-joint local transform (before FK + IBM).
struct JointTRS
{
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation    = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // w,x,y,z identity
    glm::vec3 scale       = glm::vec3(1.0f);
};

// Per-joint linear blend between two TRS sets: lerp translation + scale, slerp
// rotation.
//
// out is resized to MAX(a.size(), b.size()), and the shorter side contributes a
// default JointTRS for the joints it does not have. It used to be min(), which
// silently dropped the tail joints of the longer side — invisible while every
// caller happened to pass two equally long vectors, and a layer blended against
// a shorter base is exactly the caller that would not.
//
// alpha == 0 returns `a` and alpha == 1 returns `b` BIT-EXACTLY: glm::slerp goes
// through sin((1-a)θ)/sinθ and is not exact at the ends, and "a layer at weight 0
// changes nothing" has to be true to the bit or it is not a claim at all.
void blendTRS(const std::vector<JointTRS>& a,
              const std::vector<JointTRS>& b,
              float                        alpha,
              std::vector<JointTRS>&       out);

namespace HE {

// How a layer's pose meets the pose beneath it.
enum class LayerBlendMode : uint8_t
{
    // Replace, weighted: the layer's own pose wins where the mask lets it.
    Override = 0,
    // Add the layer's DIFFERENCE against a reference pose. This is the mode an
    // aim offset or a breathing wobble wants: it keeps whatever the base is
    // doing and leans on top of it.
    Additive = 1,
};

// A saved mode is a raw int in the .hescene JSON, and a file from a newer editor
// — or a hand-edit — can name a value this build does not have. Casting it blind
// makes an enum with no enumerator, and the branch that reads it then falls
// through to whichever arm happens to be last. Same guard, same reason, as
// HE::rootMotionLockFromInt.
constexpr LayerBlendMode layerBlendModeFromInt(int v)
{
    switch (v)
    {
        case (int)LayerBlendMode::Override: return LayerBlendMode::Override;
        case (int)LayerBlendMode::Additive: return LayerBlendMode::Additive;
        default:                            return LayerBlendMode::Override;
    }
}

// Resolve a mask's joint NAMES against a concrete skeleton: one weight per joint
// of `mesh`, in joint order. Two rules, and both matter:
//
//  * A joint the mask does not name gets 0. A mask is an allow-list.
//  * A NAME THE SKELETON DOES NOT HAVE gets nothing and is reported (throttled),
//    it does not fall back to 1. A mask that hits nothing must mean "this layer
//    does nothing", never "this layer overwrites the whole character" — that is
//    the difference between a bug you see and a bug you cannot explain.
//
// An EMPTY mask (no entries) is the caller's "no mask at all" and gives 1
// everywhere; callers with no mask asset at all skip the resolve entirely and
// use a null weight vector, which the appliers read the same way.
void resolveBoneMask(const SkeletalMeshAsset& mesh, const BoneMask& mask,
                     std::vector<float>& outWeights);

// Lay one layer's pose on top of `base` and write the result to `out`
// (`out` may alias neither `base` nor `layer`).
//
//   w_eff(j) = weight * (maskWeights ? (*maskWeights)[j] : 1)
//
// Override:  out = blend(base, layer, w_eff), per joint.
// Additive:  out = base + w_eff * (layer - ref), the difference taken in the
//            joint's LOCAL frame — rotation as inverse(ref) * layer, applied on
//            the RIGHT of the base rotation. `ref` must be supplied in this mode
//            (a null ref is treated as the identity pose, which is what a layer
//            authored against the bind pose wants).
//
// rootJoint >= 0 forces the root's TRANSLATION weight to 0 whatever the mask
// says. Root motion has already been extracted from the base pose and locked out
// of it by the time a layer runs; a layer that wrote a root translation back in
// would move the character twice. Rotation and scale of the root are left alone —
// only the translation is the thing root motion took.
void applyLayerPose(const std::vector<JointTRS>& base,
                    const std::vector<JointTRS>& layer,
                    const std::vector<JointTRS>* ref,
                    const std::vector<float>*    maskWeights,
                    float weight, LayerBlendMode mode, int rootJoint,
                    std::vector<JointTRS>& out);

} // namespace HE
