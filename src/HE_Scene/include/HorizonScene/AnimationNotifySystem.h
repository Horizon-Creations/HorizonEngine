#pragma once
#include <HorizonScene/AnimationNotify.h>
#include <HorizonScene/AnimatorHost.h>
#include <HorizonScene/HorizonWorld.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Scripting/ScriptEngine.h>
#include <unordered_map>
#include <cstdint>

// Routes the notifies collected during the animation phase to whatever code sits
// on the entity that fired them. Sibling of CollisionSystem, built the same way
// and for the same reason: there is exactly ONE dispatch entry point and it
// serves every frontend at once.
//
// ⚠ WHERE it is called matters. Immediately AFTER SceneSystems::tickAnimation,
// in both applications — not at the collision drain, which sits in the frame
// BEFORE the animation phase and would therefore hand every notify to its
// handler one frame late.
struct AnimationNotifySystem
{
    // entity id → the Lua/Python script instance on it.
    using InstanceMap   = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;
    // entity id → the HorizonCode Entity-class instance on it (EntityHost).
    using HcInstanceMap = std::unordered_map<uint32_t, HorizonCode::InstanceId>;

    // Deliver every queued event, then EMPTY the queue. Emptying here rather than
    // at the call site is what makes "one drain point" structural instead of a
    // rule two applications have to remember: a second call finds nothing left,
    // the same way polling the physics queues does.
    //
    // Three receivers, in this order:
    //   1. the Lua/Python script on the entity,
    //   2. the HorizonCode Entity class on the entity,
    //   3. the sync graph of that entity's state machine.
    //
    // The third is deliberate and not a completeness gesture: a sync graph is
    // precisely where "the hit window is open" would be turned into a parameter
    // the state machine reads, and without it that path would have to detour
    // through the entity class and back.
    static void dispatch(HE::NotifyQueue& queue, HorizonWorld& world,
                         ScriptContext* scripts, const InstanceMap& instances,
                         HorizonCode::Runtime* runtime = nullptr,
                         const HcInstanceMap& hcInstances = {},
                         AnimatorHost* animators = nullptr)
    {
        for (const HE::AnimationNotifyEvent& ev : queue)
        {
            // Checked PER EVENT and not once up front: a handler for the first
            // notify may destroy its own entity (a death animation whose notify
            // despawns the corpse is the ordinary case), and the second notify in
            // the same frame would then reach an id that resolves to nothing —
            // or, once entt recycles the slot, the WRONG entity. entt handles
            // carry a version, so the version check catches that too.
            if (!world.registry().valid(static_cast<Entity>(ev.entity))) continue;

            using Kind = HE::AnimationNotifyEvent::Kind;

            if (scripts)
            {
                const auto it = instances.find(ev.entity);
                if (it != instances.end())
                {
                    switch (ev.kind)
                    {
                        case Kind::Fire:  scripts->callOnAnimationNotify(it->second, ev.name); break;
                        case Kind::Begin: scripts->callOnAnimationNotifyBegin(it->second, ev.name); break;
                        case Kind::End:   scripts->callOnAnimationNotifyEnd(it->second, ev.name); break;
                    }
                }
            }

            if (runtime)
            {
                const auto it = hcInstances.find(ev.entity);
                if (it != hcInstances.end()) fire(*runtime, it->second, ev);

                // The sync graph of the same entity. A separate lookup because it
                // is a separate instance: a character's own class and its
                // animator's graph both live on one entity, which is exactly why
                // EntityHost and AnimatorHost keep separate tables.
                if (animators)
                {
                    const HorizonCode::InstanceId sync =
                        animators->instanceOf(static_cast<Entity>(ev.entity));
                    if (sync) fire(*runtime, sync, ev);
                }
            }
        }
        queue.clear();
    }

private:
    static void fire(HorizonCode::Runtime& rt, HorizonCode::InstanceId id,
                     const HE::AnimationNotifyEvent& ev)
    {
        switch (ev.kind)
        {
            case HE::AnimationNotifyEvent::Kind::Fire:  rt.fireOnAnimationNotify(id, ev.name); break;
            case HE::AnimationNotifyEvent::Kind::Begin: rt.fireOnAnimationNotifyBegin(id, ev.name); break;
            case HE::AnimationNotifyEvent::Kind::End:   rt.fireOnAnimationNotifyEnd(id, ev.name); break;
        }
    }
};
