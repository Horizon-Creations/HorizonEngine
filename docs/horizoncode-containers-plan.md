# HorizonCode Set + Map container types

Set\<T\> and Map\<K,V\> become first-class HorizonCode types, the way Array,
Struct and Enum already are. A type in HorizonCode is never one change: it runs
through the interpreter, the C++ codegen for packaged builds, the Lua and Python
boundaries, the type registry with its serialization, and the editor's variable
and details UI. This file is the list of everything a new container has to
touch, written before the implementation so nothing is discovered halfway.

## 1. The two decisions that had to be made first

### 1.1 Representation: `isArray` widens to "is a container"

`Value`, `Variable`, `FuncParam`, `Node` and `PinDesc` each already carry a
`bool isArray`. Rather than a second, independent flag, `isArray` now means
**"this is a container"** and a new `ContainerKind container` says *which*:

| `isArray` | `container` | reads as |
|---|---|---|
| `false` | `None` | scalar |
| `true` | `None` | **Array** (every graph and asset authored before this change) |
| `true` | `Array` / `Set` / `Map` | that kind |

`containerKindOf(isArray, container)` is the single read path; it resolves the
legacy row. Every write site sets both fields together, and `fromJson` forces
`isArray = true` whenever a kind is present, so the inconsistent state is not
representable after a load.

Why not a clean `ContainerKind` replacing the bool: a site that has not learned
about the new kinds then falls into the **scalar** path and reads `v.f` off a
map — silent data loss. With the widened flag it falls into the container path
and sees the payload in `items`. Old JSON (`"arr": true`, no `"ctr"`) keeps
loading with no migration.

Payload, on `Value`:

* Array / Set → `items` holds the elements, `type` is the element type.
* Map → `items` holds the **values**, the new parallel `keys` holds the keys,
  `keyType` (+ `keyTypeName` for enum keys) types them. Code that iterates
  `items` without knowing about maps therefore still sees a list of values.

### 1.2 Iteration order: **insertion order**, for both backends

A Set iterates in the order elements were first added; a Map in the order keys
were first inserted. Three rules make that total:

1. Inserting a key a Map already has **updates the value in place** and keeps
   the key's original position.
2. Adding an element a Set already has is a **no-op** — it does not move to the
   back.
3. Remove erases and preserves the relative order of everything else.

Why insertion order and not sorted-by-key:

* It is the order the author wrote, which is the only order they can predict
  while looking at their graph.
* `Ref` (entity/object) keys have no meaningful sort — handles are reused, so
  "sorted" changes between runs while insertion order does not.
* Both backends are vector-backed (`Value::items` in the interpreter,
  `hc::Set`/`hc::Map` in generated C++), so parity is *structural* rather than
  something two implementations have to be careful about.
* Persistence is free: the serialized list **is** the order.
* Python `dict` is insertion-ordered natively, so the Python boundary is exact.

The cost, accepted: two maps with the same content built in a different order
are not equal under iteration, and a save round-trip has to preserve the list
order (it does — parallel `keys`/`items` arrays).

`std::unordered_map` is banned from the generated code for this reason; so is a
JSON **object** for a map payload, because nlohmann's default object is a sorted
`std::map` and would silently alphabetize the keys.

### 1.3 Legal key types

`Int`, `String`, `Enum`, `Ref`. Nothing else.

* `Float` is out: `0.1 + 0.2` as a key is a bug generator, and equality on
  floats is not an equivalence anyone wants keyed data to depend on.
* `Bool` is out because a two-slot map is a struct.
* `Vec*`, `Color`, `Transform`, `Struct` are out: composite equality invites the
  same float problem and has no cheap identity.

`Enum` keys persist as entry **names** (the existing convention for enum
values — renumber-safe). A name the definition no longer has **drops the pair**
on load rather than falling back to the first entry, because a fallback would
silently merge two keys into one.

## 2. Touch list

### Layer 1 — core model, interpreter, graph JSON (`HE_Core`)

* `HorizonCode.h`
  * `enum class ContainerKind`, `containerKindOf()`.
  * `Value`: `container`, `keys`, `keyType`, `keyTypeName`.
  * `FuncParam`, `Variable`, `Node`, `PinDesc`: `container`, `keyType`,
    `keyTypeName`.
  * `Variable`: `defaultKeys` next to `defaultItems`.
  * New `NodeType`s, appended last (never inserted — persisted ints are stable):
    * Set: `SetMake`, `SetAdd`, `SetRemove`, `SetContains`, `SetLength`,
      `SetClear`, `SetToArray`, `ForEachSet`.
    * Map: `MapMake`, `MapSet`, `MapRemove`, `MapContains`, `MapLength`,
      `MapClear`, `MapGet` (value + default), `MapKeys`, `MapValues`,
      `ForEachMap`.
