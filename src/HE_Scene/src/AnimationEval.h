#pragma once
// Internal header: shared between the animation systems (clip, blend, state
// machine, property). Not part of the public include path.
#include <ContentManager/Assets.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

// Per-joint local transform (before FK + IBM).
struct JointTRS
{
    glm::vec3 translation = glm::vec3(0.0f);
    glm::quat rotation    = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // w,x,y,z identity
    glm::vec3 scale       = glm::vec3(1.0f);
};

// Advance a playhead by dt and wrap/stop it — the shared rule of the clip, blend
// and property animators (the state machine's playhead has no `playing` flag and
// clamps without stopping, so it keeps its own advance).
// looping: wrap into [0, duration) — fmod returns a NEGATIVE remainder for a
//          rewinding playbackSpeed, which is corrected by adding one duration.
// else:    clamp at the end and clear `playing`.
// duration must be > 0 (every caller checks that before sampling anyway).
void advancePlayback(float& playbackTime, bool& playing,
                     float playbackSpeed, bool looping, float duration, float dt);

// Sample all channels of clip at time t, writing one JointTRS per joint into localTRS.
// localTRS must already be sized to the skeleton joint count (filled with defaults).
void sampleClip(const AnimationClipAsset& clip, float t, std::vector<JointTRS>& localTRS);

// Forward-kinematics: accumulate world joint matrices from localTRS (parent < child assumed),
// multiply each by the joint's IBM, and write the result into boneMatrices (resized to jointCount).
void composeBoneMatrices(const SkeletalMeshAsset& mesh,
                         const std::vector<JointTRS>& localTRS,
                         std::vector<glm::mat4>&       boneMatrices);

// Per-joint linear blend between two TRS sets: lerp translation + scale, slerp rotation.
// out is resized to min(a.size(), b.size()).
void blendTRS(const std::vector<JointTRS>& a,
              const std::vector<JointTRS>& b,
              float                        alpha,
              std::vector<JointTRS>&       out);
