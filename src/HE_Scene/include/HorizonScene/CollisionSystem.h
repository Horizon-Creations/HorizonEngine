#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <unordered_map>
#include <cstdint>

// Bridges collision events from PhysicsWorld to Lua scripts.
// Call once per physics step (or per game frame) during play mode.
struct CollisionSystem
{
    using InstanceMap = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;

    // Poll PhysicsWorld for enter/exit events and call onCollisionEnter/Exit
    // on any script instance associated with either entity in the pair.
    static void dispatch(PhysicsWorld& physics, ScriptContext& ctx,
                         const InstanceMap& instances)
    {
        for (const auto& ev : physics.pollCollisionEnter())
        {
            auto itA = instances.find(ev.entityA);
            auto itB = instances.find(ev.entityB);
            if (itA != instances.end())
                ctx.callOnCollisionEnter(itA->second, ev.entityB);
            if (itB != instances.end())
                ctx.callOnCollisionEnter(itB->second, ev.entityA);
        }
        for (const auto& ev : physics.pollCollisionExit())
        {
            auto itA = instances.find(ev.entityA);
            auto itB = instances.find(ev.entityB);
            if (itA != instances.end())
                ctx.callOnCollisionExit(itA->second, ev.entityB);
            if (itB != instances.end())
                ctx.callOnCollisionExit(itB->second, ev.entityA);
        }
    }
};
