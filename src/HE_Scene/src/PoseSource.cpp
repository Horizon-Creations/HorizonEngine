#include "PoseSource.h"
#include "RootMotionApply.h"
#include "NotifyCollect.h"

#include <ContentManager/ContentManager.h>
#include <Diagnostics/Log.h>

#include <algorithm>

namespace HE {

const BlendSpace* resolveBlendSpace(BlendSpaceCache& cache,
                                    ContentManager& cm, HE::UUID id)
{
    if (id == HE::UUID{}) return nullptr;

    for (const BlendSpaceCacheEntry& e : cache)
        if (e.id == id) return e.ok ? &e.space : nullptr;

    BlendSpaceCacheEntry entry;
    entry.id = id;
    if (const BlendSpaceAsset* asset = cm.getBlendSpace(id))
    {
        entry.ok = blendSpaceFromJson(asset->json, entry.space);
        if (!entry.ok)
            HE_LOG_WARN(Animation, "Blend space asset '%s' has unparsable JSON — "
                                   "anything pointing at it poses nothing",
                        asset->name.c_str());
    }
    else
    {
        HE_LOG_WARN(Animation, "Blend space %016llx%016llx is missing — anything "
                               "pointing at it poses nothing",
                    static_cast<unsigned long long>(id.hi),
                    static_cast<unsigned long long>(id.lo));
    }

    // Cached either way: a miss remembered is a miss that costs one log line
    // instead of one per frame for as long as the scene is open.
    cache.push_back(std::move(entry));
    const BlendSpaceCacheEntry& stored = cache.back();
    return stored.ok ? &stored.space : nullptr;
}

PoseSource makePoseSource(ContentManager& cm, BlendSpaceCache& cache,
                          HE::UUID clipId, HE::UUID blendSpaceId,
                          const std::unordered_map<std::string, float>* params)
{
    PoseSource src;

    if (blendSpaceId != HE::UUID{})
    {
        src.space = resolveBlendSpace(cache, cm, blendSpaceId);
        if (!src.space) return src;

        const auto readParam = [&](const std::string& name) -> float {
            if (name.empty() || !params) return 0.0f;
            auto it = params->find(name);
            return (it == params->end()) ? 0.0f : it->second;
        };
        const float x = readParam(src.space->paramX);
        const float y = (src.space->kind == BlendSpaceKind::TwoD) ? readParam(src.space->paramY) : 0.0f;

        blendSpaceWeights(*src.space, x, y, src.weights);

        const size_t n = src.space->samples.size();
        src.clips.assign(n, nullptr);
        src.durations.assign(n, 0.0f);
        for (size_t i = 0; i < n; ++i)
        {
            if (src.weights[i] <= 0.0f) continue;   // capped out: do not even look it up
            src.clips[i] = cm.getAnimationClip(src.space->samples[i].clipId);
            if (src.clips[i] && src.clips[i]->duration > 0.0f)
                src.durations[i] = src.clips[i]->duration;
            else
                // A sample whose clip is gone cannot contribute a pose, so it must
                // not keep its share of the weight either — the survivors are
                // renormalised below, which is the same answer as "that sample was
                // never placed".
                src.weights[i] = 0.0f;
        }

        float sum = 0.0f;
        for (float w : src.weights) sum += w;
        if (sum > 0.0f)
            for (float& w : src.weights) w /= sum;

        src.weightedDuration = blendSpaceWeightedDuration(*src.space, src.weights, src.durations);

        // Which sample fires. The heaviest one, ties to the lower index — the
        // literal generalisation of kNotifyDominanceAlpha from two clips to N,
        // and the tie rule is what stops it flickering between two frames.
        std::vector<size_t> order;
        blendSpaceEvalOrder(src.weights, order);
        if (!order.empty()) src.heaviest = static_cast<int>(order.front());

        return src;
    }

    src.clip = cm.getAnimationClip(clipId);
    return src;
}

void PoseSource::evaluate(entt::entity e, const SkeletalMeshAsset& mesh, bool looping,
                          float tPrev, float tEnd, float tSample,
                          const RootMotionComponent* rmc,
                          bool notifyDominant, bool& primed, NotifyQueue* notifies,
                          std::vector<JointTRS>& outPose,
                          RootMotionDelta& outDelta, bool& outHaveDelta) const
{
    outHaveDelta = false;
    outDelta     = RootMotionDelta{};
    outPose.assign(mesh.skeleton.size(), JointTRS{});

    if (clip)
    {
        if (clip->duration > 0.0f)
            sampleClip(*clip, tSample, outPose);
        if (rmc)
            outHaveDelta = rootMotionSampleClip(e, *rmc, mesh, *clip, tPrev, tEnd,
                                                looping, outPose, outDelta);
        // Outside the duration check on purpose, exactly as the drivers had it: a
        // footstep is not root motion and a zero-length clip is still a playhead.
        notifyCollectClip(e, *clip, tPrev, tEnd, looping, notifyDominant, primed, notifies);
        return;
    }

    if (!space || weightedDuration <= 0.0f) return;

    std::vector<size_t> order;
    blendSpaceEvalOrder(weights, order);
    if (order.empty()) return;

    // One pose per active sample, each sampled at ITS OWN seconds — the shared
    // phase times that sample's duration. This is the whole point of the phase:
    // walk (1.2 s) and run (0.8 s) at p = 0.5 are read at 0.6 s and 0.4 s, i.e.
    // both exactly halfway through their own cycle, and the feet stay in step.
    std::vector<std::vector<JointTRS>> poses(space->samples.size());
    std::vector<RootMotionDelta>       deltas(space->samples.size());
    std::vector<bool>                  haveDelta(space->samples.size(), false);

    for (size_t i : order)
    {
        const float dur = durations[i];
        poses[i].assign(mesh.skeleton.size(), JointTRS{});
        sampleClip(*clips[i], tSample * dur, poses[i]);

        if (rmc)
            haveDelta[i] = rootMotionSampleClip(e, *rmc, mesh, *clips[i],
                                                tPrev * dur, tEnd * dur, looping,
                                                poses[i], deltas[i]);
    }

    blendPosesN(poses, weights, outPose);

    // The deltas, mixed with the SAME weights in the SAME order as the poses.
    // Different weights for delta and pose make a character speed up or slow down
    // exactly where the blend is working, which is the bug the two-clip case
    // already documents.
    float accW = weights[order[0]];
    outDelta     = deltas[order[0]];
    outHaveDelta = haveDelta[order[0]];
    for (size_t k = 1; k < order.size(); ++k)
    {
        const size_t i = order[k];
        const float denom = accW + weights[i];
        if (denom <= 0.0f) continue;
        outDelta      = blendRootMotion(outDelta, deltas[i], weights[i] / denom);
        outHaveDelta  = outHaveDelta || haveDelta[i];
        accW          = denom;
    }

    // Only the heaviest sample fires; the others are not even walked past,
    // because they are not separate playheads — all N share one phase and
    // therefore one `primed` flag.
    if (heaviest >= 0 && clips[static_cast<size_t>(heaviest)])
    {
        const size_t h   = static_cast<size_t>(heaviest);
        const float  dur = durations[h];
        notifyCollectClip(e, *clips[h], tPrev * dur, tEnd * dur, looping,
                          notifyDominant, primed, notifies);
    }
}

} // namespace HE
