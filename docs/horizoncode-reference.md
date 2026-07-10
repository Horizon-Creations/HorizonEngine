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
`horizon.<group>.<fn>` Lua/Python APIs simultaneously. **19 groups, 122 functions.**

| Group | # | Functions |
|-------|---|-----------|
| **Debug** | 5 | `log`, `debug.line`, `debug.sphere`, `debug.box`, `debug.clear` |
| **Entity** | 8 | `getName`, `spawn`, `destroy`, `distance`, `findByName`, `exists`, `setVisible`, `getVisible` |
| **Transform** | 6 | `getPosition`/`setPosition`, `getRotation`/`setRotation`, `getScale`/`setScale` |
| **Physics** | 3 | `raycast`, `setVelocity`, `isGrounded` |
| **Material** | 2 | `getParam`, `setParam` |
| **UI** | 11 | element access: `getText`/`setText`, `getColor`/`setColor`, `getVisible`/`setVisible`, `getPosition`/`setPosition`, `getSize`/`setSize`, `setMaterialParam` |
| **Widget** | 7 | `create`, `destroy`, `show`, `hide`, `setZOrder`, `isVisible`, `callFunction` |
| **Cursor** | 1 | `setVisible` |
| **Math** | 4 | `clamp`, `lerp`, `length`, `distance` (plus per-op nodes in §2) |
| **Random** | 5 | `seed`, `value`, `range`, `rangeInt`, `chance` |
| **Time** | 3 | `deltaTime`, `elapsed`, `frameCount` |
| **Input** | 5 | `keyDown`, `mouseButton`, `mousePosition`, `mouseDelta`, `scrollDelta` |
| **Camera** | 6 | `getPosition`/`setPosition`, `getRotation`/`setRotation`, `getFov`/`setFov` |
| **Environment** | 10 | `get/setTimeOfDay`, `get/setCloudCoverage`, `get/setFogDensity`, `get/setWindDirection`, `get/setWindSpeed` |
| **Audio** | 7 | `play`, `playAt`, `stop`, `stopAll`, `isPlaying`, `setBusVolume`, `setSoundPosition` |
| **String** | 11 | `length`, `substring`, `contains`, `find`, `replace`, `toUpper`, `toLower`, `trim`, `startsWith`, `endsWith`, `toNumber` |
| **File** (`fs`) | 5 | `writeText`, `readText`, `exists`, `remove`, `makeDir` — jailed to a per-user sandbox |
| **Save** | 11 | `set/getNumber`, `set/getString`, `set/getBool`, `hasKey`, `deleteKey`, `saveToSlot`, `loadFromSlot`, `slotExists` |
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
- **File**/**Save** are sandboxed to `<user pref>/Saved`; absolute paths and `..` are
  rejected.
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

## 5. Related design docs

- `horizoncode-completion-plan.md` — feature roadmap / status tracker.
- `horizoncode-cpp-codegen-plan.md` — planned HorizonCode → C++ codegen for shipped builds.
- `material-system-design.md` — the shared node-graph frontend + cross-backend shader codegen.
- `hpak-format-plan.md` — the pak container format.
