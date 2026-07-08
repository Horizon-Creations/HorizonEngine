# HorizonCode → C++ Codegen at Packaging: Design Plan

Status: **design only** (no code yet). How a packaging-time parser/codegen turns
HorizonCode graphs into native C++ for shipping builds, while the editor keeps
interpreting the same graphs. The HorizonCode analogue of the Material
node-graph → shader cross-compile: *generic in the editor, native at pack time,
identical behavior in both.*

Two pillars, in order:
1. **An engine-wide C++ API** (`HE::api`) that exposes every subsystem as plain,
   codegen-friendly C++ — the single surface the generated code calls. (§2)
2. **A parser/codegen** that lowers HC nodes to C++ that calls that API. (§4)

The node roadmap that fills out the API coverage lives in a companion doc:
`horizoncode-completion-plan.md`. This doc assumes those nodes exist and focuses
on translating them.

## 1. Goal & constraints

- **Dual execution.** Editor + PIE interpret (`HorizonCode::Runner` via
  `HorizonCode::Runtime`) — instant iteration. Shipping runs **compiled C++**
  (AOT). No interpreter in shipping except the hybrid fallback (§7).
- **Behavioral parity is the contract.** For every graph the compiled output
  must produce identical observable behavior to the interpreter (same event
  order, values, engine calls). Verified with golden tests (§6), like the
  material codegen's per-backend contract.
- **One API, four frontends.** Lua, Python, the HC interpreter, and the compiled
  HC all call the *same* `HE::api`. No behavior can exist in one frontend that
  the others can't reach — that is what makes parity mechanical.

## 2. The engine-wide C++ API (`HE::api`) — the keystone

### 2.1 Why it comes first

Today engine functionality reaches scripts through **`ScriptApi`** (HE_Scene) —
~40 free functions (transforms, `spawn`/`destroy`, physics raycast/velocity,
material params, entity-UI, widgets, cursor, log), bound *twice* (Lua in
`ScriptContext`, Python in `PyScriptBackend`). HorizonCode reaches a *different,
smaller* surface through `HorizonCode::Context` + `Runtime::Services`
(widgets/objects/property get-set). These are three hand-maintained bindings of
overlapping functionality. Codegen would be a fourth.

Instead: promote `ScriptApi` into a formal, complete **`HE::api`** and make every
frontend a thin adapter over it. Then the compiled C++ is not a new API — it is
the most *direct* consumer of the one that already exists.

### 2.2 Shape

`HE::api` is a set of per-subsystem namespaces of **plain free functions** that
take an explicit context and use only codegen-friendly types (`float`, `int`,
`bool`, `std::string`, `glm::vec2/3/4`, and opaque handles like `EntityId`,
`WidgetId`, `hc::Ref`). No hidden globals; deterministic.

```cpp
namespace HE::api {
  struct Ctx { HorizonWorld* world; PhysicsWorld* physics; ContentManager* content;
               AudioEngine* audio; InputState* input; /* … */ };

  namespace transform { glm::vec3 getPosition(const Ctx&, EntityId); 
                        void setPosition(const Ctx&, EntityId, glm::vec3); /* … */ }
  namespace physics   { RaycastHit raycast(const Ctx&, glm::vec3 o, glm::vec3 d, float max); /* … */ }
  namespace spawn     { EntityId spawn(const Ctx&, EntityId parent, std::string name);
                        void destroy(const Ctx&, EntityId); }
  namespace audio     { void playSound(const Ctx&, std::string asset, float vol); /* … */ }
  namespace input     { bool keyDown(const Ctx&, KeyCode); glm::vec2 mousePos(const Ctx&); /* … */ }
  namespace ui        { void setText(const Ctx&, EntityId, std::string); /* … */ }
  namespace fs        { std::string readText(const Ctx&, std::string relPath); /* … (§ completion) */ }
  namespace time      { float delta(const Ctx&); double now(const Ctx&); }
  // … one namespace per subsystem; see the completion plan for the full list.
}
```

`ScriptApi` becomes a **compatibility shim** over `HE::api` (or is renamed to it);
Lua/Python binding tables generate their thunks from the same descriptor set.

### 2.3 The function registry (drives nodes + dispatch + codegen)

A HorizonCode node cannot be hand-written per API function (hundreds of them).
Instead, one machine-readable **descriptor table** describes every `HE::api`
function:

