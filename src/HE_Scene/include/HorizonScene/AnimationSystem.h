#pragma once
#include <vector>

class HorizonWorld;
class ContentManager;
namespace HE {
struct RootMotionContext;
struct AnimationNotifyEvent;
using NotifyQueue = std::vector<AnimationNotifyEvent>;
}

namespace AnimationSystem
{
    // Advances all AnimatorComponents by dt and evaluates the referenced
    // AnimationClipAsset against the entity's SkeletalMeshComponent skeleton,
    // writing world-space bone matrices (joint * IBM) into boneMatrices.
    // Call every frame (editor always advances for preview; game loop too).
    //
    // `rootMotion` gates only the APPLYING of root motion; extraction and the
    // root lock happen either way, so the pose is the same in the editor as in
    // the game. nullptr outside a play session — see HE::RootMotionContext.
    //
    // `notifies` is where the clip's timeline events are appended. nullptr means
    // they are not evaluated at all — and the playhead does not spend its one
    // priming either, so the first frame of a play session still fires a notify
    // authored on frame 0.
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                HE::RootMotionContext* rootMotion = nullptr,
                HE::NotifyQueue* notifies = nullptr);
}
