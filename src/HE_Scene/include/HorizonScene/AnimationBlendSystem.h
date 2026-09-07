#pragma once
#include <vector>

class HorizonWorld;
class ContentManager;
namespace HE {
struct RootMotionContext;
struct AnimationNotifyEvent;
using NotifyQueue = std::vector<AnimationNotifyEvent>;
}

namespace AnimationBlendSystem {
    // `rootMotion`: see AnimationSystem::update — nullptr extracts and locks but
    // moves nothing. Both clips' deltas are mixed with the same blendAlpha that
    // mixes the pose.
    //
    // `notifies`: also see AnimationSystem::update. Unlike the deltas, only ONE
    // of the two clips fires — the one carrying the greater weight. Two clips
    // firing at once would give two footsteps for every blended step, which is
    // the complaint this feature would earn rather than the feature itself.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                HE::RootMotionContext* rootMotion = nullptr,
                HE::NotifyQueue* notifies = nullptr);
}
