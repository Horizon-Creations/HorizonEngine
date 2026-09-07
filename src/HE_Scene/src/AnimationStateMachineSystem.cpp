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
                                         AnimatorHost* sync, HE::RootMotionContext* rootMotion)
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
                break;
            }
        }

        // Advance crossfade. inPrev is read here and not earlier on purpose: a
        // transition that STARTED above set transitionElapsed to 0, and 0 is
        // exactly where its incoming playhead's first span begins.
        const float inPrev = sm.transitionElapsed;
        if (sm.inTransition)
            sm.transitionElapsed += dt * sm.playbackSpeed;

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
                sm.clipTime          = sm.transitionElapsed;
                sm.inTransition      = false;
                sm.transitionElapsed = 0.0f;
            }
        }
        else
        {
            if (haveOut) HE::rootMotionApply(world, rootMotion, e, *rmc, deltaOut, dt);
            final_trs = std::move(trsOut);
        }

        composeBoneMatrices(*mesh, final_trs, smc.boneMatrices);
        smc.dirty = true;
    }
}
