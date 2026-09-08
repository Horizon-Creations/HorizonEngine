#pragma once
// Internal header: "one clip" and "one blend space" said in one voice.
//
// A state has ONE pose source, and every playhead in the state machine does the
// same four things with it — advance, sample, collect notifies, extract root
// motion. Without an adapter the machine grows an `if (blendSpace) … else …` at
// each of those, a dozen in all, and the two arms drift apart within a release.
//
// Not in AnimationEval.h, even though the plan pencilled it in there: resolving
// a blendSpaceId into clips means holding a ContentManager, and that header is
// documented as deliberately not knowing one (the same split that put
// RootMotionApply.h in its own file).
#include <HorizonScene/AnimationPose.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/RootMotion.h>
#include "AnimationEval.h"
#include <entt/entt.hpp>
#include <string>
#include <unordered_map>
#include <vector>

class ContentManager;
struct RootMotionComponent;

namespace HE {

// Look `id` up in `cache`, parsing the asset's JSON on the first miss. Returns
// null when the id is empty, the asset is missing, or its JSON does not parse —
// and remembers that, so a broken reference costs one parse and one log line,
// not one of each per frame.
const BlendSpace* resolveBlendSpace(BlendSpaceCache& cache,
                                    ContentManager& cm, HE::UUID id);

// A resolved pose source: either one clip, or a blend space with its samples
// already weighted against the current parameters.
//
// Built once per playhead per frame by makePoseSource() and thrown away again —
// it holds borrowed pointers into ContentManager and must not outlive the tick
// (the usual rule: the next loadAsset invalidates every asset pointer).
struct PoseSource
{
    // Exactly one of these is set on a usable source.
    const AnimationClipAsset* clip  = nullptr;
    const BlendSpace*         space = nullptr;

    // Blend-space side, all indexed by sample.
    std::vector<const AnimationClipAsset*> clips;
    std::vector<float>                     weights;    // sums to 1 over usable samples
    std::vector<float>                     durations;
    float                                  weightedDuration = 0.0f;
    int                                    heaviest = -1; // fires notifies; -1 = none

    bool valid() const { return clip ? clip->duration > 0.0f : (space && weightedDuration > 0.0f); }

    // The playhead's own unit. A CLIP playhead counts seconds and wraps at the
    // clip's duration; a BLEND SPACE playhead counts phase and wraps at 1. That
    // double meaning is the price of one shared clock across clips of different
    // length, and it is why AnimatorStateMachineComponent::clipTime now means two
    // things depending on the state — see its header.
    float period() const { return clip ? clip->duration : 1.0f; }

    // How much the playhead moves per second of real time at speed 1: one second
    // per second for a clip, one lap per weightedDuration seconds for a blend
    // space. 0 when nothing is usable, which stands the phase still rather than
    // dividing by zero.
    float rate() const
    {
        if (clip)  return 1.0f;
        if (space && weightedDuration > 0.0f) return 1.0f / weightedDuration;
        return 0.0f;
    }

    // Does this playhead wrap? For a clip it is the caller's flag (the state's
    // `looping`, the layer's `looping`). For a blend space it is the SPACE's own,
    // because all its samples share one phase: a locomotion set either cycles or
    // it does not, and that is a property of the set, not of each place it is
    // used.
    bool looping(bool callerLooping) const { return space ? space->looping : callerLooping; }

    // Sample the pose at `tSample` (already wrapped into [0, period())), take
    // root motion out of it over the UNWRAPPED span (tPrev, tEnd], and collect
    // notifies over that same span.
    //
    // The three are one call and not three because a blend space cannot separate
    // them: each sample's root has to be extracted and locked in ITS OWN pose,
    // before the N-way mix, exactly as the two-clip case locks each clip before
    // blendTRS. Split apart, the mix would happen first and no sample would know
    // which clip its root came from any more.
    //
    // `rmc` null = no extraction (outHaveDelta stays false). `notifies` null = no
    // queue; `primed` still travels, see notifyCollectClip.
    void evaluate(entt::entity e, const SkeletalMeshAsset& mesh, bool looping,
                  float tPrev, float tEnd, float tSample,
                  const RootMotionComponent* rmc,
                  bool notifyDominant, bool& primed, NotifyQueue* notifies,
                  std::vector<JointTRS>& outPose,
                  RootMotionDelta& outDelta, bool& outHaveDelta) const;
};

// Resolve a state's / a layer's pose source. A set `blendSpaceId` WINS over
// `clipId`: a state has one source, and the newer field is the one the author
// just picked. `params` may be null — an axis whose parameter is not there reads
// 0, which is the leftmost/bottom sample after the clamp.
PoseSource makePoseSource(ContentManager& cm, BlendSpaceCache& cache,
                          HE::UUID clipId, HE::UUID blendSpaceId,
                          const std::unordered_map<std::string, float>* params);

} // namespace HE
