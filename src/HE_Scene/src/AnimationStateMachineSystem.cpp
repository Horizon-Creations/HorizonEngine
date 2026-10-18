#include <HorizonScene/AnimationStateMachineSystem.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/Components/AnimatorStateMachineComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/RootMotionComponent.h>
#include <ContentManager/ContentManager.h>
#include <ContentManager/Assets.h>
#include "AnimationEval.h"
#include "RootMotionApply.h"
#include "PoseFinalize.h"
#include "PoseSource.h"
#include "NotifyCollect.h"
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <mutex>
#include <unordered_map>

namespace
{
// One parse per state machine asset TEXT, shared by every component resolved
// from it (SCN-302): two hundred characters on one machine hold one graph. The
// same discipline as the blend spaces in PoseSource.cpp — keyed by id, handed
// out only for exactly the JSON it was parsed from (ContentManager has no
// asset revision, and equal text parses to an equal graph), weak so a graph
// goes with its last component, only successful parses kept so a broken asset
// still logs per component. markConfigDirty after an edit therefore still
// works per component: the first one to re-resolve parses the new text, the
// others share that parse.
struct SharedGraph
{
    std::string                                        json;
    std::weak_ptr<const HE::AnimatorStateMachineGraph> graph;
};
std::mutex                                g_sharedGraphsMutex;
std::unordered_map<HE::UUID, SharedGraph> g_sharedGraphs;

std::shared_ptr<const HE::AnimatorStateMachineGraph> sharedGraph(HE::UUID id, const std::string& json)
{
    std::lock_guard<std::mutex> lock(g_sharedGraphsMutex);
    if (auto it = g_sharedGraphs.find(id); it != g_sharedGraphs.end() && it->second.json == json)
        if (auto live = it->second.graph.lock()) return live;

    HE::AnimatorStateMachineGraph parsed;
    if (!HE::animatorStateMachineFromJson(json, parsed)) return nullptr;
    auto graph = std::make_shared<const HE::AnimatorStateMachineGraph>(std::move(parsed));
    for (auto it = g_sharedGraphs.begin(); it != g_sharedGraphs.end();)
        it = it->second.graph.expired() ? g_sharedGraphs.erase(it) : std::next(it);
    g_sharedGraphs[id] = SharedGraph{ json, graph };
    return graph;
}

// One-off migration for scenes saved before AnimatorStateMachineComponent
// referenced an AnimatorStateMachineAsset: bake the legacy inline
// states/transitions/params into a real asset — same "asset instead of inline
// fields" move Material/ParticleSystem made for their own components, just
// resolved lazily here instead of in SceneSerializer (which has no
// ContentManager access).
HE::UUID migrateLegacyConfig(const AnimatorStateMachineComponent::LegacyConfig& legacy, ContentManager& cm)
{
    HE::AnimatorStateMachineGraph g;
    g.states       = legacy.states;
    g.transitions  = legacy.transitions;
    g.defaultParams = legacy.params;
    g.startState   = legacy.currentStateName;

    AnimatorStateMachineAsset asset;
    asset.name      = "Migrated State Machine";
    asset.graphJson = HE::animatorStateMachineToJson(g);
    return cm.registerAnimatorStateMachine(std::move(asset));
}

void resolveConfigIfNeeded(AnimatorStateMachineComponent& sm, ContentManager& cm)
{
    if (sm.legacy.hasData)
    {
        sm.stateMachineAssetId = migrateLegacyConfig(sm.legacy, cm);
        sm.legacy.hasData      = false;
        sm.configDirty         = true;
    }

    if (!sm.configDirty && sm.resolvedFromAssetId == sm.stateMachineAssetId) return;

    std::shared_ptr<const HE::AnimatorStateMachineGraph> graph;
    if (const AnimatorStateMachineAsset* asset = cm.getAnimatorStateMachine(sm.stateMachineAssetId);
        asset && !asset->graphJson.empty())
    {
        if (auto parsed = sharedGraph(sm.stateMachineAssetId, asset->graphJson))
        {
            HE_LOG_DEBUG(Animation, "Resolved state machine '%s': %zu state(s), %zu transition(s)",
                         asset->name.c_str(), parsed->states.size(), parsed->transitions.size());
            graph = std::move(parsed);
        }
        else
        {
            // The component keeps an empty graph and the character just T-poses;
            // without this the only clue is the missing animation.
            HE_LOG_ERROR(Animation, "State machine asset '%s' has unparsable graph JSON — "
                                    "the animator will have no states", asset->name.c_str());
        }
    }
    else if (sm.stateMachineAssetId != HE::UUID{})
    {
        HE_LOG_WARN(Animation, "State machine asset %016llx%016llx is missing or empty — "
                               "the animator will have no states",
                    static_cast<unsigned long long>(sm.stateMachineAssetId.hi),
                    static_cast<unsigned long long>(sm.stateMachineAssetId.lo));
    }

    sm.resolvedGraph       = std::move(graph);
    sm.resolvedFromAssetId = sm.stateMachineAssetId;
    sm.configDirty         = false;
    // The parsed blend spaces belonged to the graph that just went away. Keeping
    // them would mean a state whose blendSpaceId was just repointed still posing
    // from the old space for as long as the entity lives.
    sm.blendSpaces.clear();

    // Seed live params from the graph's defaults (only newly-appeared keys —
    // an in-flight edit shouldn't clobber a param a script already tweaked at
    // runtime, e.g. re-resolving after the graph gained a NEW param).
    for (const auto& [k, v] : sm.graph().defaultParams)
        sm.params.try_emplace(k, v);

    // First resolve for this entity (no current state yet) — enter the
    // graph's start state (or its first state, if any).
    if (sm.currentStateName.empty())
    {
        sm.currentStateName = !sm.graph().startState.empty()
            ? sm.graph().startState
            : (sm.graph().states.empty() ? std::string() : sm.graph().states.front().name);
    }
}

const HE::AnimationState* findState(const AnimatorStateMachineComponent& sm, const std::string& name)
{
    for (const auto& s : sm.graph().states)
        if (s.name == name) return &s;
    return nullptr;
}

bool evalTransition(const AnimatorStateMachineComponent& sm, const HE::AnimationTransition& t)
{
    auto it = sm.params.find(t.paramName);
    if (it == sm.params.end()) return false;
    const float v = it->second;
    switch (t.op) {
        case HE::TransitionOp::Greater: return v >  t.threshold;
        case HE::TransitionOp::Less:    return v <  t.threshold;
        case HE::TransitionOp::Equal:   return v == t.threshold;
    }
    return false;
}

// Which driver a PoseKey came from (see AnimationSystem.cpp).
constexpr uint64_t kPoseKeyStateMachine = 3;
} // namespace

