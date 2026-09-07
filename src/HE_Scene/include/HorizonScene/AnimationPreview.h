#pragma once
#include <ContentManager/Assets.h>
#include <HorizonScene/RootMotion.h>
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

    // The same pose, with the root joint locked exactly as `opt` says — which is
    // the pose a character with root motion actually wears, because the motion has
    // been taken out of it and put onto the entity. A preview that skips the lock
    // shows the mesh sliding away from its own feet and then contradicts the game.
    void evaluateClipPoseLocked(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                float t, const HE::RootMotionOptions& opt,
                                std::vector<glm::mat4>& outBoneMatrices);

    // Where one lap of `clip` would take a character: the root motion of the whole
    // clip integrated the way HE::rootMotionApply integrates it frame by frame —
    // each span's translation rotated by the heading accumulated so far, each
    // span's yaw added to that heading.
    //
    // The path is in the character's own frame at t = 0: it starts at the origin
    // facing forward, so a caller draws it by rotating it into the entity's world
    // yaw and offsetting by its world position. Not in world space, because the
    // useful question ("what does this CLIP do") has no entity in it — the mesh
    // editor asks it with no scene at all.
    //
    // `outPoints` gets `samples + 1` points (the origin plus one per span);
    // `outYawDegrees`, when given, the heading at each of them. `samples` is
    // clamped to a sane range: the path is drawn, and a million-segment polyline
    // is not more informative than a five-hundred-segment one.
    //
    // Empty for a clip with no duration, no root joint, or `hasRootMotion` off —
    // the same three answers that make the systems extract nothing, so the preview
    // shows a path exactly when the game would move.
    void rootMotionPath(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                        const HE::RootMotionOptions& opt, int samples,
                        std::vector<glm::vec3>& outPoints,
                        std::vector<float>* outYawDegrees = nullptr);
}
