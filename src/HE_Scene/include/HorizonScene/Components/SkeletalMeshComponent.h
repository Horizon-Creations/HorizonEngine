#pragma once
#include <Types/UUID.h>
#include <Math/Math.h>
#include <cstdint>
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
    // Runtime, never serialized: what the clip, blend or state-machine driver
    // posed boneMatrices from, as a hash of its inputs (HE::PoseKey,
    // PoseFinalize.h). A driver whose inputs hash the same again leaves the bones
    // standing instead of sampling and composing them anew. 0 = nothing to keep:
    // every poseFinalize clears it, so any other writer of the bones must too.
    uint64_t               poseKey        = 0;
};
