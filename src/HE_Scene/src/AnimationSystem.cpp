#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"

#include <algorithm>
#include <cmath>

void AnimationSystem::update(HorizonWorld& world, ContentManager& cm, float dt)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorComponent, SkeletalMeshComponent>();

    for (auto [e, animator, smc] : view.each())
    {
        if (!animator.playing) continue;

        const AnimationClipAsset* clip = cm.getAnimationClip(animator.clipAssetId);
        if (!clip || clip->duration <= 0.0f) continue;

        animator.playbackTime += dt * animator.playbackSpeed;
        if (animator.looping)
        {
            animator.playbackTime = std::fmod(animator.playbackTime, clip->duration);
            if (animator.playbackTime < 0.0f)
                animator.playbackTime += clip->duration;
        }
        else
        {
            if (animator.playbackTime >= clip->duration)
            {
                animator.playbackTime = clip->duration;
                animator.playing      = false;
            }
        }

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty()) continue;

        std::vector<JointTRS> localTRS(mesh->skeleton.size());
        sampleClip(*clip, animator.playbackTime, localTRS);
        composeBoneMatrices(*mesh, localTRS, smc.boneMatrices);
        smc.dirty = true;
    }
}
