#include "PoseFinalize.h"
#include "AnimationEval.h"
#include "NotifyCollect.h"

#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/RootMotion.h>
#include <HorizonScene/Components/AnimationLayerComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <BoneMask/BoneMask.h>
#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <utility>

namespace HE {

namespace {

// Rebuild the (mask, skeleton) resolution cache when it no longer describes what
// the component actually holds. Three things can invalidate it and only one of
// them (`masksDirty`) is a flag somebody has to remember to set — the other two
// are checked against reality, which is why a forgotten flag is a missed frame
// and not a mask that quietly affects the wrong joints for ever.
void refreshMaskCache(AnimationLayerComponent& lc, ContentManager& cm,
                      const SkeletalMeshAsset& mesh, HE::UUID meshId)
{
    const size_t n = lc.layers.size();
    bool stale = lc.masksDirty
              || !(lc.resolvedForMeshId == meshId)
              || lc.resolvedMasks.size() != n
              || lc.resolvedMaskIds.size() != n;
    for (size_t i = 0; !stale && i < n; ++i)
        if (!(lc.resolvedMaskIds[i] == lc.layers[i].maskId)) stale = true;
    if (!stale) return;

    lc.resolvedMasks.assign(n, {});
    lc.resolvedMaskIds.assign(n, HE::UUID{});
    for (size_t i = 0; i < n; ++i)
    {
        lc.resolvedMaskIds[i] = lc.layers[i].maskId;
        if (lc.layers[i].maskId == HE::UUID{}) continue;  // no mask = whole skeleton

        const BoneMaskAsset* asset = cm.getBoneMask(lc.layers[i].maskId);
        if (!asset)
        {
            // An unresolvable mask affects NOTHING, not everything. A missing
            // asset that opened the layer up to the whole skeleton would turn a
            // broken reference into a character overwritten from the neck down.
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Animation layer '%s': bone mask %016llx%016llx could not be "
                            "resolved — the layer affects nothing",
                            lc.layers[i].name.c_str(),
                            static_cast<unsigned long long>(lc.layers[i].maskId.hi),
                            static_cast<unsigned long long>(lc.layers[i].maskId.lo));
            lc.resolvedMasks[i].assign(mesh.skeleton.size(), 0.0f);
            continue;
        }

        BoneMask mask;
        if (!boneMaskFromJson(asset->json, mask))
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Animation layer '%s': bone mask asset '%s' is not readable — "
                            "the layer affects nothing",
                            lc.layers[i].name.c_str(), asset->name.c_str());
            lc.resolvedMasks[i].assign(mesh.skeleton.size(), 0.0f);
            continue;
        }
        resolveBoneMask(mesh, mask, lc.resolvedMasks[i]);
    }

    lc.resolvedForMeshId = meshId;
    lc.masksDirty        = false;
}

