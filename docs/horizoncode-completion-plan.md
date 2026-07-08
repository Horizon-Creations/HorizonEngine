# HorizonCode Completion Plan: full engine + platform API coverage

Status: **in progress**. How HorizonCode grows from "widget/UI logic" into a
general visual scripting language with **full parity to Lua/Python** (every
engine subsystem) **plus platform APIs** (file I/O, time, persistence, …).

**Landed (Phase 1, step 1 — the foundation):** the central `HE::api` namespace
(HE_Scene `EngineApi.h/.cpp`) with an explicit `Ctx {world, physics, content}`
and subsystem groups (entity / transform / physics / material / ui / widget /
cursor + a pure math library), plus the machine-readable **`ApiFn` registry**
(`registry()` / `find(id)`) — one row per function carrying `id`, `category`,
`isExec`, typed `params`/`results`, `cppCall`, and a Value-marshalling `invoke`
thunk. Covered by `tests/test_engine_api.cpp`. **Still ahead:** route the
HorizonCode interpreter through it (a generic **Engine Call** node + a
`Context::callApi` seam), regenerate the Lua/Python bindings from the registry,
and add the subsystems that need new `Ctx` providers (audio/input/camera/time/
scene/fs — see §3.4+ and §4).

Companion to `horizoncode-cpp-codegen-plan.md`: everything here is designed so
each new capability is *simultaneously* interpretable (editor/PIE) and
C++-compilable (shipping), with zero per-function special-casing.

## 1. The one mechanism: an API descriptor registry

Adding a NodeType per engine function would explode the enum and the interpreter
switch, and force codegen to special-case each. Instead, **one machine-readable
registry** describes every engine/platform function, and a single generic
**Engine Call** node type is parameterized by it. (Full definition in the codegen
plan §2.3.)

```cpp
struct ApiFn {
  const char* id;         // "transform.setPosition", "fs.readText", …
  const char* category;   // add-menu group
  bool        isExec;     // exec node (has side effect) vs pure data node
  std::vector<ApiParam> params;   // typed inputs
  std::vector<ApiParam> results;  // typed outputs
  const char* cppCall;    // "HE::api::transform::setPosition"
};
```

One row per function feeds **four** consumers: the editor add-menu + pins, the
interpreter (a `Context` thunk per `id`), the C++ codegen (`cppCall`), and the
Lua/Python bindings. **Completing HorizonCode = filling this table + writing one
thunk each.** No enum growth, no codegen changes per function.

Rule of thumb for every entry:
- Pure (no side effect, deterministic) → **pure data node** (compact chip, no
  exec pins); the interpreter may re-evaluate, codegen inlines it.
- Side-effecting → **exec node**; its results (if any) are cached per run
  (`m_execOutputs`) and codegen assigns them to a local.

## 2. Where we are (grounded)

- **`HE::api` today** (as `ScriptApi`, HE_Scene) — ~40 functions, bound
  identically to Lua + Python: `log`; transforms (`get/setPosition/Rotation/
  Scale`); `spawn`/`destroy`; physics (`raycast`, `setVelocity`, `isGrounded`);
  materials (`get/setMaterialParam`); entity-UI (`get/setUIText/Color/Visible/
  Position/Size`, `setUIMaterialParam`); widgets (`create/destroy/show/hide
  Widget`, `setWidgetZOrder`, `isWidgetVisible`, `callWidgetFunction`); cursor
  (`show/hide/setCursorVisible`); helpers (`getName`, `distance`).
- **HorizonCode nodes today** reach only a slice: widget nodes (Create/Show/
  Hide/Destroy), UI-element `Get/SetProperty`, object create/destroy, and the
  reference family (external var/fn, bind/emit). **No** transforms, physics,
  spawn, materials, audio, input, camera, time, or platform APIs.
- **Not exposed to *any* frontend yet** (new engine surface): input, camera,
  audio, time, scene/level loading, entity queries/tags, generic components.

So "parity with Lua/Python" is a floor (~40 fns); the real target is broader.

## 3. Engine API node families (the roadmap)

Each family = a set of registry entries in one `HE::api::<subsystem>` namespace.
Status: **[api]** already in `ScriptApi` (just needs a registry row + node),
**[new]** needs an engine implementation too.

### 3.1 Core / entities
- Transform: get/set position, rotation, scale; translate/rotate helpers;
  world↔local. **[api]** for the six; **[new]** for helpers.
- Lifecycle: `spawn(parent,name)`, `destroy(entity)`, `spawnPrefab(asset,pos)`.
  **[api]** spawn/destroy; **[new]** prefab spawn.
- Hierarchy/query: `findByName`, `findByTag`, `getParent/Children`,
  `setParent`, tags get/set. **[new]**.
- Components: generic `getComponentField/setComponentField` (typed), enable/
  disable. **[new]** — the escape hatch for anything not otherwise wrapped.
- Entity as a **Ref/handle** in HorizonCode so entities flow through pins like
  objects/widgets already do.

