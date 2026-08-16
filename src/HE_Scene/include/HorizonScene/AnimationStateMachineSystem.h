#pragma once
class HorizonWorld;
class ContentManager;
class AnimatorHost;
struct AnimatorStateMachineComponent;

namespace AnimationStateMachineSystem {
    // `sync` (optional) is where the asset's sync graphs live. Each entity's
    // graph is fired HERE, immediately before that entity's transitions are
    // evaluated — so a parameter it writes reaches the transition it feeds in
    // the same frame. Firing it anywhere else would make the ordering something
    // two applications have to agree on by hand, which is exactly how the editor
    // ended up animating a frame behind the game.
    //
    // nullptr (edit mode, tests, a scene with no sync graphs) simply means the
    // parameters keep whatever they were last set to.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                AnimatorHost* sync = nullptr);

    // Force a re-resolve on the next update() — call after editing the graph of
    // the AnimatorStateMachineAsset this component references (e.g. from the
    // Animator State Machine Editor).
    void markConfigDirty(AnimatorStateMachineComponent& sm);
}
