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

        advancePlayback(animator.playbackTime, animator.playing,
                        animator.playbackSpeed, animator.looping, clip->duration, dt);

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty()) continue;

        std::vector<JointTRS> localTRS(mesh->skeleton.size());
        sampleClip(*clip, animator.playbackTime, localTRS);
        composeBoneMatrices(*mesh, localTRS, smc.boneMatrices);
        smc.dirty = true;
    }
}