### 3.2 Physics
- `raycast(origin,dir,max) → {hit, entity, point, normal, distance}` **[api]**;
  `sphereCast`/`overlap` **[new]**.
- `setVelocity`, `getVelocity`, `applyImpulse`, `isGrounded` — **[api]** for two,
  **[new]** for the rest.
- Collision/trigger events already fire to scripts (kollisions-callbacks) →
  expose as HorizonCode **events** (OnCollisionEnter/Exit, OnTriggerEnter/Exit).

### 3.3 Materials / rendering
- `get/setMaterialParam(entity,name,vec4)` **[api]**; per-widget-element material
  param **[api]** (`setUIMaterialParam`).
- Set material/texture on an entity; toggle visibility/cast-shadow. **[new]**.
- Environment: sun direction/color, fog, time-of-day, weather knobs (already
  wired in EnvironmentComponent) → setter nodes. **[new]**.

### 3.4 Audio
- `playSound(asset, volume, pitch)`, `playSound3D(asset, pos, …)`, `stop`,
  `setListener`. **[new]** (audio engine exists via Audio-Play-Mode; expose it).

### 3.5 Input  **[done — core]**
- **[done]** `input.keyDown(name)` (SDL scancode names — "W"/"Space"/…),
  `input.mouseButton(i)` (0/1/2), `input.mousePosition`, `input.mouseDelta`,
  `input.scrollDelta` — pure getters over a process-global snapshot in
  `HE::api::input`, pushed each frame by the app (`GameApplication` always,
  `EditorApplication` during PIE) from SDL's global keyboard/mouse state. Live in
  HC + Lua/Python `horizon.input.*`. **[new]** mouseDelta/scrollDelta are wired but
  left at 0 by the app push (to avoid consuming SDL's relative-motion accumulator
  the camera controller owns); pressed/released edges, gamepad, and action maps
  still to come.

### 3.6 Camera  **[new]**
- Get/set active camera, position/rotation, FOV; `worldToScreen`/`screenToWorld`;
  `screenToRay` (pairs with physics raycast for picking).

### 3.7 UI (entity UI + standalone widgets)
- Entity UI: `get/setUIText/Color/Visible/Position/Size` **[api]**.
- Widgets: create/show/hide/destroy/zorder/visible/callFunction — **[api]**, and
  HorizonCode already has the widget nodes + widgets-as-Ref. Rounds out with
  element property get/set (already have `Get/SetProperty`).
- Cursor: show/hide/visible **[api]**; set hover cursor/hit-test at runtime
  (the new per-element props) **[new]**.

### 3.8 Scene / level  **[new]**
- `loadScene(name)`, `loadSceneAdditive`, `unloadScene`, `getActiveScene`.
  (Level scripts already get OnLevelLoaded/Unloaded events.)

### 3.9 Time / frame  **[done — core]**
- **[done]** `time.deltaTime()`, `time.elapsed()` (seconds since play-start),
  `time.frameCount()` — pure getters over a process-global clock in `HE::api::time`
  advanced once per frame by the app (`GameApplication` always,
  `EditorApplication` during PIE; reset on play-start). Live in HC + Lua/Python
  `horizon.time.*`. **[new]** `timeScale get/set` and `setTimeout`/timers (need an
  event when elapsed) still to come.

### 3.10 Math / random / string
- Math: extend beyond the current Add/Sub/Mul/Div/compare — `min/max/clamp/abs/
  floor/ceil/round/pow/sqrt/sin/cos/atan2/lerp`, vector ops (dot/cross/normalize/
  length/distance), `distance` **[api]**. All **pure**.
- Random: `random.seed(n)`, `random.value()` [0,1), `random.range(min,max)`,
  `random.rangeInt(min,max)` (inclusive), `random.chance(p)` — **[done]** a seeded
  process-global PRNG in `HE::api::random`. Stateful, so each is an exec node in
  HorizonCode (caches one draw per run, not re-rolled per pin read); seeding makes
  a run reproducible for the parity harness. Live in HC + Lua (`horizon.random.*`)
  + Python via the registry-driven dispatcher.
- String: `format`, `substring`, `find`, `split`, `toNumber`, `length`
  (have `Concat`/`ToString`). **pure**.

### 3.11 Log / debug
- `log(msg)` **[api]** (have `Print`); `warn`/`error`, `drawDebugLine/Sphere`,
  on-screen debug text. **[new]** for the rest.

## 4. Platform APIs (`HE::api::fs`, `save`, `sys`, `time`, `net`)

Games need host services, but with a **sandbox** (§5). All new.

### 4.1 File I/O — `HE::api::fs`, sandboxed
- `readText(rel) → string`, `writeText(rel, string)`, `readBytes`/`writeBytes`,
  `exists(rel)`, `remove(rel)`, `listDir(rel) → [names]`, `mkdir(rel)`.
- Paths are **relative to a sandbox root** (the mounted pak's read-only content
  for reads, and a writable **user/save dir** for writes) — never raw absolute
  paths. In shipping, content reads route through the hpak VFS (may be
  compressed/encrypted); writes go to the per-user save dir.

