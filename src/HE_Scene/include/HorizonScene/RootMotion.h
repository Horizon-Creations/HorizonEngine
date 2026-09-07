#pragma once
#include <ContentManager/Assets.h>
#include <glm/glm.hpp>
#include <cstdint>
#include <string>

class PhysicsWorld;

// ── Root motion ──────────────────────────────────────────────────────────────
// The motion an artist put into the root bone, taken OUT of the pose and put onto
// the entity instead. Without the taking-out the character moves twice: the pose
// slides the mesh and the entity drives on top of it.
//
// Public because three things need the same arithmetic and none of them share an
// ECS: the clip/blend/state-machine systems (via the internal AnimationEval.h),
// the editor's preview, and the tests. The ECS side is RootMotionComponent, which
// is nothing but a Mode plus the RootMotionOptions below.
namespace HE {

// What happens to the root joint in the POSE once its motion has been extracted.
// Only what is extracted gets locked — locking an axis that stays in the entity's
// hands would delete motion nobody took over.
enum class RootMotionLock : uint8_t
{
    // Translation to the origin, heading back to the clip's first frame. The
    // default, and what a clip authored around the origin wants.
    Zero = 0,
    // Translation to the clip's own value at t=0 — for clips whose root does not
    // start at the origin. The heading is treated exactly as in Zero: "remove the
    // turning since frame 0" IS "keep the first frame's heading".
    FirstFrame,
    // Translation locked, rotation left in the pose. For a clip whose root spins
    // without the character being meant to turn — pair it with extractYaw = false,
    // or the turn happens twice.
    TranslationOnly,
};

// The value range of RootMotionComponent without the ECS, so the preview and the
// tests can drive the same helpers the systems drive.
struct RootMotionOptions
{
    // Empty = the first joint with parent < 0. glTF skeletons may have several
    // roots, which is the only reason this is a name and not an index.
    std::string rootJointName;

    bool extractTranslationXZ = true;
    // Off by default: a character controller owns its vertical (gravity, a jump
    // still in flight), and a root-motion Y would fight it every frame.
    bool extractTranslationY = false;
    bool extractYaw = true;

    RootMotionLock lock = RootMotionLock::Zero;
};

// One span's worth of motion, expressed in the character's own frame at the START
// of the span. Pitch and roll are deliberately absent: a figure that tips over is
// a pose, not root motion.
struct RootMotionDelta
{
    glm::vec3 translation{0.0f};
    float     yawDegrees = 0.0f;
};

// Gate and wiring for APPLYING a delta. A null context at the tick means "extract
// and lock, but move nothing" — the editor's case, where an authored clip has to
// animate in place and must not walk the entity across the scene into the save
// file. Same shape as AnimatorHost* on the same call, for the same reason.
struct RootMotionContext
{
    PhysicsWorld* physics = nullptr;
};

// The root joint of `mesh`. Empty name = the first joint with parent < 0.
// -1 when the name matches nothing, which the caller reports rather than
// silently disabling root motion for the entity.
int findRootJoint(const SkeletalMeshAsset& mesh, const std::string& name);

// The delta over the UNWRAPPED span (tPrev, tEnd]. Unwrapped is the whole point:
// out of two wrapped playheads neither the direction nor the number of laps can be
// recovered — t < tPrev could mean "forward over the edge" or "backwards", and a
// clip that runs one and a half laps in a frame looks like half a frame. Given the
// raw span, rewind and multi-lap are free and no `looping` flag has to be guessed.
//
// The caller of a NON-looping clip clamps tEnd into [0, duration] itself; otherwise
// a playhead that has just stopped at the end produces a spurious wrap.
//
// The displacement is referenced against the root's rotation at t = 0, not against
// its raw local frame: a Blender export carries a constant -90° X on the root, and
// un-rotating by that tilt turns a horizontal step into a vertical one — the
// character climbs. Referencing frame 0 cancels any constant tilt and reduces to
// the plain relative transform when the root starts at identity.
RootMotionDelta extractRootMotion(const AnimationClipAsset& clip, int rootJoint,
                                  float tPrev, float tEnd,
                                  const RootMotionOptions& opt);

// Mix two deltas sampled over the same span, with the alpha that mixes the pose.
// A delta is a translation and a yaw; mixing it any other way makes the figure
// speed up or stall in every crossfade.
RootMotionDelta blendRootMotion(const RootMotionDelta& a, const RootMotionDelta& b,
                                float alpha);

} // namespace HE
