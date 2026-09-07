#include <HorizonScene/AnimationSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"
#include "RootMotionApply.h"
#include "PoseFinalize.h"
#include "NotifyCollect.h"
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

void AnimationSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                             HE::RootMotionContext* rootMotion,
                             HE::NotifyQueue* notifies)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorComponent, SkeletalMeshComponent>();

    for (auto [e, animator, smc] : view.each())
    {
        if (!animator.playing) continue;

        const AnimationClipAsset* clip = cm.getAnimationClip(animator.clipAssetId);
        if (!clip || clip->duration <= 0.0f)
        {
            // "The character is stuck in T-pose" — this is why, in one line.
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: animator is playing but its clip %s "
                            "(%016llx%016llx) — the pose will not advance",
                            static_cast<uint32_t>(e),
                            clip ? "has zero duration" : "was not found",
                            static_cast<unsigned long long>(animator.clipAssetId.hi),
                            static_cast<unsigned long long>(animator.clipAssetId.lo));
            continue;
        }

        // Captured BEFORE the advance, and unwrapped: the span the root-motion
        // helper wants is (tPrev, tPrev + dt*speed], laps and all. Reading it off
        // the wrapped playhead afterwards throws away both the direction and the
        // number of laps.
        const float tPrev = animator.playbackTime;
        const float tEnd  = tPrev + dt * animator.playbackSpeed;

        advancePlayback(animator.playbackTime, animator.playing,
                        animator.playbackSpeed, animator.looping, clip->duration, dt);

        // Over the same span as the delta below, and BEFORE the skeleton is
        // looked up on purpose: the timeline moved, so what is written on it
        // happened. A footstep sound does not depend on there being a mesh to
        // pose, and a missing one is already reported a few lines down.
        HE::notifyCollectClip(e, *clip, tPrev, tEnd, animator.looping,
                              /*dominant=*/true, animator.notifiesPrimed, notifies);

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty())
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: animator has no usable skeletal mesh (%s)",
                            static_cast<uint32_t>(e),
                            mesh ? "skeleton is empty" : "mesh not found");
            continue;
        }

        std::vector<JointTRS> localTRS(mesh->skeleton.size());
        sampleClip(*clip, animator.playbackTime, localTRS);

        if (auto* rm = reg.try_get<RootMotionComponent>(e))
        {
            HE::RootMotionDelta delta;
            if (HE::rootMotionSampleClip(e, *rm, *mesh, *clip, tPrev, tEnd,
                                         animator.looping, localTRS, delta))
                HE::rootMotionApply(world, rootMotion, e, *rm, delta, dt);
        }

        // Layer stack (if any) → FK → IBM. After rootMotionApply on purpose: a
        // layer must not write back the root translation that was just taken out.
        HE::poseFinalize(world, cm, dt, e, *mesh, localTRS, smc, rootMotion ? rootMotion->physics : nullptr, notifies);
    }
}
