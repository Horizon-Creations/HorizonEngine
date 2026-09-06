#pragma once
#include <cstdint>
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
    // The same two on a REFERENCED instance, addressing the element by NAME —
    // an id belongs to the asset that authored it, and a reference points at
    // one this graph did not. `target` is where they differ from the pair
    // above: those act on the instance that is running, these on the one the
    // Target pin names. Unbound (a host with no elements) reads as nothing and
    // writes nothing, like every other binding here.
    std::function<Value(InstanceId target, const std::string& elem, const std::string& prop)>
        getPropertyOn;
    std::function<void (InstanceId target, const std::string& elem, const std::string& prop,
                        const Value&)> setPropertyOn;
    std::function<void (InstanceId id)> showSelf;
    std::function<void (InstanceId id)> hideSelf;
};

// Which class an instance IS — the thing a Ref alone never said. Without it the
// runtime knows an instance's graph but not where the graph came from, so there
// is nothing for a Cast to compare against.
//
// `key` is the canonical class key, spelled exactly like CompiledClassTable's:
// the content-relative asset path for a class or widget, "__game_instance__" for
// the GameInstance, "level:<uuid>" for a level script. `baseClass` is the
// engine taxonomy row (HorizonCode.h engineClasses()); empty reads as "Object".
//
// Both default to empty so existing callers — and every test that just wants a
// graph to run — keep compiling. An instance with no key is simply castable to
// nothing but its base class, which is the honest answer for one nobody named.
struct ClassIdentity
{
    std::string key;
    std::string baseClass;
    // The HorizonCode classes this one derives from, NEAREST FIRST. Resolved
    // at load time (HcClassResolve.h) because the runtime cannot read an
    // asset — and because a class's ancestry cannot change under a running
    // instance anyway. Empty for a class that derives from an engine row
    // directly, which is every class that predates inheritance.
    std::vector<std::string> chain;
};

class HE_API Runtime
{
public:
    Runtime() = default;
    // Instances hold move-only compiled backends (CompiledPtr); spell that out
    // explicitly rather than relying on implicit deletion — MSVC's STL hard-errors
    // deep in <list> trying to instantiate unordered_map's copy-assign otherwise.
    Runtime(const Runtime&)            = delete;
    Runtime& operator=(const Runtime&) = delete;
    Runtime(Runtime&&)                 = default;
    Runtime& operator=(Runtime&&)      = default;

    // Register a running script. The runtime takes ownership of the graph (so a
    // caller's container reallocating can't dangle execution) and seeds the
    // instance's private variable store from the graph's declared defaults.
    InstanceId add(Graph graph, HostBindings bindings = {}, ClassIdentity cls = {});
    // The same, for a class with an inheritance chain: one graph per class,
    // ROOT FIRST, the class itself last. `add` is this with a single level.
    InstanceId addLevels(std::vector<Graph> levels, HostBindings bindings = {},
                         ClassIdentity cls = {});
    // Register a COMPILED script instance (ahead-of-time generated C++, see
    // HorizonCodeCompiled.h). Behaves exactly like add(): same id space, same
    // Context wiring, same delegation/GC participation. Returns 0 on null.
    // An omitted `cls` is filled in from the instance itself — a generated class
    // already knows its classKey() and baseClassKey(), so callers on that path
    // never have to repeat them.
    InstanceId addCompiled(CompiledPtr inst, HostBindings bindings = {}, ClassIdentity cls = {});

    // ── class identity ───────────────────────────────────────────────────────
    // Is `id` an instance of `classKey` — either exactly that class, or a class
    // derived from that engine base? This is what the Cast node runs on, and it
    // is deliberately ONE function for both backends: an interpreted and a
    // compiled instance must answer identically, or the codegen parity harness
    // would be comparing two different languages.
    bool instanceIsA(InstanceId id, const std::string& classKey) const;
    // The instance's class key / engine base ("" when it was registered without
    // one). Tooling and the Cast node's diagnostics; not a hot path.
    const std::string& classKeyOf(InstanceId id) const;
    const std::string& baseClassOf(InstanceId id) const;