// One layer, onto `pose`. Advances the layer's own playhead, collects its
// notifies and blends its clip on top.
void applyOneLayer(entt::entity e, AnimationLayerComponent::Layer& layer,
                   const std::vector<float>* maskWeights, ContentManager& cm, float dt,
                   const SkeletalMeshAsset& mesh, int rootJoint,
                   std::vector<JointTRS>& pose, NotifyQueue* notifies)
{
    if (layer.source == AnimationLayerComponent::Layer::Source::BlendSpace)
    {
        // The field exists so the on-disk format does not change when blend
        // spaces arrive; the sampler does not exist yet.
        HE_LOG_THROTTLE(Animation, Warning, 10.0,
                        "Entity %u: animation layer '%s' is set to a blend space, which "
                        "this build cannot sample yet — the layer contributes nothing",
                        static_cast<uint32_t>(e), layer.name.c_str());
        return;
    }

    const AnimationClipAsset* clip = cm.getAnimationClip(layer.clipId);
    if (!clip || clip->duration <= 0.0f)
    {
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: animation layer '%s' %s — the layer contributes nothing",
                        static_cast<uint32_t>(e), layer.name.c_str(),
                        clip ? "has a zero-duration clip" : "has no resolvable clip");
        return;
    }

    // Unwrapped, exactly as the drivers capture it: the span (tPrev, tEnd] with
    // its direction and its laps intact. Read off the WRAPPED playhead afterwards
    // it would be neither.
    const float tPrev = layer.playbackTime;
    const float tEnd  = layer.playing ? tPrev + dt * layer.playbackSpeed : tPrev;
    if (layer.playing)
        advancePlayback(layer.playbackTime, layer.playing,
                        layer.playbackSpeed, layer.looping, clip->duration, dt);

    // A layer fires its notifies as soon as it has ANY weight — not past
    // kNotifyDominanceAlpha. That threshold answers a different question: the two
    // sides of a crossfade are ALTERNATIVES (walk and run), and letting both fire
    // gives two footsteps per step. A layer is not an alternative to the base, it
    // is an addition to it: "magazine drops" on the reload layer and "footstep"
    // on the base are two events and both belong in the queue.
    //
    // A layer at weight 0 is still walked past (dominant=false), or it would hoard
    // its frame-0 notify and dump it the instant the weight came up.
    notifyCollectClip(e, *clip, tPrev, tEnd, layer.looping,
                      /*dominant=*/layer.weight > 0.0f, layer.notifiesPrimed, notifies);

    // Root motion is the BASE's business. A layer clip that carries motion in its
    // root simply does not contribute it — rootMotionSampleClip is never called
    // here, and the root's translation is pinned by applyLayerPose below.
    std::vector<JointTRS> layerTRS(mesh.skeleton.size());
    sampleClip(*clip, layer.playbackTime, layerTRS);

    std::vector<JointTRS> refTRS;
    const std::vector<JointTRS>* ref = nullptr;
    if (layer.mode == LayerBlendMode::Additive)
    {
        // The reference the difference is taken against: another clip if one was
        // named, otherwise this layer's own clip at additiveRefTime — which is
        // "the pose this animation starts from", the ordinary way an additive
        // clip is authored.
        const AnimationClipAsset* refClip = clip;
        if (layer.additiveRefClipId != HE::UUID{})
            if (const AnimationClipAsset* c = cm.getAnimationClip(layer.additiveRefClipId))
                refClip = c;
        refTRS.assign(mesh.skeleton.size(), JointTRS{});
        sampleClip(*refClip, layer.additiveRefTime, refTRS);
        ref = &refTRS;
    }

    std::vector<JointTRS> out;
    applyLayerPose(pose, layerTRS, ref, maskWeights, layer.weight, layer.mode, rootJoint, out);
    pose = std::move(out);
}

} // namespace

void poseBeginFrame(HorizonWorld& world)
{
    for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
        lc.finalizedThisFrame = false;
}

void poseFinalize(HorizonWorld& world, ContentManager& cm, float dt, entt::entity e,
                  const SkeletalMeshAsset& mesh, std::vector<JointTRS>& localTRS,
                  SkeletalMeshComponent& smc, NotifyQueue* notifies)
{
    auto& reg = world.registry();
    auto* lc  = reg.try_get<AnimationLayerComponent>(e);

    if (lc && !lc->layers.empty())
    {
        if (lc->finalizedThisFrame)
        {
            // The entity carries a second pose driver. The last one still wins on
            // boneMatrices (that wart is older than this file), but the layer
            // stage must not run twice: two runs would advance every layer
            // playhead by 2·dt and queue every layer notify twice.
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u carries more than one animation driver — its layer "
                            "stack ran with the first one and is skipped here; the pose "
                            "you see is the last driver's, unlayered",
                            static_cast<uint32_t>(e));
        }
        else
        {
            lc->finalizedThisFrame = true;

            // The root joint's translation is off limits to layers, but only when
            // something actually took it: without a RootMotionComponent nothing
            // was extracted and nothing can be applied twice, so pinning the root
            // there would delete motion the base pose is legitimately carrying.
            int rootJoint = -1;
            if (const auto* rm = reg.try_get<RootMotionComponent>(e))
                rootJoint = findRootJoint(mesh, rm->options.rootJointName);

            refreshMaskCache(*lc, cm, mesh, smc.meshAssetId);

            for (size_t i = 0; i < lc->layers.size(); ++i)
            {
                const std::vector<float>* weights =
                    (lc->layers[i].maskId == HE::UUID{} || i >= lc->resolvedMasks.size())
                        ? nullptr : &lc->resolvedMasks[i];
                applyOneLayer(e, lc->layers[i], weights, cm, dt, mesh, rootJoint,
                              localTRS, notifies);
            }
        }
    }

    composeBoneMatrices(mesh, localTRS, smc.boneMatrices);
    smc.dirty = true;
}

void poseEndFrame(HorizonWorld& world)
{
    for (auto [e, lc] : world.registry().view<AnimationLayerComponent>().each())
    {
        if (lc.finalizedThisFrame || lc.layers.empty()) continue;
        // Nothing posed this entity, so there was no base pose to lay anything on.
        // Falling back to the bind pose here would make this a fourth writer of
        // boneMatrices for a case nobody builds on purpose; saying so is enough.
        HE_LOG_THROTTLE(Animation, Warning, 10.0,
                        "Entity %u has animation layers but no animator driving it — "
                        "layers are laid ON a base pose and there is none, so they do "
                        "nothing", static_cast<uint32_t>(e));
    }
}

} // namespace HE
