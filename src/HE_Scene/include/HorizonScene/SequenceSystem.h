#pragma once
#include <entt/entt.hpp>
#include <cstdint>
#include <vector>

class HorizonWorld;
class ContentManager;
class AudioEngine;
class PhysicsWorld;
struct SequenceTrack;
struct SequenceSkeletalSection;
namespace HE {
struct AnimationNotifyEvent;
using NotifyQueue = std::vector<AnimationNotifyEvent>;

// What a play session hands the sequence players. Its null is the gate, the
// same shape as RootMotionContext and the notify queue: SceneSystems::
// tickAnimation is NOT gated on play mode, and a cutscene that ran in the
// editor's edit world would move its actors there — and that would be SAVED.
// So without a context no SequencePlayerComponent advances, writes or sounds;
// the editor's own scrub preview is a separate session (plan §3.6, step 6).
struct SequenceContext
{
    AudioEngine*  audio   = nullptr;   // null or uninitialised: sequences play silent
    PhysicsWorld* physics = nullptr;   // the ground IK looks for on a posed actor
};
}

// ── Sequence playback ────────────────────────────────────────────────────────
// Runs every SequencePlayerComponent: the clock, the actors, the events, the
// sound (docs/sequencer-cinematics-plan.md §3.5). The world state at a time is
// HE::SequenceEval's; this is the part that is not a function of one instant.
//
// Two calls per frame, bracketing the other animation drivers in
// SceneSystems::tickAnimation, because the sequence has to win on two different
// grounds:
//
//   begin()  at the TOP. Advances each clock, fires events and starts sounds over
//            the span (tPrev, tEnd], and CLAIMS every skeleton a skeletal section
//            is posing at the new time (SkeletalMeshComponent::sequencePosed).
//            The clip, blend and state-machine drivers skip a claimed skeleton
//            entirely — no playhead advance, no notifies, no pose. That is what
//            makes the sequence the skeleton's BASE driver: the layer stack and
//            IK run once, on its pose, and not a second time with the warning
//            poseFinalize gives for two drivers (plan §3.3).
//   apply()  at the BOTTOM, after the Property Animator. Writes the property
//            tracks (the sequence wins over a clip on the same target), then the
//            claimed skeletons — after the transforms, so IK casts its foot rays
//            from where the actor now stands.
//
// A skeleton is only claimed when its clip and mesh are loaded. A claim that
// then wrote no pose would freeze the bones and starve layers and IK for as long
// as the clip streams; unclaimed, the actor's own animator simply keeps going.
//
// Sound is started by the SAME span rule as the events (HE::collectNotifySpan
// over the audio sections' start times), so laps and the first frame behave
// identically. Played backwards, nothing starts: a sound is not reversible.
// Jumping (setTime) into the middle of a section does not start it either;
// starting it part-way in is a seek, and v1 does not seek (plan §3.3). pause()
// pauses the sequence's sounds with the clock, play() resumes them.
namespace SequenceSystem
{
    void begin(HorizonWorld& world, ContentManager& cm, float dt,
               HE::SequenceContext* ctx, HE::NotifyQueue* notifies);
    void apply(HorizonWorld& world, ContentManager& cm, float dt,
               HE::SequenceContext* ctx, HE::NotifyQueue* notifies);

    // ── Transport ────────────────────────────────────────────────────────────
    // The C++ half of the script rows in step 5. None of them needs a play
    // session to be called; they change state that the next begin()/apply()
    // acts on, and do nothing on an entity without a SequencePlayerComponent.

    // Resume a paused player; otherwise start one. A player standing at its end
    // (or at 0 when it plays backwards) starts over; one moved with setTime while
    // stopped starts there. Resolves the bindings. False without a component.
    bool play(HorizonWorld& world, ContentManager& cm, entt::entity player);
    // Hold the clock. The actors stay owned: the frame keeps being written, so
    // another driver cannot take them over mid-cutscene.
    void pause(HorizonWorld& world, entt::entity player);
    // Stop and rewind to 0, and stop the sounds this sequence started. The actors
    // keep whatever the last written frame left them in.
    void stop(HorizonWorld& world, entt::entity player);
    // Move the playhead, clamped to [0, duration] (or wrapped, for a looping
    // player). Nothing between the old and the new time fires — the next span
    // starts here. A stopped player writes the new frame once.
    void setTime(HorizonWorld& world, ContentManager& cm, entt::entity player, float t);
    // Point a binding slot at `target` for this player (entt::null clears the
    // override). Takes effect at once if it is playing.
    void bindSlot(HorizonWorld& world, entt::entity player, uint16_t slot, entt::entity target);

    // ── Skeletal section rule ────────────────────────────────────────────────
    // Pure, public for the tests. The live section at `t`: the one with the
    // LATEST start among those whose [start, end] contains t, the later-listed on
    // a tie — the rule the camera cuts use, so two abutting sections hand over at
    // the seam whichever order they are listed in. Null between sections.
    const SequenceSkeletalSection* activeSection(const SequenceTrack& track, float t);
    // The clip time a section plays at sequence time `t`:
    // clipOffset + (t - start) * playRate, wrapped into [0, clipDuration) when the
    // section loops, clamped to [0, clipDuration] when it does not.
    float sectionClipTime(const SequenceSkeletalSection& s, float t, float clipDuration);
}
