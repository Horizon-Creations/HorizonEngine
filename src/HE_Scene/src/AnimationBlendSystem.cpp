#include <HorizonScene/AnimationBlendSystem.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorBlendComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"
#include "RootMotionApply.h"
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>

namespace
{
// Wrap a playhead into [0, duration) the way sampleClip's callers do — fmod alone
// returns a negative remainder for a rewinding clip.
float wrapTime(float t, float duration)
{
    if (duration <= 0.0f) return 0.0f;
    float w = std::fmod(t, duration);
    if (w < 0.0f) w += duration;
    return w;
}
} // namespace

void AnimationBlendSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                                  HE::RootMotionContext* rootMotion)
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

        // How far the shared timeline moves this frame, clamped for a non-looping
        // blend so a timeline that stops at the end does not hand the root-motion
        // helper a span running past it.
        const float tPrev = blend.playbackTime;
        float step = dt * blend.playbackSpeed;
        if (!blend.looping) step = std::clamp(tPrev + step, 0.0f, refDuration) - tPrev;

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

        // Root motion per clip, on its OWN playhead: each clip is sampled at
        // fmod(t, its own duration), so each has its own span and its own root to
        // lock. Locking after the blend would mean locking against a pose that no
        // longer knows which clip it came from.
        //
        // Each span is handed over as looping regardless of blend.looping: a clip
        // shorter than the timeline wraps against its own duration either way, and
        // the non-looping case is already covered by the clamped `step` above.
        if (auto* rm = reg.try_get<RootMotionComponent>(e))
        {
            HE::RootMotionDelta dA, dB;
            bool any = false;
            if (clipA)
            {
                const float aPrev = wrapTime(tPrev, clipA->duration);
                any |= HE::rootMotionSampleClip(e, *rm, *mesh, *clipA, aPrev, aPrev + step,
                                                /*looping=*/true, trsA, dA);
            }
            if (clipB)
            {
                const float bPrev = wrapTime(tPrev, clipB->duration);
                any |= HE::rootMotionSampleClip(e, *rm, *mesh, *clipB, bPrev, bPrev + step,
                                                /*looping=*/true, trsB, dB);
            }
            // Same alpha as the pose. Anything else makes the figure speed up or
            // stall exactly where the blend is doing its work.
            if (any)
                HE::rootMotionApply(world, rootMotion, e, *rm,
                                    HE::blendRootMotion(dA, dB, blend.blendAlpha), dt);
        }

        std::vector<JointTRS> blended;
        blendTRS(trsA, trsB, blend.blendAlpha, blended);
        composeBoneMatrices(*mesh, blended, smc.boneMatrices);
        smc.dirty = true;
    }
}
