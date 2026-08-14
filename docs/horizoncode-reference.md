# HorizonCode — Node & Subsystem Reference

HorizonCode is HorizonEngine's Blueprint-style visual scripting system. Graphs are
authored in the shared node editor (the same canvas the material editor uses) and
run on one central interpreter, `HorizonCode::Runtime` (HE_Core). This document is
the reference for **every built-in node** and **every engine subsystem** a graph can
reach — kept in sync with `HorizonCode.h` (`NodeType`) and the `HE::api` registry
(`EngineApi.cpp`).

Editor display names are used throughout (what you see on the node), with the
internal `NodeType` in parentheses where useful.

---

## 1. Where graphs run — the four hosts

A HorizonCode graph is always owned by exactly one host. All hosts share one
`Runtime`, so references (`Ref`) can cross between them.

| Host | Asset / source | Lifetime | Fires |
|------|----------------|----------|-------|
| **GameInstance** | `GameInstance.hcode` (project root) | whole app | `OnInit`, `OnShutdown`, `OnWindowFocusChanged` |
| **Level Script** | embedded in the `.hescene` | one scene/zone | `OnLevelLoaded`, `OnLevelUnloaded` |
| **Widget graph** | a UI Widget asset | while the widget lives | `Construct`, `Destruct` + UI element events |
| **HC Class** | a HorizonCode Class asset | while the object lives | `Construct`, `Destruct` + custom events |
| **Entity class** | an HC Class with base `Entity`, named by an entity's **Script** component | while that entity lives | the above + `BeginPlay`, `Tick`, `OnBeginOverlap`, `OnEndOverlap`, `OnHit`, `OnHitEnd` |

An Entity class is attached through the ordinary **Script** component — the same
slot that carries a `.lua`/`.py` script; the engine branches on the referenced
asset's type. There is no separate "HorizonCode component". The instance and the
entity die together in both directions: destroying the entity fires the
instance's `Destruct`, and destroying the object takes its entity with it. Its
scene entity is reachable with **Get Owning Entity** (`entity.self`), which is
what every transform/physics/material call takes.

The GameInstance persists across scene switches and is reachable from any graph via
**Get Game Instance**. Objects created with **Create Object** live on the runtime;
only those held by the GameInstance survive a scene change. In the packaged game the
GameInstance's UI is **app-level**: widgets it creates in `OnInit` live in a
WidgetManager owned by the app (not any world), so they appear from frame one and
**persist across `scene.load`** — a HUD stays up through level changes. (`OnInit`
therefore fires before the first world is even built.)

---

## 2. Built-in nodes

### Events & entry points
| Node | Purpose |
|------|---------|
| **Event** (`Event`) | Host-fired entry point (`OnInit`, `Construct`, `OnLevelLoaded`, a UI event, or a custom event). Optional argument data-out. |
| **Function** (`FunctionEntry`) | Declares a function: typed input params (data-outs here) + an access modifier. Body is a subgraph. |
| **Return** (`FunctionReturn`) | Writes the owning function's return values. Terminal (no exec-out). |
| **Get Self** (`GetSelf`) | `Ref` to this instance. |
| **Get Game Instance** (`GetGameInstance`) | `Ref` to the app-wide GameInstance. |

### Control flow
| Node | Purpose |
|------|---------|
| **Branch** (`Branch`) | `if` — True / False exec-outs from a Bool. |
| **Sequence** (`Sequence`) | Run several exec-outs in order. |
| **For Each** (`ForEach`) | Loop an array: `Body` (Element + Index) per element, then `Done`. Accepts any array type and re-types its pins to the connected array. The sanctioned way to reach elements of an object array. |
| **Delay** (`Delay`) | Latent: pause the chain, resume from `Completed` after `Duration` seconds (driven per frame by the runtime). Retriggering while pending is ignored. The continuation is a FRESH run — event args and cached exec outputs of the original run are gone; wire through variables instead. Loops back into a Delay are the sanctioned "timer loop". |
| **Do Once** (`DoOnce`) | Lets the chain through only the FIRST time per instance. Resets with the instance's variables (fresh play session). |
| **Flip Flop** (`FlipFlop`) | Alternates its `A` / `B` exec-outs (A first); the `Is A` data-out reports which side just ran. State persists per instance like Do Once. |

