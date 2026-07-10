# HorizonCode → C++ Codegen: Implementation Plan

Status: **WP0–WP5 IMPLEMENTED** (this branch). The codegen ships: 15 parity
fixtures trace-exact interpreted↔compiled, the export builds + ships
`HorizonCodeGen.dylib`, the game loads it through `CompiledClassTable` with the
version handshake, all four hosts consult it. Remaining: WP6 (docs/CI polish,
deployed-editor SDK staging) and the WP7 optimizations; plus a manual HW smoke
of a real packaged export. Implementation deviations from this plan are marked
inline with **[impl]**. This is the concrete, step-by-step successor to
the design doc `horizoncode-cpp-codegen-plan.md` (§ references to "design doc"
point there). It is grounded in the code as of this branch — every file, type
and behavior named below was verified against the sources.

Contents:
1. [What already exists (verified inventory)](#1-what-already-exists-verified-inventory)
2. [Architecture at a glance](#2-architecture-at-a-glance)
3. [The semantic contract — exact interpreter behavior codegen must reproduce](#3-the-semantic-contract)
4. [The compiled object model — `CompiledInstance` + Runtime integration](#4-the-compiled-object-model)
5. [The codegen library — stages, IR, full node lowering](#5-the-codegen-library)
6. [Type mapping & the support header](#6-type-mapping--the-support-header)
7. [Engine-API integration & expandability](#7-engine-api-integration--expandability)
8. [Packaging & toolchain integration](#8-packaging--toolchain-integration)
9. [Runtime loading in the shipped game](#9-runtime-loading-in-the-shipped-game)
10. [Parity verification (tests)](#10-parity-verification)
11. [Work packages & order](#11-work-packages--order)
12. [Risks & decided questions](#12-risks--decided-questions)
13. [Extension: function-local variables](#13-extension-function-local-variables)

---

## 1. What already exists (verified inventory)

The design doc's prerequisites have all landed. Codegen builds ON these; none
of them need redesign:

| Piece | Where | State |
|---|---|---|
| Graph model (`Node`, `Link`, `Variable`, `Value`, `PinType`, `signatureOf`, pin ranges, `fromJson`/`toJson`, `syncFunctionSignatures`, `assignSubgraphs`) | `src/HE_Core/include/HorizonCode/HorizonCode.h` + `.cpp` | complete; 50 node types incl. arrays, ForEach, EngineCall, Transform |
| Interpreter (`Runner`: push exec / pull data, `m_execOutputs` cache, call frames, `coerce`, step/depth limits) | `HorizonCode.cpp` | complete — §3 is its exact spec |
| Central interpreter host (`Runtime`: instances, private var stores, delegation/bind/emit, GameInstance, GC `retainOnlyReachableFrom`, `Services` incl. `callApi`) | `HorizonCode/HorizonCodeRuntime.h/.cpp` | complete |
| `HE::api` + machine-readable `ApiFn` registry (19 groups / ~140 fns; `id`, `category`, `isExec`, typed `params`/`results`, `cppCall`, Value-marshalling `invoke`, `displayName`) | `src/HE_Scene/include/HorizonScene/EngineApi.h` + `src/EngineApi.cpp` | complete; the single source the EngineCall node, Lua/Py dispatchers and codegen all read |
| Generic **EngineCall** node (pins mirrored from the descriptor; `hasArg` = `isExec`; exec results cached, pure calls re-dispatch per pin read) | `HorizonCode.h/.cpp` | complete |
| Four hosts (GameInstance `__game_instance__`, level script in `.hescene`, widgets via `WidgetManager` (`widget id == scriptId`), HC class assets via `svc.createObject`) | `GameApplication.cpp` ~140–260, `WidgetManager.cpp` | complete |
| Native game-logic dylib load path (`IGameLogic` C factory exports, `GameLogicLoader`, dlopen hot-copy; exporter routes `GameLogic.*` into the bundle) | `IGameLogic.h`, `GameLogicLoader.h`, `ProjectExporter.cpp:604` | complete — the model our generated dylib copies |
| Export pipeline (async worker thread in `EditorUI.cpp` ~2299, `ProjectExporter::exportProject`, profiles with **`compileHorizonCode` flag already plumbed** (`ProjectManager.h:50`, persisted, editor toggle), runtime-bundle resolution `findRuntimeBundle`, `.app` bundling, incremental pack) | `EditorUI.cpp`, `Hpak/ProjectExporter.h/.cpp`, `ProjectManager.h/.cpp` | complete; the flag currently only logs |
| Class enumeration & asset access (`HcEditorUtil::listAssets/listHorizonCodeClasses`, `ContentManager::getHorizonCodeClass`, `graphJson` on widget + HC-class assets, `kGameInstanceEntry`, `sceneUuidForPath`) | `HE_Editor/HcClassList.h`, ContentManager | complete |
| Test infra: fixture dylib compiled by CMake and loaded at test time (`test_gamelogic` target), `test_engine_api`, `test_horizoncode_runtime`, `test_game_instance`, `test_level_script` | `tests/CMakeLists.txt:100` | the pattern the parity harness reuses |

What does **not** exist yet (the actual work): the codegen library, the
compiled-instance object model in the Runtime, the generated-dylib
build/ship/load path, and the parity harness.

---

## 2. Architecture at a glance

```
                     EDITOR / PIE (unchanged)                    SHIPPED GAME
                 ┌──────────────────────────────┐      ┌────────────────────────────────┐
 .hcode/.hasset  │ HorizonCode::Runtime          │      │ HorizonCode::Runtime            │
 graphJson ─────▶│   Inst = Graph + Runner       │      │   Inst = Graph+Runner  ◀─fallback│
                 │   (interpreted, always)       │      │   Inst = CompiledInstance ◀─────┼── HorizonCodeGen.dylib
                 └──────────────────────────────┘      └────────────────────────────────┘         ▲
                                                                                                    │ cmake --build
        EXPORT (async worker thread, profile.compileHorizonCode)                                   │
        collect graphs ─▶ HE::hccg::generate() ─▶ <out>/_hcgen/*.cpp/.h + hc_registry.cpp ─▶ toolchain
        (classes, widgets,   (validate → IR →        + CMakeLists.txt (links HorizonCore/Scene SDK)
         level scripts, GI)   emit C++)
```

Principles (unchanged from the design doc):
- **Editor always interprets.** Codegen runs only at export; PIE iteration cost
  stays zero.
- **One Runtime, two instance backends.** Compiled instances register in the
  *same* `HorizonCode::Runtime`, get the *same* `InstanceId` handles, the same
  `Context` (services, delegation) — so Refs, Bind/Emit, `callExternal`, GC and
  mixed compiled/interpreted populations just work.
- **The registry is the only engine surface.** Generated code reaches the
  engine exactly where the interpreter does (`Context::callApi` → `ApiFn`), so
  a new API function needs a registry row and *zero* codegen changes (§7).
- **Per-asset hybrid fallback.** Any graph the generator can't (or shouldn't)
  compile ships as `graphJson` + interpreter — which it does today anyway, so
  fallback is "registry lookup misses → existing code path". Never a silent
  cap: every fallback is logged with a reason and listed in the export result.

---

## 3. The semantic contract

This section is the normative spec of `Runner`/`Runtime` behavior (from
`HorizonCode.cpp` / `HorizonCodeRuntime.cpp`). The parity harness (§10) tests
exactly these clauses; the emitter (§5) implements them. Line refs are to
`HorizonCode.cpp` at the time of writing.

### 3.1 Run lifecycle
- **`fireEvent(name, elem, arg)`** (`:875`): resets `m_steps`, stores `arg`,
  **clears `m_execOutputs` and the call stack once**, then fires **every**
  `Event` node with `n.s == name` and (`n.elem == 0 || n.elem == elem`), **in
  `graph.nodes` vector order**, each via `runExecChain`. All matching handlers
  of one fire share one exec-output cache.
- **`callFunction(name, requirePublic, args, results)`** (`:889`): finds the
  **first** `FunctionEntry` with that name; private + `requirePublic` → `false`
  (no side effects). Resets steps/cache/stack. Call frame: `args[i] =
  coerce(passed[i], param[i].type)`, missing args → typed default; result slots
  seeded typed defaults. Runs the entry's exec chain; copies frame results out.
- One `Runner` is constructed **per fire/call** (`Runtime::fireEvent :220`), so
  nested runs (dispatcher → other instance → back) have independent caches.

### 3.2 Exec flow (`runExecChain` `:919`)
- Follows the single exec link off the given pin; at each hop `++m_steps`,
  abort with a warning at **4096** steps. Chain continues from each node's
  **first** exec-out (`execOut0`) — except `Branch`, `Sequence`, `ForEach`,
  which steer their own exec-outs internally and terminate the outer chain.
- `FunctionReturn` has no exec-out → terminal.
- Recursion/nesting depth guard: **64** (`kMaxDepth`) on both exec and data
  recursion; exceeding silently stops that sub-tree.

### 3.3 Data flow (pull model, `evalData`/`evalInput` `:1078`)
- **Wired input**: evaluate the source node's data-out **at every read** (pure
  nodes re-evaluate; no memoization). `Graph::connect` guarantees wire ends
  have **equal** `PinType` + equal array-ness, so raw field reads (`.f`, `.b`,
  `.s`, …) across a wire are type-safe.
- **Unwired input**: the node's `pinDefaults[dataInIndex]` **coerced to the pin
  type**, else the pin type's zero value.
- **Exec-cached outputs** (`m_execOutputs[nodeId]`, a `vector<Value>` per
  node): written when the exec node runs (`CreateWidget`/`CreateObject` → the
  Ref; `FunctionCall`/`CallExternal` → returned results; exec `EngineCall` →
  raw results; `ForEach` → `{element, index}` per iteration). Reads **before**
  the node ran (or out-of-range pin) return `Value{}` → the C++ zero of the
  read field. The cache is **one flat map per run** — not scoped per call
  frame. Consequences codegen must reproduce (§5.4): values persist after
  `ForEach`'s `Done`; a recursive inner call **overwrites** the outer
  invocation's cached node outputs.
- **`coerce(v, want)`** (`:847`): arrays pass through untouched; equal type
  passes through; only Float↔Int↔Bool convert (`Bool→Float` 1/0, `Float→Int`
  C-cast truncation, `Int→Bool` `!= 0`, …); **any other mismatch → the zero
  value of the target type** (e.g. String→Float is 0, not parsed).
  Coercion points: Set{Variable,Property,External} value → `propType`;
  FunctionCall/CallExternal/EngineCall args → declared param types;
  FunctionReturn values → result types; EmitEvent arg → `propType`; Event
  data-out ← `m_eventArg` coerced to `propType`; Get{Property,Variable,
  External} result → `propType`; pin defaults → pin type.

### 3.4 Node-level semantics (the fine print)
- `Branch`: reads `Cond` (Bool by wiring), True chain else False chain.
- `Sequence`: Then 0 chain fully, then Then 1 chain.
- `ForEach`: evaluates `Array` **once**; per element sets cache
  `{items[i], Int(i)}` then runs Body; after the last element runs Done. Empty
  array → Done only. Element/Index reads outside Body see the **last**
  iteration's values.
- `SetVariable`/`SetProperty`/`SetExternal` **data pass-through**: the Value
  data-out **re-evaluates the Value input** (not the store) — an assignment
  expression, no extra side effect.
- Set on an **undeclared** variable name creates a store entry
  (`Runtime::setVariable` inserts); Get on an unknown name yields `Value{}`.
- `Divide`: divisor `== 0.0f` → result 0. `Equals`: `|a-b| < 1e-6`.
  `ToString`: `snprintf("%g")`. `Concat`: `a + b`.
- Arrays: pure **copy semantics** (Add/Set/Insert/Remove return a new array).
  `ArrayGet` out of range → warning log + element-type default; `ArraySet`/
  `ArrayRemove` out of range → unchanged copy; `ArrayInsert` clamps the index
  to `[0, size]`. `Contains`/`IndexOf` compare with `valueEquals` (exact field
  equality per element type, `Float` bit-equal `==`, not epsilon).
- `Print`: `Logger Info`, prefix `"[Widget] "`, value coerced to String.
- `CreateObject` failure (class not found) → Error log + cached Ref 0.
- `FunctionCall`: resolves the entry **in the same graph** by name; no entry →
  silent no-op (nothing cached). Args are evaluated **in the caller's frame
  before pushing** the callee frame; `FunctionEntry` param data-outs always
  read the **innermost** frame.
- Pure `EngineCall` (no exec pins): args evaluated and the registry `invoke`
  dispatched **on every data-out read** — a node with N wired outputs read once
  each dispatches **N times**. (Observable: `physics.raycast` as a pure call
  runs per read.) Exec `EngineCall`: dispatched once at its exec position, args
  coerced to the node's `params` types, results cached raw.
- Unbound `Context` members (null `std::function`) → no-op / `Value{}`.

### 3.5 Runtime-level semantics (shared by both backends — NOT reimplemented by codegen)
These live in `Runtime` and stay there; compiled instances reach them through
the same bound `Context`, so behavior is identical by construction:
- `bindEvent`: both ids must exist; per (owner, event) listener list, **append
  order, deduplicated**; binding a dead target logs the "null reference" error.
- `fireEvent` on an instance also dispatches to its listeners **after** the
  owner's own handlers; `emitEvent` dispatches to listeners **only**. Dispatch
  recursion guard: depth **32**; the listener list is **copied** before
  iterating (a handler may re-bind); a listener never self-dispatches
  (`l != owner`).
- `destroy(id)` fires `"Destruct"` then removes; re-entrant destroy of the same
  id is guarded (`m_destructing`).
- `callExternal`/`get/setExternal` null-target → `hcError` "null reference…";
  missing/private member → `hcWarn`. The public-access check reads the
  target's **declared variable list** (interpreted: `graph.findVariable`;
  compiled: `varInfos()`, §4).
- GC `retainOnlyReachableFrom(root)`: mark via **Ref-typed variable values**
  (interpreted: the var store; compiled: `collectRefs()`), sweep with
  `destroy` (fires Destruct).
- `WidgetManager::isInteractive` scans the script's **Event nodes** for
  pointer-event names per element (`WidgetManager.cpp:183`); compiled widgets
  must expose the equivalent static list (§4, `eventInfos`).

### 3.6 Sharpened (non-goals for parity)
Two limits are reproduced *in spirit*, not step-exactly, because their exact
trigger points encode interpreter internals:
- The 4096-step abort: generated code counts one step per emitted statement and
  per inlined pure-node evaluation (same constant, `RunState.steps`), sets an
  `aborted` flag and unwinds. Ordinary graphs never hit it; parity fixtures
  stay far below the limit and one dedicated fixture asserts only "both sides
  abort without crashing", not the identical cut point.
- The depth-64 guard: emitted as a recursion-depth counter on function entry
  and ForEach nesting. Same rationale.

Everything else in §3.1–3.5 is exact and trace-verified.

---

## 4. The compiled object model

### 4.1 `CompiledInstance` (new, HE_Core)

New header `src/HE_Core/include/HorizonCode/HorizonCodeCompiled.h` (+ small
`.cpp`), exported with `HE_API`:

```cpp
namespace HorizonCode {

// Static, per-class metadata the Runtime needs where it reads the Graph today.
struct CompiledVarInfo   { const char* name; PinType type; bool isArray; int access; };
struct CompiledEventInfo { const char* name; int elem; };   // one per Event node

class HE_API CompiledInstance
{
public:
    virtual ~CompiledInstance() = default;

    // ── class metadata (backed by static tables in the generated class) ──
    virtual const char* classKey() const = 0;                       // registry key (§9.1)
    virtual const std::vector<CompiledVarInfo>&   varInfos()   const = 0;
    virtual const std::vector<CompiledEventInfo>& eventInfos() const = 0;

    // ── execution (mirror Runner's entry points; §3.1) ──
    virtual void fireEvent(const std::string& name, int elem, const Value& arg) = 0;
    virtual bool callFunction(const std::string& name, bool requirePublic,
                              const std::vector<Value>& args,
                              std::vector<Value>* results) = 0;

    // ── variable reflection (Get/SetExternal, GC, reseed, tooling) ──
    virtual Value getVariable(const std::string& name) const = 0;
    virtual bool  setVariable(const std::string& name, const Value& v) = 0; // false = unknown
    virtual void  reseedVariables() = 0;                    // back to declared defaults
    virtual void  collectRefs(std::vector<uint32_t>& out) const = 0; // Ref-typed values (incl. Ref arrays)

    // ── wiring (Runtime calls this right after construction) ──
    void bindContext(Context ctx) { m_ctx = std::move(ctx); }

protected:
    Context m_ctx;   // the SAME Context the interpreter gets (makeContext(id))
};

// C-ABI manifest exported by the generated dylib (same pattern as IGameLogic).
struct CompiledClassEntry
{
    const char*        key;                       // canonical class key (§9.1)
    CompiledInstance* (*create)();
    void              (*destroy)(CompiledInstance*);
};
// extern "C" HE_GAME_API const HorizonCode::CompiledClassEntry*
//     HE_HorizonCodeGenClasses(int* count, const char** engineVersion);

} // namespace HorizonCode
```

Notes:
- `m_ctx` is the exact `Context` from `Runtime::makeContext(id)` — variable
  get/set routed back to the instance (see 4.2), host property access,
  delegation, services, `callApi`. Generated code calls host effects through
  it *identically* to the interpreter, which is what makes §3.5 free.
- `getVariable`/`setVariable` are implemented by the generated class as a
  name→member switch (typed members ↔ `Value` conversion, §6). Undeclared
  names: `setVariable` returns false and the Runtime falls back to a per-
  instance **overflow map** (4.2) so §3.4's "Set on undeclared name creates an
  entry" still holds.
- Dylib-boundary rule: create/destroy pairs cross the C ABI; the Runtime wraps
  them in a `unique_ptr` with a custom deleter. Same-toolchain C++ ABI beyond
  that is already accepted practice (IGameLogic passes `HorizonWorld&`).

### 4.2 Runtime changes (`HorizonCodeRuntime.h/.cpp`)

`Inst` gains a compiled backend; every member function branches once:

```cpp
struct Inst
{
    Graph                                   graph;      // interpreted; empty for compiled
    CompiledPtr                             compiled;   // unique_ptr<CompiledInstance, ManifestDeleter>
    HostBindings                            host;
    std::unordered_map<std::string, Value>  vars;       // interpreted store; for compiled:
                                                        // OVERFLOW store for undeclared names only
};
```

- **`addCompiled(CompiledPtr, HostBindings)`** → new `InstanceId`; calls
  `compiled->bindContext(makeContext(id))`; no var seeding (the generated ctor
  initializes members to the declared defaults).
- `fireEvent` / `callFunction`: compiled → forward to the instance (then, for
  fireEvent, `dispatchToListeners` exactly as today).
- `getVariable`/`setVariable`: compiled → try `compiled->get/setVariable`,
  fall back to the overflow map.
- `reseedVariables`: compiled → `compiled->reseedVariables()` + clear overflow.
- `variablesOf` (tooling/tests): keep for interpreted; add
  `variablesSnapshot(id)` that materializes a map for either backend (compiled:
  iterate `varInfos()` + `getVariable`).
- `retainOnlyReachableFrom`: marking step per instance = interpreted var-store
  scan **or** `compiled->collectRefs(...)`; sweep unchanged.
- `makeContext`'s `getExternal`/`setExternal`: access check = interpreted
  `graph.findVariable(var)` **or** compiled `varInfos()` lookup (`access == 0`).
- **New** `eventBindingsOf(InstanceId) → span/vector of {name, elem}` serving
  both backends (interpreted: scan Event nodes; compiled: `eventInfos()`).
  `WidgetManager::isInteractive` switches from `graphOf(...).nodes` to it —
  the only `graphOf` consumer outside the runtime (verified by grep).
- `setGameInstanceCompiled(CompiledPtr, HostBindings)` alongside
  `setGameInstance`.

Everything else (`bindEvent`, `emitEvent`, `dispatchToListeners`, `destroy`,
listener bookkeeping, `remove`, `clear`) is backend-agnostic already.

**Gate:** WP0 lands with a *hand-written* `CompiledInstance` in the tests
(tests/fixtures) exercising: add/fire/call, external get/set with access check,
bind/emit both directions compiled↔interpreted, GC via `collectRefs`,
widget-interactivity via `eventBindingsOf`. This proves the object model before
any generator exists.

---

## 5. The codegen library

### 5.1 Placement & API

New module in **HE_Scene** (it needs `HorizonCode::Graph` from HE_Core *and*
the `HE::api` registry, both of which HE_Scene already links/contains):

```
src/HE_Scene/include/HorizonScene/HcCodegen.h
src/HE_Scene/src/HcCodegen.cpp            (front end + IR + emitter; split if it grows)
```

```cpp
namespace HE::hccg {

struct ClassSource
{
    std::string           key;        // canonical registry key (§9.1)
    std::string           label;      // for diagnostics ("MainMenu.hasset")
    HorizonCode::Graph    graph;      // post-fromJson (signatures synced, subgraphs assigned)
};

struct Options
{
    bool        traceHooks = false;   // parity harness: emit HC_TRACE at every host/engine call
    std::string namespaceName = "hcgen";
    std::string engineVersion;        // baked into the manifest, checked at load (§9.2)
};

struct GeneratedFile { std::string name; std::string contents; };

struct Result
{
    bool ok = false;                              // false only on internal errors
    std::vector<GeneratedFile> files;             // <Class>.h/.cpp per class + hc_registry.cpp
    struct Fallback { std::string key, reason; }; // validated-out graphs (ship interpreted)
    std::vector<Fallback> fallbacks;
    std::vector<std::string> warnings;
};

Result      generate(const std::vector<ClassSource>&, const Options&);
std::string generateCMakeLists(const Options&, const std::vector<std::string>& cppFiles);
}
```

Consumers: the editor export worker (in-process), the `hc_codegen` CLI (§10.2),
tests.

### 5.2 Stage A — load & validate (per class)

Input graphs come from `HorizonCode::fromJson` (which already runs
`syncFunctionSignatures` + `assignSubgraphs`). Validation produces either an IR
or a `Fallback{key, reason}`:

1. **EngineCall nodes**: `HE::api::find(n.s)` must succeed. If the node's
   mirrored `params`/`results` disagree with the descriptor (stale asset after
   a registry change), **re-mirror from the registry** and warn (the node
   stores the mirror only so pins resolve without the registry; the registry is
   authoritative). Unknown id → fallback (`"unknown engine api 'x.y'"`).
2. **Exec-graph cycle check**: DFS over exec links (per entry point). A cycle
   would compile to an unbounded loop → fallback (`"exec cycle at node N"`).
   (The interpreter tolerates cycles only via the 4096-step abort; authored
   graphs with intentional cycles keep working — interpreted.)
3. **Pure-data cycle check**: DFS over data links restricted to pure nodes →
   fallback (interpreter yields `{}` via the depth guard; not worth emulating).
4. **FunctionCall without a local `FunctionEntry`** → warn, lower to no-op
   (matches interpreter `:1018`).
5. Names: build the mangling tables (variables, functions, event handlers) —
   deterministic, collision-suffixed (`v_Score`, `f_Reset`, `ev_Tick_12`).
6. Collect undeclared-but-Set variable names → synthetic private members typed
   by the Set node's `propType` (§3.4), marked non-public in `varInfos` so
   external access warns exactly like the interpreter.

Validation failures never fail the export: the class ships interpreted and the
reason is reported (§8.4).

### 5.3 Stage B — IR

Small and explicit; one `ClassIR` per graph:

```cpp
struct ExprIR    // a pure data read, tree-shaped
{  enum Kind { Const, PinDefault, ZeroOf, VarRead, PropRead, ExternalRead, SelfRef, GIRef,
               EventArg, FrameParam, CachedOut, PureEngineCall, MathOp, ArrayOp, SetPassThrough, … };
   PinType type; bool isArray; /* per-kind payload + children */ };

struct StmtIR    // one exec node occurrence
{  enum Kind { Branch, Sequence, ForEach, SetVar, SetProp, SetExternal, ShowSelf, HideSelf,
               CreateWidget, WidgetOp, CreateObject, DestroyObject, BindEvent, EmitEvent,
               CallExternal, FunctionCall, FunctionReturn, EngineCallExec, Print };
   int nodeId; std::vector<ExprIR> inputs; std::vector<StmtIR> blocks[2]; /* branch arms / seq / body+done */ };

struct HandlerIR { std::string event; int elem; PinType argType; bool hasArg; std::vector<StmtIR> body; int nodeId; };
struct FunctionIR{ std::string name; int access; params/results; std::vector<StmtIR> body; };
struct ClassIR   { key, className; std::vector<VarIR> vars; std::vector<HandlerIR> handlers /*graph order!*/;
                   std::vector<FunctionIR> functions; std::vector<CacheSlot> runStateSlots; };
```

Lowering walk = exactly `runExecChain`: from each entry node, follow the first
exec-out link; recurse into Branch arms / Sequence thens / ForEach body+done;
stop at FunctionReturn or link end. At each statement, `evalInput` boundaries
become `ExprIR` trees (wired source → recurse into the producer's data-out;
unwired → `PinDefault` **constant-folded through `coerce` at generation time**,
or `ZeroOf`).

`CacheSlot`s (the `RunState` fields, §5.4) are allocated for every data-out of
every exec-cached node encountered: `CreateWidget`/`CreateObject` (1 Ref),
`FunctionCall`/`CallExternal` (one per result), exec `EngineCall` (one per
result), `ForEach` (element + index). Multiple occurrences share by node id —
the map key in the interpreter.

### 5.4 Stage C — emission model

Per class `Foo` → `hcgen_Foo.h/.cpp`:

```cpp
namespace hcgen {

class C_MainMenu final : public HorizonCode::CompiledInstance
{
public:
    C_MainMenu();                                  // members ← declared defaults
    const char* classKey() const override;
    const std::vector<HorizonCode::CompiledVarInfo>&   varInfos()   const override;  // static table
    const std::vector<HorizonCode::CompiledEventInfo>& eventInfos() const override;  // static table

    void fireEvent(const std::string& name, int elem, const HorizonCode::Value& arg) override;
    bool callFunction(const std::string&, bool, const std::vector<HorizonCode::Value>&,
                      std::vector<HorizonCode::Value>*) override;
    HorizonCode::Value getVariable(const std::string&) const override;   // switch on name
    bool setVariable(const std::string&, const HorizonCode::Value&) override;
    void reseedVariables() override;
    void collectRefs(std::vector<uint32_t>&) const override;

private:
    // ── graph variables → typed members ──
    float        v_Score  = 0.0f;
    hc::Array<uint32_t> v_Items;      // seeded in ctor from defaultItems

    // ── per-run state: mirrors Runner exactly (§3.3) ──
    struct RunState
    {
        int  steps = 0, depth = 0;
        bool aborted = false;
        HorizonCode::Value eventArg;        // this fire's arg (coerced per Event read)
        // one field per exec-cached data-out, zero-initialized (= unwritten cache):
        uint32_t     n7_o0  = 0;            // CreateWidget #7 → Widget ref
        float        n12_o0 = 0.0f; int n12_o1 = 0;   // ForEach #12 element/index
        std::string  n19_o0;                // CallExternal #19 result 0
    };

    // one body function per Event node / FunctionEntry:
    void ev_Construct_3(RunState& rs);
    void ev_OnClicked_9(RunState& rs);                       // elem check done in fireEvent
    void f_AddScore(RunState& rs, float p_Amount, float& r_Total);
};
} // namespace hcgen
```

Dispatch:

```cpp
void C_MainMenu::fireEvent(const std::string& name, int elem, const Value& arg)
{
    RunState rs; rs.eventArg = arg;              // fresh per fire == clear-once (§3.1)
    if (name == "Construct") { ev_Construct_3(rs); }         // node order preserved:
    else if (name == "OnClicked") {                          // consecutive same-name events
        if (/*n9.elem*/ 2 == 0 || 2 == elem) ev_OnClicked_9(rs);
        // …every further OnClicked Event node, in graph order, sharing rs
    }
}

bool C_MainMenu::callFunction(const std::string& name, bool requirePublic,
                              const std::vector<Value>& args, std::vector<Value>* results)
{
    if (name == "AddScore") {
        // access = public → no requirePublic gate here (private fns emit the gate)
        RunState rs;
        float p0 = hc::coerceFloat(hc::arg(args, 0));        // §3.1 arg coercion
        float r0 = 0.0f;                                     // typed default result
        f_AddScore(rs, p0, r0);
        if (results) *results = { Value::ofFloat(r0) };
        return true;
    }
    return false;
}
```

**The `RunState` decision (the one subtle lowering, design doc §4.4):** the
interpreter's `m_execOutputs` is a single map per run shared across nested
function calls. Emitting cache slots as *locals* of each generated function
would diverge under recursion (the outer invocation would keep its own values,
the interpreter does not — the inner call overwrites). Making them `RunState`
fields, allocated **once per fireEvent/callFunction entry** and passed
`RunState&` through every internal body function, reproduces the interpreter
**exactly**: zero-init = "not yet cached", overwritten by re-execution,
persisting after ForEach/Done, shared across recursion, fresh per run and per
nested Runtime dispatch. Per-invocation locals become a later, parity-gated
optimization (WP7).

Internal `FunctionCall` lowering (frame args/results become C++
parameters/out-params — the frame stack maps onto the native call stack, which
is exact because `FunctionEntry` reads only the innermost frame):

```cpp
{ HC_STEP(rs);
  float a0 = hc::coerceFloat(/*expr for arg 0*/);   // caller-frame evaluation before the call
  float r0 = 0.0f;
  if (++rs.depth <= 64) f_AddScore(rs, a0, r0);
  --rs.depth;
  rs.n21_o0 = r0;                                   // the call node's cached data-outs
}
```

`FunctionReturn`: assign the (coerced) expressions to the out-params, then
`return;`.

### 5.5 Full node lowering table

`E(x)` = the inlined expression for data-in x (re-emitted at **every** read —
that is the interpreter's re-evaluation, §3.3). `rs.nID_oK` = cache slot.
Every statement begins with `HC_STEP(rs)` (guard: sets `rs.aborted`, callers
early-return; §3.6).

| NodeType | Lowering |
|---|---|
| `Event` (stmt) | entry point → `ev_<name>_<id>(rs)`; (expr, data-out) → `hc::coerce<T>(rs.eventArg)` per the node's `propType` |
| `FunctionEntry` | member fn; param data-out k → the C++ parameter `p_k` |
| `FunctionCall` | §5.4 block; data-outs → `rs.nID_oK`; no local entry → omit (warn) |
| `FunctionReturn` | assign out-params (coerced to result types), `return` |
| `Branch` | `if (E(Cond)) { …True… } else { …False… }` |
| `Sequence` | `{ …Then0… } { …Then1… }` |
| `ForEach` | `auto arr = E(Array); for (i…){ rs.nID_o0 = arr[i]; rs.nID_o1 = (int)i; …Body… } …Done…` |
| `GetVariable` | member read (declared) / overflow via reflection is N/A in-class — undeclared reads emit `T{}` only if never Set; Set-anywhere names became synthetic members (§5.2.6) |
| `SetVariable` | `v_X = hc::coerce<T>(E(Value));` — pass-through data-out re-emits `E(Value)` |
| `GetProperty` | `hc::coerce<T>(m_ctx.getProperty ? m_ctx.getProperty(elem, "prop") : Value{})` |
| `SetProperty` | `if (m_ctx.setProperty) m_ctx.setProperty(elem, "prop", hc::toValue(coerced));` |
| `ShowWidget`/`HideWidget` | `if (m_ctx.showSelf) m_ctx.showSelf();` etc. |
| `CreateWidget` | `rs.nID_o0 = (uint32_t)(m_ctx.createWidget ? m_ctx.createWidget("path") : 0);` |
| `ShowWidgetId`/`HideWidgetId`/`DestroyWidget` | `m_ctx.showWidget((int)E(Widget))` … |
| `CreateObject` | `rs.nID_o0 = m_ctx.createObject ? m_ctx.createObject("path") : 0;` (+the interpreter's error log on 0 comes from the service side already? — **no**: the log is in `execNode :984`; emit `hc::logCreateObjectFail("path")` on 0) |
| `DestroyObject` | `if (m_ctx.destroyObject) m_ctx.destroyObject(E(Object));` |
| `GetExternal` | `hc::coerce<T>(m_ctx.getExternal ? m_ctx.getExternal(E(Target), "var") : Value{})` |
| `SetExternal` | `m_ctx.setExternal(E(Target), "var", toValue(coerce(E(Value))))`; pass-through = `E(Value)` |
| `BindEvent` | `if (m_ctx.bindEvent) m_ctx.bindEvent(E(Target), "event");` |
| `EmitEvent` | `if (m_ctx.emitEvent) m_ctx.emitEvent("event", hasArg ? toValue(coerce(E(Arg))) : Value{});` |
| `CallExternal` | build `std::vector<Value>` args (coerced to node `params`); `auto r = m_ctx.callExternal(E(Target), "fn", args); rs.nID_oK = hc::fromValue<T>(r, K);` |
| `GetGameInstance`/`GetSelf` | `hc::refOf(m_ctx.getGameInstance)` / `…getSelf…` (0 when unbound) |
| `Const*` | literal (`0.5f`, `true`, `"text"`, `glm::vec2(…)`, `glm::vec4(…)`, `hc::Transform{…}`) |
| `Add/Sub/Mul` | `(E(A) <op> E(B))` on `float` |
| `Divide` | `hc::div(E(A), E(B))` (0-divisor → 0) |
| `Greater/Less` | `(E(A) < E(B))` etc. → `bool` |
| `Equals` | `hc::feq(E(A), E(B))` (`fabs < 1e-6`) |
| `And/Or/Not` | `&&`, `||`, `!` — **note**: C++ short-circuits, the interpreter evaluates both sides. With pure-only inputs the only observable is pure-EngineCall dispatch count → emit `hc::land(E(A), E(B))` (a function call: both args evaluated) to stay trace-exact |
| `Concat` | `(E(A) + E(B))` |
| `ToString` | `hc::toStringG(E(x))` (`%g`) |
| `ArrayMake` | `hc::Array<T>{}` |
| `ArrayLength` | `(int)E(Array).size()` |
| `ArrayGet` | `hc::arrGet(E(Array), E(Index))` (range-checked, warn + `T{}`) |
| `ArrayAdd/Set/Insert/Remove` | `hc::arrAdd(E(Array), coerce(E(Value)))` … (copying helpers, clamped per §3.4) |
| `ArrayContains/IndexOf` | `hc::arrContains/IndexOf(E(Array), coerce(E(Value)))` (exact equality) |
| `Print` | `hc::print(hc::coerceString(E(x)))` (`"[Widget] "` + Logger Info) |
| `EngineCall` exec | `{ HC_STEP(rs); auto r = hc::callApi(m_ctx, "id", { toValue(coerce(E(p0))), … }); rs.nID_oK = hc::fromValue<T>(r, K); }` |
| `EngineCall` pure (expr, per read) | `hc::fromValue<T>(hc::callApi(m_ctx, "id", { …args… }), K)` — one dispatch **per read**, matching `:1232` |

Operator inputs read the `.f`/`.b`/`.s` fields raw in the interpreter; wiring
guarantees type equality, so typed C++ expressions are exact.

### 5.6 `hc_registry.cpp`

```cpp
static const HorizonCode::CompiledClassEntry kClasses[] = {
    { "Content/UI/MainMenu.hasset",  +[]{ return (CompiledInstance*)new hcgen::C_MainMenu(); },
                                     +[](CompiledInstance* p){ delete p; } },
    { "__game_instance__",           … },
    { "level:Content/Scenes/World1.hescene", … },
};
extern "C" HE_GAME_API const HorizonCode::CompiledClassEntry*
HE_HorizonCodeGenClasses(int* count, const char** engineVersion)
{ *count = (int)std::size(kClasses); *engineVersion = "<baked Options.engineVersion>"; return kClasses; }
```

---

## 6. Type mapping & the support header

New header **`src/HE_Core/include/HorizonCode/HorizonCodeGenSupport.h`**
(namespace `hc`, header-mostly; the few functions with engine deps — logging —
exported from HE_Core). It ships in the codegen SDK (§8.3) and is included by
all generated code and by the parity tests.

| PinType | C++ type | `Value` field |
|---|---|---|
| Float | `float` | `.f` |
| Bool | `bool` | `.b` |
| Int | `int` | `.i` |
| String | `std::string` | `.s` |
| Vec2 | `glm::vec2` | `.v2` |
| Color | `glm::vec4` | `.col` |
| Ref | `uint32_t` | `.ref` |
| Transform | `hc::Transform { glm::vec3 pos, rot{0}, scl{1}; }` | `.tpos/.trot/.tscl` |
| array of T | `hc::Array<T>` = `std::vector<T>` | `.items` (scalar Values) |

Contents of `hc`:
- `toValue(T) → Value` / `fromValue<T>(const std::vector<Value>&, size_t)`
  (missing index → `T{}`, matching `Value{}` field reads) for every row incl.
  arrays (element-wise convert).
- `coerceFloat/Int/Bool/String/… (const Value&)` — byte-for-byte the
  interpreter's `coerce` (`HorizonCode.cpp:847`), plus typed↔typed variants
  that collapse to identity/known conversions at compile time.
- `arg(args, i)` (bounds-tolerant `Value` read), `div`, `feq`, `land/lor`,
  `toStringG`, `arrGet/arrAdd/arrSet/arrInsert/arrRemove/arrContains/arrIndexOf`
  (semantics + warning logs per §3.4), `print`, `logCreateObjectFail`,
  `callApi(Context&, const char* id, std::vector<Value>)` (null-tolerant),
  `refOf`.
- `HC_STEP(rs)` (`if (rs.aborted || ++rs.steps > 4096) { rs.aborted = true; return; }`
  as a statement macro; expression contexts use `hc::step(rs)`), the depth
  helpers.
- **Trace hooks** (§10): `hc::TraceSink` (a `std::function<void(const char* kind,
  const std::string& id, const std::vector<Value>&)>` process-global, unset in
  shipping). With `Options.traceHooks`, every `callApi`, `getProperty`,
  `setProperty`, `createWidget`, … call site is wrapped by `HC_TRACE(...)`;
  without the option, the macro expands to nothing. Zero shipping overhead.

Determinism note: nothing in `hc` may consult wall clock/locale; `%g`
formatting is locale-independent for the C locale — force `snprintf` semantics
identical to the interpreter (same function, same buffer size 48).

---

## 7. Engine-API integration & expandability

### 7.1 Emission strategy: invoke-first, direct-call later

**v1 (WP2): generated code dispatches EngineCall through the same seam the
interpreter uses** — `m_ctx.callApi(id, values)`, which the host binds to
`HE::api::find(id)->invoke(ctx, values)` (exactly today's `svc.callApi`,
`GameApplication.cpp:197`).

Why this is right for v1:
- **Parity is structural.** Marshalling, arg-count tolerance, null-Ctx
  forgiveness, vec3-in-Color packing are the *same code* on both sides;
  the trace comparison can't drift.
- **No new metadata.** `ApiFn` today does not record C++ parameter types — the
  `invoke` thunk hand-marshals (`aV3` vs `aV4`: a Color pin is `glm::vec3` for
  `transform.setPosition` but `glm::vec4` for `material.setParam`). Direct
  emission needs that knowledge; invoke does not.
- **The interpreter overhead is not here.** What codegen removes is the graph
  walk: link scans per input, `evalData` recursion, `Value` churn on every
  math/flow node, per-node switch dispatch, string-keyed variable maps. The
  boundary cost of one `vector<Value>` per *engine* call is minor against the
  engine work behind it.

**v2 (WP7, optional per function): direct typed calls.** Extend the descriptor:

```cpp
enum class ApiMarshal : uint8_t { Auto /*= PinType default*/, Vec3InColor, EntityU32, HandleInt };
struct ApiParam { const char* name; PinType type; bool isArray = false;
                  ApiMarshal marshal = ApiMarshal::Auto; };
```

With marshal tags a row's `cppCall` becomes directly emittable:
`HE::api::transform::setPosition(ctx, (HE::api::Entity)a0, hc::v3(a1))`. The
generated dylib already links HorizonScene (§8.2), so the symbols are there.
Migration is per-row and parity-gated: a row without complete tags keeps
invoking. `Ctx` for direct calls: the host passes its `Ctx` provider alongside
`callApi` (a `Context::apiCtx` accessor added in WP7) — not needed before then.

### 7.2 What "adding a new engine API function" costs (the expandability contract)

Exactly what it costs today, **codegen included**:

1. Implement `HE::api::<group>::<fn>` in `EngineApi.cpp` (+ header decl).
2. Add **one `ApiFn` row** (id, category, isExec, params, results, cppCall,
   invoke thunk, display name).

Nothing else. The editor add-menu, the interpreter, Lua/Python dispatchers,
**and the codegen** all consume the row: codegen reads the *node's* mirrored
`params`/`results` (re-validated against the registry, §5.2.1) and emits the
generic invoke path keyed by `id`. New group with a new `Ctx` member → also
extend `Ctx` + the host bindings, as today. Optional 3rd step for max
performance later: marshal tags (§7.1 v2).

**Adding a new `PinType`** (e.g. a real Vec3) is the bigger, rarer event; the
checklist: `Value` payload + `coerce` + `valueEquals` + JSON scalar (de)ser in
HE_Core; §6 type row (`hc::toValue/fromValue/coerce`); the emitter's literal
and type-name tables; editors. The codegen structure (IR, RunState, lowering)
is PinType-generic and needs only those table rows.

### 7.3 Sandbox / capability flags

When `ApiFn` gains `capability`/`reproducible` flags (completion plan §5),
codegen enforcement is: gated-off function → emit the same warn-log no-op the
interpreter's dispatch produces. Because both sides route through `invoke` in
v1, the enforcement point is the registry itself — write it once, both
execution modes inherit it. (No work in this plan; noted so nobody adds a
second enforcement point.)

---

## 8. Packaging & toolchain integration

### 8.1 Where codegen hooks into the export

Inside the existing async export worker (`EditorUI.cpp` ~2299), when the
profile's `compileHorizonCode` is set, **before** `ProjectExporter::exportProject`
(so the result message can carry the summary):

1. **Collect `ClassSource`s** (editor side, has ContentManager + scenes):
   - every HC **class asset** + **widget asset** (walk content root as
     `HcEditorUtil::listAssets` does; graph = the asset's `graphJson`);
     key = canonical content-relative path (forward slashes — the exact string
     `CreateObject`/`CreateWidget` nodes store).
   - every **scene's level script** (the export already loads/serializes each
     scene for `extraScenes`; extract the embedded level graph in the same
     pass); key = `"level:" + projectRelativeScenePath`.
   - the **GameInstance** graph (already in hand as `gameInstanceJson`);
     key = `"__game_instance__"`.
2. `HE::hccg::generate(sources, {traceHooks:false, engineVersion:…})`.
3. Write files to `<outputDir>/_hcgen/` + `generateCMakeLists(...)`.
4. **Drive the toolchain** (new `HE::hccg::buildDylib(...)` helper): run
   `cmake -S _hcgen -B _hcgen/build -DCMAKE_BUILD_TYPE=Release -DHE_SDK_DIR=<sdk>`
   then `cmake --build _hcgen/build --config Release`, capturing stdout/stderr
   to `_hcgen/build.log`. (Process spawn: `std::system` with quoted paths is
   fine for v1 — the worker is already a background thread; no UI stall.)
5. On success: copy the artifact as `HorizonCodeGen.<dylib|dll|so>` into the
   staging set. `ProjectExporter` places it with the same rule as
   `GameLogic.*` (`ProjectExporter.cpp:604`: base-path load location; in the
   .app bundle → `Contents/Resources`) — extend that filename filter to
   `HorizonCodeGen.*`.
6. `project.hcfg` gains `"horizonCode": "compiled"` (else `"interpreted"`), so
   a missing/undloadable dylib at runtime is a *loud* misconfiguration log,
   not a mystery.
7. **Any failure in 2–5** (validation is per-class and never fails; this means
   toolchain/config errors): log, append to the export result message
   ("HorizonCode: compile failed — shipped interpreted (see _hcgen/build.log)"),
   and continue the export. Graphs are packed regardless (next point), so the
   game is always complete.

**Graphs always ship.** The pak keeps packing `graphJson`/`__game_instance__`
exactly as today, even when compiled. That is the hybrid fallback (design doc
§7): at runtime, registry hit → compiled; miss → interpreted. It also keeps
mod overlays and older runtimes working. (Optional later: strip graphs of
fully-compiled classes for size — only after the fallback reporting has
soaked.)

### 8.2 The generated project (CMakeLists template)

```cmake
cmake_minimum_required(VERSION 3.20)
project(HorizonCodeGen CXX)
set(CMAKE_CXX_STANDARD 20)
add_library(HorizonCodeGen SHARED <generated .cpp files…>)
target_include_directories(HorizonCodeGen PRIVATE "${HE_SDK_DIR}/include")
target_link_directories(HorizonCodeGen PRIVATE "${HE_SDK_DIR}/lib")
target_link_libraries(HorizonCodeGen PRIVATE HorizonCore HorizonScene)
# macOS: @rpath like GameLogic (loaded from the game's base path next to the engine dylibs)
set_target_properties(HorizonCodeGen PROPERTIES INSTALL_RPATH "@loader_path" BUILD_WITH_INSTALL_RPATH ON)
```

Link facts (verified): `HorizonCore` and `HorizonScene` are `SHARED`;
HE_Core exports via `HE_API`, HE_Scene exports everything on Windows
(EXPORT_ALL — per the Windows portability rules, HE_Scene never uses
`HE_API`), so `Value`, `Context`, `CompiledInstance`, `HE::api::find` all
resolve. This is the identical linking situation as the existing
`test_gamelogic` fixture (`tests/CMakeLists.txt:103`).

### 8.3 The codegen SDK

The generated project needs headers + import/link libraries on the *export
host*. `HE_SDK_DIR` resolution order:

1. Explicit override (`HE_HCGEN_SDK` env var / editor setting) — CI, unusual
   layouts.
2. `<editorBaseDir>/SDK/` — the deployed-editor case. The engine's deploy step
   gains a target that stages: `include/` = the public headers of HE_Core +
   HE_Scene (they are already self-contained: `Types/…`, `HorizonCode/…`,
   `HorizonScene/EngineApi.h`, …) **+ glm** (header-only) **+ nlohmann_json
   headers are NOT needed** (generated code never touches JSON); `lib/` =
   the editor's own `HorizonCore`/`HorizonScene` dylibs (macOS/Linux link
   directly against dylibs; Windows: the corresponding `.lib` import
   libraries, added to the deploy).
3. Dev fallback: the source tree + build dir (a `he_sdk_config.json` written
   by CMake configure into the editor's runtime dir with absolute paths) — so
   exports from a development build work without staging an SDK.

ABI rule (documented in the export dialog tooltip + log): the SDK's libraries,
the runtime bundle, and the toolchain must come from the same engine build/
compiler family. The manifest's `engineVersion` string (baked at generation,
checked at load, §9.2) turns a mismatch into a clean fallback instead of UB.

Toolchain presence: `cmake --version` probe before step 4; absent → the same
loud interpreted-fallback path.

### 8.4 Target platforms

v1 compiles for **Host only** (the platform the editor runs on — same arch as
the `../Game` runtime bundle). Cross-platform profiles (`Windows`/`macOS`/
`Linux` prebuilt bundles) select the interpreter with an explicit notice
("HorizonCode compile: target ≠ host — shipped interpreted"). Cross-compiling
generated C++ is deliberately out of scope until there is a real need; the
design keeps it possible (the generated sources are platform-neutral; only
§8.2/8.3 need per-target SDKs).

### 8.5 Export UI / reporting

The export dialog's existing HorizonCode toggle becomes functional. The result
line (already assembled in the worker) appends:
`"HorizonCode: 14 classes compiled, 2 interpreted (validation)"`, and the log
lists each fallback `key: reason` (design-doc rule: no silent caps).
Incremental packing note: `_hcgen/` is regenerated every export in v1 (codegen is
fast; the C++ build is the cost — CMake's own incrementality already skips
unchanged generated files since the emitter writes files only when contents
differ).

---

## 9. Runtime loading in the shipped game

### 9.1 Keys (the factory lookup contract)

| Host | Key | Lookup site |
|---|---|---|
| HC class asset | content-relative path (e.g. `Content/Logic/Enemy.hasset`) | `svc.createObject` (`GameApplication.cpp:178`) |
| Widget asset | content-relative path | `WidgetManager::createWidget` |
| Level script | **[impl]** `levelScriptKeyForUuid(sceneUuidForPath(path))` = `"level:<32-hex-uuid>"` — UUID-derived instead of the plan's path form, because the BOOTING game only has the startup scene's UUID (project.hcfg), not its project-relative path; `scene.load(path)` derives the same key via `sceneUuidForPath`. Both ends share the one helper in `ProjectExporter.h` | `HorizonWorld::fireLevelLoaded` (key set by `GameApplication` at scene load) |
| GameInstance | `"__game_instance__"` | `GameApplication` GameInstance setup (`:141`) |

Path normalization: forward slashes, exactly the string stored in nodes /
passed to `loadAsset` — one `hc::canonicalKey()` helper used by both the
exporter (key emission) and the runtime (lookup), so they cannot drift.

### 9.2 Loader

New small `HcCompiledLoader` (HE_Core, beside `GameLogicLoader`, reusing
`DynLib`): open `HorizonCodeGen.*` from the base path (same probe locations as
`GameLogic.*`), resolve `HE_HorizonCodeGenClasses`, **check `engineVersion`**
against the engine's own version string (mismatch → error log + don't
register), and hand the entry table to a `CompiledClassTable` (a plain
map<string, entry> owned by the app). The library stays loaded for process
lifetime (packaged builds never hot-reload it).

`GameApplication` changes (all four hosts):
- After pak mount, before GameInstance setup: load the table; log
  `"HorizonCode: N compiled classes"` or, if `project.hcfg` says compiled but
  the dylib is missing/rejected, a loud warning + full interpreter fallback.
- **GameInstance**: table hit for `__game_instance__` →
  `runtime().setGameInstanceCompiled(entry.create(), {})`; miss → today's
  `setGraph(json)`.
- **`svc.createObject`**: table hit for the class path → `addCompiled` + fire
  `"Construct"`; miss → today's interpreter path. (This one lambda is the
  whole per-asset hybrid mechanism.)
- **`WidgetManager::createWidget`**: after loading the widget asset (layout is
  still needed!), table hit for the asset path → `addCompiled(..,
  HostBindings{widget property fns})` instead of `add(graph, …)`; miss →
  interpreter. `isInteractive` already switched to `eventBindingsOf` (§4.2).
- **Level script**: at scene load where the level graph joins the runtime →
  table hit for `"level:"+path` → `addCompiled`; miss → interpreter.

The editor/PIE never consults the table (it doesn't exist there) — zero editor
behavior change.

---

## 10. Parity verification

### 10.1 Shape

`tests/test_horizoncode_codegen.cpp` + checked-in fixture graphs
`tests/fixtures/hcodegen/*.hcode.json`. Generation happens **at build time**
(no compiler needed at test runtime), following the `test_gamelogic` pattern:

```cmake
# tests/CMakeLists.txt
add_executable(hc_codegen ...)            # §10.2 — or reuse the tool target
add_custom_command(OUTPUT ${gen_cpps}
    COMMAND hc_codegen --trace --out ${CMAKE_CURRENT_BINARY_DIR}/hcgen_fixtures
            ${fixture_files}
    DEPENDS hc_codegen ${fixture_files})
target_sources(he_tests PRIVATE ${gen_cpps})   # compiled INTO the test binary
```

Each test case, per fixture:
1. Build a `Runtime` + **mock host**: `Services.callApi` wraps
   `HE::api::find(id)->invoke` with a **null-world `Ctx`** (the API's null
   tolerance makes every call well-defined) and records `("api", id, args)`
   into trace A; `HostBindings.get/setProperty` backed by a map, recording
   too; create/destroy services backed by counters returning deterministic
   ids.
2. Instantiate **interpreted** (`Runtime::add(fromJson(fixture))`) and
   **compiled** (the build-time-generated class, `addCompiled`) instances in
   two separate Runtimes with identical mocks; the compiled build has
   `traceHooks` on → `hc::TraceSink` records trace B in the same format
   (coerced args, same Value printer: `%g` floats, quoted strings, `[..]`
   arrays).
3. Drive the same script: `random::seed(k)`, `time::reset/advance`, a fixed
   sequence of `fireEvent`/`callFunction` calls with args.
4. Assert: **trace A == trace B** (ordered), `variablesSnapshot` equal after
   every step, `callFunction` results equal, property-map end states equal.

### 10.2 `hc_codegen` CLI

Tiny tool target (`tools/hc_codegen/main.cpp`, links HorizonScene + Core):
`hc_codegen [--trace] --out DIR file...` → writes generated files; also useful
for eyeballing emission during development and for CI artifacts. It is *not*
shipped with the editor (the export worker calls the library in-process).

### 10.3 Fixture list (initial, one clause of §3 each)

1. `flow_branch_sequence` — nesting, both arms, sequence order.
2. `coerce_matrix` — pin defaults of every convertible/inconvertible pairing.
3. `math_ops` — all operators incl. divide-by-zero, Equals epsilon, ToString.
4. `variables` — defaults (all types + arrays), set/get, pass-through out,
   undeclared-set-then-get, public/private metadata.
5. `functions_basic` — params/results, multiple returns-in-branches, missing
   args, private + `requirePublic`.
6. `functions_recursive` — bounded recursion **including the stale-exec-cache
   read** (§5.4: read a cached call output after an inner recursive call — the
   case RunState exists for).
7. `foreach_arrays` — all array ops, clamps, out-of-range, element/index reads
   after Done, nested ForEach.
8. `events_multi` — two Event nodes same name (order + shared cache), elem
   filtering, event arg coercion, Tick with float arg.
9. `engine_pure_multiout` — pure EngineCall with several outputs each read →
   dispatch-per-read counts in the trace (e.g. `physics.raycast` null-world).
10. `engine_exec_cached` — exec EngineCall (e.g. `random.value` seeded), cache
    reads, arg coercion; `save.*` round-trip (process-global store).
11. `refs_objects` — CreateObject/DestroyObject (mock), Get/Set/CallExternal
    incl. null-target and private-member warn paths, GetSelf/GameInstance.
12. `dispatchers` — Bind/Emit chains compiled↔interpreted **mixed in one
    Runtime** (a compiled listener on an interpreted owner and vice versa),
    listener order, re-bind during dispatch, depth guard smoke.
13. `widget_props` — property get/set + show/hide via HostBindings, pin
    defaults on property values.
14. `limits_smoke` — a graph designed to trip the step guard: assert both
    sides terminate cleanly (no trace equality, §3.6).
15. `functions_locals` — function-local variables (§13.4): per-invocation
    reset, recursion (fresh local per frame beside a per-run exec cache),
    defaults incl. arrays, external-invisibility warn paths.

Golden rule inherited from the material codegen: **every emitter change runs
the whole suite**; a lowering change that shifts a trace is a bug or a
consciously updated fixture.

### 10.4 End-to-end packaged test

One integration test (macOS, gated like the `.app` bundle test): export the
sample project with `compileHorizonCode`, assert `HorizonCodeGen.dylib` exists,
load it via `HcCompiledLoader`, instantiate `__game_instance__`, fire `OnInit`
against mocks. Plus a manual HW smoke: packaged game logs
`"HorizonCode: N compiled classes"` and the UI behaves identically.

---

## 11. Work packages & order

Strictly ordered; each has a hard gate. (Estimates are relative sizes.)
**Status: WP0–WP5 DONE** (all gates green: 15 fixtures trace-parity incl.
mixed-mode + the real-dylib loader test; the generated project builds against
the dev SDK end-to-end). Implementation notes:
- **[impl]** Stage B's explicit IR was folded into the emitter (validate →
  direct exec-walk emission); the stages A/C behave exactly as specced.
- **[impl]** `Options.traceHooks` is accepted but not emitted — the parity
  harness records at the shared Context/host seam instead, which captures the
  same observables (order, count, coerced args, results) on both backends.
- **[impl]** The dispatch semantics of §3.5 hide an exponential landmine the
  fixtures exposed: each fireEvent can spawn TWO dispatch subtrees (a
  handler's EmitEvent + fireEvent's own trailing listener dispatch). A
  bind CYCLE of relaying listeners therefore branches into ~2^32 fires — the
  depth guard bounds depth, not total work. Backend-agnostic (Runtime-side),
  pre-existing; fixture 12's cycle case uses non-relaying sinks. A total
  per-fire dispatch budget is a candidate WP6/engine fix.

**WP0 — Compiled object model (M).** `HorizonCodeCompiled.h`, Runtime compiled
backend (§4.2: addCompiled, reflection paths, overflow store, GC via
collectRefs, `eventBindingsOf` + WidgetManager switch,
`setGameInstanceCompiled`, `variablesSnapshot`). Hand-written mock
CompiledInstance tests (§4 gate). *No codegen yet.*

**WP1 — Codegen core + harness (L).** `HcCodegen.h/.cpp` stages A–C for:
variables (incl. function-locals, §13.4), literals, math/logic/string, arrays,
ForEach, Branch/Sequence, functions (RunState model), events, Print,
property/self nodes, pin defaults.
`HorizonCodeGenSupport.h`. `hc_codegen` CLI + build-time fixture wiring.
Fixtures 1–8, 13, 14 green. **Gate: trace parity on all landed fixtures.**

**WP2 — Engine calls (S).** EngineCall exec + pure lowering via `hc::callApi`,
registry re-validation (§5.2.1), trace hooks around callApi. Fixtures 9, 10.
**Gate: dispatch-count parity (the per-read clause).**

**WP3 — References & dispatchers (M).** Create/DestroyObject/Widget*, external
get/set/call, Bind/Emit, GetSelf/GameInstance lowering. Fixtures 11, 12
(mixed-mode). **Gate: mixed compiled↔interpreted Runtime passes.**

**WP4 — Packaging (M).** `generateCMakeLists` + `buildDylib`, SDK staging +
resolution (§8.3), export-worker integration + reporting (§8.1/8.5),
`ProjectExporter` HorizonCodeGen.* placement, hcfg flag. **Gate: exporting the
sample project on the host produces a loadable dylib; toolchain-missing and
build-error paths produce the documented interpreted fallback.**

**WP5 — Game runtime integration (M).** `HcCompiledLoader` + version check,
`CompiledClassTable`, the four host hookups (§9.2), level-script keying.
§10.4 integration test. **Gate: packaged sample runs with
"N compiled classes"; deleting the dylib runs identically interpreted.**

**WP6 — Hardening & docs (S).** Fallback reasons end-to-end (validate → export
message → runtime log), editor toggle tooltip, update
`horizoncode-reference.md` §4 + the design doc's status header, CI: fixture
generation on Windows runner (portability rules: the generated lib target
mirrors `test_gamelogic`'s linkage; no `HE_API` in HE_Scene, `HE_GAME_API` on
the manifest export).

**WP7 — Optimization (later, each parity-gated).** Direct typed calls via
`ApiMarshal` tags (§7.1 v2) starting with the hot groups (transform, math —
though math is already inlined as operators via… no: registry math fns stay
invoke; tag them first). Per-invocation locals instead of RunState slots where
no recursion/stale-read can observe the difference (escape analysis on the
call graph). CSE for pure subtrees **except** pure EngineCalls (observable).
Cross-platform export targets. Graph stripping for compiled classes.

Dependency note: nothing here blocks on the completion plan's remaining node
roadmap — new registry rows keep flowing in with zero codegen impact (§7.2).

---

## 12. Risks & decided questions

| Risk / question | Decision |
|---|---|
| Recursion vs. exec-output cache scoping | **Solved by design**: RunState-per-run fields, not locals (§5.4). Fixture 6 pins it. |
| Pure EngineCall multi-output re-dispatch | Emit per read (§5.5); trace counts verify. Documented as interpreter-authoritative semantics, not a codegen bug. |
| `And`/`Or` short-circuit divergence | `hc::land/lor` helpers evaluate both sides (§5.5). |
| Exec/data cycles in authored graphs | Validation → per-asset interpreted fallback with reason (§5.2). |
| Step/depth limits | Sharpened, not exact (§3.6); fixtures avoid the edge, one smoke test. |
| Export host toolchain missing / build fails | Loud interpreted fallback; export never fails because of codegen (§8.1.7). |
| ABI mismatch generated-dylib ↔ engine | Same-toolchain requirement + `engineVersion` handshake at load → clean fallback (§8.3/§9.2). |
| Windows linkage of generated code | Same as `test_gamelogic`: links HorizonCore (HE_API) + HorizonScene (EXPORT_ALL); manifest export uses `HE_GAME_API`. Covered in WP6 CI. |
| `WidgetManager` reads the graph for interactivity | Replaced by `eventBindingsOf` fed from `eventInfos()` (§4.2) — WP0. |
| GC over compiled instances | `collectRefs()` enumerates Ref members incl. Ref arrays; overflow store scanned too (§4.2). |
| Ref-typed variables holding class paths (`className`) | Editor metadata only; codegen ignores it (refs stay `uint32_t`, dispatch stays dynamic via Runtime). Direct typed cross-class calls are explicitly NOT in scope (design doc §8.6 keeps them "later"). |
| Non-deterministic APIs in parity runs | Seeded `random`, app-driven `time` (test controls `advance`), null-world Ctx for the rest; wall-clock APIs (future) get the `reproducible=false` flag and are excluded (§7.3). |
| `graphJson` still shipped when compiled | Intentional (hybrid + mods + older runtimes); size optimization deferred (§8.1). |
| Where does codegen live | HE_Scene (`HE::hccg`) — needs Graph + registry; no new deps (§5.1). |
| GameLogic.dll vs. own dylib | Own `HorizonCodeGen.*` with its own C manifest export; GameLogic remains the user's hand-written gameplay slot. Both may coexist. |

---

## 13. Extension: function-local variables

Status: **interpreted side + editor LANDED** (this branch); the codegen
lowering below is part of WP1. Locals are per-*invocation* state — the
counterpart to instance variables' per-*instance* state — and they slot into
the frame model both sides already have: the interpreter's `CallFrame`, and
(compiled) the native C++ stack.

### 13.1 Model (landed — `HorizonCode.h`)

`Variable` gained `int scope = 0`: `0` = instance variable (exactly today's
behavior), non-zero = **function-local**, owned by the `FunctionEntry` node
with that id (the same id `Node::subgraph` uses). Rules:

- Variable **names stay unique across the whole graph** — no shadowing. The
  editor already enforced global uniqueness (create/rename); resolution
  everywhere stays a plain `findVariable(name)`, and the variable's `scope`
  then picks the store. This deliberately avoids scope-dependent name lookup
  in the interpreter, the codegen name mangler, and every editor site that
  resolves a variable by name (rename, drag-drop, ForEach class adoption).
- Locals have **no access modifier**: never visible to `Get/SetExternal`
  (Runtime checks `scope != 0` alongside `access != 0`), never part of the
  public class interface (`classInfoFromGraph` filters them), never in
  `varInfos()`/`collectRefs()` of a compiled class (§13.4).
- JSON: `"scope"` serialized only when non-zero → old assets load unchanged.
- `Graph::removeNode` of a `FunctionEntry` erases its locals with it.

### 13.2 Interpreter (landed — `HorizonCode.cpp` / `HorizonCodeRuntime.cpp`)

- `Runner::CallFrame` gained `fnEntryId` + a `locals` map, **seeded from the
  declared defaults on every frame push** (both `callFunction` and the
  `FunctionCall` exec node) — per invocation, like `Runtime::add` seeds the
  instance store from the scope-0 variables (which now skips locals, as does
  `reseedVariables`).
- `Get/SetVariable` resolution: `findVariable(n.s)`; `scope != 0` → the
  **innermost frame whose `fnEntryId` matches** (`Runner::frameFor`) —
  recursion-correct by construction. No matching frame (the node executed
  outside its function, only reachable via cross-subgraph wiring) → reads
  yield the type's default, writes are dropped. `scope == 0` / undeclared
  names → the instance store, unchanged (undeclared Set still creates an
  entry there).
- Semantic-contract addendum (§3): locals reset on every invocation; a
  recursive call sees fresh locals per frame while the exec-output cache
  stays per-run — the discriminating test exists
  (`test_horizoncode_runtime.cpp` "Recursion keeps a fresh local per call
  frame", plus seeding/reset/external-invisibility/round-trip cases).

### 13.3 Editor (landed — `LevelScriptPanel.cpp` / `UIEditorPanel.cpp`)

- The sidebar shows a **"Local Variables"** section while a function
  sub-graph is open (`+ Add` creates the variable with
  `scope = currentGraph`); rows drag onto the canvas like any variable.
- Locals are offered **only inside their owning function**: the add-menu and
  pin-drag menus filter by the open sub-graph, the Get/Set node's variable
  combo filters by the node's `subgraph`, and the drag-drop popup disables
  Get/Set with a "Local to another function." hint elsewhere.
- The variable details swap the Access combo for a "Local to: `<fn>`" line.
- Drive-by fix in both drop popups: the created node now mirrors the
  variable's `isArray` (array variables previously dropped as scalar pins).

### 13.4 Codegen lowering (WP1)

Locals become **plain C++ locals at the top of the generated member
function**, initialized to their declared defaults — NOT `RunState` fields.
That is the whole point of the frame mapping in §5.4: `CallFrame::locals` is
per-invocation ⇒ native stack locals reproduce it exactly, recursion
included, with zero bookkeeping. (Contrast: the exec-output cache is per-run
⇒ `RunState`. The two coexist; fixture `functions_locals` pins the split.)

```cpp
void C_Foo::f_AddScore(RunState& rs, float p_Amount, float& r_Total)
{
    float l_Bonus = 10.0f;               // local, declared default
    hc::Array<int> l_Hits{};             // local array (default items seeded likewise)
    // Get → l_Bonus; Set → l_Bonus = hc::coerceFloat(...); pass-through as usual
}
```

- IR: `FunctionIR` gains `locals[] {name, type, isArray, default}`;
  `ClassIR.vars` keeps only scope-0 variables. Name mangling gets an `l_`
  prefix (members `v_`, params `p_`, results `r_`).
- Reflection: locals appear in none of `varInfos()` / `getVariable()` /
  `setVariable()` / `collectRefs()` / `reseedVariables()` — matching their
  interpreter invisibility.
- **Validation rule (Stage A)**: a data link that reads a local's `Get` node
  from a statement outside the owning function's body (cross-subgraph wire)
  → per-asset interpreter fallback (`"local 'x' read outside its function"`).
  The interpreter resolves such reads via the frame search; compiled C++ has
  no symbol in scope — rather than emulating a case the editor cannot
  produce, exclude it. Same-function reads (the only authorable case) are
  exact.
- Parity fixture `functions_locals` (add to §10.3): defaults of every type +
  arrays, per-invocation reset across two calls, recursion with a local
  beside an exec-cached node (per-frame vs. per-run), read-outside-function
  via a hand-built graph asserting both sides yield the default, and
  external Get/Set warn-paths on a local name.

Possible later step on the same mechanics: **event-graph locals**
(`scope` = a sentinel for sub-graph 0) — those would be per-*run* state:
a run-scoped map in the Runner, `RunState` fields in the compiled class.
Same building blocks, different scope; not needed now.

## Related
- `horizoncode-cpp-codegen-plan.md` — the design this implements (§ mapping:
  its §2 → done pre-work; §3 → our §3; §4 → our §5; §5 → §5.5; §6 → §10;
  §7 → §8/§9; §8 phasing → §11).
- `horizoncode-completion-plan.md` — registry roadmap; every addition rides §7.2.
- `horizoncode-reference.md` — node/API reference (update in WP6).
- Material node-graph → shader codegen — the "generic in editor, native at
  pack, parity-tested" precedent.
