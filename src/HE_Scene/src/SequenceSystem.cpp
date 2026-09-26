#include <HorizonScene/SequenceSystem.h>
#include <HorizonScene/SequenceEval.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityActive.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/SequencePlayerComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"
#include "PoseFinalize.h"
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <string>

namespace
{

// The entity a slot resolved to, or null for an unbound slot, a missing actor
// and an actor destroyed since play() resolved it.
entt::entity slotEntity(const entt::registry& reg, const SequencePlayerComponent& sp, uint16_t slot)
{
    if (slot == kSequenceNoBinding || slot >= sp.slots.size()) return entt::null;
    const entt::entity e = sp.slots[slot];
    return (e != entt::null && reg.valid(e)) ? e : entt::entity{ entt::null };
}

void resolve(HorizonWorld& world, const SequenceAsset& seq, SequencePlayerComponent& sp)
{
    sp.slots            = HE::SequenceEval::resolveBindings(world, seq, sp.overrides);
    sp.bindingsResolved = true;
}

// The playhead of a looping player lives in [0, duration).
float wrapTime(float t, float duration)
{
    float w = std::fmod(t, duration);
    if (w < 0.0f) w += duration;
    return w;
}

void fireEvents(const entt::registry& reg, entt::entity player, const SequencePlayerComponent& sp,
                const SequenceAsset& seq, float tPrev, float tEnd, bool includeStart,
                HE::NotifyQueue& out)
{
    for (const SequenceTrack& tr : seq.tracks)
    {
        if (tr.kind != SequenceTrackKind::Event || tr.events.empty()) continue;
        // No actor: the event is the cutscene's own and goes to its owner. An
        // actor that is missing takes its events with it — firing them at the
        // owner instead would hand a door's "Open" to whatever holds the player.
        entt::entity target = player;
        if (tr.binding != kSequenceNoBinding)
        {
            target = slotEntity(reg, sp, tr.binding);
            if (target == entt::null) continue;
        }
        HE::collectNotifySpan(tr.events, seq.duration, static_cast<uint32_t>(target),
                              tPrev, tEnd, includeStart, out);
    }
}

void startSounds(HorizonWorld& world, ContentManager& cm, AudioEngine& audio,
                 SequencePlayerComponent& sp, const SequenceAsset& seq,
                 float tPrev, float tEnd, bool includeStart)
{
    auto& reg = world.registry();
    std::vector<AnimationNotify> starts;
    HE::NotifyQueue              fired;
    for (const SequenceTrack& tr : seq.tracks)
    {
        if (tr.kind != SequenceTrackKind::Audio || tr.audio.empty()) continue;

        // The section starts as notifies named by their index, walked by the one
        // span rule there is — laps, the closed first-frame origin and all.
        starts.clear();
        fired.clear();
        for (size_t i = 0; i < tr.audio.size(); ++i)
            starts.push_back({ std::to_string(i), tr.audio[i].start, 0.0f });
        HE::collectNotifySpan(starts, seq.duration, 0, tPrev, tEnd, includeStart, fired);
        if (fired.empty()) continue;

        entt::entity at = entt::null;
        if (tr.binding != kSequenceNoBinding)
        {
            at = slotEntity(reg, sp, tr.binding);
            if (at == entt::null) continue;   // missing actor: skipped, like its other tracks
        }

        for (const HE::AnimationNotifyEvent& ev : fired)
        {
            const SequenceAudioSection& s = tr.audio[std::stoul(ev.name)];
            const AudioAsset* a = cm.getAudio(s.assetId);
            if (!a || a->audioData.empty())
            {
                HE_LOG_THROTTLE(Audio, Warning, 5.0,
                                "Sequence sound %016llx%016llx is not loaded — the section "
                                "at %.2f s stays silent",
                                static_cast<unsigned long long>(s.assetId.hi),
                                static_cast<unsigned long long>(s.assetId.lo), s.start);
                continue;
            }
            // At the actor, in the world; without one, flat — the cutscene's
            // music and narration belong to nobody's position.
            uint64_t h = 0;
            if (at != entt::null)
            {
                const glm::vec3 p = HE::worldPositionOf(world, at);
                h = audio.playSpatial(*a, s.volume, s.pitch, false, p.x, p.y, p.z);
            }
            else
                h = audio.play(*a, s.volume, s.pitch, false);
            if (h != 0) sp.audioHandles.push_back(h);
        }
    }
}

void stopSounds(AudioEngine* audio, SequencePlayerComponent& sp)
{
    if (audio && audio->isInitialized())
        for (uint64_t h : sp.audioHandles) audio->stop(h);
    sp.audioHandles.clear();
}

// The pose a skeletal track wants at `t`, if everything it needs is here.
struct SkeletalPose
{
    entt::entity                   entity = entt::null;
    SkeletalMeshComponent*         smc    = nullptr;
    const SkeletalMeshAsset*       mesh   = nullptr;
    const AnimationClipAsset*      clip   = nullptr;
    const SequenceSkeletalSection* section = nullptr;
};

bool skeletalPoseAt(entt::registry& reg, ContentManager& cm, const SequencePlayerComponent& sp,
                    const SequenceTrack& tr, float t, SkeletalPose& out)
{
    out.entity = slotEntity(reg, sp, tr.binding);
    if (out.entity == entt::null) return false;
    out.smc = reg.try_get<SkeletalMeshComponent>(out.entity);
    if (!out.smc) return false;
    out.section = SequenceSystem::activeSection(tr, t);
    if (!out.section) return false;   // between sections: the actor's own animator has it

    out.clip = cm.getAnimationClip(out.section->clipId);
    out.mesh = cm.getSkeletalMesh(out.smc->meshAssetId);
    if (!out.clip || out.clip->duration <= 0.0f || !out.mesh || out.mesh->skeleton.empty())
    {
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: sequence skeletal section at %.2f s has no %s — the "
                        "actor keeps its own animation until it is loaded",
                        static_cast<uint32_t>(out.entity), out.section->start,
                        (!out.clip || out.clip->duration <= 0.0f) ? "usable clip" : "usable skeletal mesh");
        return false;
    }
    return true;
}

} // namespace

