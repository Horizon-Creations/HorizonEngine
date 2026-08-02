#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"
#include <Diagnostics/Log.h>

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
        if (!clipA && !clipB)
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: blend animator is playing but neither clip A nor "
                            "clip B could be resolved", static_cast<uint32_t>(e));
            continue;
        }
        // A half-resolved blend still plays, but the missing side contributes an
        // identity pose — which looks like a broken animation, not a missing asset.
        if (!clipA || !clipB)
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: blend clip %s is missing — that side blends "
                            "towards the bind pose", static_cast<uint32_t>(e),
                            clipA ? "B" : "A");

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty())
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: blend animator has no usable skeletal mesh (%s)",
                            static_cast<uint32_t>(e),
                            mesh ? "skeleton is empty" : "mesh not found");
            continue;
        }

        const size_t jointCount = mesh->skeleton.size();

        // Reference duration: longest valid clip drives the timeline
        float refDuration = 0.0f;
        if (clipA) refDuration = std::max(refDuration, clipA->duration);
        if (clipB) refDuration = std::max(refDuration, clipB->duration);
        if (refDuration <= 0.0f) continue;

        advancePlayback(blend.playbackTime, blend.playing,
                        blend.playbackSpeed, blend.looping, refDuration, dt);

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
