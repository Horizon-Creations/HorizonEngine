# HorizonCode → C++ Codegen: Design Plan

Status: **design only** (no code yet). This document describes how a later
parser/codegen turns HorizonCode graphs into native C++ for packaged/shipping
builds, while the editor keeps interpreting the same graphs. It is the
HorizonCode analogue of the Material system's node-graph → shader cross-compile:
*generic in the editor, native at pack time, identical behavior in both.*

## 1. Goal & constraints

- **Dual execution model.** Editor + PIE keep using the interpreter
  (`HorizonCode::Runner`, driven by `HorizonCode::Runtime`) — instant iteration,
  no build step. Packaged builds run **compiled C++** generated ahead of time
  (AOT). No interpreter in shipping (except the hybrid fallback in §7).
- **Behavioral parity is the contract.** For every graph, the compiled output
  must produce byte-identical observable behavior to the interpreter (same event
  order, same values, same host calls). This is verifiable with golden tests
  (§6), exactly like the material codegen is validated per backend.
- **No architectural drift.** The current model was deliberately shaped so this
  translation stays mechanical. The pieces that already make codegen tractable:
  typed function I/O (`FuncParam` params/results), typed variables (`Variable`
  + `className`), the class registry (`HcEditorUtil::listClasses`), and the
  host-agnostic `Context`/`HostBindings` seam. Keep new features inside those
  seams.

## 2. What we translate (the current model)

Source of truth lives in `src/HE_Core/include/HorizonCode/HorizonCode.h`:

- `Graph` = `{ nodes, links, variables }`. One graph == one **class**
  (a widget, an HC class asset, the level script, or the GameInstance).
- `Node` (`NodeType`) with unified pins `[execIns][execOuts][dataIns][dataOuts]`
  computed by `signatureOf`. Exec pins are white; data pins carry a `PinType`.
- `Link` connects pins by unified index.
- `Variable` = `{ name, type (PinType), default, access, className }`.
- `FuncParam` = `{ name, type }`; a `FunctionEntry` owns `params`/`results`,
  mirrored onto `FunctionCall`/`FunctionReturn`.
- Execution semantics, from `Runner`:
  - **Exec flow** is push-based: `Event`/`FunctionEntry` start a chain;
    `runExecChain` walks exec links; `Branch`/`Sequence` steer.
  - **Data flow** is pull-based: `evalData` recursively evaluates a data node's
    inputs on demand. Pure nodes (math/const/get) are re-evaluated on each read.
  - **Side-effecting exec nodes cache** their outputs for that run
    (`m_execOutputs`): `CreateWidget`, `CreateObject`, and `FunctionCall`
    results are computed once at the exec point and read back downstream.
  - **Call frames** (`m_callStack`): a `FunctionCall` pushes args, runs the
    entry's body, a `FunctionReturn` writes results, the call reads them back.
  - **References** (`PinType::Ref`, an `InstanceId`): `GetSelf`,
    `GetGameInstance`, object variables, `CreateObject`/`CreateWidget` produce
    refs; `CallExternal`, `Get/SetExternal`, `BindEvent`, `EmitEvent` act
    through them. Access is gated on the public/private modifier at runtime.
  - **Host effects** go through `Context` callbacks bound by the owning system
    (`getProperty/setProperty`, `showSelf`, widget/object services, delegation).

## 3. Translation strategy

### 3.1 A class → a C++ class

Each graph compiles to a class deriving from a thin `CompiledInstance` base that
exposes the same surface the interpreter drives:

```cpp
class Login_Widget final : public hc::CompiledInstance {
    // variables → typed members
    std::string  m_user;
    bool         m_ready = false;
    hc::Ref      m_panel;            // object variable (was PinType::Ref + className)
public:
    void onEvent(hc::EventId, const hc::Value& arg) override;   // event dispatch
    bool callPublic(hc::FnId, hc::ArgView in, hc::RetView out) override; // public fns
    // private functions are ordinary private methods
};
```

- **Variables → members.** `PinType` maps to concrete C++ types:
  Float→`float`, Bool→`bool`, Int→`int`, String→`std::string`,
  Vec2→`glm::vec2`, Color→`glm::vec4`, Ref→`hc::Ref` (an `InstanceId` handle).
  Object variables with a `className` can additionally get a typed accessor.
- **Access modifier** decides `public`/`private` membership; `callPublic`
  only routes public functions (mirrors `callFunction(requirePublic)`).

### 3.2 Exec chains → statements

Lower each exec chain (from an `Event` or `FunctionEntry`) to a straight-line
function body:

