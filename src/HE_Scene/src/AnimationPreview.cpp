#include <HorizonScene/AnimationPreview.h>
#include "AnimationEval.h" // internal: sampleClip/composeBoneMatrices, same src/ tree

void AnimationPreview::evaluateClipPose(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                        float t, std::vector<glm::mat4>& outBoneMatrices)
{
    std::vector<JointTRS> localTRS(mesh.skeleton.size());
    sampleClip(clip, t, localTRS);
    composeBoneMatrices(mesh, localTRS, outBoneMatrices);
}
