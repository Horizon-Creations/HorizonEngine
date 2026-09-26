#pragma once
#include <ContentManager/Assets.h>
#include <entt/entt.hpp>

#include <cstdint>
#include <vector>

class HorizonWorld;
class ContentManager;

// ── Sequence evaluation ──────────────────────────────────────────────────────
// The one place that turns a cinematic Sequence and a time into what the world
// should look like at that time (docs/sequencer-cinematics-plan.md §3.1). The
// runtime player and the editor's preview both call it; what scrubbing shows is
// therefore what the game shows, by construction and not by two switches kept in
// step.
//
// Split in two on purpose:
//   evaluate()  pure: asset + time in, a list of values out. No world, no
//               ContentManager, no entities — a test needs nothing but a struct.
//   apply()     writes those values into a world through the SAME per-channel
//               write the Property Animator uses (PropertyAnimationSystem::
//               applyChannel), so a clip and a sequence cannot disagree about
//               what "Rotation Y = 90" does to an entity.
//
// What evaluate() does NOT cover, deliberately:
//   * Event and Audio tracks. Neither is a function of one instant — events fire
//     over a span (tPrev, tEnd], sound is started, not sampled — so they run only
//     while PLAYING, never while scrubbing (plan §3.1). The span rule is
//     HE::collectNotifySpan (AnimationNotify.h).
//   * Skeletal tracks: they need the pose pipeline (plan §2.4, step 3).
//   * The blended camera POSE. evaluate() says which camera is live, which one
//     it blends from and how far along the blend is; turning that into a pose
//     needs both cameras' world transforms, and SequenceSystem::apply does it
//     (see "Camera and input" in SequenceSystem.h).
namespace HE::SequenceEval {

// One property value for one bound actor.
struct PropertyWrite
{
    uint16_t   slot   = kSequenceNoBinding;
    PropTarget target = PropTarget::PosX;
    float      value  = 0.0f;
};

// Who holds the camera at this instant.
struct CameraState
{
    // False before the first cut and for a sequence without a camera-cut track:
    // the gameplay camera stays where it is.
    bool     active = false;
    // The camera the latest cut at or before t switched to.
    uint16_t slot = kSequenceNoBinding;

    // Within `blendIn` seconds after that cut, the view is still travelling from
    // the previous camera. `alpha` is the SHAPED progress (applyBlendCurve), 0 at
    // the cut and 1 once the blend is over; outside a blend it is 1.
    bool     blending = false;
    float    alpha    = 1.0f;
    // What it blends from: the previous cut's camera, or — for the very first cut
    // (or the first after a cut to no camera) — gameplay (`fromGameplay`,
    // `fromSlot` = none). The runtime freezes the gameplay camera's pose at the
    // moment the sequence takes the camera, i.e. at that cut, not at play():
    // until then the player may still move it. A cut to the camera that is
    // already live never blends.
    uint16_t fromSlot     = kSequenceNoBinding;
    bool     fromGameplay = false;
};

struct Result
{
    // One entry per property track that has a binding and at least one key, in
    // track order. Two tracks on the same slot and target both appear; apply()
    // writes them in order, so the later track wins.
    std::vector<PropertyWrite> writes;
    CameraState                camera;
};

// The whole sequence at time `t`. `t` is used as given: property channels hold
// their first/last key outside their range, and the last cut stays live past the
// end. Clamping or looping the playhead is the player's rule, not this one's.
//
// Only the FIRST camera-cut track counts; a sequence has one camera. Later ones
// are ignored (the editor offers one Camera Cut row, plan §3.6).
Result evaluate(const SequenceAsset& seq, float t);

// The entity each binding slot resolves to: index = slot, entt::null for a slot
// that is unbound or whose actor is missing. Resolution is by EntityIdComponent
// UUID (HorizonWorld::findByEntityId), never by name — the name is a label for
// the editor's "actor missing: Door_North". `overrides` (slot, entity) take
// precedence over the asset's UUID: a sequence that needs "the player" cannot
// know the UUID of a character spawned at runtime (plan §3.2).
struct SlotOverride { uint16_t slot; entt::entity entity; };
std::vector<entt::entity> resolveBindings(const HorizonWorld& world, const SequenceAsset& seq,
                                          const std::vector<SlotOverride>& overrides = {});

// Write `r.writes` into the world. A slot with no entity (unbound, missing
// actor) is skipped, never an error — a cutscene with one actor deleted still
// plays the rest. The camera state is NOT applied here: SequenceSystem::apply
// does that, because taking and giving back the camera is state over time.
void apply(HorizonWorld& world, ContentManager& cm, const Result& r,
           const std::vector<entt::entity>& slots);

} // namespace HE::SequenceEval
