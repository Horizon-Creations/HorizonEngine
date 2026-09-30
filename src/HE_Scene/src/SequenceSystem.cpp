#include <HorizonScene/SequenceSystem.h>
#include <HorizonScene/SequenceEval.h>
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/AudioEngine.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/EntityActive.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/CameraRigController.h>
#include <HorizonScene/Components/CameraComponent.h>
#include <HorizonScene/Components/CameraRigComponent.h>
#include <HorizonScene/Components/HierarchyComponent.h>
#include <HorizonScene/Components/SequencePlayerComponent.h>
#include <HorizonScene/Components/SkeletalMeshComponent.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <ContentManager/ContentManager.h>
#include "AnimationEval.h"
#include "PoseFinalize.h"
#include <Diagnostics/Log.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

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

void resolve(HorizonWorld& world, entt::entity player, const SequenceAsset& seq,
             SequencePlayerComponent& sp)
{
    if (sp.namedOverrides.empty())
        sp.slots = HE::SequenceEval::resolveBindings(world, seq, sp.overrides);
    else
    {
        // Names first, numbers after: resolveBindings applies overrides in
        // order, so a slot bound by number wins over the same slot by name.
        std::vector<HE::SequenceEval::SlotOverride> all;
        all.reserve(sp.namedOverrides.size() + sp.overrides.size());
        for (const SequencePlayerComponent::NamedOverride& n : sp.namedOverrides)
        {
            const auto it = std::find_if(seq.bindings.begin(), seq.bindings.end(),
                                         [&](const SequenceBinding& b) { return b.name == n.name; });
            if (it == seq.bindings.end())
            {
                HE_LOG_THROTTLE(Animation, Warning, 5.0,
                                "Entity %u: the sequence has no binding named \"%s\" — "
                                "sequence.bindSlot binds nothing", static_cast<uint32_t>(player),
                                n.name.c_str());
                continue;
            }
            all.push_back({ it->slot, n.entity });
        }
        all.insert(all.end(), sp.overrides.begin(), sp.overrides.end());
        sp.slots = HE::SequenceEval::resolveBindings(world, seq, all);
    }
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

// ── Camera ───────────────────────────────────────────────────────────────────
// Who holds the camera, in the registry's context rather than on a player: the
// lease has to outlive its owner, because an owner destroyed mid-cutscene still
// leaves its camera on screen, and somebody has to hand the view back.
struct CameraLease
{
    entt::entity   owner          = entt::null;
    // What was on screen when the sequence took over, and its world pose then:
    // where the view goes back to, and where a blend into the first cut starts.
    entt::entity   gameplayCamera = entt::null;
    HE::SolvedPose gameplayPose;   // valid == false: there was no camera to blend from
    // The owner's blend-out, copied every frame it holds the camera so a
    // destroyed owner still hands back the way it was authored.
    float          blendOutSeconds = 0.0f;
    HE::BlendCurve blendOutCurve   = HE::BlendCurve::SmoothStep;
    // The camera whose transform holds a BLENDED pose right now, and what it held
    // before that write. Put back before the tracks run again: the blend is this
    // frame's output, never the camera's state (the rig's law, CameraPose.h).
    entt::entity   blended = entt::null;
    glm::vec3      savedPosition{ 0.0f };
    glm::vec3      savedRotation{ 0.0f };
    float          savedFovOffset = 0.0f;
};

// The camera on screen: isMain, else the first — the renderer's rule.
entt::entity shownCamera(entt::registry& reg)
{
    entt::entity found = entt::null;
    for (auto [e, cam] : reg.view<CameraComponent>().each())
    {
        if (found == entt::null) found = e;
        if (cam.isMain) return e;
    }
    return found;
}

bool isCamera(const entt::registry& reg, entt::entity e)
{
    return e != entt::null && reg.valid(e) && reg.all_of<CameraComponent, TransformComponent>(e);
}

// A camera's world pose NOW — composed from the parent chain, not read out of
// worldMatrix, because the tracks have just moved it and nothing has propagated.
HE::SolvedPose cameraWorldPose(HorizonWorld& world, entt::entity e)
{
    HE::SolvedPose p;
    const glm::mat4 m = HE::worldMatrixOf(world, e);
    p.position = glm::vec3(m[3]);
    glm::mat3 basis(m);
    for (int i = 0; i < 3; ++i)
    {
        const float len = glm::length(basis[i]);
        if (len > 1e-6f) basis[i] /= len;
    }
    p.rotation     = glm::normalize(glm::quat_cast(basis));
    p.eulerDegrees = glm::degrees(glm::eulerAngles(p.rotation));
    if (const auto* cam = world.registry().try_get<CameraComponent>(e))
        p.fovDegrees = cam->fovDegrees + cam->fovOffset;
    p.valid = true;
    return p;
}

// Put a world pose on a camera: the transform in its parent's space, the FOV as
// an offset against the authored value (fovDegrees is never written).
void writeCameraPose(HorizonWorld& world, entt::entity e, const HE::SolvedPose& p)
{
    auto& reg = world.registry();
    auto* t = reg.try_get<TransformComponent>(e);
    if (!t) return;
    const auto* h = reg.try_get<HierarchyComponent>(e);
    const entt::entity parent = h ? h->parent : entt::null;
    if (parent == entt::null || parent == world.rootEntity() || !reg.valid(parent))
    {
        t->position = p.position;
        t->rotation = p.eulerDegrees;
    }
    else
    {
        const glm::mat4 desired =
            glm::translate(glm::mat4(1.0f), p.position) * glm::mat4_cast(p.rotation);
        const glm::mat4 local = glm::inverse(HE::worldMatrixOf(world, parent)) * desired;
        t->position = glm::vec3(local[3]);
        t->rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(local)));
    }
    t->dirty = true;
    if (auto* cam = reg.try_get<CameraComponent>(e))
        cam->fovOffset = p.fovDegrees - cam->fovDegrees;
}

