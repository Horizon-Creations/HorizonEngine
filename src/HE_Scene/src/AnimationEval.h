#pragma once
// Internal header: shared between the animation systems (clip, blend, state
// machine, property). Not part of the public include path.
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPose.h>   // JointTRS + blendTRS (public: preview and tests need them)
#include <HorizonScene/RootMotion.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <vector>

// Advance a playhead by dt and wrap/stop it — the shared rule of the clip, blend
// and property animators (the state machine's playhead has no `playing` flag and
// clamps without stopping, so it keeps its own advance).
// looping: wrap into [0, duration) — fmod returns a NEGATIVE remainder for a
//          rewinding playbackSpeed, which is corrected by adding one duration.
// else:    clamp at the end and clear `playing`.
// duration must be > 0 (every caller checks that before sampling anyway).
void advancePlayback(float& playbackTime, bool& playing,
                     float playbackSpeed, bool looping, float duration, float dt);

// Sample ONE joint's channels at time t. The root-motion helpers need the root at
// an arbitrary time (both ends of a span, plus frame 0) and paying for the whole
// skeleton three times per frame to get it would be silly.
JointTRS sampleJoint(const AnimationClipAsset& clip, uint32_t jointIndex, float t);

// Sample all channels of clip at time t, writing one JointTRS per joint into localTRS.
// localTRS must already be sized to the skeleton joint count (filled with defaults).
void sampleClip(const AnimationClipAsset& clip, float t, std::vector<JointTRS>& localTRS);

// Forward-kinematics: accumulate world joint matrices from localTRS (parent < child assumed),
// multiply each by the joint's IBM, and write the result into boneMatrices (resized to jointCount).
void composeBoneMatrices(const SkeletalMeshAsset& mesh,
                         const std::vector<JointTRS>& localTRS,
                         std::vector<glm::mat4>&       boneMatrices);

// Neutralise the root joint in an already-sampled pose, so the motion that was
// just extracted is not ALSO still in the mesh. Only what `opt` extracts is
// locked. No-op for rootJoint < 0.
//
// Call it per clip, BEFORE any blend: each clip's own root motion has to come out
// against its own first frame, and a blended pose no longer knows which clip it
// came from.
void lockRootJoint(std::vector<JointTRS>& localTRS, int rootJoint,
                   const AnimationClipAsset& clip, const HE::RootMotionOptions& opt);