```cpp
struct ApiFn {
  const char* id;          // stable key, e.g. "transform.setPosition"
  const char* category;    // add-menu grouping ("Transform", "Physics", "Audio"…)
  bool        isExec;      // exec node (side effect) vs pure data node
  std::vector<ApiParam> params;   // typed inputs  {name, PinType}
  std::vector<ApiParam> results;  // typed outputs {name, PinType}
  const char* cppCall;     // codegen template, e.g. "HE::api::transform::setPosition"
};
const std::vector<ApiFn>& apiRegistry();
```

This one table is consumed by **all four** paths:
- **Editor** — the add-menu lists entries by `category`; `signatureOf` for the
  generic *Engine Call* node reads `params`/`results` for its pins.
- **Interpreter** — the `Context` binds each `id` to a `std::function` thunk
  that calls the real `HE::api` function (coercing pin values ↔ C++ args).
- **Codegen** — emits `cppCall(ctx, args…)` directly, reading results back.
- **Lua/Python** — generate their binding thunks from the same table.

Adding subsystem coverage = adding rows to this table + one thunk each. New
NodeTypes are *not* minted per function (that is the completion plan's core
mechanism). The **Engine Call** node is the single new NodeType this requires:
`s = ApiFn.id`, pins from the registry, exactly mirroring how `FunctionCall`
already mirrors a function's typed interface.

## 3. What we translate (the HorizonCode model)

Source of truth: `src/HE_Core/include/HorizonCode/HorizonCode.h`.
- `Graph` = `{ nodes, links, variables }`; one graph == one **class** (widget,
  HC class asset, level script, GameInstance).
- Execution (`Runner`): push-based exec flow (`Event`/`FunctionEntry` start;
  `Branch`/`Sequence` steer), pull-based data (`evalData` recurses on demand;
  pure nodes re-evaluate, side-effecting exec nodes cache in `m_execOutputs`),
  a call-frame stack for function args/returns, and `hc::Ref` (`InstanceId`)
  references for cross-class calls/dispatchers. Host effects go through
  `Context` — which, post-unification, is just the `HE::api` registry thunks.

## 4. The parser / codegen

A conventional compiler in four stages; each maps onto pieces we already have.

### 4.1 Front end — load & validate
Read the HC asset(s) via `HorizonCode::fromJson` (already exists) + the
**class registry** (`HcEditorUtil::listClasses`, promoted to a headless HE_Core
structure so the packer builds it without the editor). Validate: pins typecheck
(they already do at edit time via `Graph::connect`), function/event names
resolve, `Engine Call` ids exist in `apiRegistry()`.

### 4.2 IR — a small typed graph IR
Lower each class to an IR: typed members (variables), functions with signatures,
event handlers, and per-handler an **ordered exec list** with, at each exec node,
its transitive **pure-data expression tree**. This is where the pull-model is
made explicit (§4.4). Reuse `signatureOf`/`pinRanges` for pin semantics.

### 4.3 Back end — emit C++
Each class → a `class X : hc::CompiledInstance` (see §5). Statements from the
exec list; expressions from the data trees; engine calls from the registry’s
`cppCall`. Name-mangle members/locals; manage includes (the `HE::api` headers +
`glm`). Emit one `hc_registry.cpp` registering every class factory by asset UUID.

### 4.4 The pull-model + exec-cache lowering (the one subtle part)
- **Pure data nodes** (`Const*`, math/logic/string, `GetVariable`,
  `GetProperty`, `GetSelf`, engine *pure* calls) → inline expressions.
  Re-emitting at each read reproduces the interpreter's re-evaluation → parity.
- **Exec-cached nodes** (`CreateWidget`/`CreateObject`/`FunctionCall`/engine
  *exec* calls with results) → the side effect emitted once at its exec position
  into a uniquely-named local; downstream reads reference that local. Maps 1:1
  onto `m_execOutputs`.
- Concretely a small SSA-ish pass: per exec node, topo-order its pure deps,
  allocate temporaries for cached nodes, emit expressions, then the statement.
