#pragma once

class HorizonWorld;
class ContentManager;

namespace AnimationSystem
{
    // Advances all AnimatorComponents by dt and evaluates the referenced
    // AnimationClipAsset against the entity's SkeletalMeshComponent skeleton,
    // writing world-space bone matrices (joint * IBM) into boneMatrices.
    // Call every frame (editor always advances for preview; game loop too).
    void update(HorizonWorld& world, ContentManager& cm, float dt);
}