### Variables (typed, persistent per instance)
| Node | Purpose |
|------|---------|
| **Get Variable** (`GetVariable`) | Read a graph variable. Draws compact. |
| **Set Variable** (`SetVariable`) | Write a graph variable; passes the value through as a data-out. |

Variables can be a **single value** or an **array** of any type, and object-typed
variables show the class name. Arrays have a default-value slot editor.

### Literals (edited inline on the node body)
**Float**, **Bool** (checkbox), **Int**, **String** (grows then scrolls),
**Vec2**, **Color** (swatch), **Transform** (position/rotation/scale).
Simple unwired inputs (Bool/Int/Float/String) also show an **inline entry right on
the node** — no literal node needed.

### Math & logic
**Add**, **Subtract**, **Multiply**, **Divide**, **Greater**, **Less**,
**Equals**, **And**, **Or**, **Not**.

### Strings
**Concat**, **To String**. (Richer string ops live in the `String` subsystem — §3.)

### Arrays (pure, copy-semantics)
**Make Array**, **Array Length**, **Array Get**, **Array Append**, **Array Set**,
**Array Insert**, **Array Remove**, **Array Contains**, **Array Index Of**.
Array pins draw as a 2×2 grid to distinguish a list-of-T from a scalar T.

### Widgets
| Node | Purpose |
|------|---------|
| **Create Widget** (`CreateWidget`) | Instantiate a UI Widget asset by path → Widget id. |
| **Show Widget** / **Hide Widget** / **Destroy Widget** | Act on a widget by id. |
| **Show Self** / **Hide Self** | A widget graph shows/hides its own widget. |

