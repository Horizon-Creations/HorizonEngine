#pragma once
#include <Types/Defines.h>
#include "HorizonCode.h"
#include <functional>
#include <string>
#include <unordered_map>

// ── HorizonCode::Runtime ─────────────────────────────────────────────────────
// The single, central interpreter host for HorizonCode. Instead of each engine
// system (UI widgets, the level script, …) owning its own graphs, variable
// stores and Runners, they all register their scripts here as *instances*. The
// runtime owns each instance's graph + private variable state and is the one
// place that executes interpreted HorizonCode.
//
//   • An InstanceId doubles as a *reference* to a running script — the basis for
//     cross-class delegation (a widget calling a public function on the level,
//     the GameInstance, etc.). Private variables/functions stay private to their
//     instance; only public functions are reachable through callFunction with
//     requirePublic.
//   • Domain side effects (reading/writing a widget element's properties,
//     show/hide) are supplied by the owning system as HostBindings — the runtime
//     stays domain-agnostic, exactly like HorizonCode::Context.
//
// This is the foundation the reference-based event dispatchers and the app-wide
// GameInstance build on.

namespace HorizonCode {

using InstanceId = uint32_t;   // 0 = invalid; also a live-script reference/handle

// Per-instance domain bindings. One set can back many instances of the same
// kind (e.g. every widget routes to the WidgetManager, disambiguated by id).
// All optional — the runtime null-checks. A level script binds none of these.
struct HostBindings
{
    std::function<Value(InstanceId id, int elem, const std::string& prop)>              getProperty;
    std::function<void (InstanceId id, int elem, const std::string& prop, const Value&)> setProperty;
    std::function<void (InstanceId id)> showSelf;
    std::function<void (InstanceId id)> hideSelf;
};

class HE_API Runtime
{
public:
    // Register a running script. The runtime takes ownership of the graph (so a
    // caller's container reallocating can't dangle execution) and seeds the
    // instance's private variable store from the graph's declared defaults.
    InstanceId add(Graph graph, HostBindings bindings = {});
    void       remove(InstanceId id);
    bool       alive(InstanceId id) const;
    // Drop every instance (whole-runtime teardown).
    void       clear();
    size_t     count() const { return m_insts.size(); }

    // The instance's graph (for hosts that inspect it, e.g. which elements are
    // interactive). Returns a shared empty graph when the id is unknown.
    const Graph& graphOf(InstanceId id) const;

    // Private per-instance variable state.
    Value getVariable(InstanceId id, const std::string& name) const;
    void  setVariable(InstanceId id, const std::string& name, const Value& v);
    // Read-only view of an instance's variable store (tooling / tests).
    const std::unordered_map<std::string, Value>& variablesOf(InstanceId id) const;

    // Fire an event on ONE instance. `elem` targets a widget element (0 = any).
    // `arg` feeds the event's data output when it has one.
    void fireEvent(InstanceId id, const std::string& event, int elem = 0, const Value& arg = {});

    // Run a function on an instance. requirePublic enforces the access modifier
    // for calls that cross a class boundary (another script / the scripting API).
    // Returns false when the instance/function is missing, or it is private and
    // requirePublic is set.
    bool callFunction(InstanceId id, const std::string& fn, bool requirePublic = true);

private:
    struct Inst
    {
        Graph                                   graph;
        HostBindings                            host;
        std::unordered_map<std::string, Value>  vars;
    };
    Inst*       find(InstanceId id);
    const Inst* find(InstanceId id) const;
    // Build a Context that routes variable access to the instance's private
    // store and property/show/hide to its host bindings.
    Context makeContext(InstanceId id);

    std::unordered_map<InstanceId, Inst> m_insts;
    InstanceId                           m_next = 1;
};

} // namespace HorizonCode
