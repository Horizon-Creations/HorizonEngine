#pragma once
#include <ContentManager/Assets.h>
#include <glm/glm.hpp>
#include <vector>

// Public wrapper around the internal clip-sampling + forward-kinematics pipeline
// (AnimationEval.h/.cpp, shared by AnimationSystem/AnimationBlendSystem) for
// callers that have no ECS entity to evaluate against — e.g. the editor's
// Skeletal Mesh preview, which scrubs a clip against a mesh asset directly.
namespace AnimationPreview
{
    // Samples `clip` at time `t` and composes one bone matrix per joint of
    // `mesh`'s skeleton. `t` is not clamped/looped by this call — callers wrap
    // time themselves (e.g. fmod against the clip's duration) if looping.
    void evaluateClipPose(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                          float t, std::vector<glm::mat4>& outBoneMatrices);
}
