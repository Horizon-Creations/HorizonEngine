#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "AnimationEval.h"

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
        if (HE::animatorStateMachineFromJson(asset->graphJson, parsed)) graph = std::move(parsed);
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

void AnimationStateMachineSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorStateMachineComponent, SkeletalMeshComponent>();

    for (auto [e, sm, smc] : view.each())
    {
        resolveConfigIfNeeded(sm, cm);

        const HE::AnimationState* curState = findState(sm, sm.currentStateName);
        if (!curState) continue;

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty()) continue;

        const AnimationClipAsset* curClip = cm.getAnimationClip(curState->clipId);

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

                sm.inTransition       = true;
                sm.transitionTarget   = t.toState;
                sm.transitionElapsed  = 0.0f;
                sm.transitionDuration = t.duration;
                break;
            }
        }

        // Advance crossfade
        if (sm.inTransition)
            sm.transitionElapsed += dt * sm.playbackSpeed;

        // Sample outgoing clip
        const size_t jointCount = mesh->skeleton.size();
        std::vector<JointTRS> trsOut(jointCount);
        if (curClip && curClip->duration > 0.0f)
            sampleClip(*curClip, sm.clipTime, trsOut);

        std::vector<JointTRS> final_trs;

        if (sm.inTransition)
        {
            const float alpha = std::min(sm.transitionElapsed / sm.transitionDuration, 1.0f);

            // Sample incoming clip at transitionElapsed
            const HE::AnimationState* nextState = findState(sm, sm.transitionTarget);
            const AnimationClipAsset* nextClip = nextState ? cm.getAnimationClip(nextState->clipId) : nullptr;

            std::vector<JointTRS> trsIn(jointCount);
            if (nextClip && nextClip->duration > 0.0f)
            {
                float inTime = sm.transitionElapsed;
                if (nextState->looping)
                    inTime = std::fmod(inTime, nextClip->duration);
                else
                    inTime = std::min(inTime, nextClip->duration);
                sampleClip(*nextClip, inTime, trsIn);
            }

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
            final_trs = std::move(trsOut);
        }

        composeBoneMatrices(*mesh, final_trs, smc.boneMatrices);
        smc.dirty = true;
    }
}
