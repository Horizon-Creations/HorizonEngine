#include <HorizonScene/AnimationPreview.h>
#include "AnimationEval.h" // internal: sampleClip/composeBoneMatrices/lockRootJoint, same src/ tree

#include <ContentManager/ContentManager.h>
#include <HorizonScene/AnimationIk.h>
#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>
#include <string>

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

void AnimationPreview::evaluateBlendSpacePose(const SkeletalMeshAsset& mesh, ContentManager& cm,
                                              const HE::BlendSpace& space, float x, float y,
                                              float phase, std::vector<glm::mat4>& outBoneMatrices,
                                              std::vector<float>* outWeights)
{
    std::vector<float> weights;
    HE::blendSpaceWeights(space, x, y, weights);

    // A sample whose clip is gone cannot contribute a pose, so it must not keep
    // its share of the weight either — the same renormalisation the runtime does,
    // because a preview that mixes differently from the game is not a preview.
    std::vector<const AnimationClipAsset*> clips(space.samples.size(), nullptr);
    for (size_t i = 0; i < space.samples.size(); ++i)
    {
        if (weights[i] <= 0.0f) continue;
        clips[i] = cm.getAnimationClip(space.samples[i].clipId);
        if (!clips[i] || clips[i]->duration <= 0.0f) { clips[i] = nullptr; weights[i] = 0.0f; }
    }
    float sum = 0.0f;
    for (float w : weights) sum += w;
    if (sum > 0.0f) for (float& w : weights) w /= sum;
    if (outWeights) *outWeights = weights;

    // Wrapped here and not by the caller: a diagram cursor has no playhead, and
    // fmod of a negative phase is negative.
    float p = std::fmod(phase, 1.0f);
    if (p < 0.0f) p += 1.0f;

    std::vector<std::vector<JointTRS>> poses(space.samples.size());
    for (size_t i = 0; i < space.samples.size(); ++i)
    {
        if (weights[i] <= 0.0f) continue;
        poses[i].assign(mesh.skeleton.size(), JointTRS{});
        sampleClip(*clips[i], p * clips[i]->duration, poses[i]);
    }

    std::vector<JointTRS> localTRS;
    HE::blendPosesN(poses, weights, localTRS);
    // Nothing usable: the bind pose, which is what the mesh preview shows for a
    // clip it cannot read either.
    if (localTRS.empty()) localTRS.assign(mesh.skeleton.size(), JointTRS{});
    composeBoneMatrices(mesh, localTRS, outBoneMatrices);
}

void AnimationPreview::evaluateClipPoseLookAt(const SkeletalMeshAsset& mesh,
                                              const AnimationClipAsset& clip, float t,
                                              const std::vector<std::string>& chain,
                                              const std::vector<float>& chainWeights,
                                              const glm::vec3& targetModel,
                                              const glm::vec3& forwardLocal,
                                              float maxYawDegrees, float maxPitchDegrees,
                                              float weight,
                                              std::vector<glm::mat4>& outBoneMatrices,
                                              glm::vec2* outAnglesDegrees)
{
    if (outAnglesDegrees) *outAnglesDegrees = glm::vec2(0.0f);

    std::vector<JointTRS> localTRS(mesh.skeleton.size());
    sampleClip(clip, t, localTRS);

    // The same two halves the runtime runs, in the same order — solve in the
    // model matrices, finish with the inverse bind. Falling back to
    // composeBoneMatrices when nothing resolves is not a shortcut but the point:
    // an empty chain has to leave the preview showing the plain clip.
    std::vector<glm::mat4> model;
    composeModelMatrices(mesh, localTRS, model);

    std::vector<int>   joints;
    std::vector<float> weights;
    for (size_t i = 0; i < chain.size(); ++i)
    {
        const int j = HE::findJointByName(mesh, chain[i]);
        if (j < 0) continue;
        joints.push_back(j);
        weights.push_back(i < chainWeights.size() ? chainWeights[i] : 1.0f);
    }

    if (!joints.empty())
    {
        const glm::vec2 angles = HE::lookAtAngles(mesh, joints, model, targetModel, forwardLocal,
                                                  maxYawDegrees, maxPitchDegrees);
        // No smoothing here, on purpose: a scrubbing preview has no previous
        // frame to ease from, and an author dragging a target wants to see where
        // it points, not where it is on its way to.
        HE::applyLookAt(mesh, joints, weights, angles, forwardLocal, weight, localTRS, model);
        if (outAnglesDegrees) *outAnglesDegrees = angles;
    }

    applyInverseBind(mesh, model, outBoneMatrices);
}
