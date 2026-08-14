#pragma once
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/PhysicsWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Scripting/ScriptEngine.h>
#include <HorizonScene/Components/ScriptComponent.h>
#include <unordered_map>
#include <cstdint>

// Routes physics contacts from PhysicsWorld to whatever code is attached to the
// two entities involved. Call once per physics step (or per game frame) during
// play mode.
//
// ⚠ Polling DRAINS the queues, so there is exactly ONE dispatch entry point and
// it serves both frontends. Two calls — one for Lua/Python, one for HorizonCode
// — would leave whichever ran second with nothing, and it would look like the
// events simply never fire for that language.
struct CollisionSystem
{
    // entity id → the Lua/Python script instance on it.
    using InstanceMap   = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;
    // entity id → the HorizonCode Entity-class instance on it (EntityHost).
    using HcInstanceMap = std::unordered_map<uint32_t, HorizonCode::InstanceId>;

    // Drain all four queues and deliver each contact to BOTH entities of the
    // pair, in both frontends.
    //
    // Lua/Python keep receiving onCollisionEnter/Exit for BLOCKING and TRIGGER
    // contacts alike — that is what they always did, and narrowing it now would
    // silently break existing scripts. onBeginOverlap/onEndOverlap are the new,
    // trigger-only pair. HorizonCode has the two apart from the start:
    // OnHit/OnHitEnd for blocking, OnBeginOverlap/OnEndOverlap for triggers.
    static void dispatch(PhysicsWorld& physics,
                         ScriptContext* scripts, const InstanceMap& instances,
                         HorizonCode::Runtime* runtime = nullptr,
                         const HcInstanceMap& hcInstances = {})
    {
        // Both directions of one pair, for one kind of contact.
        auto deliver = [&](const PhysicsWorld::CollisionEvent& ev, bool overlap, bool begin)
        {
            auto one = [&](uint32_t self, uint32_t other)
            {
                if (scripts)
                {
                    const auto it = instances.find(self);
                    if (it != instances.end())
                    {
                        if (begin) scripts->callOnCollisionEnter(it->second, other);
                        else       scripts->callOnCollisionExit(it->second, other);
                        if (overlap)
                        {
                            if (begin) scripts->callOnBeginOverlap(it->second, other);
                            else       scripts->callOnEndOverlap(it->second, other);
                        }
                    }
                }
                if (runtime)
                {
                    const auto it = hcInstances.find(self);
                    if (it != hcInstances.end())
                    {
                        if (overlap) begin ? runtime->fireOnBeginOverlap(it->second, other)
                                           : runtime->fireOnEndOverlap(it->second, other);
                        else         begin ? runtime->fireOnHit(it->second, other)
                                           : runtime->fireOnHitEnd(it->second, other);
                    }
                }
            };
            one(ev.entityA, ev.entityB);
            one(ev.entityB, ev.entityA);
        };

        for (const auto& ev : physics.pollCollisionEnter()) deliver(ev, false, true);
        for (const auto& ev : physics.pollCollisionExit())  deliver(ev, false, false);
        for (const auto& ev : physics.pollOverlapEnter())   deliver(ev, true,  true);
        for (const auto& ev : physics.pollOverlapExit())    deliver(ev, true,  false);
    }

    // The pre-HorizonCode spelling, kept so the existing call sites and tests
    // read unchanged.
    static void dispatch(PhysicsWorld& physics, ScriptContext& scripts,
                         const InstanceMap& instances)
    {
        dispatch(physics, &scripts, instances);
    }
};
