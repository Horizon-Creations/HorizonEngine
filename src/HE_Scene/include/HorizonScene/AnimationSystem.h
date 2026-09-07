#pragma once

class HorizonWorld;
class ContentManager;
namespace HE { struct RootMotionContext; }

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
    void update(HorizonWorld& world, ContentManager& cm, float dt,
                HE::RootMotionContext* rootMotion = nullptr);
}
