#pragma once
#include <HorizonScene/CameraPose.h>
#include <HorizonScene/SequenceEval.h>
#include <Types/UUID.h>
#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

// Plays a cinematic Sequence asset (docs/sequencer-cinematics-plan.md §3.5).
// The entity carrying it is the cutscene's owner: events on a track without an
// actor go to it, sound without an actor plays flat. The actors themselves are
// the sequence's bindings, resolved by EntityIdComponent UUID when playback
// starts.
//
// Driven by SequenceSystem, and only inside a play session — see
// SequenceSystem.h for why the editor's edit world never runs one.
struct SequencePlayerComponent
{
    // ── Authored (serialized) ────────────────────────────────────────────────
    HE::UUID sequenceId;
    // Start on the first frame of the play session. Off: something (a script,
    // step 5's sequence.play) has to start it.
    bool     autoplay = true;
    // Start over at the end. Off: the playhead stops on the last frame and the
    // actors keep the pose it left them in.
    bool     loop     = false;
    // 1 is as authored; negative plays backwards (events mirror, sound does not
    // start — see SequenceSystem.h).
    float    playRate = 1.0f;

    // How the view goes back to gameplay when the sequence lets go of the camera
    // (its end, stop(), a cut to no camera): CameraRigController::blendTo into
    // the camera that was showing before, over this many seconds. 0 is a cut,
    // and so is a gameplay camera without a rig — there is nothing to blend.
    // Here and not in the asset: the same sequence in a menu level has no
    // player rig to go back to (plan §3.4).
    float          blendOutSeconds = 0.0f;
    HE::BlendCurve blendOutCurve   = HE::BlendCurve::SmoothStep;
    // Silence the player's gameplay input while this sequence plays (paused
    // included): PlayerHost delivers no action events, the same silence a pause
    // or UI-only mode gives — so the actions marked "run while paused" still
    // arrive, which is what a skip key or the pause menu needs. Off by default:
    // a looping ambient sequence must not take the controls away for good.
    bool           lockPlayerInput = false;

    // ── Runtime (never serialized) ───────────────────────────────────────────
    // Everything below is session state. The editor's play snapshot is a
    // serialize/reload of the scene, so a second play session starts from these
    // defaults again and autoplays again.
    float time    = 0.0f;   // the playhead, in sequence seconds
    bool  playing = false;  // the sequence owns its actors: it writes them every frame
    bool  paused  = false;  // … but its clock stands still
    bool  started = false;  // autoplay has been consumed (or play() was called)
    // The next span opens with its origin closed, so an event sitting exactly
    // where playback starts (the ordinary "at 0" case) fires. Same flag, same
    // rule as an animator's notifiesPrimed.
    bool  primed  = false;
    // Write the state at `time` once even though nothing is playing: setTime on
    // a stopped player. Cleared by the frame that writes it.
    bool  applyOnce = false;
    // The natural end was reached this frame: write it, then stop.
    bool  finishing = false;

    // Slot → entity, resolved at play() and on bindSlot. Not per frame: the
    // lookup is a scan over every EntityIdComponent. An entity that dies
    // mid-sequence is checked with valid() at the write and skipped.
    std::vector<entt::entity>                    slots;
    // False after play() and bindSlot: the next frame resolves again (lazily,
    // because the sequence asset may still be streaming when play() runs).
    bool                                         bindingsResolved = false;
    // Slot overrides set before or during playback (sequence.bindSlot, step 5).
    // Precede the asset's UUID: "the player" is spawned at runtime and has no
    // UUID the asset could know.
    std::vector<HE::SequenceEval::SlotOverride>  overrides;

    // Sounds this sequence started, so a stop can stop them. Finished ones are
    // pruned as the list is walked.
    std::vector<uint64_t> audioHandles;
    // stop() asks, begin() does: stopping a sound needs the AudioEngine, which
    // the transport functions deliberately do not take.
    bool  stopAudio = false;
    // Whether audioHandles are paused right now; begin() brings them in line
    // with `paused`.
    bool  audioPaused = false;

    // This player holds the camera (SequenceSystem::ownsCamera). The lease
    // itself lives in the registry, because it has to outlive this component:
    // an owner destroyed mid-cutscene still has to hand the view back. This flag
    // is the owner's half of it — a lease whose owner does not say so (a scene
    // reloaded under the same entity ids) is stale and dropped.
    bool  cameraOwned = false;
};