    // ── the scene entity an instance owns (Entity classes) ───────────────────
    // Deliberately an opaque uint32: HE_Core knows nothing about entt, and this
    // is the whole of what it needs to know. EntityHost (HE_Scene) sets it when
    // it binds an instance to an entity; 0 = this instance owns none.
    void     setOwnedEntity(InstanceId id, uint32_t entity);
    uint32_t ownedEntity(InstanceId id) const;
    void       remove(InstanceId id);
    // Fire the instance's "Destruct" lifecycle event, then remove it — the
    // teardown counterpart to the "Construct" fired on create. Use this (not
    // remove) whenever an object/widget is intentionally destroyed so its
    // destructor graph runs; no-op if the id is already gone.
    void       destroy(InstanceId id);
    bool       alive(InstanceId id) const;
    // Drop every instance (whole-runtime teardown).
    void       clear();
    size_t     count() const { return m_insts.size() - m_doomed.size(); } // live only

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

    // Advance latent flow (Delay nodes): decrement every pending resume and run
    // the expired ones. The app calls this once per frame (game update / PIE
    // tick) on its runtime; an un-ticked runtime simply never resumes.
    //
    // TWO clocks, because a Delay can be either: `dt` is game time (already
    // scaled by HE::api::time, so a pause stops these waits), `unscaledDt` is
    // real frame time for the Delays whose Real Time pin is set — the only ones
    // that may still finish while the game is paused. Omitting the second
    // argument means "same clock for both", which is what every caller that has
    // no notion of a time scale (tests, tools) wants.
    void update(float dt, float unscaledDt = -1.0f);

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

