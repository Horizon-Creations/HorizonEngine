#pragma once
#include <vector>

class HorizonWorld;
class ContentManager;
class AnimatorHost;
struct AnimatorStateMachineComponent;
namespace HE {
struct RootMotionContext;
struct AnimationNotifyEvent;
using NotifyQueue = std::vector<AnimationNotifyEvent>;
}

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
    //
    // `rootMotion`: see AnimationSystem::update. A crossfade has TWO playheads,
    // and each contributes its own delta over its own span; the two are mixed
    // with the same alpha the pose is mixed with.
    //
    // `notifies`: see AnimationSystem::update. During a crossfade only the
    // heavier playhead fires; the lighter one is still walked past, so it cannot
    // hold back a frame-0 notify and dump it the instant the weight tips over.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                AnimatorHost* sync = nullptr,
                HE::RootMotionContext* rootMotion = nullptr,
                HE::NotifyQueue* notifies = nullptr);

    // Force a re-resolve on the next update() — call after editing the graph of
    // the AnimatorStateMachineAsset this component references (e.g. from the
    // Animator State Machine Editor).
    void markConfigDirty(AnimatorStateMachineComponent& sm);
}
