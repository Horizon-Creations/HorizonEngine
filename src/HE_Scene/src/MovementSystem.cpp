#include "HorizonScene/MovementSystem.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/PhysicsWorld.h"
#include "HorizonScene/Components/MovementComponent.h"
#include "HorizonScene/Components/TransformComponent.h"
#include "HorizonScene/Components/CharacterControllerComponent.h"

#include <algorithm>
#include <cmath>

void MovementSystem::update(HorizonWorld& world, PhysicsWorld* physics, float dt)
{
    if (dt <= 0.0f) return;
    auto& reg = world.registry();

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