// ── Skeletal section rule ────────────────────────────────────────────────────

const SequenceSkeletalSection* SequenceSystem::activeSection(const SequenceTrack& track, float t)
{
    const SequenceSkeletalSection* best = nullptr;
    for (const SequenceSkeletalSection& s : track.sections)
    {
        if (t < s.start || t > s.end) continue;
        // >= on the start: of two starting together, the later-listed wins.
        if (!best || s.start >= best->start) best = &s;
    }
    return best;
}

float SequenceSystem::sectionClipTime(const SequenceSkeletalSection& s, float t, float clipDuration)
{
    const float ct = s.clipOffset + (t - s.start) * s.playRate;
    if (clipDuration <= 0.0f) return 0.0f;
    if (s.loop) return wrapTime(ct, clipDuration);
    return std::clamp(ct, 0.0f, clipDuration);
}

// ── Transport ────────────────────────────────────────────────────────────────

bool SequenceSystem::play(HorizonWorld& world, ContentManager& cm, entt::entity player)
{
    auto* sp = world.registry().try_get<SequencePlayerComponent>(player);
    if (!sp) return false;
    sp->started = true;

    if (sp->playing)
    {
        sp->paused = false;
        return true;
    }

    const SequenceAsset* seq = cm.getSequence(sp->sequenceId);
    // Once per start, not per frame: a sequence that cannot be found anywhere
    // would otherwise be asked for sixty times a second.
    if (!seq && sp->sequenceId != HE::UUID{}) cm.loadAssetAsync(sp->sequenceId);

    // A player left standing at its end starts over. (A looping one never stands
    // there; a stopped one was rewound by stop().)
    if (seq && seq->duration > 0.0f && !sp->loop)
    {
        if (sp->playRate >= 0.0f && sp->time >= seq->duration) sp->time = 0.0f;
        if (sp->playRate <  0.0f && sp->time <= 0.0f)          sp->time = seq->duration;
    }

    sp->playing          = true;
    sp->paused           = false;
    sp->primed           = false;
    sp->finishing        = false;
    sp->bindingsResolved = false;
    return true;
}