* `HorizonCode.cpp`
  * `signatureOf`, `signatureCountsOf`, `dataPinDescOf` for all of them.
  * `nodeDisplayName`, `nodeCategory`, `nodeTooltip`, `nodeRegistry`.
  * `Graph::connect`: container kind must match, and for maps the key type
    (and enum key definition) too.
  * `Runner::evalData` / `execNode`: every node above; `ForEachSet` /
    `ForEachMap` in the exec path next to `ForEach`.
  * `coerce`: containers pass through untouched (already true via `isArray`).
  * Unwired container inputs evaluate to an **empty container of the declared
    kind**, not scalar zero.
  * JSON: `"ctr"`, `"kt"`, `"ktn"` on node/param/variable; `"keys"` next to
    `"items"` for seeded defaults; `fromJson` normalizes `isArray`.
  * `variableDefaultValue`: seed Set/Map from the authored default slots.
  * `inferUserTypeNames`, `syncTypeSignatures`, link-remap: new nodes listed
    wherever the Array nodes are.
* `TypeRegistry.h/.cpp`: `StructField` gains the same container fields;
  `makeDefaultValue`, `structToJson`, `structFromJson` branch on kind.

### Layer 2 — C++ codegen (`HE_Scene`, `HE_Core` gen support)

* `HorizonCodeGenSupport.h`: `hc::Set<T>` and `hc::Map<K,V>` — insertion-ordered,
  vector-backed, with the three ordering rules above; `toValue`/`raw`/`coerce`/
  `tagOf` overloads; `VarSlot` support.
* `HcCodegen.cpp`: `TypeRef` gains kind + key; `cppType`, `zeroLit`,
  `convertExpr`, `defaultLiteral`, the node lowering for every new node, and the
  `VarSlot` emission for Set/Map variables.
* `CppTypesHeaderGen` (`HE_Tools`): struct fields of container kind emit
  `hc::Set`/`hc::Map`.

### Layer 3 — script boundaries

* Lua (`ScriptContext.cpp`): Set ⇄ ordered array table; Map ⇄ table with
  `key → value` plus a `__keys` array carrying the order. A table arriving
  without `__keys` gets its keys collected and **sorted by their string form**,
  so the boundary is deterministic even when a script built the table by hand.
* Python (`PyScriptBackend.cpp`): Set ⇄ `list` (ordered; duplicates collapse to
  first occurrence when it comes back), Map ⇄ `dict` (insertion-ordered by the
  language, so exact).

### Layer 4 — savegames

Save fields are a `StructDef` (`SaveGameTemplateAsset` carries CHUNK_STDF JSON),
so container fields fall out of Layer 1 — but `save.*`'s field validation
compares the container kind, not only the `PinType`.

### Layer 5 — editor UI

* `HcEditorUtil`: pin label/colour, container-kind dropdown, key-type dropdown.
* `HcGraphHost`: variable declaration UI, default editors, pin rendering.
* `LevelScriptPanel`, `UIEditorPanel`: the variable rows.
* `TypeAssetPanel`: struct-field container + key type.

### Layer 6 — verification

* `test_horizoncode_types.cpp` — interpreter semantics per node, ordering rules.
* `test_type_registry.cpp` — struct field round-trip.
* `test_scripting_binding.cpp` / `test_python_scripting.cpp` — one per boundary.
* `test_horizoncode_codegen.cpp` + `tests/fixtures/hcodegen/fixtures.h` — the
  equivalence harness. `valueStr`/`valueEq` must learn `container`, `keys` and
  `keyType` first, otherwise two divergent maps compare equal and the parity
  claim is fiction. Fixtures: one per ordering rule, one seeded-Map variable
  default, one "length of an unwired Map is 0".

## 3. Deliberately not built

* **The editor's authored-default editor for a MAP.** A Set variable and a Set
  struct field get the same slot list an Array has; a Map declares its key and
  value types and starts **empty**, filled by `Map Set`. Everything underneath
  is finished — `Variable::defaultKeys` and `StructField::defaultValue.keys`
  persist, round-trip through JSON, seed an instance and lower to a C++ literal,
  and there are tests for all four — so this is a two-column slot widget and
  nothing more. It was cut rather than half-built: a pair editor needs its own
  key-uniqueness handling and reordering, and the rest of the feature is worth
  more finished than that widget is.

* **Directly nested containers** (`Set<Array<T>>`, `Map<K, Set<V>>`). A pin
  carries one element type and one container kind, so a nested container is not
  expressible without a second type system on every pin. Nesting goes through a
  **Struct**: a struct field may itself be an Array/Set/Map, and a
  `Map<string, Stats>` reaches it.
* **Set/Map in the `HE::api` engine registry.** No engine function takes or
  returns one, and `ApiFn` descriptors stay array-or-scalar.
