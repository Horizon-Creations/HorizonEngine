#pragma once
#include <Types/Defines.h>
#include "HorizonCode.h"
#include "HorizonCodeCompiled.h"
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

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
    // Register a COMPILED script instance (ahead-of-time generated C++, see
    // HorizonCodeCompiled.h). Behaves exactly like add(): same id space, same
    // Context wiring, same delegation/GC participation. Returns 0 on null.
    InstanceId addCompiled(CompiledPtr inst, HostBindings bindings = {});
    void       remove(InstanceId id);
    // Fire the instance's "Destruct" lifecycle event, then remove it — the
    // teardown counterpart to the "Construct" fired on create. Use this (not
    // remove) whenever an object/widget is intentionally destroyed so its
    // destructor graph runs; no-op if the id is already gone.
    void       destroy(InstanceId id);
    bool       alive(InstanceId id) const;
    // Drop every instance (whole-runtime teardown).
    void       clear();
    size_t     count() const { return m_insts.size(); }

    // The instance's graph (for hosts that inspect it). Returns a shared empty
    // graph when the id is unknown — and for compiled instances, which carry no
    // graph. Hosts that only need the Event bindings use eventBindingsOf, which
    // serves both backends.
    const Graph& graphOf(InstanceId id) const;

    // The instance's host-firable events — one entry per Event node (interpreted)
    // or per CompiledEventInfo (compiled). Backend-agnostic replacement for
    // scanning graphOf(...).nodes (e.g. WidgetManager interactivity).
    struct EventBinding { std::string name; int elem = 0; };
    std::vector<EventBinding> eventBindingsOf(InstanceId id) const;

    // Private per-instance variable state.
    Value getVariable(InstanceId id, const std::string& name) const;
    void  setVariable(InstanceId id, const std::string& name, const Value& v);
    // Reset an instance's variables to its graph's declared defaults (used to
    // give the persistent GameInstance a fresh start each play session).
    void  reseedVariables(InstanceId id);
    // Read-only view of an instance's variable store (tooling / tests).
    // Interpreted instances only — for a compiled instance this exposes just the
    // overflow store; use variablesSnapshot for the full backend-agnostic view.
    const std::unordered_map<std::string, Value>& variablesOf(InstanceId id) const;
    // Materialized name→value map for either backend (compiled: declared members
    // via reflection + overflow entries). Tooling/tests; not for hot paths.
    std::unordered_map<std::string, Value> variablesSnapshot(InstanceId id) const;

    // Fire an event on ONE instance. `elem` targets a widget element (0 = any).
    // `arg` feeds the event's data output when it has one.
    void fireEvent(InstanceId id, const std::string& event, int elem = 0, const Value& arg = {});

    // Run a function on an instance, passing `args` and copying its return values
    // into `results` (when non-null). requirePublic enforces the access modifier
    // for calls that cross a class boundary (another script / the scripting API).
    // Returns false when the instance/function is missing, or it is private and
    // requirePublic is set.
    bool callFunction(InstanceId id, const std::string& fn, bool requirePublic = true,
                      const std::vector<Value>& args = {}, std::vector<Value>* results = nullptr);

    // ── Reference-based delegation (Unreal-style event dispatchers) ───────────
    // Subscribe `listener` to `owner`'s `event`: when the owner fires that event,
    // the listener's own Event node of the same name fires too (with the arg).
    // Both instances must exist; a listener is auto-unbound when removed.
    void bindEvent(InstanceId owner, const std::string& event, InstanceId listener);
    // Broadcast `event` from `owner` to everyone bound to it (does NOT fire the
    // owner's own nodes — that is what fireEvent does). This is the EmitEvent node.
    void emitEvent(InstanceId owner, const std::string& event, const Value& arg = {});

    // The app-wide GameInstance: a single always-present script referenced from
    // any graph via the Get Game Instance node. setGameInstance registers/
    // replaces it; gameInstance() is its handle (0 when none).
    InstanceId setGameInstance(Graph graph, HostBindings bindings = {});
    InstanceId setGameInstanceCompiled(CompiledPtr inst, HostBindings bindings = {});
    InstanceId gameInstance() const { return m_gameInstance; }

    // Scene-switch garbage collection: keep `root` and every instance reachable
    // from it through Ref-typed variables, remove all others. Called on scene
    // teardown with the GameInstance as root, so only objects the GameInstance
    // still holds persist across levels — scene-scoped ones are dropped.
    void retainOnlyReachableFrom(InstanceId root);

    // World-level services forwarded into every instance's Context so any graph
    // can spawn/manage widgets and instantiate HorizonCode classes. The app binds
    // these to the current world's WidgetManager + ContentManager (+ this runtime).
    struct Services
    {
        std::function<int(const std::string& assetPath)> createWidget;
        std::function<void(int)> showWidget;
        std::function<void(int)> hideWidget;
        std::function<void(int)> destroyWidget;
        std::function<uint32_t(const std::string& classPath)> createObject;
        std::function<void(uint32_t)> destroyObject;
        // Generic engine-API dispatch, forwarded to every instance's Context so any
        // EngineCall node reaches the HE::api registry. The app binds it to the
        // current world's registry Ctx (world/physics/content).
        std::function<std::vector<Value>(const std::string& apiId, const std::vector<Value>& args)> callApi;
    };
    void setServices(Services s) { m_services = std::move(s); }

private:
    struct Inst
    {
        Graph                                   graph;      // interpreted; empty for compiled
        CompiledPtr                             compiled;   // compiled backend (null = interpreted)
        HostBindings                            host;
        // Interpreted: the private variable store. Compiled: OVERFLOW store for
        // undeclared names only (Set on an undeclared name still creates an entry).
        std::unordered_map<std::string, Value>  vars;
    };
    Inst*       find(InstanceId id);
    const Inst* find(InstanceId id) const;
    // Build a Context that routes variable access to the instance's private
    // store, property/show/hide to its host bindings, and the delegation hooks
    // (emit/bind/callExternal/self/gameInstance) back to the runtime.
    Context makeContext(InstanceId id);
    // Fire `event` on every listener bound to (owner, event). Bounded recursion.
    void dispatchToListeners(InstanceId owner, const std::string& event, const Value& arg);

    std::unordered_map<InstanceId, Inst> m_insts;
    // owner → event name → subscribed listener instances.
    std::unordered_map<InstanceId, std::unordered_map<std::string, std::vector<InstanceId>>> m_listeners;
    InstanceId m_next         = 1;
    InstanceId m_gameInstance = 0;
    int        m_dispatchDepth = 0;   // guards cross-instance event recursion (depth)
    // Total listener fires per top-level event cascade. The depth guard bounds
    // RECURSION only — a bind cycle of re-emitting listeners branches the
    // dispatch tree exponentially (every fire spawns an emit-dispatch AND a
    // trailing dispatch), so total work needs its own cap. Reset when the
    // cascade unwinds to depth 0; exceeding it aborts the cascade with an error.
    static constexpr int kMaxDispatchFires = 256;
    int        m_dispatchFires = 0;
    std::unordered_set<InstanceId> m_destructing; // ids mid-destroy (self-destruct guard)
    Services   m_services;
};

} // namespace HorizonCode