    // ── the engine's own events, without a name ─────────────────────────────
    // Same delivery as fireEvent(id, "OnClicked", elem, …) — a compiled
    // instance takes the hook, an interpreted one the Runner, and either way
    // everyone bound to this instance still hears it. The difference is that
    // the caller never builds the string, and the compiled side never compares
    // one. The name still exists here, for the interpreted path and for the
    // listener list, which is keyed by it.
    void fireOnClicked(InstanceId id, int elem);
    void fireOnPressed(InstanceId id, int elem);
    void fireOnReleased(InstanceId id, int elem);
    void fireOnHovered(InstanceId id, int elem);
    void fireOnUnhovered(InstanceId id, int elem);
    void fireOnMouseEnter(InstanceId id, int elem);
    void fireOnMouseLeave(InstanceId id, int elem);
    void fireOnFocused(InstanceId id, int elem);
    void fireOnUnfocused(InstanceId id, int elem);
    void fireOnTextChanged(InstanceId id, int elem, const std::string& text);
    void fireOnTextCommitted(InstanceId id, int elem, const std::string& text);
    void fireOnValueChanged(InstanceId id, int elem, float value);
    void fireOnCheckChanged(InstanceId id, int elem, bool checked);
    void fireOnSelectionChanged(InstanceId id, int elem, int index);
    // List rows: fill this one in, and open this one. Both carry the ITEM index.
    void fireOnRowBind(InstanceId id, int elem, int index);
    void fireOnRowActivated(InstanceId id, int elem, int index);
    // A table's column title, by column index. Sorting is the owner's business.
    void fireOnHeaderClicked(InstanceId id, int elem, int column);
    void fireOnRightClicked(InstanceId id, int elem);
    // A file dropped from the desktop — the payload is its absolute path, and
    // elem 0 means the window itself took it (nothing under the pointer accepts).
    void fireOnFileDropped(InstanceId id, int elem, const std::string& path);
    // An entry in the tray menu was chosen (elem 0: the tray belongs to the
    // application, not to any element).
    void fireOnTrayItem(InstanceId id, int elem, const std::string& itemId);
    // …and one in the menu bar (elem 0 for the same reason).
    void fireOnMenuItem(InstanceId id, int elem, const std::string& itemId);
    // An HTTP request this application started has been answered (elem 0: it
    // belongs to the application). The payload is the TICKET, not the answer —
    // an event carries one value and a response is four, so the readers
    // (http.status, http.body, …) say what came back.
    void fireOnHttpResponse(InstanceId id, int elem, int ticket);
    // A path this application asked fs.watch about has appeared, vanished or
    // changed (elem 0: it belongs to the application). The payload is the PATH
    // in the spelling the watch used, so the graph can hand it straight back to
    // fs.readText; whether it now exists, and how big it is, are the readers'
    // job for the same reason the HTTP ticket leaves that to its own.
    void fireOnFileChanged(InstanceId id, int elem, const std::string& path);
    // A timer this application started has come due (elem 0: it belongs to the
    // application). The payload is the HANDLE, which is what tells two timers
    // apart — the same trade the HTTP ticket and the watched path make.
    void fireOnTimer(InstanceId id, int elem, int handle);
    // A link in a rich-text label — the payload is the id the markup gave it.
    void fireOnLinkClicked(InstanceId id, int elem, const std::string& linkId);
    // A day was picked in a calendar. The payload is "YYYY-MM-DD": the one
    // spelling of a date that no locale disagrees about, and the only one a
    // parser on the other end can be sure of.
    void fireOnDateChanged(InstanceId id, int elem, const std::string& isoDate);
    // A colour was picked. Its own event rather than OnValueChanged, because a
    // colour is four numbers and the pin type for it already exists.
    void fireOnColorChanged(InstanceId id, int elem, const glm::vec4& color);
    // Dragging inside the application. OnDrop's payload is what the SOURCE said
    // it was; OnDragEnded's bool is whether anything took it.
    void fireOnDragStarted(InstanceId id, int elem);
    void fireOnDrop(InstanceId id, int elem, const std::string& payload);
    void fireOnDragEnded(InstanceId id, int elem, bool accepted);
    // An animation reached its target — the payload is the property's name.
    void fireOnAnimationFinished(InstanceId id, int elem, const std::string& prop);
    // An authored CLIP reached its end — the payload is the clip's name. Its
    // own event, because a clip belongs to the widget and a tween to one
    // element, and a graph waiting for one must not hear the other.
    void fireOnClipFinished(InstanceId id, const std::string& clip);
    // A dialog, popup or menu closing — the whole widget, so no element.
    void fireOnDismissed(InstanceId id);
    void fireConstruct(InstanceId id);
    void fireDestruct(InstanceId id);
    void fireBeginPlay(InstanceId id);
    void fireTick(InstanceId id, float dt);
    void fireOnInit(InstanceId id);
    void fireOnShutdown(InstanceId id);
    void fireOnWindowFocusChanged(InstanceId id, bool focused);
    void fireOnLevelLoaded(InstanceId id);
    void fireOnLevelUnloaded(InstanceId id);
    // Physics contacts (Entity classes). `other` is the other entity's id, which
    // is what the whole event carries — CollisionEvent holds two ids and nothing
    // else. Overlap = a trigger was involved, Hit = a blocking contact.
    void fireOnBeginOverlap(InstanceId id, uint32_t other);
    void fireOnEndOverlap(InstanceId id, uint32_t other);
    void fireOnHit(InstanceId id, uint32_t other);
    void fireOnHitEnd(InstanceId id, uint32_t other);

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
        // position/rotationEuler are 3 floats each, or nullptr for "spawn where
        // the class authored it" (see Context::createObject — a zero vector is
        // NOT the same thing, which is why these are pointers).
        std::function<uint32_t(const std::string& classPath,
                               const float* position,
                               const float* rotationEuler)> createObject;
        std::function<void(uint32_t)> destroyObject;
        // Generic engine-API dispatch, forwarded to every instance's Context so any
        // EngineCall node reaches the HE::api registry. The app binds it to the
        // current world's registry Ctx (world/physics/content).
        // `self` is the instance whose graph made the call. The engine API
        // needs it for the handful of rows that answer "who am I" — which
        // scene entity this object sits on, above all — and world state alone
        // cannot say. 0 means the call came from outside HorizonCode.
        std::function<std::vector<Value>(InstanceId self, const std::string& apiId,
                                         const std::vector<Value>& args)> callApi;
    };
    void setServices(Services s) { m_services = std::move(s); }

