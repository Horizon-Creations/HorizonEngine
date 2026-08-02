# Recorded deferrals — 2026-07 rework

Things the 2026-07 rework (`docs/rework-audit-2026-07.md`) named and then deliberately did
**not** do. They are written down here for one reason: an undocumented non-change is
indistinguishable from an oversight, so the next person either redoes the analysis or —
worse — starts the work without knowing why it was stopped last time.

Each entry states the current state with file:line, the size, why it was out of scope for
that pass, and what actually doing it involves.

---

## 1. `ScriptApi` → `HE::api` inversion — not done

Audit §6 proposed inverting the scripting bindings onto the `HE::api` registry, which
"makes ~500 lines of hand shims obsolete". That inversion did not happen.

**Current state.** Every *flat* gameplay function is still a hand-written C shim, once per
language:

- Lua: 35 `lua_horizon_*` functions in `src/HE_Scene/src/ScriptContext.cpp:53–374`,
  registered in `kHorizonFuncs[]` (`:479–516`).
- Python: their 29 twins in `src/HE_Scene/src/PyScriptBackend.cpp:36–258`, registered in
  `kHorizonMethods[]` (`:339`).

What *is* registry-driven is the grouped surface: one generic dispatcher per backend
(`ScriptContext.cpp:381+`, `PyScriptBackend.cpp:262+`) marshals `HorizonCode::Value`s by
pin type, and `HE::api::isScriptGroup` (`src/HE_Scene/src/EngineApi.cpp:1175`) decides
which id namespaces reach it — currently `math, random, time, input, string, camera, env,
entity, audio, debug, fs, save, scene`. The namespaces that are in the registry but *not*
on that list — `ui`, `widget`, `transform`, `physics`, `material`, `cursor` — are exactly
the ones the hand shims still serve (`horizon.setPosition`, `horizon.setUIText`,
`horizon.createWidget`, `horizon.raycast`, `horizon.setMaterialParam`,
`horizon.showCursor`, …).

**Why it was out of scope.** This is not mechanical de-duplication; it changes the
*script-visible* calling convention. The hand shims are ergonomic per function (a vec3 as
three numbers, optional arguments, Lua multiple returns), while the generic dispatcher has
one uniform ABI in which a packed vec3/`Color` param spreads as **four** numbers — the
`isScriptGroup` comment in `EngineApi.h` calls that out. Moving a function from one to the
other therefore edits the signature every existing `.lua`/`.py` in every user project calls.
That is a migration with a compatibility story, not a refactor; this rework pass was
behaviour-preserving by construction.

**What doing it involves**, roughly in order:

1. A registry row per gameplay function: `params`/`results`/`cppCall`/`invoke`, satisfying
   the one-distinct-callee-per-row invariant that `tests/test_engine_api.cpp` enforces.
2. A decision on the arity convention, because the shims and the dispatcher currently
   disagree on vec3/`Color` — either the dispatcher grows per-row spreading, or scripts get
   a compat shim, or the break is accepted and documented.
3. Widening `isScriptGroup` and deleting shim + table row **in both backends in the same
   commit**: Lua and Python must expose the identical surface or a script works in one
   language and not the other (that is why the group list is one list, not one per backend).
4. HorizonCode codegen and the interpreter reach the same rows by id
   (`hc::callApi(ctx, "<id>", …)`), and `tests/test_horizoncode_codegen.cpp` asserts on the
   traced call text (`"callApi random.seed(i:42) -> ()"`, `:482`) — so every id or arity
   change lands there too.

One prerequisite is already done and is *not* a blocker any more: `HE::api::find()` (audit
§6's linear scan) is now an O(1) `unordered_map` index built once from `registry()`
(`src/HE_Scene/src/EngineApi.cpp:1184–1200`), which is what makes it viable as the per-call
lookup for everything.

**Note on the in-code comments.** The banners at `ScriptContext.cpp:381` and
`PyScriptBackend.cpp:262` used to end with "until `ScriptApi` is inverted onto `HE::api`"
— an intention that predated this rework, not a decision. They now point here instead, and
their claim that "the first registry-driven group is Math" was dropped: `isScriptGroup`
has grown well past Math, so the comment understated the code.

---

## 2. Component (de)serialisation typed twice — 2 of 30 blocks converted

Audit §2's headline was "every component field typed twice" in
`src/HE_Scene/src/SceneSerializer.cpp` (once in `serializeComponents`, once in
`applyComponents`). **Two** of the thirty component blocks were converted to generate both
halves from a single field list:

- `environment` — reuses `HE_ENV_FIELDS_*` from `HorizonScene/EngineApi.h`, the same list
  that generates its scripting API.
- `navmesh.config` — `HE_NAVCFG_FIELDS`, defined locally in `SceneSerializer.cpp`.

The other 28 still type every field twice: `transform`, `transform2d`, `mesh`, `material`,
`camera`, `light`, `rigidbody`, `collider`, `characterController`, `script`, `weather`,
`terrain`, `foliage`, `lod`, `navagent`, `animator`, `animatorblend`, `animstatemachine`,
`propertyanimator`, `particlesystem`, `skeletalmesh`, `audiosource`, `audiolistener`,
`uicanvas`, `uielement`, `uitext`, `uiimage`, `uibutton`.

**Why not all 28.** The two that were converted are the two flat, uniform blocks — a list
of scalars, 1:1 on both sides. The rest are not uniform: base64 blobs with size validation
(`terrain`, `navmesh.geometry`), legacy-format fallbacks (`particlesystem`,
`animstatemachine`, `terrain.sculptHeights`), enum casts, nested vectors (`lod.levels`,
`material.paramOverrides`, `script.properties`) and skip rules (`mesh` is not written at
all on a terrain entity). Each would need its own X-list plus escape hatches for the
irregular fields — 28 bespoke macro dialects, harder to read than the hand-written pairs
they replace, and still carrying the same risk in the escape hatches.

**What protects against the failure instead.** The failure this redundancy causes is a
field serialised but not applied, or applied but not serialised — silent data loss noticed
after the user saves. That is now a red test:
`tests/test_scene_serializer.cpp` → *"Every component survives a round-trip with
non-default values in every persisted field"*, covering all 28 blocks above in both the
JSON and the CBOR/undo path.

**What that test does not catch:** a field that is in *neither* half — never serialised at
all. One known instance: `TerrainComponent::heightmapTexture`
(`src/HE_Scene/include/HorizonScene/Components/TerrainComponent.h:25`) is declared and is
collected as an asset reference (`src/HE_Scene/src/SceneSystems.cpp:127`), but is neither
written nor read by `SceneSerializer`. Harmless today because nothing assigns it; it
becomes data loss on the day the editor does.

---

## 3. Method casing on `Application` — kept, recorded as a convention exception

Audit §5's own named example. `Run`/`Quit` and the `On*` subclass hooks are PascalCase next
to camelCase accessors on the same class, which contradicted
`docs/coding-conventions.md` §4 ("methods: camelCase by default"). The names were **not**
changed — `Application` is `HE_API`-exported and the PascalCase members are precisely the
ones out-of-repo game code calls or overrides, so renaming them is a source break for
users, not a cosmetic cleanup.

The reasoning now lives where the contradiction was, as an explicit exception in
`docs/coding-conventions.md` §4.