### Objects, references & members
| Node | Purpose |
|------|---------|
| **Create Object** (`CreateObject`) | Instantiate a HorizonCode Class asset → `Ref`. Fires its `Construct`. |
| **Destroy Object** (`DestroyObject`) | Destroy a referenced object (fires `Destruct`). |
| **Call Function** (`FunctionCall`) | Call a function in this graph. |
| **Call Function (Ref)** (`CallExternal`) | Call a public function on another instance, passing typed args + returns. |
| **Get (Ref)** / **Set (Ref)** (`GetExternal`/`SetExternal`) | Read/write a public variable on a referenced instance. |
| **Get Property** / **Set Property** | Read/write a property on the graph's target element. |
| **Bind Event** (`BindEvent`) | Subscribe: when the target fires an event, this instance's matching Event fires. |
| **Emit Event** (`EmitEvent`) | Broadcast an event to everyone bound to this instance. |
| **Is Valid** (`IsValid`) | Bool: is the `Ref` a LIVE instance? The guard before touching an object that may have been destroyed (a dead Ref otherwise null-refs with an error log). |
| **Cast** (`Cast`) | Checked downcast, Unreal's Cast node. Exec-outs **Success** / **Failure**; the `As <Class>` output carries the same reference on success and 0 otherwise, so it is only meaningful on the Success branch. The target is picked from a dropdown: an **engine class** (§2.1) or one of the project's HC classes. A reference that is 0, destroyed, or of another class all take Failure — Cast therefore doubles as an Is Valid. Its input is an object `Ref`, not an any-type pin: only a reference names a runtime class (the same rule Unreal's object pin follows). |

### 2.1 Engine class taxonomy — what a class asset derives from

A HorizonCode class asset picks a **base class** in its tab header. The chain is

```
Object → Entity → { PlayerCharacter, PlayerController }
```

mirroring Unreal's `AActor → { AController, APawn }`. A base class is not a
label — it decides which lifecycle events the class's event catalog offers
(`Object` contributes Construct/Destruct, `Entity` adds BeginPlay/Tick, the two
player classes add the `Input.<Action>.*` set), and it is what a **Cast** to a
base class matches against. `Object` is stored as an empty string, which is what
every asset predating the taxonomy already carries, so nothing had to migrate.

A class may also derive from **another class asset**, not just from an engine
row — pick it in the same Base dropdown. That is what makes `Cast To Enemy`
succeed on a Goblin, and what lets a Goblin use everything Enemy defines.

Inheritance is resolved by **flattening**, once, when the class is loaded: the
ancestors' graphs are merged into one, nearest-wins, and the runtime then runs
a single ordinary graph. Everything downstream — the variable store, event
dispatch, Get/Set/Call External, the GC, the C++ codegen — is untouched,
because there is still exactly one graph and one object per instance.

- A derived **Event or Function** whose name the base also has **replaces**
  it. Only the override runs, for events exactly as for functions.
- A derived **variable** of the same name shadows the base's declaration:
  one name, one slot, the nearest declaration's type and default.
- A base member is only offered for overriding when it is marked
  **Overridable** on its Event / Function node — opt-in, like `virtual`. A
  derived class's add menu then lists them under *Inherited*; picking one
  drops in an override with the same name and signature.
- A derived **Entity** class starts from its parent's component list.
- A cycle (A derives from B derives from A) is logged and stops the walk
  rather than hanging; what resolved up to that point still runs.

A class from `Entity` down also has a **body**: its class tab has a *Components*
mode next to *Graph*, editing a real entity subtree with the same Details panel
the scene inspector uses. `EntityHost::spawn` instantiates that subtree, so a
spawned class arrives with its mesh, collider and whatever else it carries. A new
class starts from its base's default list — a PlayerCharacter comes with a
character controller, a capsule collider and an empty skeletal-mesh slot. The
list is stored in the same payload format as a prefab (`CHUNK_HCCP`), because a
class's component list and a prefab are the same idea.

A base class also brings **members**: drag off any reference of that type and the
menu shows an *Inherited* section — `Possess` / `Un Possess` /
`Get Possessed Character` on a PlayerController, `Get Controller` on a
PlayerCharacter, `Get Owning Entity` on anything below Entity. Picking one drops
an **Engine Call** already wired to that reference, so the member surface is
registry rows rather than machinery of its own — and Lua/Python/codegen get the
same functions for free.

**Player input.** The PlayerController is the engine's central point of contact:
every `Input.<Action>.*` event reaches **every** controller, always, whether or
not it possesses anything. A controller that possesses a PlayerCharacter also has
that same event delivered to the character — possessing makes the controller
forward input, it does not make it passive. A project with no PlayerController at
all keeps the older behaviour and gets input straight on its characters. Exactly
one controller and one character in a project → they are possessed
automatically; anything else is the game's call, in the controller's `BeginPlay`.

### Debug
**Print** (`Print`) — log a value.

### Engine Call — the universal subsystem node
**Engine Call** (`EngineCall`) routes to the `HE::api` registry (§3). One node type
exposes **every** engine subsystem without growing the node enum; its pins mirror the
chosen function's parameters/results, and it is an exec node (cached side-effect
outputs) or a pure data node depending on the function. The add-menu lists every
registry function under a readable name (e.g. *Set Position*, *Sine*, *Play Sound*).

---

## 3. Engine subsystems (the `HE::api` registry)

One descriptor registry (`EngineApi.cpp`) lights up **Engine Call** nodes **and** the
`horizon.<group>.<fn>` Lua/Python APIs simultaneously. **19 groups, 131 functions.**

