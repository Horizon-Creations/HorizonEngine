#include <HorizonScene/AnimationPreview.h>
#include "AnimationEval.h" // internal: sampleClip/composeBoneMatrices/lockRootJoint, same src/ tree

#include <glm/gtc/quaternion.hpp>
#include <algorithm>

void AnimationPreview::evaluateClipPose(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                        float t, std::vector<glm::mat4>& outBoneMatrices)
{
    std::vector<JointTRS> localTRS(mesh.skeleton.size());
    sampleClip(clip, t, localTRS);
    composeBoneMatrices(mesh, localTRS, outBoneMatrices);
}

void AnimationPreview::evaluateClipPoseLocked(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                              float t, const HE::RootMotionOptions& opt,
                                              std::vector<glm::mat4>& outBoneMatrices)
{
    std::vector<JointTRS> localTRS(mesh.skeleton.size());
    sampleClip(clip, t, localTRS);
    // Same order as the three pose drivers: sample, lock, compose. A lock applied
    // after the forward kinematics would have to undo every child's world matrix.
    const int root = HE::findRootJoint(mesh, opt.rootJointName);
    if (root >= 0 && clip.hasRootMotion)
        lockRootJoint(localTRS, root, clip, opt);
    composeBoneMatrices(mesh, localTRS, outBoneMatrices);
}

void AnimationPreview::rootMotionPath(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                      const HE::RootMotionOptions& opt, int samples,
                                      std::vector<glm::vec3>& outPoints,
                                      std::vector<float>* outYawDegrees)
{
    outPoints.clear();
    if (outYawDegrees) outYawDegrees->clear();

    const int root = HE::findRootJoint(mesh, opt.rootJointName);
    if (root < 0 || !clip.hasRootMotion || clip.duration <= 0.0f) return;

    const int steps = std::clamp(samples, 2, 512);

    glm::vec3 pos{0.0f};
    float     yaw = 0.0f;   // degrees, accumulated heading
    outPoints.push_back(pos);
    if (outYawDegrees) outYawDegrees->push_back(yaw);

    for (int i = 1; i <= steps; ++i)
    {
        // Both ends off the same ladder rather than "previous + step": adding a
        // float `steps` times drifts, and the last point would miss the clip's end
        // by enough to lose a whole span of a short clip.
        const float t0 = clip.duration * (static_cast<float>(i - 1) / static_cast<float>(steps));
        const float t1 = clip.duration * (static_cast<float>(i)     / static_cast<float>(steps));

        const HE::RootMotionDelta d = HE::extractRootMotion(clip, root, t0, t1, opt);

        // The delta is in the character's frame at the START of the span, exactly
        // as HE::rootMotionApply receives it — so it is rotated by the heading the
        // path has reached, and only then added.
        pos += glm::angleAxis(glm::radians(yaw), glm::vec3(0.0f, 1.0f, 0.0f)) * d.translation;
        yaw += d.yawDegrees;

        outPoints.push_back(pos);
        if (outYawDegrees) outYawDegrees->push_back(yaw);
    }
}
