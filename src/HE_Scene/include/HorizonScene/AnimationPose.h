#pragma once
#include <BlendSpace/BlendSpace.h>
#include <BoneMask/BoneMask.h>
#include <ContentManager/Assets.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <cstdint>
#include <deque>
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

// ── Blend spaces ─────────────────────────────────────────────────────────────
// The arithmetic of "N clips, mixed by where the parameters stand". Public for
// the same reason blendTRS is: the state machine, the layer stage, the editor's
// blend-space diagram and the tests all need it and none of them share an ECS.
// Everything here is a pure function of authored data — no assets are resolved,
// no playhead is advanced, no ECS is touched. That half lives in PoseSource.h.

// At most this many samples are actually sampled per blend space per frame; the
// rest are dropped and the survivors renormalised.
//
// A two-clip blend costs two sampleClip passes per character per frame today; a
// 3x3 blend space would be nine. Four is the line under which the cost stays
// predictable, and a fifth sample with meaningful weight means the space is
// packed tighter than anybody can see anyway.
inline constexpr size_t kBlendSpaceMaxActiveSamples = 4;

// One weight per sample of `space` at the parameter point (x, y), summing to 1
// (all zero only when the space has no samples at all).
//
// 1D: sort by x, find the bracketing pair, interpolate linearly, and CLAMP
// outside the sample range — never extrapolate. A character at speed 12 moving
// its legs twice as fast as the fastest authored sample is not a feature.
//
// 2D: the gradient band (Rune Skovbo Johansen; the method behind Unity's
// "Freeform Cartesian"). Thirty lines, no triangulation, any sample placement,
// and exactly 1.0 at a sample point. Delaunay — Unreal's way — would need a
// triangulator this tree does not have plus a degenerate case (collinear
// samples) the gradient band simply does not have.
void blendSpaceWeights(const BlendSpace& space, float x, float y,
                       std::vector<float>& outWeights);

// The seconds one lap of the CURRENT mix takes: Σ w_i · dur_i / speedScale_i.
//
// This is the denominator of the shared phase, and the shared phase is the whole
// point of a blend space. Walk (1.2 s) and run (0.8 s) wrapped against their own
// durations on one absolute clock drift apart within a second, and mid-blend the
// left foot of one pose meets the right foot of the other. On a phase they stay
// in step by construction.
//
// `durations[i]` is sample i's clip duration; 0 for a sample whose clip is
// missing, which then contributes nothing here either. Returns 0 when nothing
// usable is left — the caller's cue to stand the phase still rather than divide.
float blendSpaceWeightedDuration(const BlendSpace& space,
                                 const std::vector<float>& weights,
                                 const std::vector<float>& durations);

// N-way pose mix: iterative normalised pairwise blending, seeded with the
// HEAVIEST pose.
//
//   acc = pose[0]; accW = w[0];                  // sorted by weight, descending
//   acc = blendTRS(acc, pose[i], w[i] / (accW + w[i])); accW += w[i];
//
// A true N-way quaternion mean is not a slerp and is not associative, so the
// result depends on the order the chain is walked in. Descending puts the
// dominant sample at the anchor, where the deviation is smallest. Samples with
// weight 0 are skipped entirely rather than blended with alpha 0, so an inactive
// sample cannot perturb the chain at all.
//
// `poses` and `weights` must be the same length. With exactly two non-zero
// weights that sum to 1, the result is bit-for-bit blendTRS(heavier, lighter, w).
void blendPosesN(const std::vector<std::vector<JointTRS>>& poses,
                 const std::vector<float>&                 weights,
                 std::vector<JointTRS>&                    out);

// The order blendPosesN walks its chain in: sample indices with weight > 0,
// heaviest first, ties broken by the LOWER index so the order — and therefore
// which sample fires its notifies — cannot flicker between two frames.
void blendSpaceEvalOrder(const std::vector<float>& weights, std::vector<size_t>& outOrder);

// One parsed blend space, cached on whatever component referenced it. The asset
// carries JSON; parsing it per frame per character wearing the same locomotion
// set is a cost that would never be found again. Same shape, and the same
// reason, as AnimatorStateMachineComponent::resolvedGraph.
//
// `ok == false` records "this id does not resolve" so the miss is not retried
// (and re-logged) every frame either.
struct BlendSpaceCacheEntry
{
    HE::UUID   id;
    BlendSpace space;
    bool       ok = false;
};

// A deque and NOT a vector, on purpose: a resolved blend space is borrowed by
// pointer (copying one per playhead per frame is the cost the cache exists to
// avoid), and vector::push_back would invalidate the pointer handed out a moment
// earlier. A crossfade whose incoming state uses a second, not-yet-cached blend
// space is exactly that moment. Deque entries do not move.
using BlendSpaceCache = std::deque<BlendSpaceCacheEntry>;

} // namespace HE