void restoreBlended(entt::registry& reg, CameraLease& lease)
{
    if (lease.blended == entt::null) return;
    if (reg.valid(lease.blended))
    {
        if (auto* t = reg.try_get<TransformComponent>(lease.blended))
        {
            t->position = lease.savedPosition;
            t->rotation = lease.savedRotation;
            t->dirty    = true;
        }
        if (auto* cam = reg.try_get<CameraComponent>(lease.blended))
            cam->fovOffset = lease.savedFovOffset;
    }
    lease.blended = entt::null;
}

// Give the view back to the camera that had it, and end the lease.
void handBack(HorizonWorld& world, CameraLease& lease)
{
    auto& reg = world.registry();
    const entt::entity shown = shownCamera(reg);
    const entt::entity back  = lease.gameplayCamera;
    if (isCamera(reg, back) && back != shown)
    {
        if (reg.all_of<CameraRigComponent>(back))
        {
            // blendTo freezes the pose on screen (the cutscene camera has no rig)
            // as its source — BEFORE the blended write is taken back below, so a
            // sequence that ends mid-blend leaves from where the view really is.
            const bool haveLast = isCamera(reg, shown);
            const HE::SolvedPose last = haveLast ? cameraWorldPose(world, shown) : HE::SolvedPose{};
            HE::CameraRigController::blendTo(world, back, lease.blendOutSeconds, lease.blendOutCurve);
            // The rig is not solved again until next frame's controller, and
            // this frame is drawn from the rig camera's transform as it stood
            // before the cutscene. Give it the cutscene's last pose instead;
            // the rig overwrites it next frame, blending or cutting from there.
            if (haveLast) writeCameraPose(world, back, last);
        }
        else
        {
            // No rig, nothing to blend: a cut. And no pose written — a fly
            // camera would be teleported to where the cutscene ended.
            HE::CameraRigController::makeMain(reg, back);
        }
    }
    // No gameplay camera to go back to (a menu level, or it was destroyed):
    // the view stays on the cutscene's last camera.
    restoreBlended(reg, lease);
    if (reg.valid(lease.owner))
        if (auto* sp = reg.try_get<SequencePlayerComponent>(lease.owner)) sp->cameraOwned = false;
    reg.ctx().erase<CameraLease>();
}

// Hand the view back if `owner` holds it; clear a stale flag otherwise.
void releaseCamera(HorizonWorld& world, entt::entity owner, SequencePlayerComponent& sp)
{
    auto& reg = world.registry();
    if (auto* lease = reg.ctx().find<CameraLease>(); lease && lease->owner == owner)
        handBack(world, *lease);
    sp.cameraOwned = false;
}

