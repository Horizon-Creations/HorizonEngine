#include "HorizonScene/CameraRigController.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include "HorizonScene/Components/MeshComponent.h"
#include "HorizonScene/Components/SkeletalMeshComponent.h"
#include "HorizonScene/Components/NameComponent.h"
#include <Application/Input.h>
#include <Diagnostics/Log.h>

#include <algorithm>
#include <cmath>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace HE {

namespace {

    // Show or hide a mesh. castsShadow is deliberately untouched: a first-person
    // character still has a shadow, it is just not drawn.
    void applyMeshVisibility(entt::registry& reg, entt::entity target, bool hide)
    {
        if (!reg.valid(target)) return;
        if (auto* m = reg.try_get<MeshComponent>(target))
        {
            m->visible = !hide;
            m->dirty   = true;
        }
        if (auto* sm = reg.try_get<SkeletalMeshComponent>(target))
        {
            sm->visible = !hide;
            sm->dirty   = true;
        }
    }

    // The camera's transform is stored in its PARENT's space, and the rig
    // computes a world pose. For the normal case — a camera sitting under the
    // world root, which has no transform at all — these are the same thing and
    // this returns identity.
    glm::mat4 parentWorldMatrix(entt::registry& reg, entt::entity e)
    {
        auto* h = reg.try_get<HierarchyComponent>(e);
        if (!h || h->parent == entt::null || !reg.valid(h->parent))
            return glm::mat4(1.0f);
        auto* pt = reg.try_get<TransformComponent>(h->parent);
        return pt ? pt->worldMatrix : glm::mat4(1.0f);
    }

    // Which entity a rig follows: its own target id, else what the caller
    // offered (the possessed player character). A default-constructed UUID —
    // both halves zero — is the engine's "no id".
    entt::entity resolveTarget(HorizonWorld& world, const CameraRigComponent& rig,
                               entt::entity fallbackTarget)
    {
        entt::registry& reg = world.registry();
        const bool hasExplicitTarget = !(rig.target == HE::UUID{});
        const entt::entity t =
            hasExplicitTarget ? world.findByEntityId(rig.target) : fallbackTarget;
        if (t == entt::null || !reg.valid(t) || !reg.all_of<TransformComponent>(t))
            return entt::null;
        return t;
    }

    // The only admissible smoother. `lerp(a, b, speed * dt)` is
    // framerate-dependent — tuned at 60 Hz it is a different camera at 144 —
    // whereas the exponential form leaves exactly exp(-speed * T) of the gap
    // after T seconds no matter how that second was cut up.
    float smoothingAlpha(float speed, float dt)
    {
        if (speed <= 0.0f || dt <= 0.0f) return 0.0f;
        return 1.0f - std::exp(-speed * dt);
    }

    // Smooth an angle in degrees the short way round. Without the fold, 179°
    // easing to −179° travels 358° through zero instead of 2° through 180.
    void smoothAngleDegrees(float& current, float target, float alpha)
    {
        current += std::remainder(target - current, 360.0f) * alpha;
    }

