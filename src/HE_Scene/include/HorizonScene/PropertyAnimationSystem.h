#pragma once
class HorizonWorld;
class ContentManager;
struct PropertyAnimChannel;

namespace PropertyAnimationSystem {
    void update(HorizonWorld& world, ContentManager& cm, float dt);

    // One channel's value at time `t`: linear between the bracketing keys, held
    // at the first key before it and at the last key after it, 0 for an empty
    // channel. Declared rather than kept file-static because the Sequencer
    // prints the value under its playhead with THIS function — a readout that
    // interpolated by its own rule would be right until the two disagreed.
    float sampleChannel(const PropertyAnimChannel& ch, float t);
}
