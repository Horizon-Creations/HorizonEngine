#pragma once
#include <HorizonScene/AnimationPose.h>   // HE::LayerBlendMode
#include <Types/UUID.h>
#include <cstdint>
#include <string>
#include <vector>

// ── AnimationLayerComponent ──────────────────────────────────────────────────
// Poses laid ON TOP of whatever posed this entity: an upper-body reload over a
// run, an aim offset over an idle, a hit reaction over both. Each layer has its
// own clip, its own playhead, its own weight, and a bone mask saying which part
// of the skeleton it is allowed to touch.
//
// A component of its own rather than layers inside the state-machine graph,
// because it is the shape this tree already uses for exactly this job:
// RootMotionComponent is an optional component that changes what the DRIVERS do
// instead of being built into one of them. The consequence is that layers work
// under all three drivers (clip, two-clip blend, state machine), cost nothing on
// an entity that has none, and do not force the state machine's whole runtime
// state into a vector. Unity's model (a state machine per layer) is the bigger
// one and is not ruled out — it arrives later as another Layer::Source.
//
// The stack is applied in index order, each layer onto the result of the one
// before it. Anything else cannot be worked out in one's head.
struct AnimationLayerComponent
{
    struct Layer
    {
        // Display only, for the inspector. Two layers may share a name.
        std::string name;

        enum class Source : uint8_t
        {
            Clip = 0,
            // A 1D/2D blend space (HE::BlendSpace): N clips mixed by where the
            // parameters stand. A layer has no parameters of its own and reads
            // the entity's AnimatorStateMachineComponent::params — the same map
            // the base pose is steered by, so a strafe set on a layer and the
            // state underneath it cannot disagree about the character's speed.
            BlendSpace = 1,
        };

        // A saved source is a raw int; an unknown one would be an enum with no
        // enumerator that every branch then falls through. Same guard as
        // HE::layerBlendModeFromInt, same reason.
        static constexpr Source sourceFromInt(int v)
        {
            switch (v)
            {
                case (int)Source::BlendSpace: return Source::BlendSpace;
                default:                      return Source::Clip;
            }
        }

        Source              source = Source::Clip;
        HE::LayerBlendMode  mode   = HE::LayerBlendMode::Override;

        HE::UUID clipId;        // Source::Clip
        HE::UUID blendSpaceId;  // Source::BlendSpace
        // Empty = no mask: the layer covers the whole skeleton at weight 1.
        HE::UUID maskId;

        // 0 = the layer contributes nothing — but its playhead keeps running, so
        // fading one back in does not restart it mid-stride.
        float weight = 1.0f;

        // ── Additive only ────────────────────────────────────────────────────
        // The pose the difference is taken against. Empty = the layer's own clip
        // at `additiveRefTime` (which defaults to 0), i.e. "the difference from
        // where this animation starts" — the ordinary way an additive clip is
        // authored. A separate reference clip is for the case where the base pose
        // of an additive set lives in its own file.
        HE::UUID additiveRefClipId;
        float    additiveRefTime = 0.0f;

        // ── Playhead, per layer ──────────────────────────────────────────────
        float playbackTime  = 0.0f;
        float playbackSpeed = 1.0f;
        bool  looping = true;
        bool  playing = true;

        // ── Runtime, not serialized ──────────────────────────────────────────
        // A playhead's first evaluated frame closes its span at the origin, so a
        // notify sitting exactly on frame 0 fires. Per layer, because each layer
        // has its own playhead. See HE::notifyCollectClip.
        bool notifiesPrimed = false;
    };

    std::vector<Layer> layers;

    // ── Mask resolution cache, not serialized ────────────────────────────────
    // Masks name joints; a skeleton has indices. The translation is done once per
    // (mask, skeleton) pair and kept here, in exactly the shape
    // AnimatorStateMachineComponent already uses for its own asset resolution:
    // a "what was this resolved against" stamp plus a dirty flag. Re-resolving
    // every frame would mean a string compare per joint per layer per character.
    HE::UUID                        resolvedForMeshId;
    std::vector<HE::UUID>           resolvedMaskIds; // what resolvedMasks was built from
    std::vector<std::vector<float>> resolvedMasks;   // one weight vector per layer
    // Belt and braces: `masksDirty` is what the inspector sets, but the cache
    // also checks resolvedMaskIds/resolvedForMeshId against reality every frame.
    // A flag that only ONE writer has to remember to set is a flag that a future
    // second writer will forget — and a mask that quietly keeps affecting the
    // wrong joints is a bug nobody would think to look for here.
    bool                            masksDirty = true;

    // Blend spaces this stack's layers have referenced, parsed once each. Not
    // serialized. Unlike the mask cache it is not skeleton-dependent — a blend
    // space is a list of clip ids and coordinates, and neither depends on which
    // mesh is wearing it.
    HE::BlendSpaceCache             blendSpaces;

    // Cleared by HE::poseBeginFrame at the top of the animation phase. An entity
    // may carry more than one pose driver (nothing stops it today) and the layer
    // stage must run exactly once per frame regardless: twice would advance every
    // layer playhead by 2·dt and fire every layer notify twice. Same device, and
    // the same reason, as RootMotionComponent::appliedThisFrame.
    bool finalizedThisFrame = false;
};
