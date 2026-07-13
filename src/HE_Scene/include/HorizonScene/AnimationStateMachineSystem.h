#pragma once
class HorizonWorld;
class ContentManager;
struct AnimatorStateMachineComponent;

namespace AnimationStateMachineSystem {
    void update(HorizonWorld& world, ContentManager& cm, float dt);

    // Force a re-resolve on the next update() — call after editing the graph of
    // the AnimatorStateMachineAsset this component references (e.g. from the
    // Animator State Machine Editor).
    void markConfigDirty(AnimatorStateMachineComponent& sm);
}
