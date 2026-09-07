#include "RootMotionApply.h"
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/TransformHierarchy.h>
#include <HorizonScene/Components/TransformComponent.h>
#include <HorizonScene/Components/CharacterControllerComponent.h>
#include <Diagnostics/Log.h>

#include <glm/gtc/quaternion.hpp>
#include <algorithm>
#include <cmath>

namespace
{

// The entity's own facing, YAW ONLY, out of a freshly walked world matrix.
//
// Yaw only, because pitch and roll are never extracted: pushing a horizontal step
// through a tilted frame drives the character into the ground or launches it off a
// slope. Scale is dropped for the same reason — normalising means a scaled-up
// character strides the distance the clip says, not that distance times its scale.
//
// worldMatrixOf and not TransformComponent::worldMatrix: nothing propagates
// transforms during the animation phase, so the stored matrix is a frame old and
// is the identity for anything that appeared this frame. That mistake has been
// made four times in this repo.
float entityWorldYaw(HorizonWorld& world, entt::entity e)
{
    const glm::mat4 w   = HE::worldMatrixOf(world, e);
    const glm::vec3 fwd = glm::vec3(w[2]);
    const float planar  = std::sqrt(fwd.x * fwd.x + fwd.z * fwd.z);
    if (planar < 1e-6f) return 0.0f;
    return std::atan2(fwd.x, fwd.z);
}

// Hand the character controller a horizontal stop, keeping whatever vertical it
// had. See RootMotionComponent::wroteVelocity for why this has to happen at all.
void releaseVelocity(HorizonWorld& world, PhysicsWorld& physics, entt::entity e,
                     RootMotionComponent& rm)
{
    const auto* cc = world.registry().try_get<CharacterControllerComponent>(e);
    physics.setCharacterVelocity(static_cast<uint32_t>(e),
                                 glm::vec3(0.0f, cc ? cc->velocity.y : 0.0f, 0.0f));
    rm.wroteVelocity = false;
}

} // namespace

bool HE::rootMotionSampleClip(entt::entity e, const RootMotionComponent& rm,
                              const SkeletalMeshAsset& mesh, const AnimationClipAsset& clip,
                              float tPrev, float tEnd, bool looping,
                              std::vector<JointTRS>& localTRS, RootMotionDelta& outDelta)
{
    outDelta = RootMotionDelta{};
    if (rm.mode == RootMotionComponent::Mode::Off) return false;
    // Per-clip switch: an idle with a pinned root should not be treated like a
    // roll. Defaults to true, so a clip that predates the flag (or came out of an
    // importer that does not set it yet) keeps working.
    if (!clip.hasRootMotion || clip.duration <= 0.0f) return false;

    const int root = findRootJoint(mesh, rm.options.rootJointName);
    if (root < 0)
    {
        // Suspended, not silently disabled: a typo in the joint name is otherwise
        // indistinguishable from a clip that carries no motion.
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: root motion joint '%s' is not in the skeleton (%zu joint(s)) — "
                        "root motion is suspended for this entity",
                        static_cast<uint32_t>(e),
                        rm.options.rootJointName.empty() ? "<first root>"
                                                         : rm.options.rootJointName.c_str(),
                        mesh.skeleton.size());
        return false;
    }

    if (!looping) tEnd = std::clamp(tEnd, 0.0f, clip.duration);

    outDelta = extractRootMotion(clip, root, tPrev, tEnd, rm.options);
    lockRootJoint(localTRS, root, clip, rm.options);
    return true;
}

void HE::rootMotionBeginFrame(HorizonWorld& world)
{
    for (auto [e, rm] : world.registry().view<RootMotionComponent>().each())
        rm.appliedThisFrame = false;
}