void SequenceSystem::pause(HorizonWorld& world, entt::entity player)
{
    if (auto* sp = world.registry().try_get<SequencePlayerComponent>(player))
        if (sp->playing) sp->paused = true;
}

void SequenceSystem::stop(HorizonWorld& world, entt::entity player)
{
    auto* sp = world.registry().try_get<SequencePlayerComponent>(player);
    if (!sp) return;
    sp->started   = true;   // a stop before the first frame also cancels the autoplay
    sp->playing   = false;
    sp->paused    = false;
    sp->finishing = false;
    sp->applyOnce = false;
    sp->primed    = false;
    sp->time      = 0.0f;
    sp->stopAudio = !sp->audioHandles.empty();
}

void SequenceSystem::setTime(HorizonWorld& world, ContentManager& cm, entt::entity player, float t)
{
    auto* sp = world.registry().try_get<SequencePlayerComponent>(player);
    if (!sp) return;
    const SequenceAsset* seq = cm.getSequence(sp->sequenceId);
    const float duration = seq ? seq->duration : 0.0f;
    if (!std::isfinite(t)) t = 0.0f;
    if (sp->loop && duration > 0.0f) t = wrapTime(t, duration);
    else                             t = std::clamp(t, 0.0f, (std::max)(duration, 0.0f));
    // The span the next frame walks starts HERE, so nothing between the old and
    // the new time fires: a jump over twenty explosions is not twenty explosions.
    sp->time      = t;
    sp->finishing = false;
    if (!sp->playing) sp->applyOnce = true;
}

void SequenceSystem::bindSlot(HorizonWorld& world, entt::entity player, uint16_t slot, entt::entity target)
{
    auto* sp = world.registry().try_get<SequencePlayerComponent>(player);
    if (!sp || slot == kSequenceNoBinding) return;
    auto it = std::find_if(sp->overrides.begin(), sp->overrides.end(),
                           [&](const HE::SequenceEval::SlotOverride& o) { return o.slot == slot; });
    if (target == entt::null)
    {
        if (it != sp->overrides.end()) sp->overrides.erase(it);
    }
    else if (it != sp->overrides.end()) it->entity = target;
    else                                sp->overrides.push_back({ slot, target });
    sp->bindingsResolved = false;
}

// ── The frame ────────────────────────────────────────────────────────────────

