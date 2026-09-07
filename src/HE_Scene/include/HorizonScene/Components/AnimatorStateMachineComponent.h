#pragma once
#include <Types/UUID.h>
#include <AnimatorStateMachine/AnimatorStateMachineGraph.h>
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
    float       clipTime      = 0.0f;
    float       playbackSpeed = 1.0f;

    // Active crossfade (only meaningful when inTransition==true)
    bool        inTransition       = false;
    std::string transitionTarget;
    float       transitionElapsed  = 0.0f;
    float       transitionDuration = 0.2f;

    // A crossfade has TWO playheads, so it has two first frames to close. The
    // incoming one is un-primed again every time a transition STARTS — it is
    // being set back to 0, and a notify on the incoming clip's frame 0 is
    // exactly what "the attack begins" means. When the transition completes the
    // incoming flag moves onto the outgoing one, the same way transitionElapsed
    // moves onto clipTime, so the surviving playhead does not re-prime and dump
    // its frame-0 notify a second time. Runtime only, never saved.
    bool        clipNotifiesPrimed       = false;
    bool        transitionNotifiesPrimed = false;

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