// The camera for one player's frame at its time: take, hold, blend, or let go.
void cameraFrame(HorizonWorld& world, entt::entity owner, SequencePlayerComponent& sp,
                 const HE::SequenceEval::CameraState& cam)
{
    auto& reg = world.registry();

    entt::entity live = entt::null;
    if (sp.playing && cam.active)
    {
        live = slotEntity(reg, sp, cam.slot);
        if (live != entt::null && !isCamera(reg, live))
        {
            HE_LOG_THROTTLE(Animation, Warning, 5.0,
                            "Entity %u: the sequence cuts to slot %u, which is not a camera — "
                            "the view stays with gameplay", static_cast<uint32_t>(owner),
                            static_cast<unsigned>(cam.slot));
            live = entt::null;
        }
    }

    CameraLease* lease = reg.ctx().find<CameraLease>();
    const bool   mine  = lease && lease->owner == owner;
    if (live == entt::null)
    {
        // Before the first cut, after a cut to no camera, or with the cut's
        // camera missing: the view is gameplay's.
        if (mine) handBack(world, *lease);
        return;
    }
    if (!mine)
    {
        // One sequence holds the camera at a time; this one waits its turn.
        if (lease) return;
        CameraLease fresh;
        fresh.owner          = owner;
        fresh.gameplayCamera = shownCamera(reg);
        if (isCamera(reg, fresh.gameplayCamera))
            fresh.gameplayPose = cameraWorldPose(world, fresh.gameplayCamera);
        // After the pose is taken: releasing zeroes the rig's FOV offset, and the
        // blend into the first cut starts from the FOV that was on screen.
        HE::CameraRigController::releaseAll(reg);
        lease = &reg.ctx().emplace<CameraLease>(fresh);
        sp.cameraOwned = true;
    }
    lease->blendOutSeconds = (std::max)(0.0f, sp.blendOutSeconds);
    lease->blendOutCurve   = sp.blendOutCurve;

    HE::CameraRigController::makeMain(reg, live);
    if (!cam.blending) return;

    HE::SolvedPose from;
    if (cam.fromGameplay)
        from = lease->gameplayPose;
    else if (const entt::entity src = slotEntity(reg, sp, cam.fromSlot); isCamera(reg, src))
        from = cameraWorldPose(world, src);
    // Nothing to come from (no gameplay camera, the previous cut's camera is
    // gone): the cut is a cut.
    if (!from.valid) return;

    const HE::SolvedPose to = cameraWorldPose(world, live);
    HE::SolvedPose shown = to;
    shown.position     = glm::mix(from.position, to.position, cam.alpha);
    // Slerp, not Euler numbers: 170° to −170° is 20° across the seam.
    shown.rotation     = glm::slerp(from.rotation, to.rotation, cam.alpha);
    shown.eulerDegrees = glm::degrees(glm::eulerAngles(shown.rotation));
    shown.fovDegrees   = glm::mix(from.fovDegrees, to.fovDegrees, cam.alpha);

    auto& t = reg.get<TransformComponent>(live);
    lease->blended        = live;
    lease->savedPosition  = t.position;
    lease->savedRotation  = t.rotation;
    lease->savedFovOffset = reg.get<CameraComponent>(live).fovOffset;
    writeCameraPose(world, live, shown);
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
    auto& reg = world.registry();
    auto* sp  = reg.try_get<SequencePlayerComponent>(player);
    if (!sp) return false;
    // Switched off: begin() stops a sequence whose owner is off, so starting one
    // here would only buy a SequenceFinished on the next frame.
    if (!HE::isEntityActive(reg, player)) return false;
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
    // A skip is a stop, and whatever waits for the end has to run after a skip
    // too. begin() sends it; a natural end that already did leaves playing false.
    if (sp->playing) sp->finishedPending = true;
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

void SequenceSystem::bindSlotByName(HorizonWorld& world, entt::entity player, const std::string& name,
                                    entt::entity target)
{
    auto* sp = world.registry().try_get<SequencePlayerComponent>(player);
    if (!sp || name.empty()) return;
    auto& list = sp->namedOverrides;
    auto  it   = std::find_if(list.begin(), list.end(),
                              [&](const SequencePlayerComponent::NamedOverride& o) { return o.name == name; });
    if (target == entt::null)
    {
        if (it != list.end()) list.erase(it);
    }
    else if (it != list.end()) it->entity = target;
    else                       list.push_back({ name, target });
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
        // Switched off while playing: over. Before the sound and the end below,
        // so this frame already stops the sounds and sends SequenceFinished, and
        // apply() hands the camera back.
        if (sp.playing && active.off(e)) stop(world, e);
        if (sp.finishedPending)
        {
            sp.finishedPending = false;
            if (notifies)
                notifies->push_back({ static_cast<uint32_t>(e), SequenceSystem::kSequenceFinished,
                                      HE::AnimationNotifyEvent::Kind::Fire });
        }

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
        if (!sp.bindingsResolved) resolve(world, e, *seq, sp);

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
    auto& reg = world.registry();
    if (!ctx)
    {
        // No session, no lease. The editor's edit frames land here between two
        // play sessions, and a lease that survived into the next one would name
        // entities of a scene that has since been reloaded.
        reg.ctx().erase<CameraLease>();
        return;
    }

    if (auto* lease = reg.ctx().find<CameraLease>())
    {
        const auto* owner = reg.valid(lease->owner)
                          ? reg.try_get<SequencePlayerComponent>(lease->owner) : nullptr;
        // Destroyed mid-cutscene, or stopped since last frame: hand back FIRST.
        // Last frame's blended write is still on the camera, and it is the pose
        // on screen — the hand-over starts there (handBack takes the write back
        // itself, after reading it). Skipping a cutscene mid-blend would jump
        // otherwise.
        if (!owner || (owner->cameraOwned && !owner->playing))
            handBack(world, *lease);
        else
        {
            // Last frame's blend is taken back before any track runs, so the
            // live camera's keys (or where it was placed) are what they see.
            restoreBlended(reg, *lease);
            if (!owner->cameraOwned) reg.ctx().erase<CameraLease>();   // stale
        }
    }

    std::vector<JointTRS> localTRS;
    for (auto [e, sp] : reg.view<SequencePlayerComponent>().each())
    {
        // Stopped since last frame (stop(), or the end): the view goes back.
        if (!sp.playing && sp.cameraOwned) releaseCamera(world, e, sp);

        if (!sp.playing && !sp.applyOnce) continue;
        const SequenceAsset* seq = cm.getSequence(sp.sequenceId);
        if (!seq) continue;

        // Properties first: the skeletons below may carry IK, and IK casts from
        // the transform the sequence has just written. The camera after them —
        // its blend reads the poses the property tracks have just written.
        const HE::SequenceEval::Result frame = HE::SequenceEval::evaluate(*seq, sp.time);
        HE::SequenceEval::apply(world, cm, frame, sp.slots);
        cameraFrame(world, e, sp, frame.camera);

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
            // In the same frame: the last frame's camera pose is what the
            // hand-over starts from, and a frame later it would be one old.
            if (sp.cameraOwned) releaseCamera(world, e, sp);
            // Into the queue drained right after tickAnimation, so a handler
            // sees the camera already on its way back and the actors on the
            // last frame.
            if (notifies)
                notifies->push_back({ static_cast<uint32_t>(e), SequenceSystem::kSequenceFinished,
                                      HE::AnimationNotifyEvent::Kind::Fire });
        }
    }
}

bool SequenceSystem::ownsCamera(const entt::registry& reg)
{
    const auto* lease = reg.ctx().find<CameraLease>();
    if (!lease) return false;
    // An owner destroyed mid-cutscene still holds it until apply() hands it
    // back; an owner that does not know it holds it is a stale lease.
    if (!reg.valid(lease->owner)) return true;
    const auto* sp = reg.try_get<SequencePlayerComponent>(lease->owner);
    return !sp || sp->cameraOwned;
}

bool SequenceSystem::locksPlayerInput(const entt::registry& reg)
{
    for (auto [e, sp] : reg.view<const SequencePlayerComponent>().each())
        if (sp.playing && sp.lockPlayerInput) return true;
    return false;
}
