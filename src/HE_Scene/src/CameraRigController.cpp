#include "HorizonScene/CameraRigController.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/TransformHierarchy.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/HierarchyComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include "HorizonScene/CameraShake.h"
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

            // Same fold rig.yaw gets, and for the same reason: chasing the
            // NEAREST equivalent of a wrapped angle keeps armYaw continuous, so
            // a player who spins one way all session winds it up unbounded and
            // out of float precision. Not serialised, so only the mush half of
            // the argument applies here — but that half applies.
            if (rig.armYaw > 180.0f || rig.armYaw < -180.0f)
                rig.armYaw -= 360.0f * std::floor((rig.armYaw + 180.0f) / 360.0f);
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

        // ── Shake ────────────────────────────────────────────────────────────
        // AFTER the shortening, never before: a sweep run against a shaking
        // camera position would land somewhere else every frame, and the
        // shortened distance itself would jitter.
        //
        // The price is that in a shortened frame the shake can push the camera
        // a little way into the surface the boom just stopped at. The sweep
        // leaves exactly one radius of clearance in front of that surface, so
        // the rule is: a shortened frame may spend that clearance and not a
        // millimetre more. Rotational shake is unbounded — it does not move the
        // camera.
        const ShakeOffset shake = evaluateShakes(rig.shakes, dt);
        glm::vec3 shakePos = shake.position;
        if (p.occluded && rig.collisionRadius > 0.0f)
        {
            if (const float reach = glm::length(shakePos); reach > rig.collisionRadius)
                shakePos *= rig.collisionRadius / reach;
        }

        // Camera space, so a shake reads the same whichever way the player is
        // facing. Built on the unshaken basis on purpose — feeding the shaken
        // rotation back into the offset would couple the two axes.
        p.position = camPos + lookRot * shakePos;

        // Shake goes on the ANGLES the rig holds, and the quaternion is rebuilt
        // from them, so a blend slerps exactly what a non-blending frame writes.
        // Roll (z) is otherwise always zero — this is the one thing that uses it.
        p.eulerDegrees = glm::vec3(rig.pitch, rig.yaw, 0.0f) + shake.rotationDegrees;
        p.rotation     = (shake.rotationDegrees == glm::vec3(0.0f))
                       ? lookRot
                       : glm::quat(glm::radians(p.eulerDegrees));

        // ── FOV ──────────────────────────────────────────────────────────────
        // Base plus kick. The base stays untouched in the component; the sum
        // lands in CameraComponent::fovOffset, which nothing saves.
        if (const auto* cam = reg.try_get<CameraComponent>(cameraEntity))
            p.fovDegrees = cam->fovDegrees + evaluateFovKick(rig.fovKick, dt);
        p.valid = true;
        return p;
    }

    // The world pose of a camera that has no rig of its own, frozen so a blend
    // can start from it. Used for a source camera the rig cannot re-solve — a
    // plain cutscene camera is the normal case.
    SolvedPose snapshotCameraPose(entt::registry& reg, entt::entity e)
    {
        SolvedPose p;
        if (const auto* t = reg.try_get<TransformComponent>(e))
        {
            const glm::mat4& m = t->worldMatrix;
            p.position = glm::vec3(m[3]);

            glm::mat3 basis(m);
            for (int i = 0; i < 3; ++i)
            {
                const float len = glm::length(basis[i]);
                if (len > 1e-6f) basis[i] /= len;
            }
            p.rotation     = glm::normalize(glm::quat_cast(basis));
            p.eulerDegrees = glm::degrees(glm::eulerAngles(p.rotation));
        }
        if (const auto* cam = reg.try_get<CameraComponent>(e))
            p.fovDegrees = cam->fovDegrees + cam->fovOffset;
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

        // A rig that stops driving hands the FOV back. Leaving the kick's
        // offset standing would leave the camera permanently a few degrees
        // wrong, with nothing left running to work that off.
        if (auto* cam = reg.try_get<CameraComponent>(f.camera))
            cam->fovOffset = 0.0f;
        rig.blend          = {};
        rig.hasLastWritten = false;
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
    else if (rig.targetYaw == CameraRigComponent::TargetYaw::FollowSmoothed)
    {
        // Turn TOWARDS the camera's yaw at a rate instead of setting it. Two
        // things this is not: it is not the lag smoother (an exponential
        // approach never quite arrives, and a character that is permanently a
        // fraction of a degree off looks like a bug at a wall), and it is not a
        // second smoothed state (rig.yaw stays the raw input — the player aims
        // with it, the body follows).
        //
        // No snap case: a rig that jumps somewhere brings the target with it,
        // and the target's own yaw is where the author or the movement code
        // last put it. There is nothing here that could have gone stale.
        auto& tt = reg.get<TransformComponent>(f.target);
        // Short way round: 179° to -179° is two degrees, not 358.
        const float delta = std::remainder(rig.yaw - tt.rotation.y, 360.0f);
        const float step  = std::max(0.0f, rig.targetTurnRate) * look.dt;
        // Clamped, so it converges exactly and never swings past.
        tt.rotation.y += std::clamp(delta, -step, step);
        tt.dirty       = true;
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
    // The source of a running blend gets the same treatment as the active rig:
    // a live re-solve and a sweep of its own. That keeps the queries at two per
    // frame however many cameras the scene carries.
    const entt::entity blendSource =
        (rig.isBlending() && !rig.blend.useFromPose) ? rig.blend.from : entt::null;

    SolvedPose active;
    SolvedPose sourcePose;
    for (auto [e, t, cam, other] :
         reg.view<TransformComponent, CameraComponent, CameraRigComponent>().each())
    {
        const bool isActive = (e == f.camera);
        const bool isSource = (e == blendSource);
        const entt::entity tgt =
            isActive ? f.target : resolveTarget(world, other, fallbackTarget);
        const SolvedPose p = solveRig(world, e, other, tgt, look.dt,
                                      (isActive || isSource) ? physics : nullptr);
        if (isActive) active     = p;
        if (isSource) sourcePose = p;

        if (!isActive)
        {
            // A rig that is not driving owns nothing on screen. It gives back
            // the mesh it was hiding — a first-person rig that lost isMain used
            // to leave its character invisible for good, which blendTo would
            // otherwise do on purpose every time — and it gives back the FOV.
            if (other.meshHiddenEntity != entt::null)
            {
                applyMeshVisibility(reg, other.meshHiddenEntity, false);
                other.meshHiddenEntity = entt::null;
            }
            cam.fovOffset        = 0.0f;
            other.hasLastWritten = false;

            // And it drops its own blend. Its `remaining` would otherwise sit
            // frozen for as long as it is off screen, so handing isMain back to
            // it by hand would RESUME a blend from a source that has moved on —
            // where the rule is that a hand-set isMain is always a cut.
            other.blend = {};
        }
    }
    f.occluded = active.occluded;

    // ── Blend ────────────────────────────────────────────────────────────────
    // Between the solve and the write, so what lands in the transform is the
    // interpolated pose and nothing downstream has to know a blend is running.
    SolvedPose shown = active;
    if (rig.isBlending())
    {
        SolvedPose from;
        bool haveSource = false;
        if (rig.blend.useFromPose)
        {
            from       = rig.blend.fromPose;
            haveSource = true;
        }
        else if (sourcePose.valid)
        {
            from       = sourcePose;
            haveSource = true;
        }

        if (!haveSource)
        {
            // The source camera was destroyed, or its own target went away.
            // End it here and show the target pose. NOT "carry on from the last
            // known source pose": that pose is a frame old and the world has
            // moved on, so it would blend from somewhere nothing is.
            rig.blend = {};
        }
        else
        {
            rig.blend.remaining = std::max(0.0f, rig.blend.remaining - look.dt);
            const float raw = (rig.blend.duration > 0.0f)
                            ? 1.0f - rig.blend.remaining / rig.blend.duration
                            : 1.0f;
            const float t   = applyBlendCurve(raw, rig.blend.curve);

            // The ends are written verbatim rather than interpolated. slerp at
            // a == 0 or 1 is the same pose only up to rounding, and "the blend
            // is over" has to mean the target pose exactly.
            if (rig.blend.remaining <= 0.0f || t >= 1.0f)
            {
                rig.blend = {};
            }
            else if (t <= 0.0f)
            {
                shown = from;
            }
            else
            {
                shown.position = glm::mix(from.position, active.position, t);
                // Slerp, not a lerp of Euler angles: interpolating 170° to
                // −170° as numbers takes the camera the long way round through
                // a full turn instead of 20° across the seam.
                shown.rotation     = glm::slerp(from.rotation, active.rotation, t);
                shown.eulerDegrees = glm::degrees(glm::eulerAngles(shown.rotation));
                shown.fovDegrees   = glm::mix(from.fovDegrees, active.fovDegrees, t);
            }
        }
    }

    // The one output the rig keeps: a COPY of what it is about to put on screen,
    // in world space, so a second blendTo can start from the pose the player is
    // actually looking at. Taken here, before the parent-space conversion — not
    // read back out of the transform afterwards.
    rig.lastWritten     = shown;
    rig.hasLastWritten  = true;

    // Store it in the camera's parent space. Identity for a camera under the
    // world root, which is where cameras normally live.
    auto& ct = reg.get<TransformComponent>(f.camera);
    const glm::mat4 parentWorld = parentWorldMatrix(reg, f.camera);
    if (parentWorld == glm::mat4(1.0f))
    {
        ct.position = shown.position;
        // The angles the rig holds, not a quaternion round trip — see
        // SolvedPose::eulerDegrees.
        ct.rotation = shown.eulerDegrees;
    }
    else
    {
        const glm::mat4 desired =
            glm::translate(glm::mat4(1.0f), shown.position) * glm::mat4_cast(shown.rotation);
        const glm::mat4 local   = glm::inverse(parentWorld) * desired;
        ct.position = glm::vec3(local[3]);
        ct.rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(local)));
    }
    ct.dirty = true;

    // The FOV as a DIFFERENCE against the value the author set, so fovDegrees is
    // never written and the inspector keeps showing what it was given. With no
    // kick and no blend this is x - x, which is exactly 0.0f — that is what
    // makes "no rig effect" indistinguishable from "no rig".
    if (auto* cam = reg.try_get<CameraComponent>(f.camera))
        cam->fovOffset = shown.fovDegrees - cam->fovDegrees;

    const bool firstPerson = (rig.mode == CameraRigComponent::Mode::FirstPerson);

    // ── First-person mesh hiding ─────────────────────────────────────────────
    // Keyed on the ENTITY, not on a flag: a rig that retargets while hiding has
    // to give the old target back before it hides the new one, or the old one
    // stays invisible for good and the camera sits inside a drawn head.
    //
    // Only the ACTIVE rig does this, which leaves a hole that blending will make
    // real: a first-person rig that hid its target and then loses isMain never
    // runs this path again, so the character stays invisible. Today that takes a
    // hand-edited isMain to reach; blendTo will do it on purpose.
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

