#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <vector>

struct SkeletalMeshComponent {
    HE::UUID               meshAssetId;
    // One matrix per skeleton joint.  Identity matrices produce the bind pose.
    // Sized to the joint count when an AnimatorComponent sets them; otherwise a
    // single identity matrix is used (safe default for non-animated skeletons).
    std::vector<glm::mat4> boneMatrices  = { glm::mat4(1.0f) };
    bool                   visible        = true;  // extractor skips invisible
    bool                   castsShadow    = true;
    bool                   receivesShadow = true;
    bool                   dirty          = true;
    // Runtime, never serialized: a cinematic Sequence poses this skeleton this
    // frame. Set by SequenceSystem::begin, and the clip, blend and state-machine
    // drivers skip the entity while it is — see SequenceSystem.h.
    bool                   sequencePosed  = false;
};