void SequenceSystem::begin(HorizonWorld& world, ContentManager& cm, float dt,
                           HE::SequenceContext* ctx, HE::NotifyQueue* notifies)
{
    auto& reg = world.registry();

    // Every claim is this frame's, made below. Cleared even without a session,
    // so a claim can never outlive the session that made it.
    for (auto [e, smc] : reg.view<SkeletalMeshComponent>().each()) smc.sequencePosed = false;

    if (!ctx) return;
    AudioEngine* audio = (ctx->audio && ctx->audio->isInitialized()) ? ctx->audio : nullptr;

    const HE::ActiveFilter active(reg);
    for (auto [e, sp] : reg.view<SequencePlayerComponent>().each())
    {
        if (sp.stopAudio) { stopSounds(audio, sp); sp.stopAudio = false; sp.audioPaused = false; }
        else if (audio && !sp.audioHandles.empty())
        {
            // The sounds follow the clock: paused with it, resumed with it.
            const bool wantPaused = sp.playing && sp.paused;
            if (wantPaused != sp.audioPaused)
                for (uint64_t h : sp.audioHandles)
                    wantPaused ? audio->pauseSound(h) : audio->resumeSound(h);
            // isPlaying() is false for a paused voice too; only a finished one goes.
            std::erase_if(sp.audioHandles,
                          [&](uint64_t h) { return !audio->isPlaying(h) && !audio->isPaused(h); });
        }
        if (audio) sp.audioPaused = sp.playing && sp.paused;

        // Autoplay is consumed on the first frame of the session, and not by a
        // switched-off owner — the same rule Audio Source's play-on-start keeps.
        if (!sp.started)
        {
            sp.started = true;
            if (sp.autoplay && !active.off(e)) play(world, cm, e);
        }

        if (!sp.playing && !sp.applyOnce) continue;

        const SequenceAsset* seq = cm.getSequence(sp.sequenceId);
        if (!seq)
        {
            if (sp.sequenceId != HE::UUID{})
                HE_LOG_THROTTLE(Animation, Warning, 5.0,
                                "Entity %u: sequence %016llx%016llx is not loaded — the "
                                "cutscene waits for it", static_cast<uint32_t>(e),
                                static_cast<unsigned long long>(sp.sequenceId.hi),
                                static_cast<unsigned long long>(sp.sequenceId.lo));
            continue;
        }
        if (!sp.bindingsResolved) resolve(world, *seq, sp);

        if (sp.playing && !sp.paused)
        {
            const float duration = seq->duration;
            if (duration <= 0.0f)
            {
                // Nothing to play through: write frame 0 once and stop.
                sp.time      = 0.0f;
                sp.finishing = true;
            }
            else
            {
                // Unwrapped, and captured before the clock moves: the span rule
                // needs direction and laps, and a wrapped playhead has neither.
                const float tPrev = sp.time;
                float       tEnd  = tPrev + dt * sp.playRate;
                if (!sp.loop) tEnd = std::clamp(tEnd, 0.0f, duration);

                const bool includeStart = !sp.primed;
                sp.primed = true;

                if (notifies) fireEvents(reg, e, sp, *seq, tPrev, tEnd, includeStart, *notifies);
                if (audio && tEnd > tPrev) startSounds(world, cm, *audio, sp, *seq, tPrev, tEnd, includeStart);

                if (sp.loop)
                    sp.time = wrapTime(tEnd, duration);
                else
                {
                    sp.time = tEnd;
                    if ((sp.playRate > 0.0f && tEnd >= duration) || (sp.playRate < 0.0f && tEnd <= 0.0f))
                        sp.finishing = true;
                }
            }
        }

        // Claim what apply() is going to pose, so the other drivers leave it.
        for (const SequenceTrack& tr : seq->tracks)
        {
            if (tr.kind != SequenceTrackKind::Skeletal) continue;
            SkeletalPose pose;
            if (skeletalPoseAt(reg, cm, sp, tr, sp.time, pose)) pose.smc->sequencePosed = true;
        }
    }
}

void SequenceSystem::apply(HorizonWorld& world, ContentManager& cm, float dt,
                           HE::SequenceContext* ctx, HE::NotifyQueue* notifies)
{
    if (!ctx) return;
    auto& reg = world.registry();

    std::vector<JointTRS> localTRS;
    for (auto [e, sp] : reg.view<SequencePlayerComponent>().each())
    {
        if (!sp.playing && !sp.applyOnce) continue;
        const SequenceAsset* seq = cm.getSequence(sp.sequenceId);
        if (!seq) continue;

        // Properties first: the skeletons below may carry IK, and IK casts from
        // the transform the sequence has just written.
        HE::SequenceEval::apply(world, cm, HE::SequenceEval::evaluate(*seq, sp.time), sp.slots);

        for (const SequenceTrack& tr : seq->tracks)
        {
            if (tr.kind != SequenceTrackKind::Skeletal) continue;
            SkeletalPose pose;
            // Only what begin() claimed: an unclaimed skeleton was posed by its
            // own driver this frame, and a second pose on top would be the
            // "two drivers" case this split exists to avoid.
            if (!skeletalPoseAt(reg, cm, sp, tr, sp.time, pose) || !pose.smc->sequencePosed) continue;

            localTRS.assign(pose.mesh->skeleton.size(), JointTRS{});
            sampleClip(*pose.clip, sectionClipTime(*pose.section, sp.time, pose.clip->duration), localTRS);
            HE::poseFinalize(world, cm, dt, pose.entity, *pose.mesh, localTRS, *pose.smc,
                             ctx->physics, notifies);
        }

        sp.applyOnce = false;
        if (sp.finishing)
        {
            // The last frame is written; the actors keep it. Sounds ring out —
            // a closing sting is not cut off because the timeline ended.
            sp.finishing = false;
            sp.playing   = false;
            sp.paused    = false;
        }
    }
}