| Group | # | Functions |
|-------|---|-----------|
| **Debug** | 5 | `log`, `debug.line`, `debug.sphere`, `debug.box`, `debug.clear` |
| **Entity** | 13 | `getName`, `spawn`, `destroy`, `distance`, `findByName`, `exists`, `self`, `owned`, `setVisible`, `getVisible`, `saveState`, `hasSavedState`, `applySavedState` |
| **Transform** | 6 | `getPosition`/`setPosition`, `getRotation`/`setRotation`, `getScale`/`setScale` |
| **Physics** | 3 | `raycast`, `setVelocity`, `isGrounded` |
| **Material** | 2 | `getParam`, `setParam` |
| **UI** | 11 | element access: `getText`/`setText`, `getColor`/`setColor`, `getVisible`/`setVisible`, `getPosition`/`setPosition`, `getSize`/`setSize`, `setMaterialParam` |
| **Widget** | 7 | `create`, `destroy`, `show`, `hide`, `setZOrder`, `isVisible`, `callFunction` |
| **Cursor** | 1 | `setVisible` |
| **Math** | 4 | `clamp`, `lerp`, `length`, `distance` (plus per-op nodes in §2) |
| **Random** | 5 | `seed`, `value`, `range`, `rangeInt`, `chance` |
| **Time** | 3 | `deltaTime`, `elapsed`, `frameCount` |
| **Player** | 6 | `possess`, `unpossess`, `possessed`, `controllerOf`, `controller`, `character` |
| **Input** | 5 | `keyDown`, `mouseButton`, `mousePosition`, `mouseDelta`, `scrollDelta` |
| **Camera** | 6 | `getPosition`/`setPosition`, `getRotation`/`setRotation`, `getFov`/`setFov` |
| **Environment** | 10 | `get/setTimeOfDay`, `get/setCloudCoverage`, `get/setFogDensity`, `get/setWindDirection`, `get/setWindSpeed` |
| **Audio** | 7 | `play`, `playAt`, `stop`, `stopAll`, `isPlaying`, `setBusVolume`, `setSoundPosition` |
| **String** | 11 | `length`, `substring`, `contains`, `find`, `replace`, `toUpper`, `toLower`, `trim`, `startsWith`, `endsWith`, `toNumber` |
| **File** (`fs`) | 5 | `writeText`, `readText`, `exists`, `remove`, `makeDir` — jailed to a per-user sandbox |
| **Save** | 17 | `create`, `load`, `write`, `close`, `activeId`, `list`, `exists`, `delete`, `fields`, `set/getNumber`, `set/getString`, `set/getBool`, `set/getStruct` |
| **Scene** | 12 | `load`, `loadAdditive`, `unloadZone`, `activate`, `hasPendingLevel`, `showZone`, `hideZone`, `zonePosition`, `setZonePosition`, `zoneScene`, `loadedZones`, `available` |

Notes:
- **Scene** enables seamless transitions: `load(path, hidden)` swaps the world only
  after the new one builds; `loadAdditive` streams a zone in at a position (hidden or
  visible), later toggled by `showZone`/`hideZone`/`setZonePosition`;
  `loadedZones`/`available` enumerate zones and shippable scenes. The `scene` input on
  `scene.load`/`scene.loadAdditive` is **picked from a dropdown** in the node inspector
  (project scenes by their project-relative path, e.g. `Content/123.hescene`) — that
  exact string is what the exporter packs the scene under and what the game resolves,
  so a hand-typed path can't silently miss.
- **File** is sandboxed to `<user pref>/Saved` (editor PIE: `<project>/Saved`);
  absolute paths and `..` are rejected.
- **Save** is a whole system, not a KV store: exactly ONE save document is active,
  shaped by a **SaveGame Template** asset (typed fields incl. Struct/Enum refs, with
  defaults; the project default is set on the template itself and ships in
  `project.hcfg`). `save.create(id)` seeds the fields from the template,
  `save.load(id)` reads `Saves/<id>.json` and re-validates against it, `save.write()`
  persists atomically; ids are `[A-Za-z0-9_-]+`. Field access is typed and validated —
  `get/setNumber` covers Float/Int/Enum fields, `get/setStruct` carries whole structs —
  and every miss (no active save, unknown field, wrong type) logs and returns the
  default, never silently. The `field` input on `save.*` nodes is **picked from a
  dropdown** of the template's fields, filtered by the accessor's type. `save.fields()`
  enumerates them at runtime. Entities opt in via the **Save State** component:
  `entity.saveState` stores the flagged attributes (transform, visibility) under the
  entity's stable UUID in the active save, `entity.applySavedState` re-applies what the
  save carries (partial by design) — play mode only. Native C++ GameLogic reaches the
  same API through `<HorizonGameServices.h>` (`he::save::*` / `he::entity::*`,
  injected after the library loads; struct fields cross as JSON).
