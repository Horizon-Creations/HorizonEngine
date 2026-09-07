#pragma once
class HorizonWorld;
class ContentManager;
namespace HE { struct RootMotionContext; }

namespace AnimationBlendSystem {
    // `rootMotion`: see AnimationSystem::update — nullptr extracts and locks but
    // moves nothing. Both clips' deltas are mixed with the same blendAlpha that
    // mixes the pose.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                HE::RootMotionContext* rootMotion = nullptr);
}
