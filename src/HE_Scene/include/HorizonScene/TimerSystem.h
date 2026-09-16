#pragma once
#include <HorizonScene/EngineApi.h>
#include <HorizonScene/ScriptContext.h>
#include <HorizonCode/HorizonCodeRuntime.h>
#include <Scripting/ScriptEngine.h>
#include <unordered_map>
#include <cstdint>

// Advances the script timers (HE::api::timer) by one frame and delivers every
// timer that came due. Sibling of CollisionSystem and AnimationNotifySystem,
// built the same way and for the same reason: ONE dispatch entry point that
// serves every frontend at once, called from both applications.
//
// It exists because the two applications had drifted apart here. The packaged
// game polled the timers and fired OnTimer at its GameInstance; the editor's
// play mode never polled them at all, so a timer.after in a graph worked in the
// shipped build and did nothing in the preview — and neither application told
// the Lua/Python scripts anything, although horizon.timer.after was already
// theirs to call. This is the one place all of that happens now.
//
// Two receivers, in this order:
//   1. the HorizonCode GameInstance (OnTimer, carrying the handle),
//   2. every Lua/Python script instance of the session (onTimer / on_timer).
// Every receiver hears every timer: the handle is how a script tells its own
// from the rest, exactly the arrangement fs.watch and the HTTP ticket have.
struct TimerSystem
{
    // entity id → the Lua/Python script instance on it.
    using InstanceMap = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;

    // Poll with the frame's REAL seconds — a timer is a clock, not a game-time
    // wait (that is what Delay is for), so a pause must not stretch it. Returns
    // how many timers fired, so an event-driven host knows whether the frame
    // that draws the reaction has to be asked for.
    static int dispatch(double dtSeconds,
                        HorizonCode::Runtime* runtime,
                        ScriptContext* scripts, const InstanceMap* instances)
    {
        HE::api::timer::poll(dtSeconds);
        int fired = 0, handle = 0;
        while (HE::api::timer::takeFired(handle))
        {
            ++fired;
            if (runtime)
                if (const HorizonCode::InstanceId gi = runtime->gameInstance())
                    runtime->fireOnTimer(gi, 0, handle);
            if (scripts && instances)
                for (const auto& [entityId, inst] : *instances)
                    scripts->callOnTimer(inst, handle);
        }
        return fired;
    }
};