void HE::rootMotionApply(HorizonWorld& world, RootMotionContext* ctx, entt::entity e,
                         RootMotionComponent& rm, const RootMotionDelta& delta, float dt)
{
    rm.lastDelta    = delta.translation;
    rm.lastYawDelta = delta.yawDegrees;
    if (!ctx || rm.mode == RootMotionComponent::Mode::Off) return;

    auto& reg = world.registry();

    if (rm.appliedThisFrame)
    {
        // Two pose drivers on one entity: the last one wins on the bone matrices,
        // which is merely odd. Two deltas would ADD, and a character that walks at
        // double speed for no visible reason is a much longer afternoon.
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: more than one animator produced root motion this frame — "
                        "only the first is applied. An entity should carry one pose driver.",
                        static_cast<uint32_t>(e));
        return;
    }
    rm.appliedThisFrame = true;

    auto* t = reg.try_get<TransformComponent>(e);
    if (!t) return;

    const glm::vec3 worldDelta =
        glm::angleAxis(entityWorldYaw(world, e), glm::vec3(0.0f, 1.0f, 0.0f)) * delta.translation;

    // Yaw first, while the translation above still refers to the frame the span
    // started in. The value is a world yaw written onto a LOCAL rotation, which is
    // exact for an unrotated parent and drifts for a rotated one — the same trade
    // MovementSystem already makes for lookYaw, kept the same on purpose.
    if (rm.options.extractYaw && delta.yawDegrees != 0.0f)
    {
        t->rotation.y += delta.yawDegrees;
        t->dirty = true;
    }

    if (rm.mode == RootMotionComponent::Mode::Transform)
    {
        // World in, local out. PhysicsWorld and this API speak world, a
        // TransformComponent speaks local, and the conversion belongs here rather
        // than in three call sites that would each get it slightly wrong.
        const glm::vec3 target = HE::worldPositionOf(world, e) + worldDelta;
        t->position = HE::localPositionForWorld(world, e, target);
        t->dirty    = true;

        // The mode may have just been switched away from CharacterController while
        // Jolt still holds the last velocity we gave it.
        if (rm.wroteVelocity && ctx->physics)
            releaseVelocity(world, *ctx->physics, e, rm);
        return;
    }

    // ── CharacterController ──────────────────────────────────────────────────
    if (!ctx->physics)
    {
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: root motion is set to Character Controller but the "
                        "animation tick has no PhysicsWorld — the character will not move.",
                        static_cast<uint32_t>(e));
        return;
    }
    auto* cc = reg.try_get<CharacterControllerComponent>(e);
    if (!cc)
    {
        HE_LOG_THROTTLE(Animation, Warning, 5.0,
                        "Entity %u: root motion is set to Character Controller but the entity "
                        "has no CharacterControllerComponent — add one, or use the Transform mode.",
                        static_cast<uint32_t>(e));
        return;
    }
    if (dt <= 0.0f) return;

    // setCharacterVelocity does not MOVE anything, it leaves a velocity for the
    // next step() — the same call MovementSystem makes from tickWorld, so the
    // latency is identical, and being the later writer is exactly the intent:
    // root motion wins over walk input for as long as it is running. The vertical
    // stays the controller's (gravity, a jump still in flight).
    ctx->physics->setCharacterVelocity(static_cast<uint32_t>(e),
                                       glm::vec3(worldDelta.x / dt, cc->velocity.y,
                                                 worldDelta.z / dt));
    rm.wroteVelocity = true;
}

void HE::rootMotionEndFrame(HorizonWorld& world, RootMotionContext* ctx)
{
    if (!ctx || !ctx->physics) return;
    for (auto [e, rm] : world.registry().view<RootMotionComponent>().each())
    {
        if (rm.appliedThisFrame || !rm.wroteVelocity) continue;
        // Nothing drove this character this frame — its clip stopped, its animator
        // was removed, root motion was switched off. Jolt holds the last velocity
        // it was handed, so without this the figure glides on for ever.
        releaseVelocity(world, *ctx->physics, e, rm);
    }
}