    // ── Solving one rig ──────────────────────────────────────────────────────
    // Turns a rig plus its target into a pose. Touches the world in exactly one
    // way: it advances THIS rig's lag state. No transform, no coupling, no mesh
    // visibility — those are the caller's, and only for the rig that is driving.
    //
    // `physics` null means no sweep, which is what every rig that is neither
    // active nor the source of a blend gets: the queries stay at two per frame
    // however many cameras the scene carries.
    SolvedPose solveRig(HorizonWorld& world, entt::entity cameraEntity,
                        CameraRigComponent& rig, entt::entity target,
                        float dt, const PhysicsWorld* physics)
    {
        SolvedPose p;
        entt::registry& reg = world.registry();
        if (target == entt::null)
        {
            // Nothing to follow. Forget the lag pose, so that when the target
            // does come back the camera is placed there rather than flying in
            // from wherever the rig last stood.
            rig.hasLagState = false;
            return p;
        }

        // pivotOffset is applied in WORLD axes on purpose. With Follow coupling
        // the rig writes the target's yaw; a pivot rotated by the target would
        // feed the rig's own output back into its input.
        const glm::vec3 targetPos =
            glm::vec3(reg.get<TransformComponent>(target).worldMatrix[3]);
        const glm::vec3 pivot = targetPos + rig.pivotOffset;

        // ── Lag ──────────────────────────────────────────────────────────────
        // Smoothed BEFORE the sweep, and on the pivot rather than on the
        // finished pose. Smoothing the finished pose fights the shortening: the
        // arm snaps at a wall, the smoothing drags the camera after it, and the
        // camera visibly crawls into the wall and back out. This order keeps the
        // shortening hard and immediate, which is the one place it has to be.
        const bool hardSet =
            !rig.lag.enabled ||
            !rig.hasLagState ||
            (rig.lag.snapDistance > 0.0f &&
             glm::distance(rig.pivotLagged, pivot) > rig.lag.snapDistance);
        if (hardSet)
        {
            rig.pivotLagged = pivot;
            rig.armYaw      = rig.yaw;
            rig.armPitch    = rig.pitch;
        }
        else
        {
            rig.pivotLagged += (pivot - rig.pivotLagged)
                             * smoothingAlpha(rig.lag.positionSpeed, dt);

            // A cap on the trailing distance, so a very fast target cannot drag
            // the camera arbitrarily far behind itself.
            if (rig.lag.maxDistance > 0.0f)
            {
                const glm::vec3 behind = rig.pivotLagged - pivot;
                if (const float trail = glm::length(behind); trail > rig.lag.maxDistance)
                    rig.pivotLagged = pivot + behind * (rig.lag.maxDistance / trail);
            }

            // Only the boom direction is smoothed. yaw/pitch stay raw: they are
            // the look direction and the value Follow writes to the target, and
            // a smoothed look would feel indirect and steer the character
            // somewhere the player is not aiming.
            const float alpha = smoothingAlpha(rig.lag.rotationSpeed, dt);
            smoothAngleDegrees(rig.armYaw,   rig.yaw,   alpha);
            smoothAngleDegrees(rig.armPitch, rig.pitch, alpha);
        }
        rig.hasLagState = true;

        // ── The rig itself ───────────────────────────────────────────────────
        // Two rotations, and they are not interchangeable: the arm is built from
        // the smoothed angles, the camera looks along the raw ones. With lag off
        // they are equal, which is exactly why mixing them up is invisible until
        // someone switches lag on.
        const glm::quat lookRot =
            glm::quat(glm::radians(glm::vec3(rig.pitch, rig.yaw, 0.0f)));
        const glm::quat armRot =
            glm::quat(glm::radians(glm::vec3(rig.armPitch, rig.armYaw, 0.0f)));
        const glm::vec3 armForward = armRot * glm::vec3(0.0f, 0.0f, -1.0f);

        const bool  firstPerson = (rig.mode == CameraRigComponent::Mode::FirstPerson);
        const float armLength   = firstPerson ? 0.0f : std::max(0.0f, rig.armLength);
        const glm::vec3 arm     = firstPerson ? glm::vec3(0.0f) : rig.armOffset;

        glm::vec3 camPos = rig.pivotLagged + armRot * arm - armForward * armLength;

        // ── Boom collision ───────────────────────────────────────────────────
        // Sweep a sphere from the pivot to where the camera wants to be and stop
        // it at the first solid thing. A sphere rather than a ray because a ray
        // is a line: it slips past the corner of a wall that the camera's near
        // plane then cuts straight through.
        //
        // The target is ignored, which covers two things at once — the
        // character's own collider, and the frozen kinematic proxy that sits at
        // its spawn point (see the character-controller skip in PhysicsWorld).
        // Without that the boom would collapse the moment the player walks near
        // where they started.
        if (physics && rig.collision && !firstPerson && rig.collisionRadius > 0.0f)
        {
            const glm::vec3 toCam = camPos - rig.pivotLagged;
            if (const float reach = glm::length(toCam); reach > 1e-4f)
            {
                const PhysicsWorld::RaycastHit hit =
                    physics->sphereCast(rig.pivotLagged, toCam / reach, rig.collisionRadius,
                                        reach, static_cast<uint32_t>(target));
                if (hit.hit)
                {
                    camPos     = rig.pivotLagged + (toCam / reach) * hit.distance;
                    p.occluded = true;
                }
            }
        }

        p.position     = camPos;
        p.rotation     = lookRot;
        p.eulerDegrees = { rig.pitch, rig.yaw, 0.0f };
        if (const auto* cam = reg.try_get<CameraComponent>(cameraEntity))
            p.fovDegrees = cam->fovDegrees;
        p.valid = true;
        return p;
    }

} // namespace

