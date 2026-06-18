#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"

#include <algorithm>
#include <cmath>

static const AnimationState* findState(const AnimatorStateMachineComponent& sm,
                                        const std::string& name)
{
    for (const auto& s : sm.states)
        if (s.name == name) return &s;
    return nullptr;
}

static bool evalTransition(const AnimatorStateMachineComponent& sm,
                             const AnimationTransition& t)
{
    auto it = sm.params.find(t.paramName);
    if (it == sm.params.end()) return false;
    const float v = it->second;
    switch (t.op) {
        case TransitionOp::Greater: return v >  t.threshold;
        case TransitionOp::Less:    return v <  t.threshold;
        case TransitionOp::Equal:   return v == t.threshold;
    }
    return false;
}

void AnimationStateMachineSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorStateMachineComponent, SkeletalMeshComponent>();

    for (auto [e, sm, smc] : view.each())
    {
        const AnimationState* curState = findState(sm, sm.currentStateName);
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
            for (const auto& t : sm.transitions)
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
            const AnimationState* nextState = findState(sm, sm.transitionTarget);
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
