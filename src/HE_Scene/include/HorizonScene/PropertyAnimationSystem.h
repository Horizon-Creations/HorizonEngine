#pragma once
#include <entt/entt.hpp>
#include <cstdint>

class HorizonWorld;
class ContentManager;
struct PropertyAnimChannel;
struct PropertyAnimClipAsset;
enum class PropTarget : uint8_t;

namespace PropertyAnimationSystem {
    void update(HorizonWorld& world, ContentManager& cm, float dt);

    // A target that is a switch rather than a quantity (Visible): sampled as a
    // step, never interpolated — linear sampling would stand at 0.5 halfway
    // between an "on" key and an "off" key.
    bool isStepTarget(PropTarget t);

    // One channel's value at time `t`: linear between the bracketing keys (a
    // STEP for isStepTarget: the last key at or before t), held at the first key
    // before it and at the last key after it, 0 for an empty channel. Declared
    // rather than kept file-static because the Sequencer prints the value under
    // its playhead with THIS function — a readout that interpolated by its own
    // rule would be right until the two disagreed.
    float sampleChannel(const PropertyAnimChannel& ch, float t);

    // One value into one target of `e` — the single write both a clip
    // (applyAt) and a cinematic Sequence (HE::SequenceEval::apply) go through,
    // so the two cannot disagree about what a target means. A target whose
    // component `e` lacks is skipped. Material targets write the SHARED
    // MaterialAsset, as they always have.
    void applyChannel(HorizonWorld& world, ContentManager& cm, entt::entity e,
                      PropTarget target, float value);

    // Every channel of `clip` at time `t`, written into `e`'s Transform and
    // material — the per-entity half of update(), without the playhead. The
    // Sequencer's preview drives the entities that play this clip with it, so
    // what scrubbing shows in the viewport is what the runtime would write
    // there, by construction rather than by a second switch kept in step.
    void applyAt(HorizonWorld& world, ContentManager& cm, entt::entity e,
                 const PropertyAnimClipAsset& clip, float t);

    // The playhead rule the runtime uses for a Property Animator, exposed for
    // the Sequencer's transport: looping wraps into [0, duration), otherwise
    // the playhead stops at the end and `playing` goes false. `duration` > 0.
    void advance(float& playbackTime, bool& playing,
                 float playbackSpeed, bool looping, float duration, float dt);
}
