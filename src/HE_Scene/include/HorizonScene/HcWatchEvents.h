#pragma once
#include "HorizonScene/EngineApi.h"       // HE::api::hc — the token kinds
#include "HorizonScene/ScriptContext.h"
#include <HorizonCode/HorizonCodeRuntime.h>
#include <IGameLogic.h>
#include <cstdint>
#include <string>
#include <vector>

// Delivers one reported change of a watched HorizonCode variable to the
// frontends that subscribed to it (docs/bind-to-variable-binding-plan.md §4.5).
// The runtime has exactly one hook for it, Runtime::onVariableChanged, and both
// applications point it here — the sibling of NetEvents::dispatchRep, built the
// same way so the packaged game and the editor's play mode cannot drift apart
// on who hears a change.
//
// Who hears it is in the tokens (HE::api::hc::scriptToken / nativeToken):
//   · a SCRIPT token names an entity; the Lua/Python instance on it gets
//     onChanged_<Var>(self, source, old, new) / on_changed_<Var>,
//   · the NATIVE token is the module; it gets
//     IGameLogic::onHcVariableChanged(source, var) and reads the value back.
// `source` is the entity whose class owns the variable, 0 for the Game
// Instance. The HorizonCode side (OnChanged_<Var>, <Var>Changed) is the
// runtime's own business and has already run when this is called.
//
// A subscriber that is gone is dropped HERE, at its first missed delivery:
// a script whose entity has no instance any more (destroyed, its zone
// unloaded), the native token while no module is loaded. Looked up per
// delivery and never cached — a handler earlier in the same list may have
// destroyed the next one.
struct HcWatchEvents
{
    using InstanceMap = ScriptContext::InstanceMap;

    static void dispatch(HorizonCode::Runtime& runtime, HorizonCode::InstanceId owner,
                         const std::string& var, const HorizonCode::Value& oldValue,
                         const HorizonCode::Value& newValue, const std::vector<uint64_t>& tokens,
                         ScriptContext* scripts, const InstanceMap* instances, IGameLogic* logic)
    {
        namespace hc = HE::api::hc;
        const uint32_t source = hc::sourceEntity(runtime, owner);
        for (uint64_t token : tokens)
        {
            if (hc::isScriptToken(token))
            {
                const uint32_t entity = hc::tokenEntity(token);
                const auto it = (scripts && instances) ? instances->find(entity)
                                                       : InstanceMap::const_iterator{};
                if (!scripts || !instances || it == instances->end())
                {
                    runtime.unwatch(owner, var, token);
                    continue;
                }
                scripts->callOnChanged(it->second, var, source, oldValue, newValue);
            }
            else if (hc::isNativeToken(token))
            {
                if (!logic)
                {
                    runtime.unwatch(owner, var, token);
                    continue;
                }
                logic->onHcVariableChanged(source, var.c_str());
            }
        }
    }

    // The native module is going away (unload, play stop): its subscriptions
    // go with it, so the next module does not hear what this one asked for.
    static void dropNative(HorizonCode::Runtime& runtime)
    {
        runtime.unwatchIf(HE::api::hc::isNativeToken);
    }
};
