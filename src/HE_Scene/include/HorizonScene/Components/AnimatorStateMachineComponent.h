#pragma once
#include <Types/UUID.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
#include <HorizonScene/AnimationPose.h>   // HE::BlendSpaceCache
#include <string>
#include <unordered_map>
#include <vector>

struct AnimatorStateMachineComponent
{
    HE::UUID stateMachineAssetId; // references an AnimatorStateMachineAsset authored in
                                  // the Animator State Machine Editor — {} = no states,
                                  // the component does nothing until one is assigned.

    // Resolved-graph cache — AnimationStateMachineSystem::update (re)computes this
    // whenever stateMachineAssetId changes or configDirty is set, NOT every frame:
    // re-parsing JSON per entity per frame for what's typically an asset shared by
    // many entities would be wasteful (same discipline as ParticleSystemComponent::
    // resolvedConfig). Call AnimationStateMachineSystem::markConfigDirty(component)
    // after editing the referenced asset's graph to force a re-resolve.
    HE::AnimatorStateMachineGraph resolvedGraph;
    HE::UUID                     resolvedFromAssetId;
    bool                         configDirty = true;

    // ── Runtime state ────────────────────────────────────────────────────────────
    // `params`/`currentStateName` ARE persisted per-entity (see SceneSerializer) even
    // though the graph itself lives in the asset: two entities sharing one state
    // machine asset (e.g. two independently moving characters) can be in different
    // states with different live param values.
    std::unordered_map<std::string, float> params; // live values, seeded from
                                                     // resolvedGraph.defaultParams
                                                     // whenever the graph (re)resolves
    std::string currentStateName;
    // THE OUTGOING PLAYHEAD, IN THE CURRENT STATE'S OWN UNIT — and that unit
    // depends on the state:
    //   * a CLIP state counts SECONDS and wraps at the clip's duration;
    //   * a BLEND SPACE state counts PHASE in [0, 1) and wraps at 1.
    // A blend space mixes clips of different lengths (walk 1.2 s, run 0.8 s) and
    // they only stay in step on a shared normalised cycle — on one absolute clock
    // they drift apart within a second and the feet cross. So the field carries
    // two meanings, and the only thing that keeps that honest is saying it here:
    // do NOT compare this against `curClip->duration` without asking which kind
    // of state you are in. HE::PoseSource::period() answers that.
    float       clipTime      = 0.0f;
    float       playbackSpeed = 1.0f;

    // Active crossfade (only meaningful when inTransition==true)
    bool        inTransition       = false;
    std::string transitionTarget;
    float       transitionElapsed  = 0.0f;
    float       transitionDuration = 0.2f;
    // The INCOMING playhead, in the incoming state's unit — the same double
    // meaning as clipTime, one state along.
    //
    // `transitionElapsed` used to double as this, and for clips it happens to be
    // the same number. For a blend space it is not: elapsed counts seconds of
    // crossfade, the incoming phase advances at 1/weightedDuration, and that
    // duration itself moves while the parameters move. Elapsed now measures the
    // crossfade and nothing else; this measures the incoming pose. Runtime only,
    // never saved (neither is transitionElapsed).
    float       transitionPlayhead = 0.0f;

    // A crossfade has TWO playheads, so it has two first frames to close. The
    // incoming one is un-primed again every time a transition STARTS — it is
    // being set back to 0, and a notify on the incoming clip's frame 0 is
    // exactly what "the attack begins" means. When the transition completes the
    // incoming flag moves onto the outgoing one, the same way transitionElapsed
    // moves onto clipTime, so the surviving playhead does not re-prime and dump
    // its frame-0 notify a second time. Runtime only, never saved.
    bool        clipNotifiesPrimed       = false;
    bool        transitionNotifiesPrimed = false;

    // Blend spaces this entity's states have referenced, parsed once each. Not
    // serialized, and cleared whenever the graph re-resolves — a state that just
    // changed which space it points at must not keep posing from the old one.
    HE::BlendSpaceCache blendSpaces;

    // ── Legacy migration staging (SceneSerializer only) ─────────────────────────
    // Scenes saved before the state machine became an asset (Forts. 70, e82137f)
    // had the whole graph INLINE on this component. SceneSerializer::load
    // populates this verbatim when the JSON has no "stateMachineAsset" key — the
    // serializer has no ContentManager dependency and adding one just for this
    // migration isn't worth it. AnimationStateMachineSystem::update (which already
    // has ContentManager access) converts it into a real AnimatorStateMachineAsset
    // on the first tick and clears hasData so it runs once.
    struct LegacyConfig
    {
        bool                                    hasData = false;
        std::vector<HE::AnimationState>         states;
        std::vector<HE::AnimationTransition>    transitions;
        std::unordered_map<std::string, float>  params;
        std::string                             currentStateName;
    } legacy;
};
