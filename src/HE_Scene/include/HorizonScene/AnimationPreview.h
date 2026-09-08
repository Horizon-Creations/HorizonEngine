#pragma once
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPose.h>
#include <HorizonScene/RootMotion.h>
#include <glm/glm.hpp>
#include <vector>

class ContentManager;

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

    // The pose a blend space wears at the parameter point (x, y) and shared phase
    // `phase` — what the blend-space editor draws under the cursor it is dragging
    // around the diagram, with no entity and no state machine anywhere.
    //
    // `phase` is a normalised cycle position, NOT seconds: each sample is read at
    // `phase * its own duration`, which is exactly what the runtime does and the
    // whole reason clips of different length stay in step. It is wrapped into
    // [0, 1) here, because a scrubbing preview has no playhead to wrap it.
    //
    // `outWeights`, when given, receives one weight per sample — the numbers the
    // diagram writes next to its points. Same numbers the runtime uses, capped
    // and renormalised the same way, or the preview would be showing a mix the
    // game never plays.
    void evaluateBlendSpacePose(const SkeletalMeshAsset& mesh, ContentManager& cm,
                                const HE::BlendSpace& space, float x, float y, float phase,
                                std::vector<glm::mat4>& outBoneMatrices,
                                std::vector<float>* outWeights = nullptr);

    // The clip's pose with the look-at chain turned onto `targetModel`, so the
    // mesh preview can show what a look-at setting actually does before there is
    // a character in a scene to try it on.
    //
    // Look-at and NOT foot placement, and that split is the same one the runtime
    // makes: a foot needs a ground ray and there is no physics world here, so a
    // preview that solved one would be showing a pose the game never produces.
    // Turning a head needs nothing but the skeleton.
    //
    // `chain` is joint NAMES, root first — the same strings IkComponent holds, so
    // the preview and the component cannot disagree about what resolves. Names
    // the skeleton does not have are skipped, with their weights.
    //
    // `targetModel` is in the mesh's own space: there is no entity here to have a
    // world matrix, which is exactly why this exists.
    //
    // `outAnglesDegrees`, when given, receives the (yaw, pitch) actually used
    // after clamping — the numbers to put next to the sliders, so an author can
    // see that the limit and not the target is what is holding the head back.
    void evaluateClipPoseLookAt(const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                                float t, const std::vector<std::string>& chain,
                                const std::vector<float>& chainWeights,
                                const glm::vec3& targetModel, const glm::vec3& forwardLocal,
                                float maxYawDegrees, float maxPitchDegrees, float weight,
                                std::vector<glm::mat4>& outBoneMatrices,
                                glm::vec2* outAnglesDegrees = nullptr);
}
