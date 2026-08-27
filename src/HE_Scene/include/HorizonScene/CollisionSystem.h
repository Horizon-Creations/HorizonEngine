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
    //
    // `world` is what makes an event's two entity ids checkable, and it is
    // MANDATORY — a reference, second, where it cannot be left out. It was a
    // trailing `HorizonWorld* = nullptr` first, and every caller simply kept
    // the old shape: the guard below was compiled in and never armed once, so
    // the bug it exists for stayed open. A default that switches a check off
    // silently is worse than no check, because the code reads as if it checks.
    static void dispatch(PhysicsWorld& physics, HorizonWorld& world,
                         ScriptContext* scripts, const InstanceMap& instances,
                         HorizonCode::Runtime* runtime = nullptr,
                         const HcInstanceMap& hcInstances = {})
    {
        // An event names two entities, and one of them can already be gone by
        // the time it is delivered. entity.destroy from Lua/Python (ScriptApi's
        // destroy → HorizonWorld::destroyEntity) touches NO physics at all: the
        // body outlives the entity until the next PhysicsWorld::step() reaps it,
        // and the contact it generates in between carries an id that resolves to
        // nothing. A script receiving it looks up a name, a transform or a
        // component on a corpse — and the id looks perfectly ordinary, so there
        // is no shape of defensive scripting that would catch it.
        //
        // PhysicsWorld::removeEntity keeps that promise for its own path by
        // dropping the pair's pending bookkeeping; this keeps it for the path
        // that never calls removeEntity. It is a registry lookup per contact,
        // which is nothing next to the script call it is guarding.
        //
        // The WHOLE event is dropped, not just the dead half: firing a handler
        // on a dead entity's own instance is as wrong as handing a live one a
        // dead id, and a contact where either end no longer exists has nothing
        // left to mean. entt handles carry a version, so this also rejects an id
        // whose slot has already been recycled by a newly spawned entity —
        // which would otherwise deliver the event to the WRONG entity.
        const auto alive = [&](uint32_t id)
        {
            return world.registry().valid(static_cast<Entity>(id));
        };

        // Both directions of one pair, for one kind of contact.
        auto deliver = [&](const PhysicsWorld::CollisionEvent& ev, bool overlap, bool begin)
        {
            if (!alive(ev.entityA) || !alive(ev.entityB)) return;

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

    // The pre-HorizonCode spelling: Lua/Python only, no HorizonCode runtime.
    // It carries the world for the same reason the full form does — there is no
    // shape of this call that may hand a script a destroyed entity's id.
    static void dispatch(PhysicsWorld& physics, HorizonWorld& world,
                         ScriptContext& scripts, const InstanceMap& instances)
    {
        dispatch(physics, world, &scripts, instances);
    }
};
