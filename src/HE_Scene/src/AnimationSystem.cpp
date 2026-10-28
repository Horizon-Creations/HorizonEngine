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
#include <JobSystem/JobSystem.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <vector>

namespace
{
// Which driver a PoseKey came from: the same clip at the same time is not the
// same pose when a blend or a state machine put it there.
constexpr uint64_t kPoseKeyClip = 1;

// -1: parallel_for decides (the pool, once the crowd clears its grain). 0: every entity
// runs on the calling thread, in view order — the exact pre-Thema-238-Schritt-6 behaviour.
// HE_ANIM_PARALLEL=0 or setParallelOverride(0), same convention as HE_FOLIAGE_PARALLEL.
int g_animParallelOverride = []{
    const char* v = std::getenv("HE_ANIM_PARALLEL");
    return (v && *v && std::atoi(v) == 0) ? 0 : -1;
}();
} // namespace

void AnimationSystem::setParallelOverride(int parallel)
{
    g_animParallelOverride = parallel;
}

void AnimationSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                             HE::RootMotionContext* rootMotion,
                             HE::NotifyQueue* notifies)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorComponent, SkeletalMeshComponent>();

    std::vector<entt::entity> entities(view.begin(), view.end());
    if (entities.empty()) return;

    // One slot per entity (view order) so the entities run below, off the calling thread, each
    // write their own notifies instead of racing on the shared queue. Merged below in view
    // order — the exact order the fully serial loop used to append notifies in, whichever of
    // the two passes an entity actually ran in.
    std::vector<HE::NotifyQueue> perEntityNotifies(notifies ? entities.size() : 0);

    auto processOne = [&](entt::entity e, HE::NotifyQueue* localNotifies)
    {
        auto [animator, smc] = view.get<AnimatorComponent, SkeletalMeshComponent>(e);
        if (!animator.playing) return;
        // A cutscene has this skeleton: the animator's clock stands still until
        // it hands it back, and nothing of it fires (SequenceSystem.h).
        if (smc.sequencePosed) return;

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
            return;
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
                              /*dominant=*/true, animator.notifiesPrimed, localNotifies);

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty())
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: animator has no usable skeletal mesh (%s)",
                            static_cast<uint32_t>(e),
                            mesh ? "skeleton is empty" : "mesh not found");
            return;
        }

        // The gate (SCN-102, PoseFinalize.h): the same clip at the same time on the
        // same skeleton is the same pose, and the bones still hold it. After the
        // notifies above on purpose — a playhead that did not move fires nothing,
        // but that is for notifyCollectClip to decide, not for the gate.
        uint64_t key = 0;
        if (HE::poseMayBeKept(reg, e))
        {
            key = HE::PoseKey(kPoseKeyClip)
                      .id(animator.clipAssetId).ptr(clip).ptr(clip->channels.data())
                      .id(smc.meshAssetId).ptr(mesh).ptr(mesh->skeleton.data())
                      .count(mesh->skeleton.size()).real(animator.playbackTime)
                      .value();
            if (key == smc.poseKey) return;
        }

        // Per thread and reused across entities and frames (SCN-101); assign
        // resets it to the defaults sampleClip expects.
        thread_local std::vector<JointTRS> localTRS;
        localTRS.assign(mesh->skeleton.size(), JointTRS{});
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
        HE::poseFinalize(world, cm, dt, e, *mesh, localTRS, smc, rootMotion ? rootMotion->physics : nullptr, localNotifies);
        smc.poseKey = key;
    };

    // Split by HE::poseMayBeKept's own test (no RootMotionComponent, no non-empty animation
    // layer stack, no active IK): that is exactly the set poseFinalize takes through its
    // FK+IBM-only path (composeModelMatrices + applyInverseBind into thread_local scratch and
    // the entity's own boneMatrices, PoseFinalize.cpp:497-525) — nothing shared, nothing
    // belonging to another entity. Root motion writes the entity's TransformComponent and, with
    // a context, its physics body; a layer stack or IK can read ANOTHER entity's transform
    // (look-at) or carry a per-component "already finalized this frame" flag — none of that is
    // safe to run concurrently with an unrelated entity's pass, so those entities keep running
    // on the calling thread, in view order, exactly as before this split.
    std::vector<uint32_t> simple, complex;
    simple.reserve(entities.size());
    for (uint32_t i = 0; i < entities.size(); ++i)
        (HE::poseMayBeKept(reg, entities[i]) ? simple : complex).push_back(i);

    auto runSimple = [&](size_t t)
    {
        const uint32_t i = simple[t];
        processOne(entities[i], notifies ? &perEntityNotifies[i] : nullptr);
    };
    if (g_animParallelOverride == 0) for (size_t t = 0; t < simple.size(); ++t) runSimple(t);
    else parallel_for(simple.size(), runSimple, "AnimationSimple", 32);

    for (uint32_t i : complex)
        processOne(entities[i], notifies ? &perEntityNotifies[i] : nullptr);

    if (notifies)
        for (HE::NotifyQueue& q : perEntityNotifies)
            if (!q.empty())
                notifies->insert(notifies->end(), q.begin(), q.end());
}