- `coerce` (the interpreter's implicit Float↔Int↔Bool conversions) is emitted
  verbatim — not C++'s implicit conversions.

## 5. Semantic mapping (summary)

| HorizonCode | C++ lowering |
|---|---|
| Graph (class) | `class X : hc::CompiledInstance` |
| Variable | typed member; default in ctor |
| FunctionEntry + params/results | member function w/ signature |
| FunctionCall / FunctionReturn | method call (args in, results to locals) / `return`/out-params |
| Event | `onEvent` switch case (names interned to `EventId`) |
| Branch / Sequence | `if/else` / ordered statements |
| Get/SetVariable (+pass-through) | member read / assignment expression |
| Get/SetProperty | host property get/set |
| Get/SetExternal, CallExternal | ref → `getPublicVar/setPublicVar/callPublic` (access-checked) |
| Create/DestroyObject, Create/…Widget | service call → `hc::Ref`/`WidgetId` local |
| BindEvent/EmitEvent | compiled dispatcher registry (same ordering + depth-32 guard) |
| GetSelf/GetGameInstance | `this` / `gameInstance()` |
| Const* | literal |
| Math/Logic/String | operators / std calls (matching `coerce`) |
| **Engine Call** | `HE::api::<subsystem>::<fn>(ctx, args…)` from the registry |

## 6. Parity verification

`test_horizoncode_codegen`: for each fixture graph, run it through (a) the
interpreter capturing a trace (ordered engine/host calls + final variable
values), (b) the generated+compiled class capturing the same trace, and assert
equal. Fuzz small random graphs (bounded node budget). This gates every codegen
change, mirroring the material "interpreter == compiled" contract.

## 7. Build-pipeline integration

Reuse the existing packaging machinery — don't invent a new one.
- **When:** at export, inside the async export worker (Export Profiles /
  `hpak_packer`), after asset discovery (which already walks the reference graph
  + bakes UUIDs).
- **Emit:** for every HC asset (widget + HC-class graphs) + per-scene level
  scripts + the GameInstance → `.cpp/.h` into a build scratch dir, plus
  `hc_registry.cpp` (factories keyed by UUID).
- **Compile:** feed the generated sources into the **GameLogic dylib** path that
  already exists (`GameLogicLoader` C1/C2, dlopen hot-copy) — compiled
  HorizonCode ships as native game logic beside C++ gameplay, loaded by the
  packaged `HorizonGame`. The export host drives the toolchain (same place the
  runtime bundle is built); no compiler embedded in the runtime.
- **Selection:** an export-profile flag `HorizonCode = Interpreted | Compiled`
  (default Compiled for Shipping, Interpreted for Development) — parallels the
  seeded Development/Shipping profiles.
- **Hybrid fallback:** if any node/feature isn't yet codegen-supported, that
  single asset falls back to the interpreter (ship its `graphJson` + a small
  interpreter) while the rest compiles. Packaging never fails on one node; log
  what fell back (no silent caps).

## 8. Phasing
1. **Unify the API** (§2): promote `ScriptApi` → `HE::api`, add the descriptor
   registry, regenerate Lua/Python from it, route `Context`/`Services` through
   it. (This is also step 1 of the completion plan.)
2. **Codegen core**: single-class lowering (variables, events, functions, flow,
   data, get/set) + the parity harness. No cross-class refs, no engine calls.
3. **Engine calls**: the Engine Call node → `HE::api` emission via the registry.
4. **References**: `hc::Ref`, dispatch registry, external/bind/emit, create/destroy.
5. **Pipeline**: wire export-time codegen + factory bake into the GameLogic dylib
   path; profile flag + hybrid fallback.
6. **Optimize**: direct typed calls for known classes; CSE hoisting (parity-gated).

## 9. Risks / open questions
- **`CallExternal` args** now exist (typed args/returns landed) — good; keep the
  ABI stable for codegen.
- **Dynamic dispatch cost** for refs of unknown class (vtable-style `callPublic`)
  — measure vs. the interpreter.
- **Dispatcher determinism**: listener iteration order + the depth-32 recursion
  guard must be reproduced exactly, or parity tests flap.
- **Toolchain in export**: the export host needs a target C++ compiler (already
  true where the Game runtime is built); keep the interpreted profile as the
  no-toolchain path.

## 10. Related
- Material node-graph → shader cross-compile (the precedent).
- `GameLogicLoader` (C1/C2) native gameplay dylib + dlopen hot-copy (the load path).
- hpak v2 pack-time UUID baking + reference graph (where factories are baked).
- Export Profiles / `hpak_packer` async export worker (where codegen hooks in).
- **`horizoncode-completion-plan.md`** — the node roadmap that fills the API.