- `Sequence` → the branches emitted in order.
- `Branch` → `if (cond) { …then… } else { …else… }`.
- `SetVariable`/`SetProperty`/`SetExternal` → assignment (and the pass-through
  data-out is just the assigned value — an assignment expression in C++).
- `FunctionCall` → a method call; args evaluated first, results captured into
  locals (matches the interpreter's cache-at-exec-point behavior).
- `CreateWidget`/`CreateObject` → a service call whose returned `hc::Ref` is
  stored in a local (single evaluation, exactly like `m_execOutputs`).
- `FunctionReturn` → write result out-params / `return`.
- `Print`, widget show/hide, destroy → the matching runtime service call.

### 3.3 Data pins → expressions (the pull model)

Data is lowered per exec step by walking each exec node's data inputs:

- **Pure data nodes** (`Const*`, math, logic, string, `GetVariable`,
  `GetProperty`, `GetSelf`, `GetGameInstance`) → inline expressions. Re-emitting
  the expression at each read reproduces the interpreter's re-evaluation, so the
  naive lowering is already parity-correct.
- **Exec-cached nodes** (`CreateWidget`/`CreateObject`/`FunctionCall`) → emit
  the side effect once at its exec position into a uniquely-named local; every
  downstream data read references that local. This is the one place the lowering
  must be careful, and it maps 1:1 onto the existing `m_execOutputs` rule.
- Optional optimization: common-subexpression elimination by hoisting pure
  sub-trees to locals — only when it does **not** change evaluation count for
  nodes with observable reads (keep it behind a parity test).

Concretely this is a small SSA-ish pass: for each exec node, topologically order
its transitive pure-data dependencies, allocate temporaries for cached nodes,
emit expressions, then emit the exec node's statement.

### 3.4 References & cross-class calls

- `hc::Ref` is the `InstanceId` handle. The runtime keeps a registry of live
  compiled instances (the compiled analogue of `Runtime::m_insts`).
- `CallExternal`/`Get/SetExternal` → a runtime dispatch on the target ref
  (`instance->callPublic(...)` / `getPublicVar/setPublicVar`), respecting the
  access check. When the target's class is statically known (typed object
  variable / `CreateObject` of a known asset), codegen may emit a **direct typed
  call** for speed; otherwise it stays a virtual dispatch.
- `BindEvent`/`EmitEvent` → the same listener-registry semantics as
  `Runtime::bindEvent`/`dispatchToListeners`, provided by the compiled runtime.
  Dispatch ordering and the recursion guard must match the interpreter exactly.
- **Gap to close first:** `CallExternal` currently has no argument/return pins.
  Before codegen, give it the same param/result mirroring as `FunctionCall`
  (and thread args/returns through `Context.callExternal`). Codegen then maps it
  straight onto `callPublic`.

### 3.5 Events & lifecycle

- `Event` nodes → cases in `onEvent`. Names are interned to `EventId` at
  codegen (a generated enum/table) so dispatch is a switch, not a string compare.
- Lifecycle events (`Construct`/`Destruct`, `OnLevelLoaded/Unloaded`,
  `OnInit/OnShutdown/OnWindowFocusChanged`, widget `Tick`) are ordinary events —
  the host fires them the same way it does today.

## 4. Semantic mapping table (summary)

| HorizonCode | C++ lowering |
|---|---|
| Graph (class) | `class X : hc::CompiledInstance` |
| Variable | typed member; default in ctor / member init |
| FunctionEntry(public/private) + params/results | member function with signature |
| FunctionCall | method call, args in / results to locals |
| FunctionReturn | write out-params / `return` |
| Event | `onEvent` switch case |
| Branch / Sequence | `if/else` / ordered statements |
| Get/SetVariable | member read / assignment (set returns value) |
| Get/SetProperty | host `getProperty/setProperty` |
| Get/SetExternal | ref → `getPublicVar/setPublicVar` (access-checked) |
| CallExternal | ref → `callPublic` (or direct typed call) |
| CreateWidget/Object | service call → `hc::Ref` local |
| Destroy* / Show/Hide* | runtime service call |
| BindEvent/EmitEvent | compiled dispatcher registry |
| GetSelf/GetGameInstance | `this` / `gameInstance()` |
| Const* | literal |
| Math/Logic/String | operators / std calls (matching `coerce` rules) |