- **User types**: Struct and Enum **assets** define project types once and light up
  everywhere — HorizonCode pins/variables (Make/Break Struct, Get/Set Struct Field,
  Enum Value, Switch on Enum, conversions; wires require the SAME definition; a
  struct variable can override individual field defaults per graph), Lua/Python
  (`horizon.enums.<Name>.<Entry>` ints and `horizon.structs.<Name>()` constructors;
  structs cross as tables/dicts with `__type`), generated C++
  (`Source/Generated/GameTypes.h`), and savegame-template fields. Array fields can
  carry authored starting elements; lists stay dynamic either way (Array
  Append/Insert/Remove at runtime). In Lua/Python a struct simply IS a table/dict —
  `stats.hp`, `stats.tags[1]` — so field access needs no API. Packed builds load the
  definitions eagerly from the pak's `__type_index__` before any script runs.
- `vec3` values ride in a `Color` value on the boundary (spread as 4 numbers in
  Lua/Python).

---

## 4. How a game ships (packaging)

The exporter packs the project into one `.hpak` (LZ4/zstd + optional AES-256-GCM) and
writes `project.hcfg`. Everything the shipped game needs is inside the pak:

- **Assets** — every `.hasset`, keyed by UUID, streamed on demand.
- **`__asset_index__`** — a `path → UUID` map so `loadAsset("<content path>")` resolves
  assets the scene's UUID reference closure never reaches (e.g. a widget a graph
  creates by path via **Create Widget**). Without it, such UI silently never appeared.
- **`__scene_index__`** + per-scene entries — every project scene as CBOR, so
  `scene.load("<path>")` and `scene.available()` work in the shipped game.
- **`__game_instance__`** — the packed `GameInstance.hcode`. The game runs its `OnInit`
  after the world + runtime services exist, so a game's UI is up from frame one.

> Two packaging bugs that broke shipped UI (fixed): the GameInstance graph was never
> shipped (so `OnInit` — and any UI it creates — never ran), and its `OnInit` fired
> before the world/services existed. Both now match the editor's proven order:
> services → world → `fireInit`, with the graph loaded from the pak.

---

## 5. Compiled HorizonCode (the C++ codegen)

With the export profile's **Compile HorizonCode** toggle on (Host platform only),
the export translates every graph — HC classes, widget scripts, level scripts and
the GameInstance — to native C++, builds them into `HorizonCodeGen.<dylib|dll|so>`
with the host toolchain (needs `cmake` on PATH), and ships that library beside the
game data. `project.hcfg` records `horizonCodeCompiled`.

- **The editor always interprets.** Compilation is an export-time step only; PIE
  behavior never changes.
- **Per-asset hybrid.** Graphs always ship in the pak too. At runtime every host
  (Create Object, widget create, level load, GameInstance) first consults the
  loaded `CompiledClassTable`; a miss — the class failed validation, the library
  is absent, or the engine version handshake rejected it — runs that one graph
  interpreted, exactly as before. Nothing ever breaks because of codegen.
- **Semantics are contract-tested.** A parity harness runs every fixture graph
  interpreted AND compiled against identical hosts and asserts equal traces
  (order/count/args/results), variable stores and function results — including
  the per-run exec-output cache under recursion, per-read dispatch of pure
  Engine Calls, and mixed compiled↔interpreted populations in one runtime
  (`tests/test_horizoncode_codegen.cpp`).
- **Compile check in the editor.** Every graph editor (widget Graph mode, Level
  Script, Game Instance, HC Class tab) has a **Compile** button in the canvas
  header: it runs the same translation the export would and either reports
  "compiles clean" or shows the reason and highlights the offending node with a
  red halo (e.g. an exec cycle, an unknown Engine Call id, a function-local read
  outside its function).
- **Reports.** The export result line shows "HorizonCode: N compiled, M
  interpreted (validation)"; the full per-class breakdown with reasons lands in
  `<output>/_hcgen/hc_report.txt`, the C++ build log in `_hcgen/build.log`.