void AnimationStateMachineSystem::markConfigDirty(AnimatorStateMachineComponent& sm) { sm.configDirty = true; }

void AnimationStateMachineSystem::update(HorizonWorld& world, ContentManager& cm, float dt,
                                         AnimatorHost* sync, HE::RootMotionContext* rootMotion,
                                         HE::NotifyQueue* notifies)
{
    auto& reg  = world.registry();
    auto  view = reg.view<AnimatorStateMachineComponent, SkeletalMeshComponent>();

    for (auto [e, sm, smc] : view.each())
    {
        resolveConfigIfNeeded(sm, cm);

        // A cutscene has this skeleton (SequenceSystem.h). The whole machine
        // stands still, its sync graph included: that graph only feeds the
        // transitions below, and they are not being taken.
        if (smc.sequencePosed) continue;

        // The sync graph writes this entity's parameters, and it runs HERE —
        // right before the transitions below read them. bind() is idempotent, so
        // calling it every frame is also how an entity that gained a state
        // machine (or swapped its asset) mid-session gets picked up.
        if (sync && sync->running())
        {
            sync->bind(e);
            sync->fireUpdate(e, dt);
        }

        const HE::AnimationState* curState = findState(sm, sm.currentStateName);
        if (!curState)
        {
            if (!sm.graph().states.empty())
                HE_LOG_THROTTLE(Animation, Warning, 5.0,
                                "Entity %u: state machine is in unknown state '%s' — "
                                "the graph has %zu state(s) and none match",
                                static_cast<uint32_t>(e), sm.currentStateName.c_str(),
                                sm.graph().states.size());
            continue;
        }

        const SkeletalMeshAsset* mesh = cm.getSkeletalMesh(smc.meshAssetId);
        if (!mesh || mesh->skeleton.empty())
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: animator has no usable skeletal mesh (%s) — not animating",
                            static_cast<uint32_t>(e), mesh ? "skeleton is empty" : "mesh not found");
            continue;
        }

        // One clip or a whole blend space, behind one interface — the state
        // machine below never asks which. A state that has both keeps the blend
        // space; see HE::makePoseSource.
        const HE::PoseSource curSrc = HE::makePoseSource(cm, sm.blendSpaces, curState->clipId,
                                                         curState->blendSpaceId, &sm.params);
        if (!curSrc.valid())
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: state '%s' has no usable pose source (%s) — "
                            "the pose will not advance",
                            static_cast<uint32_t>(e), curState->name.c_str(),
                            curSrc.space ? "every blend-space sample is missing its clip"
                                         : "missing or empty animation clip");

        const bool  curLooping = curSrc.looping(curState->looping);
        const float curPeriod  = curSrc.period();

        // A crossfade has TWO playheads, and each one needs the position it had
        // BEFORE this frame's advance. Captured per playhead, not per entity —
        // that distinction is the whole reason this is not one variable.
        const float outPrev = sm.clipTime;
        // In the playhead's own unit: seconds for a clip (rate 1), laps of the
        // shared phase for a blend space (rate 1/weightedDuration).
        const float step    = dt * sm.playbackSpeed * curSrc.rate();

        // Advance the outgoing playhead
        if (curSrc.valid() && curPeriod > 0.0f)
        {
            sm.clipTime += step;
            if (curLooping)
            {
                sm.clipTime = std::fmod(sm.clipTime, curPeriod);
                if (sm.clipTime < 0.0f) sm.clipTime += curPeriod;
            }
            else
            {
                sm.clipTime = std::min(sm.clipTime, curPeriod);
            }
        }

        // Check transitions (only when not mid-transition)
        if (!sm.inTransition)
        {
            for (const auto& t : sm.graph().transitions)
            {
                if (t.fromState != sm.currentStateName) continue;
                if (!evalTransition(sm, t)) continue;

                HE_LOG_TRACE(Animation, "Entity %u: state '%s' -> '%s' (%s %.3f, over %.2f s)",
                             static_cast<uint32_t>(e), sm.currentStateName.c_str(),
                             t.toState.c_str(), t.paramName.c_str(), t.threshold, t.duration);
                sm.inTransition       = true;
                sm.transitionTarget   = t.toState;
                sm.transitionElapsed  = 0.0f;
                sm.transitionDuration = t.duration;
                // The incoming playhead is being set back to 0, so it gets a
                // fresh first frame — and a notify on the incoming clip's frame 0
                // is exactly what "the attack starts here" is written as.
                sm.transitionNotifiesPrimed = false;
                // A transition INTO a state starts that state at its beginning —
                // second 0 for a clip, phase 0 for a blend space. The plan calls
                // this out for blend spaces specifically; it was always true, it
                // just used to be spelled `transitionElapsed = 0`.
                sm.transitionPlayhead = 0.0f;
                break;
            }
        }

        // Advance the crossfade CLOCK — seconds, and only seconds. It used to
        // double as the incoming playhead, which works only as long as every
        // state is a clip: a blend space's playhead is a phase advancing at
        // 1/weightedDuration, and that duration moves while the parameters move.
        //
        // inPrev is read here and not earlier on purpose: a transition that
        // STARTED above set transitionPlayhead to 0, and 0 is exactly where its
        // incoming playhead's first span begins.
        const float inPrev = sm.transitionPlayhead;
        if (sm.inTransition)
            sm.transitionElapsed += dt * sm.playbackSpeed;

        // The crossfade weight, needed before the pose branch below because BOTH
        // playheads' notify collection has to agree on which of them is the one
        // that fires. Outside a transition the outgoing playhead is the only one
        // there is, so it always wins.
        const float notifyAlpha = sm.inTransition
            ? std::min(sm.transitionElapsed / sm.transitionDuration, 1.0f) : 0.0f;
        const bool  outFires = !sm.inTransition || notifyAlpha < HE::kNotifyDominanceAlpha;

        // The outgoing playhead: pose, root motion and notifies in one call. They
        // are one call because a blend space cannot separate them — every sample's
        // root has to come out of ITS OWN pose before the N-way mix, exactly as
        // each side of a crossfade is locked before blendTRS.
        const size_t jointCount = mesh->skeleton.size();
        auto* rmc = reg.try_get<RootMotionComponent>(e);

        // The gate (SCN-102, PoseFinalize.h), for a state resting on one clip —
        // the last frame of a non-looping clip, a playbackSpeed of 0. Never during
        // a crossfade, and never for a blend space: its weights come from the
        // parameters every frame, so its pose moves while its phase stands still.
        // The notify walk is what evaluate() would have done for this clip.
        uint64_t key = 0;
        if (!sm.inTransition && curSrc.clip && !curSrc.space && HE::poseMayBeKept(reg, e))
        {
            key = HE::PoseKey(kPoseKeyStateMachine)
                      .id(curState->clipId).ptr(curSrc.clip).ptr(curSrc.clip->channels.data())
                      .id(smc.meshAssetId).ptr(mesh).ptr(mesh->skeleton.data()).count(jointCount)
                      .real(sm.clipTime)
                      .value();
            if (key == smc.poseKey)
            {
                HE::notifyCollectClip(e, *curSrc.clip, outPrev, outPrev + step, curLooping,
                                      outFires, sm.clipNotifiesPrimed, notifies);
                continue;
            }
        }

        // Per thread and reused across entities and frames (SCN-101). `pose`
        // points at whichever of them holds the finished result — moving a
        // scratch buffer into another would throw its capacity away.
        thread_local std::vector<JointTRS> trsOut, trsIn, blended;
        std::vector<JointTRS>* pose = &trsOut;

        HE::RootMotionDelta   deltaOut;
        bool                  haveOut = false;
        curSrc.evaluate(e, *mesh, curLooping, outPrev, outPrev + step, sm.clipTime,
                        rmc, outFires, sm.clipNotifiesPrimed, notifies,
                        trsOut, deltaOut, haveOut);

        if (sm.inTransition)
        {
            const float alpha = std::min(sm.transitionElapsed / sm.transitionDuration, 1.0f);

            // Sample incoming clip at transitionElapsed
            const HE::AnimationState* nextState = findState(sm, sm.transitionTarget);
            if (!nextState)
                HE_LOG_THROTTLE(Animation, Error, 5.0,
                                "Entity %u: transition targets state '%s', which does not "
                                "exist in the graph — the crossfade blends to nothing",
                                static_cast<uint32_t>(e), sm.transitionTarget.c_str());
            const HE::PoseSource inSrc = nextState
                ? HE::makePoseSource(cm, sm.blendSpaces, nextState->clipId,
                                     nextState->blendSpaceId, &sm.params)
                : HE::PoseSource{};
            const bool inLooping = nextState ? inSrc.looping(nextState->looping) : true;

            trsIn.assign(jointCount, JointTRS{});
            HE::RootMotionDelta deltaIn;
            bool haveIn = false;
            // The incoming playhead runs at ITS OWN rate, not the outgoing one's:
            // fading from a clip into a blend space, one side counts seconds and
            // the other counts phase, and `step` is only ever right for one of them.
            const float inPeriod = inSrc.period();
            const float inStep   = dt * sm.playbackSpeed * inSrc.rate();
            if (inSrc.valid() && inPeriod > 0.0f)
            {
                sm.transitionPlayhead = inPrev + inStep;

                float inTime = sm.transitionPlayhead;
                float inFrom = inPrev;
                if (inLooping)
                {
                    inTime = std::fmod(inTime, inPeriod);
                    inFrom = std::fmod(inFrom, inPeriod);
                }
                else
                {
                    inTime = std::min(inTime, inPeriod);
                    inFrom = std::min(inFrom, inPeriod);
                }

                // Pose, root motion and notifies for the incoming side. It is
                // walked past even while it is the lighter half — that is what
                // stops it from holding its first frame back and firing it the
                // instant the weight tips over.
                inSrc.evaluate(e, *mesh, inLooping, inFrom, inFrom + inStep, inTime,
                               rmc, !outFires, sm.transitionNotifiesPrimed, notifies,
                               trsIn, deltaIn, haveIn);
            }

            // Both deltas, mixed with the alpha that mixes the pose. Letting only
            // the heavier one through would make the character jump forward the
            // moment the weight tipped over.
            if (haveOut || haveIn)
                HE::rootMotionApply(world, rootMotion, e, *rmc,
                                    HE::blendRootMotion(deltaOut, deltaIn, alpha), dt);

            blendTRS(trsOut, trsIn, alpha, blended);
            pose = &blended;

            // Complete transition
            if (sm.transitionElapsed >= sm.transitionDuration)
            {
                sm.currentStateName  = sm.transitionTarget;
                // The incoming playhead becomes the outgoing one, in whatever
                // unit it was already counting. Wrapped the same way it was
                // sampled: a crossfade longer than the cycle it fades INTO leaves
                // a playhead past the end, and the pose is wrapped a line later
                // anyway — but root motion reads this value BEFORE that wrap and
                // would spend one frame with a span pinned to the end.
                sm.clipTime = sm.transitionPlayhead;
                if (inPeriod > 0.0f)
                {
                    if (inLooping)
                    {
                        sm.clipTime = std::fmod(sm.clipTime, inPeriod);
                        if (sm.clipTime < 0.0f) sm.clipTime += inPeriod;
                    }
                    else
                    {
                        sm.clipTime = std::min(sm.clipTime, inPeriod);
                    }
                }
                sm.inTransition       = false;
                sm.transitionElapsed  = 0.0f;
                sm.transitionPlayhead = 0.0f;
                // The incoming playhead just became the outgoing one, so its
                // priming travels with it — exactly as transitionElapsed travels
                // onto clipTime a few lines up. Without this the surviving
                // playhead would look fresh again and fire its frame-0 notify a
                // second time in the frame the crossfade ends.
                sm.clipNotifiesPrimed = sm.transitionNotifiesPrimed;
            }
        }
        else
        {
            if (haveOut) HE::rootMotionApply(world, rootMotion, e, *rmc, deltaOut, dt);
        }

        // Layer stack (if any) → FK → IBM. AFTER the crossfade, not before it: a
        // layer is laid on a finished result — a reload on the upper body holds
        // against whatever the legs are doing, idle, run or the blend between
        // them. A layer applied before the crossfade would be mixed straight back
        // out again. And after rootMotionApply, so it cannot write back the root
        // translation that was just taken out.
        HE::poseFinalize(world, cm, dt, e, *mesh, *pose, smc, rootMotion ? rootMotion->physics : nullptr, notifies);
        // 0 after any crossfade frame: the key is only taken outside one.
        smc.poseKey = key;
    }
}