### 4.2 Persistence / save games — `HE::api::save`
- Key/value: `setInt/Float/String/Bool(key,val)`, `getInt/...(key,default)`,
  `hasKey`, `deleteKey`, `saveToSlot(slot)`, `loadFromSlot(slot)`, `listSlots`.
- Or blob: serialize a HorizonCode object/struct to a slot. Built on `fs` +
  the save dir; JSON or the engine's binary format.

### 4.3 Time / clock — `HE::api::time`
- Wall clock `now()` (epoch), `localTime` fields, `formatTime`. (Frame time is
  §3.9.) Note: non-deterministic → for **codegen parity tests**, wall-clock
  functions are marked *impure/non-reproducible* and excluded from golden
  comparison (like `Date.now()` in the workflow runtime).

### 4.4 System — `HE::api::sys`
- `quit()`, `platformName()`, `openURL(url)` (gated), `getCommandLineArg`,
  environment info (screen size, locale). Clipboard get/set (gated).

### 4.5 Networking — `HE::api::net`  (later, gated)
- `httpGet/Post(url) → {status, body}` async → completion event; sockets later.
  Off by default; opt-in per project (security).

## 5. Sandboxing & security

- **No raw filesystem.** `fs`/`save` operate only under the content sandbox
  (read) and the user save dir (write). Reject `..`/absolute paths.
- **Gated capabilities.** `net`, `sys.openURL`, clipboard require an explicit
  per-project capability flag in the `.heproj`; absent → the node compiles to a
  no-op + a logged warning, and the editor greys it out.
- **Determinism.** Random is seedable; wall-clock/net are flagged non-reproducible
  so the parity harness excludes them.
- These constraints live in the descriptor (`ApiFn` gains `capability` +
  `reproducible` flags), so all four frontends enforce them uniformly.

## 6. Codegen compatibility (the constraint on every addition)

Every registry entry must be expressible as a single C++ call
`HE::api::<subsystem>::<fn>(ctx, args…) → results`. That forces the API to stay:
- **Free functions over an explicit `Ctx`** (no hidden globals) — already the
  `ScriptApi` shape.
- **Plain value/handle types** on the boundary (no `std::function`, no engine
  internals) so both a `std::function` thunk (interpreter) and a direct call
  (codegen) bind to the same signature.
- **Side-effect classification** (`isExec`) fixed per function, so the pull-model
  lowering (codegen plan §4.4) is unambiguous.

If a capability can't be shaped this way, it doesn't get a node until it can —
this is what keeps the interpreter and the compiled build permanently in lockstep.

## 7. Phasing

1. **Unify + registry** (shared with codegen plan §8.1): **[done]** the central
   `HE::api` + `ApiFn` `registry()` exist (`EngineApi.h/.cpp`), promoted from
   `ScriptApi` and seeded with the full engine surface + math. **[done]** the
   generic **Engine Call** node (`NodeType::EngineCall`) now routes the HorizonCode
   interpreter through the registry via a `Context::callApi` seam (bound by the app
   to an `HE::api::Ctx` over the current world/content); the shared editors offer it
   in the add-menu grouped by subsystem. So HorizonCode already reaches everything
   Lua/Python can. **[in progress]** the Lua + Python frontends are now
   *registry-driven* for whole groups: a single generic Value-marshalling
   dispatcher (Lua `lua_engine_dispatch` / Python `_engineCall`) exposes a group
   by iterating `registry()` — no per-function C shim — surfaced as
   `horizon.<group>.<fn>` (spread ABI: a vec2 = 2 numbers, a Color = 4, matching
   the hand-written bindings). First group live in both languages: the pure
   **Math** library (`horizon.math.*`), which no frontend could reach before. The
   gameplay groups keep their ergonomic hand-written shims (they still call
   `ScriptApi`, which `HE::api` wraps, so they are transitively central already)
   until `ScriptApi` is inverted onto `HE::api` — then each group widens the same
   dispatcher's filter, a one-line change.
2. **High-value gameplay**: Transform, Physics (+ collision events), Spawn/
   Lifecycle, Time/frame, Input, Math/Random. This is what most game logic needs.
3. **Presentation**: Audio, Camera, Materials/Environment, Scene/level, debug draw.
4. **Platform**: `fs` + `save` (with sandbox), `sys`. 
5. **Gated**: `net`, clipboard, `openURL`.
6. Keep the codegen plan's phases in step — each family lands interpretable
   first, then its `cppCall` is verified by the parity harness.

## 8. Related
- `horizoncode-cpp-codegen-plan.md` — how the registry drives C++ emission.
- `ScriptApi` (HE_Scene) — the seed of `HE::api`; Lua `ScriptContext`, Python
  `PyScriptBackend` — the frontends to regenerate from the registry.
- hpak v2 VFS (content reads) + per-user save dir (writes) — the `fs`/`save`
  backends.
