#pragma once
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
};
