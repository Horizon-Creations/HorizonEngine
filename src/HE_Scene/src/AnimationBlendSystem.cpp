#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"

#include <algorithm>
#include <cmath>

void AnimationBlendSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorBlendComponent, SkeletalMeshComponent>();

    for (auto [e, blend, smc] : view.each())
    {
        if (!blend.playing) continue;

        const AnimationClipAsset* clipA = cm.getAnimationClip(blend.clipAId);
        const AnimationClipAsset* clipB = cm.getAnimationClip(blend.clipBId);
        if (!clipA && !clipB) continue;

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty()) continue;

        const size_t jointCount = mesh->skeleton.size();

        // Reference duration: longest valid clip drives the timeline
        float refDuration = 0.0f;
        if (clipA) refDuration = std::max(refDuration, clipA->duration);
        if (clipB) refDuration = std::max(refDuration, clipB->duration);
        if (refDuration <= 0.0f) continue;

        blend.playbackTime += dt * blend.playbackSpeed;
        if (blend.looping)
        {
            blend.playbackTime = std::fmod(blend.playbackTime, refDuration);
            if (blend.playbackTime < 0.0f)
                blend.playbackTime += refDuration;
        }
        else
        {
            if (blend.playbackTime >= refDuration)
            {
                blend.playbackTime = refDuration;
                blend.playing      = false;
            }
        }

        const float t = blend.playbackTime;

        // Sample each clip at its own wrapped time — missing clip leaves defaults (identity TRS)
        std::vector<JointTRS> trsA(jointCount);
        if (clipA && clipA->duration > 0.0f)
            sampleClip(*clipA, std::fmod(t, clipA->duration), trsA);

        std::vector<JointTRS> trsB(jointCount);
        if (clipB && clipB->duration > 0.0f)
            sampleClip(*clipB, std::fmod(t, clipB->duration), trsB);

        std::vector<JointTRS> blended;
        blendTRS(trsA, trsB, blend.blendAlpha, blended);
        composeBoneMatrices(*mesh, blended, smc.boneMatrices);
        smc.dirty = true;
    }
}