bool CameraRigController::blendTo(HorizonWorld& world, entt::entity toCamera,
                                  float seconds, BlendCurve curve)
{
    entt::registry& reg = world.registry();
    if (toCamera == entt::null || !reg.valid(toCamera) ||
        !reg.all_of<CameraComponent>(toCamera))
        return false;

    // Which camera the picture is coming FROM. Read before isMain is rewritten,
    // because that is what decides it.
    entt::entity from = entt::null;
    for (auto [e, cam] : reg.view<CameraComponent>().each())
    {
        if (from == entt::null) from = e;
        if (cam.isMain) { from = e; break; }
    }

    // The blend's precondition, not its decoration: exactly one isMain. See the
    // note on the declaration.
    for (auto [e, cam] : reg.view<CameraComponent>().each())
        cam.isMain = (e == toCamera);

    auto* rig = reg.try_get<CameraRigComponent>(toCamera);
    if (!rig) return true;   // switched; a camera without a rig cannot blend

    if (from == entt::null || from == toCamera || seconds <= 0.0f)
    {
        rig->blend = {};     // a cut, which is also what setting isMain by hand does
        return true;
    }

    CameraRigComponent::Blend b;
    b.duration  = seconds;
    b.remaining = seconds;
    b.curve     = curve;

    // A blend started while one is already running begins at the pose currently
    // on screen. That pose is an interpolation between two rigs, which is not
    // something any entity holds — so it is frozen. The same applies to a source
    // camera that has no rig of its own: there is nothing to re-solve, so its
    // world pose is taken once, here.
    //
    // The interpolated pose is the OUTGOING rig's `lastWritten`, not the
    // incoming one's: the camera that has been showing the picture is the one
    // that wrote it. Asking the incoming rig instead looks plausible and is
    // never true — a rig that is not driving has its hasLastWritten cleared in
    // update(), and a rig that IS driving would be `from == toCamera` and has
    // already returned above as a cut.
    auto* fromRig = reg.try_get<CameraRigComponent>(from);
    if (fromRig && fromRig->isBlending() && fromRig->hasLastWritten)
    {
        b.useFromPose = true;
        b.fromPose    = fromRig->lastWritten;
    }
    else if (!fromRig)
    {
        // World matrices first — a camera's worldMatrix is whatever the last
        // propagate left there, and for a cutscene camera that was parented or
        // moved this frame that is not where it is.
        HE::propagateTransforms(world);
        b.useFromPose = true;
        b.fromPose    = snapshotCameraPose(reg, from);
    }
    else
    {
        b.from = from;
    }

    rig->blend = b;
    return true;
}

bool CameraRigController::isBlending(entt::registry& reg)
{
    const entt::entity cam = findRigCamera(reg);
    if (cam == entt::null) return false;
    const auto* rig = reg.try_get<CameraRigComponent>(cam);
    return rig && rig->isBlending();
}

} // namespace HE
