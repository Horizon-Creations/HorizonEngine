#pragma once
#include "HorizonScene/Net/NetGameSession.h"
#include "HorizonScene/Net/PropertyReplicator.h"
#include "HorizonScene/HorizonWorld.h"
#include "HorizonScene/ScriptContext.h"
#include <HorizonCode/HorizonCodeRuntime.h>
#include <IGameLogic.h>
#include <Scripting/ScriptEngine.h>
#include <Scripting/ScriptTypes.h>
#include <cstdint>
#include <unordered_map>

// Delivers one session lifecycle event to every script frontend, in the plan's
// order (docs/gameplay-replication-plan.md §7.4). Sibling of AntiCheatEvents,
// built the same way and for the same reason: ONE entry point both applications
// drain NetGameSession::takeEvent into, so the packaged game and the editor's
// play mode cannot drift apart on who hears a join.
//
// Receivers, in this order:
//   1. the HorizonCode Game Instance — a session is session-wide by definition,
//   2. the level script,
//   3. every Lua/Python script instance of the session (onPlayerJoined /
//      on_player_joined …; every instance hears every event, like a timer),
//   4. the native C++ GameLogic module (IGameLogic::onPlayerJoined …).
//
// NO ENTITY STATION, unlike AntiCheatEvents. A report names the entity the
// flagged connection drives, so there is one obvious class to tell; a join does
// not. At OnPlayerJoined the player's character does not exist yet — the host
// spawns and assigns it afterwards (plan §5.4) — so an entity station would be
// reliably empty on the one event where somebody would expect it, which is
// worse than not having one. An entity that wants to know asks net.playerCount.
//
// WHY THIS IS NOT CALLED FROM A HANDLER. NetGameSession queues its events
// rather than firing them, and this is drained at the frame's end. Running a
// graph from inside pump() would run game code in the middle of draining a
// socket — the rule Memory `hc-breakpoints-latent-resume` states for the
// debugger, and it is the same rule.
struct NetEvents
{
    // entity id → the Lua/Python script instance on it.
    using InstanceMap = std::unordered_map<uint32_t, ScriptEngine::InstanceId>;

    static void dispatch(const NetGameSession::Event& ev,
                         HorizonCode::Runtime* runtime, HorizonWorld* world,
                         ScriptContext* scripts, const InstanceMap* instances,
                         IGameLogic* logic)
    {
        // The three shapes an event has, decided once here rather than at each
        // of the four stations: which hook, and what integer it carries.
        NetScriptEvent kind = NetScriptEvent::SessionEnded;
        int            arg  = 0;
        switch (ev.kind)
        {
            case NetGameSession::Event::Kind::PlayerJoined:
                kind = NetScriptEvent::PlayerJoined;
                arg  = static_cast<int>(ev.player);
                break;
            case NetGameSession::Event::Kind::PlayerLeft:
                kind = NetScriptEvent::PlayerLeft;
                arg  = static_cast<int>(ev.player);
                break;
            case NetGameSession::Event::Kind::Connected:
                kind = NetScriptEvent::Connected;
                break;
            case NetGameSession::Event::Kind::Disconnected:
                kind = NetScriptEvent::Disconnected;
                arg  = ev.reason;
                break;
            case NetGameSession::Event::Kind::SessionStarted:
                kind = NetScriptEvent::SessionStarted;
                break;
            case NetGameSession::Event::Kind::SessionEnded:
                kind = NetScriptEvent::SessionEnded;
                break;
        }

        if (runtime)
        {
            if (const HorizonCode::InstanceId gi = runtime->gameInstance())
                fireHc(*runtime, gi, kind, arg);
            if (world)
                if (const HorizonCode::InstanceId level = world->levelScriptInstance())
                    fireHc(*runtime, level, kind, arg);
        }
        if (scripts && instances)
            for (const auto& [entityId, inst] : *instances)
            {
                (void)entityId;
                scripts->callOnNetEvent(inst, kind, arg);
            }
        if (logic) fireLogic(*logic, kind, arg);
    }

private:
    static void fireHc(HorizonCode::Runtime& rt, HorizonCode::InstanceId id,
                       NetScriptEvent kind, int arg)
    {
        switch (kind)
        {
            case NetScriptEvent::PlayerJoined:   rt.fireOnPlayerJoined(id, arg);   break;
            case NetScriptEvent::PlayerLeft:     rt.fireOnPlayerLeft(id, arg);     break;
            case NetScriptEvent::Connected:      rt.fireOnConnected(id);           break;
            case NetScriptEvent::Disconnected:   rt.fireOnDisconnected(id, arg);   break;
            case NetScriptEvent::SessionStarted: rt.fireOnSessionStarted(id);      break;
            case NetScriptEvent::SessionEnded:   rt.fireOnSessionEnded(id);        break;
        }
    }

public:
    // ── OnRep: a replicated variable arrived (plan §6.4) ─────────────────────
    // A sibling of dispatch() above and deliberately a SEPARATE entry point,
    // because the two have opposite addressing: a session event is heard by
    // every script in the game, and this one by the entity whose property
    // changed. Passing both through one function would mean a `target` that is
    // meaningless half the time.
    //
    // Three stations, not four — there is no Game Instance and no level script
    // here for that same reason: the value belongs to an entity.
    //
    //   1. the entity's HorizonCode class — OnRep_<Var>(old), called with
    //      requirePublic = false, because a notify handler is the class's own
    //      business and nobody outside calls it,
    //   2. the Lua/Python instance on that entity — onRep_<var> / on_rep_<var>,
    //   3. the native module — IGameLogic::onRep(entity, name).
    //
    // NEVER ON THE HOST. The replicator queues nothing there (it set the value
    // and knows it), so this is only ever reached on a client — the guard lives
    // at the source rather than here, where a second copy of it could drift.
    static void dispatchRep(const PropertyReplicator::Notification& n,
                            HorizonCode::Runtime* runtime,
                            HorizonCode::InstanceId classInstance,
                            ScriptContext* scripts, const InstanceMap* instances,
                            IGameLogic* logic)
    {
        if (runtime && classInstance)
            runtime->callFunction(classInstance, "OnRep_" + n.name,
                                  /*requirePublic*/ false, { n.oldValue });

        if (scripts && instances)
        {
            const auto it = instances->find(static_cast<uint32_t>(n.entity));
            if (it != instances->end())
                scripts->callOnRep(it->second, n.name, n.oldValue);
        }

        if (logic) logic->onRep(static_cast<uint32_t>(n.entity), n.name.c_str());
    }

private:
    static void fireLogic(IGameLogic& logic, NetScriptEvent kind, int arg)
    {
        switch (kind)
        {
            case NetScriptEvent::PlayerJoined:   logic.onPlayerJoined(arg);  break;
            case NetScriptEvent::PlayerLeft:     logic.onPlayerLeft(arg);    break;
            case NetScriptEvent::Connected:      logic.onConnected();        break;
            case NetScriptEvent::Disconnected:   logic.onDisconnected(arg);  break;
            case NetScriptEvent::SessionStarted: logic.onSessionStarted();   break;
            case NetScriptEvent::SessionEnded:   logic.onSessionEnded();     break;
        }
    }
};