- **Adding engine API functions costs nothing extra**: one `ApiFn` registry row
  serves the editor menu, the interpreter, Lua/Python AND the codegen (generated
  code dispatches through the same `callApi` seam).
- **Cast has two lowerings, chosen by the export mode.** With the hybrid
  (a fallback may ship interpreted) it goes through the `Context::isA` seam onto
  the same `Runtime::instanceIsA` the interpreter asks — anything cleverer would
  answer differently for an interpreted target. With **Stop on failure** every
  class is native by construction, so the compatibility layer drops: a HC class
  target becomes a `hc::as<T>` pointer comparison and an engine base class a
  `resolveCompiled` + `baseClassKey()` chain walk. The parity harness generates
  its fixtures in Stop mode, so the fast path is the one it proves.

Dispatch safety note: event cascades are bounded twice — recursion depth 32 and
a total budget of 256 listener fires per cascade. The budget exists because a
Bind/Emit **cycle** of re-emitting listeners would otherwise branch into ~2^32
fires (each fire spawns an emit dispatch AND the fired instance's own trailing
listener dispatch); exceeding it aborts the cascade with a "dispatch budget
exceeded" error in the log.

---

## 6. Editor shortcuts

The graph canvas is shared with the material / particle / animator editors, so the
navigation keys work in all of them; the node keys are HorizonCode's. Every
shortcut is ignored while a text field has the keyboard (a rename box, a search
field), and only fires while the cursor is over the canvas.

### Drop a node — hold the key, click empty canvas

| Key | Node | Key | Node |
|-----|------|-----|------|
| `B` | Branch   | `O` | Do Once |
| `S` | Sequence | `P` | Print |
| `D` | Delay    | `V` | Is Valid |
| `F` | For Each | `N` | Not |

`B`/`S`/`D`/`F`/`O` are Unreal's Blueprint bindings, key for key.

**The same keys work mid-wire.** Drag a link off any pin and hit the key instead of
releasing: the node appears at the cursor **already connected** (the drag-off menu's
result without the menu). If the node has no pin that fits what you dragged, it is
still created — unwired. Pressing a key while the drag-off *menu* is already open
types into its search box instead, which is what you want there.

Literals have no shortcut on purpose: an unwired simple data input (Bool/Int/Float/
String) edits its value right on the pin, so a literal node is the exception now.

### Palettes

| Key | Opens |
|-----|-------|
| `Space` | the add-node palette at the cursor (same as right-click), search focused |
| `G` + click | this graph's variables → **Get** node |
| `Shift`+`G` + click | this graph's variables → **Set** node |
| `E` + click | the engine API registry → **Engine Call** node |

All four are search fields: type a few letters, `↑`/`↓` to move the highlight,
`Enter` to insert.

### Selection, navigation, tidying

| Key | Does |
|-----|------|
| `Delete` | remove the selection (Backspace deliberately does **not** — it is the text-editing key) |
| `Ctrl`/`Cmd`+`A` | select every node in the visible graph or function body |
| `Home` | fit the whole graph in view |
| `F` (tap, no click) | frame the selection — nothing selected frames everything |
| `Q` | straighten the selection's wires: downstream nodes slide until each wire runs horizontally. With a single node selected, its neighbours move onto it instead |
| `Ctrl`/`Cmd`+`C`/`X`/`V`/`D` | copy / cut / paste / duplicate (the clipboard is shared across all HorizonCode editors) |
| `Alt`+click a pin | break that pin's links |

`F` carries both a node and a command: the click is what tells them apart — `F`
plus a click drops a For Each, `F` released without one frames the selection.

The bindings live in one table, `src/HE_Editor/HcGraphShortcuts.cpp`.

---

## 7. Related design docs

- `horizoncode-completion-plan.md` — feature roadmap / status tracker.
- `horizoncode-cpp-codegen-plan.md` — the codegen design (implemented; see below).
- `horizoncode-cpp-codegen-implementation-plan.md` — the implemented codegen's
  semantic contract, lowering tables, packaging + runtime integration (WP0–WP5).
- `material-system-design.md` — the shared node-graph frontend + cross-backend shader codegen.
- `hpak-format-plan.md` — the pak container format.