`coerce` (the interpreter's implicit numeric conversions) must be reproduced
exactly — generate the same Float↔Int↔Bool conversions, not C++'s.

## 5. Components to build

1. **`HE_HorizonCodeGen`** (new static lib / part of `HE_Tools`): pure function
   `std::string generate(const Graph&, const ClassRegistry&, GenOptions)` →
   `.h`/`.cpp` text. No engine runtime deps; takes the graph + the registry
   (already available as `HcEditorUtil::listClasses` data, promoted to a
   headless `HE_Core` structure so the packer can build it without the editor).
2. **Compiled runtime support** (`HE_Core`): `hc::CompiledInstance` base,
   `hc::Ref`, the instance/dispatch registry, and the `Services`/`HostBindings`
   surface shared with the interpreter (one definition, two consumers).
3. **Factory registry / bake**: generated code self-registers a factory keyed by
   the asset **UUID** (pack-time-baked, matching hpak v2 UUID baking). At load,
   `CreateObject`/`CreateWidget` and scene/level/GI loading resolve a compiled
   factory instead of parsing `graphJson`.
4. **Parity harness** (tests): run N sample graphs through both the interpreter
   and the generated+compiled class; assert identical variable stores, host-call
   traces, and event outcomes.

## 6. Parity verification

- A `test_horizoncode_codegen` suite: for each fixture graph, (a) run the
  interpreter capturing a trace (ordered host calls + final variable values),
  (b) compile+run the generated class capturing the same trace, (c) assert
  equal. This is the compiled analogue of the material "interpreter result ==
  compiled result" contract and gates every codegen change.
- Fuzz small random graphs (bounded node budget) to shake out lowering bugs.

## 7. Build-pipeline integration

Reuse the existing export/packaging machinery rather than inventing a new one:

- **When:** at export time, inside the async export worker (Export Profiles /
  `hpak_packer`), after asset discovery. The packer already walks the reference
  graph and bakes UUIDs — hook a codegen pass there.
- **What it emits:** for every HC asset (widget graphs, HC class assets) plus the
  per-scene level scripts and the project GameInstance, generate `.cpp/.h` into a
  build scratch dir, plus one generated `hc_registry.cpp` that registers all
  factories by UUID.
- **How it's compiled:** feed the generated sources into the **GameLogic dylib**
  path that already exists (`GameLogicLoader` C1/C2, dlopen hot-copy) — i.e.
  compiled HorizonCode ships as native game logic alongside C++ gameplay, loaded
  by the packaged `HorizonGame`. This avoids embedding a compiler in the runtime;
  the export step drives the toolchain (same place the runtime bundle is built),
  mirroring how the editor build already deploys the Game runtime.
- **Selection:** an export-profile flag `HorizonCode = Interpreted | Compiled`
  (default Compiled for Shipping, Interpreted for Development) — parallels the
  Development/Shipping profiles already seeded in the `.heproj`.
- **Hybrid fallback (safety):** if any node/feature isn't yet supported by
  codegen, that single asset falls back to the interpreter (ship the `graphJson`
  + a small interpreter for it) while everything else is compiled. Packaging must
  never fail because of one unsupported node; log what fell back (no silent caps).

## 8. Phasing

1. **Prep the model** (mostly done): typed functions ✅, typed variables ✅,
   class registry ✅. Remaining: `CallExternal` arg/result pins + threaded
   `Context.callExternal` args; intern event/function names.
2. **Codegen core**: single-class lowering (variables, events, functions, flow,
   data, get/set) with the parity harness. No cross-class refs yet.
3. **References**: `hc::Ref`, dispatch registry, `CallExternal`/`Get/SetExternal`,
   `BindEvent`/`EmitEvent`, `CreateObject/Widget`, `Get*Instance`.
4. **Pipeline**: wire the export-time codegen + factory bake into the GameLogic
   dylib path; add the profile flag + hybrid fallback.
5. **Optimize**: direct typed calls for known classes, CSE hoisting (parity-gated).

## 9. Open questions / risks

- **CallExternal signature** must gain arg/result pins before it can be compiled
  (and it improves the interpreter too). Do this in the model first.
- **Dynamic dispatch cost**: refs of unknown class need a vtable-style
  `callPublic`; measure vs. the interpreter to confirm the win.
- **Dispatcher determinism**: listener iteration order + the depth-32 recursion
  guard must be reproduced exactly, or parity tests will flap.
- **Coercion semantics**: generate HorizonCode's `coerce`, not C++'s implicit
  conversions, for Float/Int/Bool boundaries.
- **Toolchain in export**: the export host needs a C++ compiler for the target
  (already true where the Game runtime is built); document the requirement and
  keep the interpreted profile as the no-toolchain path.

## 10. Related

- Material node-graph → shader cross-compile (the precedent for "generic in
  editor, native at pack time").
- `GameLogicLoader` (C1/C2) native gameplay dylib + dlopen hot-copy — the load
  path compiled HorizonCode should reuse.
- hpak v2 pack-time UUID baking + reference graph — where the factory registry
  is baked.
- Export Profiles / `hpak_packer` async export worker — where the codegen pass
  hooks in.
