#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "AnimationEval.h"
#include "RootMotionApply.h"
#include "PoseFinalize.h"
#include "NotifyCollect.h"
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

namespace
{
// One-off migration for scenes saved before AnimatorStateMachineComponent
// referenced an AnimatorStateMachineAsset: bake the legacy inline
// states/transitions/params into a real asset — same "asset instead of inline
// fields" move Material/ParticleSystem made for their own components, just
// resolved lazily here instead of in SceneSerializer (which has no
// ContentManager access).
HE::UUID migrateLegacyConfig(const AnimatorStateMachineComponent::LegacyConfig& legacy, ContentManager& cm)
{
    HE::AnimatorStateMachineGraph g;
    g.states       = legacy.states;
    g.transitions  = legacy.transitions;
    g.defaultParams = legacy.params;
    g.startState   = legacy.currentStateName;

    AnimatorStateMachineAsset asset;
    asset.name      = "Migrated State Machine";
    asset.graphJson = HE::animatorStateMachineToJson(g);
    return cm.registerAnimatorStateMachine(std::move(asset));
}

void resolveConfigIfNeeded(AnimatorStateMachineComponent& sm, ContentManager& cm)
{
    if (sm.legacy.hasData)
    {
        sm.stateMachineAssetId = migrateLegacyConfig(sm.legacy, cm);
        sm.legacy.hasData      = false;
        sm.configDirty         = true;
    }

    if (!sm.configDirty && sm.resolvedFromAssetId == sm.stateMachineAssetId) return;

    HE::AnimatorStateMachineGraph graph;
    if (const AnimatorStateMachineAsset* asset = cm.getAnimatorStateMachine(sm.stateMachineAssetId);
        asset && !asset->graphJson.empty())
    {
        HE::AnimatorStateMachineGraph parsed;
        if (HE::animatorStateMachineFromJson(asset->graphJson, parsed))
        {
            HE_LOG_DEBUG(Animation, "Resolved state machine '%s': %zu state(s), %zu transition(s)",
                         asset->name.c_str(), parsed.states.size(), parsed.transitions.size());
            graph = std::move(parsed);
        }
        else
        {
            // The component keeps an empty graph and the character just T-poses;
            // without this the only clue is the missing animation.
            HE_LOG_ERROR(Animation, "State machine asset '%s' has unparsable graph JSON — "
                                    "the animator will have no states", asset->name.c_str());
        }
    }
    else if (sm.stateMachineAssetId != HE::UUID{})
    {
        HE_LOG_WARN(Animation, "State machine asset %016llx%016llx is missing or empty — "
                               "the animator will have no states",
                    static_cast<unsigned long long>(sm.stateMachineAssetId.hi),
                    static_cast<unsigned long long>(sm.stateMachineAssetId.lo));
    }

    sm.resolvedGraph       = graph;
    sm.resolvedFromAssetId = sm.stateMachineAssetId;
    sm.configDirty         = false;

    // Seed live params from the graph's defaults (only newly-appeared keys —
    // an in-flight edit shouldn't clobber a param a script already tweaked at
    // runtime, e.g. re-resolving after the graph gained a NEW param).
    for (const auto& [k, v] : sm.resolvedGraph.defaultParams)
        sm.params.try_emplace(k, v);

    // First resolve for this entity (no current state yet) — enter the
    // graph's start state (or its first state, if any).
    if (sm.currentStateName.empty())
    {
        sm.currentStateName = !sm.resolvedGraph.startState.empty()
            ? sm.resolvedGraph.startState
            : (sm.resolvedGraph.states.empty() ? std::string() : sm.resolvedGraph.states.front().name);
    }
}

const HE::AnimationState* findState(const AnimatorStateMachineComponent& sm, const std::string& name)
{
    for (const auto& s : sm.resolvedGraph.states)
        if (s.name == name) return &s;
    return nullptr;
}

bool evalTransition(const AnimatorStateMachineComponent& sm, const HE::AnimationTransition& t)
{
    auto it = sm.params.find(t.paramName);
    if (it == sm.params.end()) return false;
    const float v = it->second;
    switch (t.op) {
        case HE::TransitionOp::Greater: return v >  t.threshold;
        case HE::TransitionOp::Less:    return v <  t.threshold;
        case HE::TransitionOp::Equal:   return v == t.threshold;
    }
    return false;
}
} // namespace

void AnimationStateMachineSystem::markConfigDirty(AnimatorStateMachineComponent& sm) { sm.configDirty = true; }

void AnimationStateMachineSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                                         AnimatorHost* sync, HE::RootMotionContext* rootMotion,
                                         HE::NotifyQueue* notifies)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorStateMachineComponent, SkeletalMeshComponent>();

    for (auto [e, sm, smc] : view.each())
    {
        resolveConfigIfNeeded(sm, cm);

        // The sync graph writes this entity's parameters, and it runs HERE —
        // right before the transitions below read them. bind() is idempotent, so
        // calling it every frame is also how an entity that gained a state
        // machine (or swapped its asset) mid-session gets picked up.
        if (sync && sync->running())
        {
            sync->bind(e);
            sync->fireUpdate(e, dt);
        }

        const HE::AnimationState* curState = findState(sm, sm.currentStateName);
        if (!curState)
        {
            if (!sm.resolvedGraph.states.empty())
                HE_LOG_THROTTLE(Animation, Warning, 5.0,
                                "Entity %u: state machine is in unknown state '%s' — "
                                "the graph has %zu state(s) and none match",
                                static_cast<uint32_t>(e), sm.currentStateName.c_str(),
                                sm.resolvedGraph.states.size());
            continue;
        }

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty())
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: animator has no usable skeletal mesh (%s) — not animating",
                            static_cast<uint32_t>(e), mesh ? "skeleton is empty" : "mesh not found");
            continue;
        }

        const AnimationClipAsset* curClip = cm.getAnimationClip(curState->clipId);
        if (!curClip)
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: state '%s' references a missing animation clip — "
                            "the pose will not advance",
                            static_cast<uint32_t>(e), curState->name.c_str());

        // A crossfade has TWO playheads, and each one needs the position it had
        // BEFORE this frame's advance. Captured per playhead, not per entity —
        // that distinction is the whole reason this is not one variable.
        const float outPrev = sm.clipTime;
        const float step    = dt * sm.playbackSpeed;

        // Advance current clip time
        if (curClip && curClip->duration > 0.0f)
        {
            sm.clipTime += dt * sm.playbackSpeed;
            if (curState->looping)
            {
                sm.clipTime = std::fmod(sm.clipTime, curClip->duration);
                if (sm.clipTime < 0.0f) sm.clipTime += curClip->duration;
            }
            else
            {
                sm.clipTime = std::min(sm.clipTime, curClip->duration);
            }
        }

        // Check transitions (only when not mid-transition)
        if (!sm.inTransition)
        {
            for (const auto& t : sm.resolvedGraph.transitions)
            {
                if (t.fromState != sm.currentStateName) continue;
                if (!evalTransition(sm, t)) continue;

                HE_LOG_TRACE(Animation, "Entity %u: state '%s' -> '%s' (%s %.3f, over %.2f s)",
                             static_cast<uint32_t>(e), sm.currentStateName.c_str(),
                             t.toState.c_str(), t.paramName.c_str(), t.threshold, t.duration);
                sm.inTransition       = true;
                sm.transitionTarget   = t.toState;
                sm.transitionElapsed  = 0.0f;
                sm.transitionDuration = t.duration;
                // The incoming playhead is being set back to 0, so it gets a
                // fresh first frame — and a notify on the incoming clip's frame 0
                // is exactly what "the attack starts here" is written as.
                sm.transitionNotifiesPrimed = false;
                break;
            }
        }

        // Advance crossfade. inPrev is read here and not earlier on purpose: a
        // transition that STARTED above set transitionElapsed to 0, and 0 is
        // exactly where its incoming playhead's first span begins.
        const float inPrev = sm.transitionElapsed;
        if (sm.inTransition)
            sm.transitionElapsed += dt * sm.playbackSpeed;

        // The crossfade weight, needed before the pose branch below because BOTH
        // playheads' notify collection has to agree on which of them is the one
        // that fires. Outside a transition the outgoing playhead is the only one
        // there is, so it always wins.
        const float notifyAlpha = sm.inTransition
            ? std::min(sm.transitionElapsed / sm.transitionDuration, 1.0f) : 0.0f;
        const bool  outFires = !sm.inTransition || notifyAlpha < HE::kNotifyDominanceAlpha;

        // Sample outgoing clip
        const size_t jointCount = mesh->skeleton.size();
        std::vector<JointTRS> trsOut(jointCount);
        if (curClip && curClip->duration > 0.0f)
            sampleClip(*curClip, sm.clipTime, trsOut);

        auto* rmc = reg.try_get<RootMotionComponent>(e);
        HE::RootMotionDelta deltaOut;
        bool haveOut = false;
        if (rmc && curClip)
            haveOut = HE::rootMotionSampleClip(e, *rmc, *mesh, *curClip, outPrev, outPrev + step,
                                               curState->looping, trsOut, deltaOut);

        // The outgoing playhead's notifies, over the span the delta above used.
        // Not gated on a RootMotionComponent — a footstep is not root motion, and
        // a character that never moves from the clip still has one.
        if (curClip)
            HE::notifyCollectClip(e, *curClip, outPrev, outPrev + step, curState->looping,
                                  outFires, sm.clipNotifiesPrimed, notifies);

        std::vector<JointTRS> final_trs;

        if (sm.inTransition)
        {
            const float alpha = std::min(sm.transitionElapsed / sm.transitionDuration, 1.0f);

            // Sample incoming clip at transitionElapsed
            const HE::AnimationState* nextState = findState(sm, sm.transitionTarget);
            if (!nextState)
                HE_LOG_THROTTLE(Animation, Error, 5.0,
                                "Entity %u: transition targets state '%s', which does not "
                                "exist in the graph — the crossfade blends to nothing",
                                static_cast<uint32_t>(e), sm.transitionTarget.c_str());
            const AnimationClipAsset* nextClip = nextState ? cm.getAnimationClip(nextState->clipId) : nullptr;

            std::vector<JointTRS> trsIn(jointCount);
            HE::RootMotionDelta deltaIn;
            bool haveIn = false;
            if (nextClip && nextClip->duration > 0.0f)
            {
                float inTime = sm.transitionElapsed;
                float inFrom = inPrev;
                if (nextState->looping)
                {
                    inTime = std::fmod(inTime, nextClip->duration);
                    inFrom = std::fmod(inFrom, nextClip->duration);
                }
                else
                {
                    inTime = std::min(inTime, nextClip->duration);
                    inFrom = std::min(inFrom, nextClip->duration);
                }
                sampleClip(*nextClip, inTime, trsIn);

                if (rmc)
                    haveIn = HE::rootMotionSampleClip(e, *rmc, *mesh, *nextClip,
                                                      inFrom, inFrom + step,
                                                      nextState->looping, trsIn, deltaIn);

                // The incoming playhead. It is walked past even while it is the
                // lighter half — that is what stops it from holding its first
                // frame back and firing it the instant the weight tips over.
                HE::notifyCollectClip(e, *nextClip, inFrom, inFrom + step,
                                      nextState->looping, !outFires,
                                      sm.transitionNotifiesPrimed, notifies);
            }

            // Both deltas, mixed with the alpha that mixes the pose. Letting only
            // the heavier one through would make the character jump forward the
            // moment the weight tipped over.
            if (haveOut || haveIn)
                HE::rootMotionApply(world, rootMotion, e, *rmc,
                                    HE::blendRootMotion(deltaOut, deltaIn, alpha), dt);

            blendTRS(trsOut, trsIn, alpha, final_trs);

            // Complete transition
            if (sm.transitionElapsed >= sm.transitionDuration)
            {
                sm.currentStateName  = sm.transitionTarget;
                // Wrapped the same way the incoming playhead was sampled. A
                // crossfade longer than the clip it fades INTO leaves an elapsed
                // time past the clip's end, and the pose is wrapped a line later
                // anyway — but root motion reads this value BEFORE that wrap and
                // would spend one frame with a span pinned to the clip's end.
                sm.clipTime = sm.transitionElapsed;
                if (nextClip && nextClip->duration > 0.0f)
                {
                    if (nextState && nextState->looping)
                    {
                        sm.clipTime = std::fmod(sm.clipTime, nextClip->duration);
                        if (sm.clipTime < 0.0f) sm.clipTime += nextClip->duration;
                    }
                    else
                    {
                        sm.clipTime = std::min(sm.clipTime, nextClip->duration);
                    }
                }
                sm.inTransition      = false;
                sm.transitionElapsed = 0.0f;
                // The incoming playhead just became the outgoing one, so its
                // priming travels with it — exactly as transitionElapsed travels
                // onto clipTime a few lines up. Without this the surviving
                // playhead would look fresh again and fire its frame-0 notify a
                // second time in the frame the crossfade ends.
                sm.clipNotifiesPrimed = sm.transitionNotifiesPrimed;
            }
        }
        else
        {
            if (haveOut) HE::rootMotionApply(world, rootMotion, e, *rmc, deltaOut, dt);
            final_trs = std::move(trsOut);
        }

        // Layer stack (if any) → FK → IBM. AFTER the crossfade, not before it: a
        // layer is laid on a finished result — a reload on the upper body holds
        // against whatever the legs are doing, idle, run or the blend between
        // them. A layer applied before the crossfade would be mixed straight back
        // out again. And after rootMotionApply, so it cannot write back the root
        // translation that was just taken out.
        HE::poseFinalize(world, cm, dt, e, *mesh, final_trs, smc, notifies);
    }
}
