#pragma once
#include "HorizonScene/AntiCheat/AntiCheatHost.h"
#include "HorizonScene/EntityHost.h"
#include "HorizonScene/GameReplication.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/ScriptContext.h"
#include <HorizonCode/HorizonCodeRuntime.h>
#include <IGameLogic.h>
#include <Scripting/ScriptEngine.h>
#include <unordered_map>
#include <cstdint>

// Delivers one anti-cheat report to every script frontend, in the plan's order
// (docs/anti-cheat-plan.md §5.4). Sibling of TimerSystem, built the same way
// and for the same reason: ONE entry point both applications bind as the
// AntiCheatHost's event sink, so the packaged game and the editor's play mode
// cannot drift apart on who hears a report.
//
// Receivers, in this order:
//   1. the HorizonCode Game Instance — everything session-wide lives there,
//   2. the level script,
//   3. the HorizonCode class on the ENTITY the report names, if any,
//   4. every Lua/Python script instance of the session (onCheatDetected /
//      on_cheat_detected; every instance hears every report, like a timer),
//   5. the native C++ GameLogic module (IGameLogic::onCheatDetected).
// The report itself travels as its ticket; the anticheat.report* readers open
// it. A handler that wants to overrule the host calls anticheat.respond inside
// its turn — the host executes at the frame's end, after all five.
struct AntiCheatEvents
{
    // entity id → the Lua/Python script instance on it.
    using InstanceMap = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;

    static void dispatch(int reportId, const HE::AntiCheat::Report& report,
                         HorizonCode::Runtime* runtime, HorizonWorld* world,
                         EntityHost* entities, GameReplication* replication,
                         ScriptContext* scripts, const InstanceMap* instances,
                         IGameLogic* logic)
    {
        if (runtime)
        {
            if (const HorizonCode::InstanceId gi = runtime->gameInstance())
                runtime->fireOnCheatDetected(gi, reportId);
            if (world)
                if (const HorizonCode::InstanceId level = world->levelScriptInstance())
                    runtime->fireOnCheatDetected(level, reportId);
            // The entity the report is about — the character the flagged
            // connection drives — resolved through the replication's id table,
            // because a network id is what the report carries.
            if (entities && replication && report.netId != 0)
            {
                const Entity e = replication->entityOf(report.netId);
                if (e != entt::null)
                    if (const HorizonCode::InstanceId inst = entities->instanceOf(e))
                        runtime->fireOnCheatDetected(inst, reportId);
            }
        }
        if (scripts && instances)
            for (const auto& [entityId, inst] : *instances)
                scripts->callOnCheatDetected(inst, reportId);
        if (logic) logic->onCheatDetected(reportId);
    }
};
