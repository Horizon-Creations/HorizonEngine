#include "HorizonScene/MovementSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"
#include "HorizonScene/Components/CameraComponent.h"
#include "HorizonScene/Components/CameraRigComponent.h"
#include <Diagnostics/Logger.h>

#include <algorithm>
#include <cmath>

namespace
{
// The yaw the main camera is looking along, for characters whose input is given
// in camera space. Resolved ONCE per update rather than per character: it is one
// question about the view, and asking it per entity would walk the camera view
// as many times as there are movers.
//
// The MAIN camera's rig, because that is what "the camera" means in a game with
// one view. A scene with two possessed characters would steer both by the same
// yaw — split screen needs a rig per character to be a concept first, and it is
// not one yet.
//
// Returns false when there is no main camera with a rig at all, which is a real
// misconfiguration for a character that asked for camera-relative input and is
// reported as one by the caller.
bool mainCameraYaw(HorizonWorld& world, float& outYaw)
{
    auto& reg = world.registry();
    bool found = false;
    for (auto [e, cam, rig] : reg.view<CameraComponent, CameraRigComponent>().each())
    {
        if (!cam.isMain && found) continue;
        outYaw = rig.yaw;
        found  = true;
        if (cam.isMain) return true;   // an explicit main camera wins outright
    }
    return found;   // else the only rig in the scene, which is the same thing
}
} // namespace

void MovementSystem::update(HorizonWorld& world, PhysicsWorld* physics, float dt)
{
    if (dt <= 0.0f) return;
    auto& reg = world.registry();

    float cameraYaw     = 0.0f;
    bool  haveCameraYaw = false;
    bool  yawResolved   = false;   // asked lazily: most scenes have no camera-space mover

    for (auto [e, mv, t] : reg.view<MovementComponent, TransformComponent>().each())
    {
        // ── Look ─────────────────────────────────────────────────────────────
        // Yaw only, and only when something asked. Pitch is carried but not
        // applied to the body: a character that leans back when the player looks
        // up is not what anyone means by "look".
        if (mv.lookYaw != 0.0f)
        {
            t.rotation.y += mv.lookYaw;
            t.dirty = true;
        }

        // ── Move ─────────────────────────────────────────────────────────────
        const float inputLen = glm::length(mv.moveInput);
        glm::vec3 planar(0.0f);
        if (inputLen > 1e-4f)
        {
            // Clamp rather than normalise: half a stick means half the speed,
            // and only a stick pushed past 1 gets pulled back.
            planar = mv.moveInput / std::max(1.0f, inputLen) * mv.maxSpeed;
            planar.y = 0.0f;

            // ── Camera space ─────────────────────────────────────────────────
            // Turn the direction by the camera's yaw, so pushing forward walks
            // where the view points instead of along the world's Z axis. Same
            // rotation the rig applies to its own forward vector, so the two
            // cannot disagree about which way "forward" is.
            if (mv.moveSpace == MovementComponent::Space::Camera)
            {
                if (!yawResolved)
                {
                    haveCameraYaw = mainCameraYaw(world, cameraYaw);
                    yawResolved   = true;
                }
                if (haveCameraYaw)
                {
                    const float r = glm::radians(cameraYaw);
                    const float s = std::sin(r), c = std::cos(r);
                    planar = glm::vec3(planar.x * c + planar.z * s,
                                       0.0f,
                                      -planar.x * s + planar.z * c);
                }
                else
                {
                    // Asked for a frame that does not exist. Silently falling
                    // back to world would look exactly like the setting doing
                    // nothing, which is the complaint this setting exists to
                    // answer.
                    HE_LOG_THROTTLE(Input, Warning, 5.0,
                        "Entity %u: movement is set to Camera space but the scene has no main "
                        "camera with a Camera Rig - moving in world space instead",
                        static_cast<uint32_t>(e));
                }
            }
        }

        if (physics && reg.all_of<CharacterControllerComponent>(e))
        {
            // The controller owns the vertical component — gravity and whatever
            // a jump left in flight. Writing a 0 here every frame is what turns
            // a fall into a slow sink, so the existing Y is read back and
            // preserved.
            const auto& cc = reg.get<CharacterControllerComponent>(e);
            physics->setCharacterVelocity(static_cast<uint32_t>(e),
                                          glm::vec3(planar.x, cc.velocity.y, planar.z));
        }

        // ── Face the way you travel ──────────────────────────────────────────
        // Off by default, because something else usually owns the facing — a
        // camera rig with coupled rotation, most of all. Two owners fighting
        // over one yaw is a jitter nobody can locate afterwards.
        if (mv.orientToMovement && glm::length(glm::vec2(planar.x, planar.z)) > 1e-4f)
        {
            const float want = glm::degrees(std::atan2(-planar.x, -planar.z));
            float delta = want - t.rotation.y;
            // Shortest way round, so turning from 179° to -179° is two degrees
            // and not three hundred and fifty-eight.
            while (delta >  180.0f) delta -= 360.0f;
            while (delta < -180.0f) delta += 360.0f;
            const float step = mv.turnRate * dt;
            t.rotation.y += std::clamp(delta, -step, step);
            t.dirty = true;
        }

        // Intent is per frame. Not clearing it would make one call to move()
        // mean "keep going forever", which is the opposite of how every caller
        // reads a function named move.
        mv.moveInput = glm::vec3(0.0f);
        mv.lookYaw = mv.lookPitch = 0.0f;
    }
}
