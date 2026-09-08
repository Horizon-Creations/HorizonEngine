#pragma once
#include <ContentManager/Assets.h>
#include <HorizonScene/AnimationPose.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <string>
#include <vector>

// ── Inverse kinematics ───────────────────────────────────────────────────────
// Where forward kinematics asks "where does this pose put the foot", IK asks the
// other way round: the foot belongs THERE, what do the hip and the knee have to
// do about it. Two solvers, because two questions are worth asking on a
// character — put the foot on the ground it is actually standing on, and point
// the head at the thing it is looking at.
//
// Everything here is a pure function of a skeleton, a pose and a target. No ECS,
// no physics, no assets are resolved: the ground ray that produces a foot target
// and the component that holds the settings live in the internal PoseFinalize.h,
// exactly the way RootMotion.h and RootMotionApply.h are split, and for the same
// reason — the editor's preview and the tests have no entity and still need the
// same arithmetic the game runs.
//
// The space is MODEL space throughout: joint frames as composeModelMatrices
// builds them, before the inverse bind matrix. A caller with a world-space
// target converts it once with the entity's world matrix, rather than every
// function in here learning what an entity is.
namespace HE {

// How close a target has to be to where the joint already is for the solver to
// decline to run. Not a tolerance on the result — an EARLY OUT, which is what
// makes "IK that has nothing to do changes nothing" true to the bit rather than
// to a slerp's idea of alpha 0.
inline constexpr float kIkNoopDistance = 1.0e-5f;

// Two-bone IK: rotate `hip` and `knee` so `foot` lands on `targetModel`.
//
// The three indices must be a parent chain (hip → knee → foot); anything else is
// declined rather than solved into nonsense. `localTRS` and `model` are both
// updated — the local rotations because that is the pose, the model matrices
// because the next solver on the same skeleton reads them (and re-running the
// whole FK per limb would be the kinematics three times over on a biped).
//
// THE BEND PLANE COMES OUT OF THE CURRENT POSE, not out of a fixed forward axis.
// A pole vector nailed to +Z or -Z bends the knee backwards on every skeleton
// whose bind pose does not happen to share that convention — and a Blender export
// carries a constant -90° X on its root, so "forward" there is not where anyone
// guesses. Taken from where the knee already is, a leg bent forwards stays bent
// forwards and a leg bent backwards stays bent backwards, on any rig.
//
// An UNREACHABLE target (further than the two bones together) straightens the
// leg and points it at the target. It does not overextend, and it does not snap
// back to the animated pose — a foot that gives up and jumps home is more
// visible than a foot that reaches as far as it can.
//
// `weight` in [0, 1] blends the solved local rotations back towards the animated
// ones. 0 returns immediately and touches nothing.
//
// Returns true when the pose was changed.
bool solveTwoBoneIk(const SkeletalMeshAsset& mesh, int hip, int knee, int foot,
                    const glm::vec3& targetModel, float weight,
                    std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model);

// Tilt `foot` so the sole follows the ground it landed on.
//
// `upModel` is the character's own up in model space (the entity's world up run
// backwards through its world matrix — NOT a hardcoded +Y, same reason as the
// bend plane above). `normalModel` is the surface normal in the same space.
//
// The tilt is split into pitch (about the character's right) and roll (about its
// forward) and each is clamped on its own. Unclamped, a foot that finds a wall
// stands itself up at 90°, which is the single most recognisable IK failure
// there is.
void alignFootToNormal(const SkeletalMeshAsset& mesh, int foot,
                       const glm::vec3& normalModel, const glm::vec3& upModel,
                       const glm::vec3& forwardModel,
                       float maxPitchDegrees, float maxRollDegrees, float weight,
                       std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model);

// The yaw and pitch, in DEGREES and already clamped, that would turn the end of
// `chain` from where it is pointing towards `targetModel`.
//
// Split off from applying them because the two belong to different owners: the
// angles are what a component smooths over time (a target that jumps out of the
// cone must not make the head snap), and applying them is what the pose needs.
// It is also the half a test can ask a question of without building a pose.
//
// Measured against the frame of the chain root's PARENT — the character's body,
// not the head — so "45° of yaw" means the same thing whatever the neck was
// already doing. `forwardLocal` is which way the head looks in its own frame;
// -Z is this engine's forward (EngineApi's `q * vec3(0,0,-1)`).
//
// Returns (yaw, pitch). Both 0 when the chain is empty or the target sits on the
// joint itself.
glm::vec2 lookAtAngles(const SkeletalMeshAsset& mesh, const std::vector<int>& chain,
                       const std::vector<glm::mat4>& model, const glm::vec3& targetModel,
                       const glm::vec3& forwardLocal,
                       float maxYawDegrees, float maxPitchDegrees);

// Spread `anglesDegrees` (yaw, pitch — the pair above) over `chain`, root first.
//
// `chainWeights` says how much of the turn each link takes; they are normalised
// here, and a missing or empty list splits it evenly. Spreading is the whole
// point: all of it on the head is the look of a bird, and a spine that carries a
// third of it is what makes a character look at something rather than swivel.
//
// Every link turns about the SAME model-space axis, so the fractions add up to
// the whole angle exactly (rotations about one axis commute) instead of
// approximately, which a per-link local-axis version would.
//
// `weight` in [0, 1] scales the whole thing; 0 returns immediately and touches
// nothing. Returns true when the pose was changed.
bool applyLookAt(const SkeletalMeshAsset& mesh, const std::vector<int>& chain,
                 const std::vector<float>& chainWeights, const glm::vec2& anglesDegrees,
                 const glm::vec3& forwardLocal, float weight,
                 std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model);

// Shift `joint` (and everything under it) by `deltaModel` metres in model space.
//
// This is the pelvis drop of foot placement, on its own because it is not a
// rotation and does not belong inside a rotation solver. The delta is converted
// into the joint's parent frame here — a local translation is expressed in the
// parent's axes, and writing a model-space vector straight into it is the bug
// that makes a rotated character sink sideways.
void offsetJointModel(const SkeletalMeshAsset& mesh, int joint, const glm::vec3& deltaModel,
                      std::vector<JointTRS>& localTRS, std::vector<glm::mat4>& model);

// The index of the joint named `name`, or -1. Empty name gives -1 too: an unset
// name is "not configured", never "joint 0".
int findJointByName(const SkeletalMeshAsset& mesh, const std::string& name);

} // namespace HE