entt::entity CameraRigController::findRigCamera(entt::registry& reg)
{
    entt::entity found = entt::null;
    for (auto [e, t, cam, rig] :
         reg.view<TransformComponent, CameraComponent, CameraRigComponent>().each())
    {
        if (found == entt::null) found = e;
        if (cam.isMain) return e;
    }
    return found;
}

CameraRigController::Frame CameraRigController::update(HorizonWorld& world,
                                                       const CameraLookInput& look,
                                                       entt::entity fallbackTarget,
                                                       const PhysicsWorld* physics)
{
    Frame f;
    entt::registry& reg = world.registry();

    f.camera = findRigCamera(reg);
    if (f.camera == entt::null)
        return f;

    auto& rig = reg.get<CameraRigComponent>(f.camera);

    f.target = resolveTarget(world, rig, fallbackTarget);
    if (f.target == entt::null)
    {
        // Say so. A rig that cannot resolve a target writes no transform at
        // all, so the view stays at the camera entity's raw position — which
        // looks like the camera is stuck inside something and sends people
        // tuning arm length and collision, neither of which is being read.
        // Throttled, because it is true every frame until the target appears.
        // The editor installs a log sink for the play session, so this lands in
        // the Play Session Report where a stuck camera is actually noticed.
        HE_LOG_THROTTLE(Scene, Warning, 5.0,
            "Camera rig on '%s' has no target to follow — it is not driving the "
            "camera (arm length and collision do nothing until it has one)",
            reg.all_of<NameComponent>(f.camera)
                ? reg.get<NameComponent>(f.camera).name.c_str() : "?");

        // Give back whatever this rig hid, so a target that goes away does not
        // leave an invisible character behind.
        if (rig.meshHiddenEntity != entt::null)
        {
            applyMeshVisibility(reg, rig.meshHiddenEntity, false);
            rig.meshHiddenEntity = entt::null;
        }
        // And forget the lag pose, so the camera is placed at the target when
        // one turns up rather than sailing in from where the rig last stood.
        rig.hasLagState = false;
        return f;
    }

    // Look input goes to the ACTIVE rig and to no other: during a blend, the rig
    // the player is leaving must not keep turning under the mouse, or they are
    // steering a camera they have already given up.
    //
    // Mouse: the delta is a DISPLACEMENT, not a rate — scaling it by delta time
    // would make the same flick turn further at a lower frame rate.
    rig.yaw   -= look.mouse.dx * rig.sensitivity;
    rig.pitch -= look.mouse.dy * rig.sensitivity;
    // Stick look is the opposite: a RATE, so it MUST be dt-scaled or the turn
    // speed depends on the framerate. Same sign convention as the mouse (SDL
    // stick Y positive = down); stickInvertY flips pitch only, the way every
    // "invert look" option means it.
    const float stickDeg = rig.stickSensitivity * look.dt;
    rig.yaw   -= look.stickX * stickDeg;
    rig.pitch -= look.stickY * stickDeg * (rig.stickInvertY ? -1.0f : 1.0f);
    rig.pitch  = std::clamp(rig.pitch, rig.pitchMin, rig.pitchMax);

    // Keep yaw in (-180, 180] so it neither drifts into float mush over a long
    // session nor serialises as a five-digit angle.
    if (rig.yaw > 180.0f || rig.yaw < -180.0f)
        rig.yaw -= 360.0f * std::floor((rig.yaw + 180.0f) / 360.0f);

    // ── Rotation coupling ────────────────────────────────────────────────────
    // Write ONLY yaw. The target's pitch and roll are left byte-for-byte alone:
    // running them through a quaternion round trip would re-express them, and
    // glm::eulerAngles picks a different (equivalent) representation near ±90°,
    // which shows up as a character silently flipping when the player looks
    // steeply up or down.
    //
    // This write is what PhysicsWorld's rigid-body write-back used to undo —
    // see the character-controller skip there.
    if (rig.targetYaw == CameraRigComponent::TargetYaw::Follow)
    {
        auto& tt = reg.get<TransformComponent>(f.target);
        tt.rotation.y = rig.yaw;
        tt.dirty      = true;
    }

    // World matrices, so the target's world position is THIS frame's no matter
    // where in the frame the caller sits. Idempotent — the extractor propagating
    // again later costs a walk and changes nothing.
    HE::propagateTransforms(world);

    // ── Solve every rig, not only the one that is driving ────────────────────
    // A blend interpolates against the source rig's pose, and a source pose that
    // stopped being computed the moment the camera switched is the classic bug
    // in that: the picture blends from where the old camera STOOD instead of
    // from where it WOULD BE. Every rig therefore keeps solving and keeps its
    // lag state moving; only the sweep and the world writes are the active
    // rig's alone.
    SolvedPose active;
    for (auto [e, t, cam, other] :
         reg.view<TransformComponent, CameraComponent, CameraRigComponent>().each())
    {
        const bool isActive = (e == f.camera);
        const entt::entity tgt =
            isActive ? f.target : resolveTarget(world, other, fallbackTarget);
        const SolvedPose p =
            solveRig(world, e, other, tgt, look.dt, isActive ? physics : nullptr);
        if (isActive) active = p;
    }
    f.occluded = active.occluded;

    // Store it in the camera's parent space. Identity for a camera under the
    // world root, which is where cameras normally live.
    auto& ct = reg.get<TransformComponent>(f.camera);
    const glm::mat4 parentWorld = parentWorldMatrix(reg, f.camera);
    if (parentWorld == glm::mat4(1.0f))
    {
        ct.position = active.position;
        // The angles the rig holds, not a quaternion round trip — see
        // SolvedPose::eulerDegrees.
        ct.rotation = active.eulerDegrees;
    }
    else
    {
        const glm::mat4 desired =
            glm::translate(glm::mat4(1.0f), active.position) * glm::mat4_cast(active.rotation);
        const glm::mat4 local   = glm::inverse(parentWorld) * desired;
        ct.position = glm::vec3(local[3]);
        ct.rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(local)));
    }
    ct.dirty = true;

    const bool firstPerson = (rig.mode == CameraRigComponent::Mode::FirstPerson);

    // ── First-person mesh hiding ─────────────────────────────────────────────
    // Keyed on the ENTITY, not on a flag: a rig that retargets while hiding has
    // to give the old target back before it hides the new one, or the old one
    // stays invisible for good and the camera sits inside a drawn head.
    const entt::entity wantHidden =
        (firstPerson && rig.hideTargetMesh) ? f.target : entt::null;
    if (wantHidden != rig.meshHiddenEntity)
    {
        if (rig.meshHiddenEntity != entt::null)
            applyMeshVisibility(reg, rig.meshHiddenEntity, false);
        if (wantHidden != entt::null)
            applyMeshVisibility(reg, wantHidden, true);
        rig.meshHiddenEntity = wantHidden;
    }

    // The camera moved after the propagate above, so run it once more — the
    // caller asked for a camera that is correct NOW, and SceneSystems reads its
    // world position for LOD and precipitation right after this.
    HE::propagateTransforms(world);

    f.driven = true;
    return f;
}

} // namespace HE
