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

    // Resolve the target: the component's id, else whatever the caller offered
    // (the possessed player character).
    // A default-constructed UUID (both halves zero) is the engine's "no id".
    const bool hasExplicitTarget = !(rig.target == HE::UUID{});
    f.target = hasExplicitTarget ? world.findByEntityId(rig.target) : fallbackTarget;
    if (f.target == entt::null || !reg.valid(f.target) ||
        !reg.all_of<TransformComponent>(f.target))
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
        return f;
    }

    // Mouse look. The delta is a displacement, not a rate — scaling it by delta
    // time would make the same flick turn further at a lower frame rate.
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

    const glm::vec3 targetPos =
        glm::vec3(reg.get<TransformComponent>(f.target).worldMatrix[3]);

    // ── The rig itself ───────────────────────────────────────────────────────
    // pivotOffset is applied in WORLD axes on purpose. With Follow coupling the
    // rig writes the target's yaw; a pivot rotated by the target would feed the
    // rig's own output back into its input.
    const glm::quat rot     = glm::quat(glm::radians(glm::vec3(rig.pitch, rig.yaw, 0.0f)));
    const glm::vec3 pivot   = targetPos + rig.pivotOffset;
    const glm::vec3 forward = rot * glm::vec3(0.0f, 0.0f, -1.0f);

    const bool  firstPerson = (rig.mode == CameraRigComponent::Mode::FirstPerson);
    const float armLength   = firstPerson ? 0.0f : std::max(0.0f, rig.armLength);
    const glm::vec3 arm     = firstPerson ? glm::vec3(0.0f) : rig.armOffset;

    glm::vec3 camPos = pivot + rot * arm - forward * armLength;

    // ── Boom collision ───────────────────────────────────────────────────────
    // Sweep a sphere from the pivot to where the camera wants to be and stop it
    // at the first solid thing. A sphere rather than a ray because a ray is a
    // line: it slips past the corner of a wall that the camera's near plane then
    // cuts straight through.
    //
    // The target is ignored, which covers two things at once — the character's
    // own collider, and the frozen kinematic proxy that sits at its spawn point
    // (see the character-controller skip in PhysicsWorld). Without that the boom
    // would collapse the moment the player walks near where they started.
    if (physics && rig.collision && !firstPerson && rig.collisionRadius > 0.0f)
    {
        const glm::vec3 toCam = camPos - pivot;
        if (const float reach = glm::length(toCam); reach > 1e-4f)
        {
            const PhysicsWorld::RaycastHit hit =
                physics->sphereCast(pivot, toCam / reach, rig.collisionRadius, reach,
                                    static_cast<uint32_t>(f.target));
            if (hit.hit)
            {
                camPos      = pivot + (toCam / reach) * hit.distance;
                f.occluded  = true;
            }
        }
    }

    // Store it in the camera's parent space. Identity for a camera under the
    // world root, which is where cameras normally live.
    auto& ct = reg.get<TransformComponent>(f.camera);
    const glm::mat4 parentWorld = parentWorldMatrix(reg, f.camera);
    if (parentWorld == glm::mat4(1.0f))
    {
        ct.position = camPos;
        ct.rotation = { rig.pitch, rig.yaw, 0.0f };
    }
    else
    {
        const glm::mat4 desired = glm::translate(glm::mat4(1.0f), camPos) * glm::mat4_cast(rot);
        const glm::mat4 local   = glm::inverse(parentWorld) * desired;
        ct.position = glm::vec3(local[3]);
        ct.rotation = glm::degrees(glm::eulerAngles(glm::quat_cast(local)));
    }
    ct.dirty = true;

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