private:
    struct Inst
    {
        Inst() = default;
        // CompiledPtr (unique_ptr) makes this move-only; spell that out explicitly
        // to match Runtime's own explicit move-only declaration (see the class-level
        // comment above) rather than leaving it implicit.
        Inst(const Inst&)            = delete;
        Inst& operator=(const Inst&) = delete;
        Inst(Inst&&)                 = default;
        Inst& operator=(Inst&&)      = default;

        // The instance's graphs, ROOT FIRST: one per class in its inheritance
        // chain, with the class itself last. A class that derives from nothing —
        // which is every class that predates inheritance — has exactly one.
        //
        // Kept as separate levels rather than merged into one graph so a base
        // class stays a base class at run time: its private variables are not
        // shadowed by a derived name that happens to match, an override can
        // still reach the version it replaced, and a node id means something
        // only within its own level. `leaf()` is the class the instance IS, and
        // is what everything that used to read a single `graph` asks for.
        std::vector<Graph>                      levels;     // interpreted; empty for compiled
        CompiledPtr                             compiled;   // compiled backend (null = interpreted)
        HostBindings                            host;

        // The most-derived level — the class this instance actually is.
        Graph&       leaf()       { return levels.back(); }
        const Graph& leaf() const { return levels.back(); }
        bool         hasGraph() const { return !levels.empty(); }
        // Its index. Anything that builds a Context for the leaf graph needs
        // this rather than 0, or per-node state would be filed under the root.
        size_t       leafLevel() const { return levels.empty() ? 0 : levels.size() - 1; }
        // Which class this is (see ClassIdentity) and, for an Entity class, the
        // scene entity it owns. Held here rather than derived from `compiled`
        // so the interpreted and the compiled path answer instanceIsA through
        // exactly the same field.
        ClassIdentity                           cls;
        uint32_t                                ownedEntity = 0;
        // Interpreted: the private variable store. Compiled: OVERFLOW store for
        // undeclared names only (Set on an undeclared name still creates an entry).
        std::unordered_map<std::string, Value>  vars;
        // Per-node state (DoOnce fired?, FlipFlop side) — persistent like vars
        // but never part of the variable store/public surface; cleared by
        // reseedVariables. Interpreted only (compiled classes keep members).
        //
        // ONE MAP PER LEVEL, and that is not tidiness: a node id is unique
        // within its own graph and nowhere else, so a base class and a derived
        // class both own a node 7. Sharing one map by id would have them share
        // one Do Once — the derived class's would arrive already fired.
        std::vector<std::unordered_map<int, Value>> nodeState;

        // The state map for one level, grown on demand so a level that never
        // runs a stateful node costs nothing.
        std::unordered_map<int, Value>& stateAt(size_t level)
        {
            if (nodeState.size() <= level) nodeState.resize(level + 1);
            return nodeState[level];
        }
    };
    // One scheduled Delay continuation (Runtime::update drives these).
    // `level` travels with it for the same reason nodeState is per level: the
    // node id alone does not say which graph to resume in.
    struct PendingResume { InstanceId id; size_t level; int node; float remaining;
                           bool realTime = false; };
    Inst*       find(InstanceId id);
    const Inst* find(InstanceId id) const;
    // Build a Context that routes variable access to the instance's private
    // store, property/show/hide to its host bindings, and the delegation hooks
    // (emit/bind/callExternal/self/gameInstance) back to the runtime.
    //
    // `level` is which of the instance's graphs the Runner about to use this
    // context is running. Everything keyed by NODE ID — per-node state and the
    // Delay continuations — needs it, because a node id only means something
    // inside its own graph.
    Context makeContext(InstanceId id, size_t level = 0);

    // ── dispatch across an instance's levels ─────────────────────────────────
    // Which level answers for a member, searched LEAF FIRST. That single rule
    // is the whole of "an override replaces the base": the derived class's
    // handler is found first and the search STOPS, so the base's never runs.
    // -1 = no level declares it, which is the ordinary case for most events.
    //
    // `elem` filters widget-element handlers (0 = any), the same rule
    // Runner::fireEvent applies within one graph.
    int levelHandlingEvent(const Inst& i, const std::string& event, int elem) const;
    // Functions searched the same way. `publicOnly` is what a call arriving
    // from ANOTHER level uses: a base class's private function is private to
    // that class, exactly as in C++.
    int levelWithFunction(const Inst& i, const std::string& fn, bool publicOnly) const;
    // Fire `event` on the one level that answers for it (no-op when none does).
    void runEventOnLevel(Inst& i, InstanceId id, const std::string& event,
                         int elem, const Value& arg);
    // Fire `event` on every listener bound to (owner, event). Bounded recursion.
    // The listener table is keyed by the interned id, not the name: dispatch
    // then costs an integer hash instead of a string one, and the engine's own
    // events intern theirs once into a static. `name` still travels along
    // because the LISTENER's own handler is still reached by name.
    void dispatchToListeners(InstanceId owner, EventId ev, const std::string& name,
                             const Value& arg);
    void dispatchToListeners(InstanceId owner, const std::string& name, const Value& arg);

    std::unordered_map<InstanceId, Inst> m_insts;
    // owner → event name → subscribed listener instances.
    std::unordered_map<InstanceId, std::unordered_map<EventId, std::vector<InstanceId>>> m_listeners;
    InstanceId m_next         = 1;
    InstanceId m_gameInstance = 0;
    // Which classes have already had their "Create Widget without a Show
    // Widget" hint printed (see widgetCreatorsWithoutShow). Keyed by class, not
    // by instance: a class spawned two hundred times has one thing wrong with
    // it, not two hundred.
    std::unordered_set<std::string> m_unshownWidgetWarned;
    // The same instance as an object, when it is a compiled one. Kept beside the
    // id so generated code can reach the GameInstance without the map lookup
    // every other reference needs — it is the one target whose class is known at
    // generation time ("__game_instance__"). Null while none is set, or while
    // the one that is set runs interpreted; both are ordinary states, not
    // errors, so the generated fast path still checks.
    CompiledInstance* m_gameInstanceCompiled = nullptr;
    int        m_dispatchDepth = 0;   // guards cross-instance event recursion (depth)
    // Total listener fires per top-level event cascade. The depth guard bounds
    // RECURSION only — a bind cycle of re-emitting listeners branches the
    // dispatch tree exponentially (every fire spawns an emit-dispatch AND a
    // trailing dispatch), so total work needs its own cap. Reset when the
    // cascade unwinds to depth 0; exceeding it aborts the cascade with an error.
    static constexpr int kMaxDispatchFires = 256;
    int        m_dispatchFires = 0;
    // Pending Delay continuations (append order preserved on expiry).
    std::vector<PendingResume> m_pending;
    std::unordered_set<InstanceId> m_destructing; // ids mid-destroy (self-destruct guard)
    // Removed instances whose BODY (graphs / compiled object) must survive until
    // no runner can still be executing it — a graph destroying its own instance
    // mid-run is legal ("Destroy Widget on Get Self"). find() hides these, so
    // they are logically gone the moment remove() marks them; purgeDoomed()
    // frees them at the next safe point.
    std::unordered_set<InstanceId> m_doomed;
    void purgeDoomed();
    // Cross-runner call depth (Call Function (Ref) / inherited calls) — each
    // such edge builds a FRESH Runner whose own depth/step budgets restart, so
    // unbounded recursion needs a runtime-wide account or it becomes a native
    // stack overflow.
    static constexpr int kMaxCallDepth = 64;
    int m_callDepth = 0;
    Services   m_services;
};

} // namespace HorizonCode
