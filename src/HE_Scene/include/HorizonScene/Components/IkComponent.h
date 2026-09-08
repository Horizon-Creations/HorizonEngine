#pragma once
#include <Types/UUID.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>
#include <vector>

// ── IkComponent ──────────────────────────────────────────────────────────────
// What this entity does to its own pose AFTER the animation has decided what it
// is: put the feet on the ground that is actually there, and point the head at
// the thing it is looking at.
//
// Its own component, orthogonal to all three pose drivers — the same shape, and
// the same reason, as RootMotionComponent and AnimationLayerComponent. It costs
// nothing when it is absent, and it works under a clip, a two-clip blend or a
// state machine without any of them knowing about it.
//
// The settings only; the arithmetic is HE::AnimationIk (public, ECS-free) and
// the stage that drives it is HE::poseFinalize (internal, PoseFinalize.h).
struct IkComponent
{
    // One leg. Ends at a foot, reaches two joints up, and is solved against a
    // point found by a ray straight down from where the foot currently is.
    struct FootIk
    {
        // Empty hip/knee = "the foot's parent and its parent", which is right for
        // every humanoid rig and wrong for none anybody exports. Named anyway,
        // because a digitigrade leg (a dog, a bird) has an extra joint and the
        // two above the foot are then not the ones that should bend.
        std::string footJoint, kneeJoint, hipJoint;

        float weight = 1.0f;

        // How far above the foot the ray starts and how far below it ends. The
        // upward part is what lets a foot find a step it is already standing
        // INSIDE of after the animation put it through the geometry.
        float traceUp = 0.5f, traceDown = 0.6f;

        // Ankle to sole. No skeleton knows this by itself — the foot joint sits
        // inside the ankle, and putting the ANKLE on the ground buries the foot.
        float footHeightOffset = 0.0f;

        bool  alignToNormal = true;
        float maxPitchDegrees = 30.0f, maxRollDegrees = 20.0f;

        // Metres per second of catching up, exponentially. Without it the foot
        // jumps a whole step height in one frame at every stair edge, because
        // that is exactly what the ground under it does.
        float interpSpeed = 10.0f;

        // ── Runtime, not serialized ──────────────────────────────────────────
        float smoothedOffset = 0.0f;   // metres along the character's up
        bool  primed         = false;  // first evaluation snaps instead of easing
    };

    std::vector<FootIk> feet;

    // The joint the whole body hangs off, lowered by the deepest foot's offset so
    // the low leg does not have to overextend. Empty = the common ancestor of the
    // feet, which on a normal rig is the pelvis.
    bool        adjustPelvis = true;
    std::string pelvisJoint;

    // Head and spine, pointed at something.
    struct LookAt
    {
        bool enabled = false;

        // Root first. One to three joints is the usual answer (spine, neck,
        // head); the weights say how much of the turn each one takes and are
        // normalised at use, so they do not have to sum to anything.
        std::vector<std::string> chain;
        std::vector<float>       chainWeights;

        // A target ENTITY beats targetWorld when it is set — a character looking
        // at another character is the case this exists for, and its position is
        // read through HE::worldPositionOf and never off TransformComponent
        // ::worldMatrix, which is a frame old inside the animation phase.
        HE::UUID  targetEntityId;
        glm::vec3 targetWorld{ 0.0f };

        // Which way the head looks in its own frame. -Z is this engine's forward
        // (EngineApi turns a rotation into a direction with q * vec3(0,0,-1)),
        // but an imported rig can carry any convention at all and there is no way
        // to guess it from the skeleton.
        glm::vec3 forwardLocal{ 0.0f, 0.0f, -1.0f };

        float weight = 1.0f;
        float maxYawDegrees = 70.0f, maxPitchDegrees = 45.0f;
        float interpSpeed = 8.0f;

        // ── Runtime, not serialized ──────────────────────────────────────────
        glm::vec2 smoothedAngles{ 0.0f };
        bool      primed = false;
    } lookAt;

    // ── Runtime, not serialized ─────────────────────────────────────────────
    // Joint names resolved against one skeleton. Rebuilt when the mesh changes or
    // when a name is edited; the same cache shape AnimationLayerComponent uses
    // for its masks, and for the same reason — a name lookup per foot per frame
    // is a string compare per joint per frame.
    HE::UUID          resolvedForMeshId;
    std::vector<glm::ivec3> resolvedFeet;   // (hip, knee, foot) per entry of `feet`
    int               resolvedPelvis = -1;
    std::vector<int>  resolvedChain;
    bool              jointsDirty = true;

    // Where the entity stood last frame, so a teleport can be told from walking.
    // A character moved across the map by a script would otherwise ease its feet
    // across the gap over the next half second.
    glm::vec3 lastEntityPos{ 0.0f };
    bool      hasLastEntityPos = false;

    // Cleared by HE::poseBeginFrame. An entity may carry more than one pose
    // driver; the IK stage has to run exactly once per frame or the smoothing
    // integrates twice and the feet arrive at the ground at double speed. Same
    // flag, same reason, as AnimationLayerComponent::finalizedThisFrame.
    bool finalizedThisFrame = false;
};
