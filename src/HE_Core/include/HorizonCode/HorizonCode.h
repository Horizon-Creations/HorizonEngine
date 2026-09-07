#pragma once
#include <cstdint>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

// ── HorizonCode ──────────────────────────────────────────────────────────────
// HorizonCode is the engine's visual-scripting system: a Blueprint-style node
// graph + interpreter. It is deliberately decoupled from any particular target —
// the host binds a Context (property get/set + host actions) so the same VM can
// later drive things other than UI widgets. Today the UI Widget editor authors
// a HorizonCode graph and WidgetManager runs it against a live widget.
//
// Graph shape: event nodes fire from the host (pointer input, tick, …); exec
// links (white) drive control flow; typed data links feed values evaluated on
// demand. Functions carry an access modifier — public functions are callable
// from gameplay scripts, private ones are graph-internal.

namespace HorizonCode {

// Defined in HorizonCodeCompiled.h — Context only ever passes the pointer on.
class CompiledInstance;

// Ref = a reference/handle to another running script instance (a Runtime
// InstanceId). Transform = a position/rotation(euler)/scale bundle. Enum/Struct
// = user-defined types from HE::TypeRegistry — a pin/value of these types names
// its definition asset in `typeName`. Appended last so existing serialized
// propType ints stay stable.
// Vec3/Vec4 are the VECTOR types — positions, velocities, directions. They exist
// separately from Color because the two mean different things and want different
// empties: a vector's zero is the null vector, a colour's is opaque black
// (alpha 1). Appended last, like everything before them.
//
// Color used to double as vec3 AND vec4 for every engine API. Those rows have
// moved to Vec3, and {Vec3, Vec4, Color} interconvert — so a graph authored
// before the split keeps its wires and coerce does the work. Vec2 stays outside
// that set: nothing ever converted into it and adding it would only widen the
// rules that two coerce implementations have to agree on.
enum class PinType : uint8_t { Exec = 0, Float, Bool, Int, String, Vec2, Color, Ref, Transform,
                               Enum, Struct, Vec3, Vec4 };

// ── Containers ───────────────────────────────────────────────────────────────
// A pin/variable/value is a scalar or a CONTAINER of its type. `isArray` is the
// "is it a container at all" flag — it predates Set/Map and every graph and
// asset on disk written before them carries it alone — and `container` says
// WHICH kind. The legacy row is `isArray == true, container == None`, which
// reads as Array; containerKindOf() is the single place that resolves it, and
// fromJson forces `isArray` true whenever a kind is present, so the
// inconsistent state is not representable after a load.
//
// Widening the existing flag rather than replacing it is deliberate: a site
// that has not learned about Set/Map still takes the CONTAINER path and sees
// the payload in `items`, instead of the scalar path where it would read `f`
// off a map and lose the data silently.
//
// ITERATION ORDER IS INSERTION ORDER, for sets and maps, in the interpreter and
// in generated C++ alike (docs/horizoncode-containers-plan.md §1.2). Re-adding
// an element a set already has is a no-op; re-inserting a key a map already has
// updates the value IN PLACE and keeps the key's position; removal preserves
// the relative order of the rest. Both backends are vector-backed, so this is
// structural rather than a rule two implementations have to remember —
// std::unordered_map (and a JSON object, whose nlohmann default is a sorted
// std::map) would break it.
enum class ContainerKind : uint8_t { None = 0, Array, Set, Map };

inline ContainerKind containerKindOf(bool isArray, ContainerKind c)
{
    if (c != ContainerKind::None) return c;
    return isArray ? ContainerKind::Array : ContainerKind::None;
}

// May `t` key a Map? Int, String, Enum and Ref — types with a cheap, exact
// identity. Float is out (equality on floats is not an equivalence anyone wants
// keyed data to rest on), Bool is out (a two-slot map is a struct), and the
// composites are out for both reasons at once.
inline bool isValidMapKeyType(PinType t)
{
    return t == PinType::Int || t == PinType::String ||
           t == PinType::Enum || t == PinType::Ref;
}

struct Value
{
    PinType     type = PinType::Float;
    float       f = 0.0f;
    bool        b = false;
    int         i = 0;
    glm::vec2   v2{ 0.0f };
    glm::vec4   col{ 0.0f, 0.0f, 0.0f, 1.0f };
    // Vectors keep their OWN storage rather than borrowing col. Sharing it would
    // hand a default-constructed Vec4 the alpha-1 of a colour, and "the empty
    // vector is (0,0,0,1)" is a bug waiting for someone to read w. This zero has
    // to match in four places: here, coerce's typed zero, the codegen's zeroLit,
    // and the coerce duplicate in HorizonCodeGenSupport.h.
    glm::vec3   v3{ 0.0f };
    glm::vec4   v4{ 0.0f };
    std::string s;
    uint32_t    ref = 0;   // instance handle when type == Ref (0 = none)
    // Transform payload (type == Transform): rotation in euler degrees, identity scale.
    glm::vec3   tpos{ 0.0f }, trot{ 0.0f }, tscl{ 1.0f };
    // Container payload: when isArray, `type` is the ELEMENT type and `items`
    // holds the elements (each a scalar Value of `type`). A container is never
    // scalar-coerced. For a MAP `items` holds the VALUES and the parallel
    // `keys` holds the keys — that way anything iterating `items` without
    // knowing about maps still sees a plain list of values.
    bool               isArray = false;
    ContainerKind      container = ContainerKind::None;
    std::vector<Value> items;
    std::vector<Value> keys;                       // Map only, parallel to items
    PinType            keyType = PinType::String;  // Map only: the key's type
    std::string        keyTypeName;                // Map with Enum keys: the definition asset
    // User-defined types (type == Enum/Struct): the definition asset's
    // project-relative path (HE::TypeRegistry key). Enum values ride in `i`;
    // a scalar Struct value holds its field values in `items` in DEFINITION
    // ORDER (resolved against the def via typeName — never keyed by position in
    // persisted JSON, see TypeRegistry). An array of structs nests normally:
    // isArray + items = struct elements.
    std::string        typeName;

    static Value ofFloat(float v)            { Value r; r.type = PinType::Float;  r.f = v;  return r; }
    static Value ofBool(bool v)              { Value r; r.type = PinType::Bool;   r.b = v;  return r; }
    static Value ofInt(int v)                { Value r; r.type = PinType::Int;    r.i = v;  return r; }
    static Value ofString(std::string v)     { Value r; r.type = PinType::String; r.s = std::move(v); return r; }
    static Value ofVec2(const glm::vec2& v)  { Value r; r.type = PinType::Vec2;   r.v2 = v; return r; }
    static Value ofColor(const glm::vec4& v) { Value r; r.type = PinType::Color;  r.col = v; return r; }
    static Value ofVec3(const glm::vec3& v)  { Value r; r.type = PinType::Vec3;   r.v3 = v; return r; }
    static Value ofVec4(const glm::vec4& v)  { Value r; r.type = PinType::Vec4;   r.v4 = v; return r; }
    static Value ofRef(uint32_t id)          { Value r; r.type = PinType::Ref;    r.ref = id; return r; }
    static Value ofTransform(const glm::vec3& p, const glm::vec3& r_, const glm::vec3& s_)
    { Value r; r.type = PinType::Transform; r.tpos = p; r.trot = r_; r.tscl = s_; return r; }

    // Empty containers. Both flags are always set together — see ContainerKind.
    static Value ofArray(PinType elem, std::string tn = {})
    { Value r; r.isArray = true; r.container = ContainerKind::Array; r.type = elem;
      r.typeName = std::move(tn); return r; }
    static Value ofSet(PinType elem, std::string tn = {})
    { Value r; r.isArray = true; r.container = ContainerKind::Set; r.type = elem;
      r.typeName = std::move(tn); return r; }
    static Value ofMap(PinType key, PinType val, std::string keyTn = {}, std::string tn = {})
    { Value r; r.isArray = true; r.container = ContainerKind::Map; r.type = val;
      r.keyType = key; r.keyTypeName = std::move(keyTn); r.typeName = std::move(tn); return r; }

    ContainerKind kind() const { return containerKindOf(isArray, container); }
};

enum class NodeType : uint8_t
{
    // Host-fired entry points.
    Event = 0,      // s = event name, elem = target (0 = any); optional arg out
    FunctionEntry,  // s = function name; access; params = inputs (one data-out each)
    // Control flow.
    Branch, Sequence,
    FunctionCall,   // s = function name; params → data-ins, results → data-outs
    // Target property access (elem + s = property name + propType = value type).
    GetProperty, SetProperty,
    // Host actions on the running widget itself ("self").
    ShowSelf, HideSelf,
    // Create + manage widgets by id (from any graph — level, GameInstance, …).
    CreateWidget,    // s = widget asset path; dataOut Widget (Int id)
    ShowWidget, HideWidget, DestroyWidget, // dataIn Widget (Int id)
    // Instantiate a HorizonCode class asset as a live runtime object.
    // s = class asset path; dataIn Location (Vec3) + Rotation (Vec3, euler deg);
    // dataOut Object (Ref). Both inputs are OPTIONAL and read by WIRE, not by
    // value: an unwired pin spawns the object exactly where the class authored
    // it, which is what every graph written before these pins existed expects.
    CreateObject,
    DestroyObject,   // dataIn Object (Ref)
    // Read/write a PUBLIC variable on a referenced instance (s = variable name).
    GetExternal,     // dataIn Target (Ref); dataOut Value (propType)
    SetExternal,     // dataIn Target (Ref) + Value (propType)
    // Literals (f[]/s).
    ConstFloat, ConstBool, ConstInt, ConstString, ConstVec2, ConstColor,
    // Math / logic.
    Add, Subtract, Multiply, Divide,
    Greater, Less, Equals, And, Or, Not,
    // Strings.
    Concat, ToString,
    // Graph variables (persistent per-instance state; s = variable name).
    GetVariable, SetVariable,
    // Reference-based delegation across script instances (s = event/function
    // name; a Ref data input picks the target instance).
    BindEvent,       // subscribe: when Target fires event s, this instance's Event s fires
    EmitEvent,       // broadcast event s to everyone bound to this instance (optional arg)
    CallExternal,    // call public function s on the Target instance
    GetGameInstance, // Ref to the app-wide GameInstance
    GetSelf,         // Ref to this instance
    // Debug.
    Print,
    // Writes the owning function's return values (s = function name); one data-in
    // per declared result. Terminal in the exec chain (no exec-out).
    FunctionReturn,
    // Generic engine-API call routed through the HE::api registry (the one node
    // that exposes every engine subsystem without growing this enum). s = the
    // registry id (e.g. "transform.setPosition"); params/results mirror the
    // ApiFn descriptor (so pins resolve without the registry, which lives a layer
    // up); hasArg carries the descriptor's isExec (true → exec node with cached
    // side-effect outputs, false → pure data node re-evaluated on demand).
    EngineCall,
    // Transform literal: editable position/rotation/scale on its body; one data-out
    // (Transform). Payload lives in the Node's tpos/trot/tscl.
    ConstTransform,
    // Array operations (pure). propType = element type; the array pins are marked
    // isArray in signatureOf. Make → empty array; Length → element count; Get →
    // element at index; Add → a copy of the array with a value appended.
    ArrayMake, ArrayLength, ArrayGet, ArrayAdd,
    // More pure array ops (all copy semantics, like Add): Set/Insert/Remove by
    // index; Contains/IndexOf search by value (element-type comparison).
    ArraySet, ArrayInsert, ArrayRemove, ArrayContains, ArrayIndexOf,
    // Loop over an array: exec-outs Body (once per element, with Element + Index
    // data-outs) then Done. The one sanctioned way to reach members of an
    // object array's elements — the element pin is a scalar Ref.
    ForEach,
    // Latent flow: pause the exec chain, resume from this node's exec-out after
    // Duration seconds (driven by Runtime::update). Retriggering while already
    // pending is ignored (like Unreal's Delay). The resumed chain is a FRESH
    // run: the event arg and exec-output caches of the original run are gone.
    Delay,
    // Is the Ref a live instance? (pure; dataIn Target, dataOut Bool). The
    // guard to run before touching an object that may have been destroyed.
    IsValid,
    // Stateful flow (per-instance node state, persists across runs; reset by
    // reseedVariables): DoOnce lets the chain through only the FIRST time;
    // FlipFlop alternates its A/B exec-outs (IsA data-out = which one just ran).
    DoOnce, FlipFlop,
    // User-defined types (HE::TypeRegistry; the node's typeName names the
    // definition asset). Make/Break/SetField mirror the struct's fields into
    // `params` (like EngineCall mirrors the ApiFn) so pins resolve without the
    // registry; the interpreter resolves fields BY NAME against the current def
    // so a def edit can't silently shift data. All pure, copy semantics.
    MakeStruct,       // one data-in per field → Struct out
    BreakStruct,      // Struct in → one data-out per field
    GetStructField,   // Struct in → ONE field out (params[0] = the field)
    SetStructField,   // Struct + Value in → updated copy out (params[0] = the field)
    ConstEnum,        // enum literal; entry VALUE in f[0], dropdown on the body
    // Exec switch: one exec-out per entry (params mirror the entry names) plus
    // a trailing Default for values no entry claims.
    SwitchOnEnum,
    EnumToInt, IntToEnum, EnumToString,   // conversions (pure)
    // Checked downcast of an object reference (Unreal's Cast node). s = the
    // TARGET class: either an engine class name from engineClasses() ("Entity",
    // "PlayerCharacter", …) or a HorizonCode class asset's content-relative
    // path. The two never collide — an asset path always carries a '/' and a
    // '.hasset' suffix — and the engine names are checked first.
    //
    // Exec-outs are Success / Failure; the single data-out carries the same
    // reference on success and 0 on failure, cached per run like every other
    // exec node's output. The data-IN is a Ref and NOT a wildcard: only a
    // reference can name a runtime class at all, and a wildcard pin has no C++
    // type for the codegen to lower to (it would have to become hc::Value and
    // drag a second type system into the generated code).
    Cast,
    // ── Input Action ─────────────────────────────────────────────────────────
    // One node per InputAction asset, entered when that action fires. `s` is the
    // action's logical name (its asset stem — what the mapping context and
    // PlayerHost key on), so a node is bound to an action the way a Cast is
    // bound to a class.
    //
    // A digital action gets TWO exec-outs, Pressed and Released, instead of the
    // two separate Event nodes it used to take: the pair belongs to one action
    // and reads as one thing. An AXIS action has no press or release — it
    // carries a value every frame — so it gets one exec-out and a Float
    // data-out instead, selected by `hasArg` exactly as an Event node's
    // argument is.
    //
    // The WIRE format is unchanged: the host still fires
    // "Input.<name>.Pressed"/".Released"/".Axis" (see Application/InputAssets.h),
    // and this node is matched against those names. That is what keeps
    // PlayerHost, Lua, Python and every graph authored before this node working
    // without knowing it exists.
    InputAction,

    // ── Vector assembly ──────────────────────────────────────────────────────
    // Build a vector pin from separate floats, and take one apart again.
    // Without these a graph could not produce a vector AT ALL: the only vector
    // literals are ConstVec2 and ConstColor, and ConstColor is edited through a
    // colour picker clamped to 0..1 — so a computed velocity or position was
    // simply not expressible, while Lua got the same API as three plain floats.
    //
    // Each width produces its own pin type: Vec2, Vec3, Vec4. They started out
    // all riding on Color — which is also four floats — and that is why
    // {Vec3, Vec4, Color} still interconvert, so a graph authored back then
    // keeps working. A colour is now a colour: for one WITH alpha, use Color.
    MakeVector2, MakeVector3, MakeVector4,
    BreakVector2, BreakVector3, BreakVector4,

    // ── Set<T> ───────────────────────────────────────────────────────────────
    // A collection with no duplicates. `propType` is the element type (+
    // `typeName` for Enum/Struct elements), exactly like the Array nodes; the
    // container pins carry ContainerKind::Set.
    //
    // Pure, copy semantics, like Array: each op returns a NEW set rather than
    // mutating the one it was handed, so a graph's data flow stays a data flow.
    // ITERATION IS INSERTION ORDER — Set Add of an element the set already has
    // is a no-op that does NOT move it to the back, and Set Remove keeps the
    // relative order of the rest.
    SetMake,      // → empty Set of the element type
    SetAdd,       // Set + Value → Set with the value (no-op if present)
    SetRemove,    // Set + Value → Set without it (no-op if absent)
    SetContains,  // Set + Value → Bool
    SetLength,    // Set → Int
    SetClear,     // Set → empty Set of the same element type
    SetToArray,   // Set → Array of the elements, in iteration order
    ForEachSet,   // exec: Body per element (Element + Index), then Done

    // ── Map<K,V> ─────────────────────────────────────────────────────────────
    // `propType`/`typeName` are the VALUE type; the key type rides in the
    // node's `keyType`/`keyTypeName` (Int, String, Enum or Ref — see
    // isValidMapKeyType). Pure and copy-semantic like the Set nodes.
    //
    // ITERATION IS INSERTION ORDER — Map Set on a key the map already has
    // updates the value IN PLACE and keeps the key's position, and Map Remove
    // keeps the relative order of the rest.
    MapMake,      // → empty Map
    MapSet,       // Map + Key + Value → Map (insert or update in place)
    MapRemove,    // Map + Key → Map without that pair
    MapContains,  // Map + Key → Bool
    MapLength,    // Map → Int
    MapClear,     // Map → empty Map of the same key/value types
    MapGet,       // Map + Key + Default → Value (the default when absent)
    MapKeys,      // Map → Array of keys, in iteration order
    MapValues,    // Map → Array of values, in the SAME order as MapKeys
    ForEachMap,   // exec: Body per pair (Key + Value + Index), then Done

    // ── The way BACK, and the set algebra ────────────────────────────────────
    // Appended at the END rather than slotted into the two blocks above: a
    // NodeType's integer position is not something anything should have to
    // depend on, and appending keeps that true without anyone having to prove
    // it. (Same rule PinType's tail carries.) The pin cases still live inside
    // the Set/Map regions of signatureInto, where the pin macros are.
    //
    // Every one is PURE and copy-semantic like the container nodes it joins,
    // and every one states an ORDER — with insertion-ordered containers a
    // result order is a promise, not an implementation detail.
    SetFromArray,     // Array<T> → Set<T>; duplicates collapse to the FIRST
    SetUnion,         // A ∪ B: A's order, then B's elements A did not have
    SetIntersect,     // A ∩ B: A's order, keeping only what B also has
    SetDifference,    // A \ B: A's order, dropping everything B has
    MapFromArrays,    // Array<K> + Array<V> paired by INDEX → Map<K,V>
    MapBreak,         // Map → Array<K> Keys + Array<V> Values (index-parallel)
    MapFindByValue,   // Map + V → the FIRST key holding it, + Found
    MapRemoveByValue, // Map + V → Map without EVERY pair holding it

    // ── A property of an element of a REFERENCED widget ──────────────────────
    // Get/Set Property reach an element of the widget the graph belongs to, by
    // the id it has in that asset. These reach one through a REFERENCE, the way
    // Get (Ref) / Set (Ref) reach another instance's variables: dataIn Target
    // (Ref) + Element (a NAME), `s` = the property, `propType` = its type.
    //
    // By name and not by id, because an id is the asset's private business and
    // a reference points at a widget this graph did not author. That is what
    // makes them the pair you can write once and point anywhere: a function
    // that takes a widget and fades whatever is called "Panel" inside it.
    GetPropertyOn, SetPropertyOn,

    COUNT
};

// One typed input or output of a HorizonCode function. The FunctionEntry owns
// the interface (params + results); FunctionCall / FunctionReturn mirror it so
// their pins resolve without a graph lookup (kept in sync by the editor and on
// load via syncFunctionSignatures).
struct FuncParam
{
    std::string name;
    PinType     type = PinType::Float;
    bool        isArray = false;   // the pin carries a CONTAINER of `type`
    // Enum/Struct params: the definition asset's path (HE::TypeRegistry key).
    std::string typeName;
    // Which container (see ContainerKind): None + isArray reads as Array, which
    // is every signature written before Set/Map existed. For a Map, `type` is
    // the VALUE type and these name the key.
    ContainerKind container = ContainerKind::None;
    PinType       keyType = PinType::String;
    std::string   keyTypeName;

    ContainerKind kind() const { return containerKindOf(isArray, container); }
};

// A user-defined graph variable: named, typed, persistent per running instance.
// The default seeds the instance's variable store; Get/SetVariable nodes read
// and write it. The default value lives in f[]/s like a literal node.
struct Variable
{
    std::string name;
    PinType     type = PinType::Float;
    bool        isArray = false;   // when true the variable holds a CONTAINER of `type`
    float       f[4] = {};
    std::string s;
    // Transform default (type == Transform): rotation in euler degrees, identity scale.
    glm::vec3   tpos{ 0.0f }, trot{ 0.0f }, tscl{ 1.0f };
    // Container default (isArray): the editor-authored slots that seed the
    // instance's container on creation. Each item is a scalar Value of `type`;
    // for a MAP `defaultKeys` is parallel to it and holds the keys.
    std::vector<Value> defaultItems;
    std::vector<Value> defaultKeys;
    // Which container (see ContainerKind); for a Map, `type` is the VALUE type
    // and these name the key. None + isArray reads as Array — every variable
    // declared before Set/Map existed.
    ContainerKind container = ContainerKind::None;
    PinType       keyType = PinType::String;
    std::string   keyTypeName;
    int         access = 0;   // 0 public (readable via a reference), 1 private
    // Scope: 0 = instance variable (persistent per running instance, seeded by
    // the Runtime — today's behavior). Non-zero = FUNCTION-LOCAL: the id of the
    // owning FunctionEntry node (matching Node::subgraph). Locals live in the
    // interpreter's call frame — fresh per invocation, never in the instance
    // store, never visible to Get/SetExternal or the public class interface.
    // Variable names stay unique across the whole graph (no shadowing).
    int         scope = 0;
    // For an Object (Ref) variable: which HorizonCode class it holds (asset
    // path). Purely editor metadata — lets the context menu surface that class's
    // public functions/variables. Empty = untyped object.
    std::string className;
    // Enum/Struct variables: the definition asset's path (HE::TypeRegistry key).
    // An Enum variable's default is the entry NAME in `s` (renumber-safe); a
    // Struct variable seeds from the definition's own field defaults, which
    // structDefaults then overrides PER GRAPH.
    std::string typeName;
    // Struct variables: per-graph overrides of individual field defaults, keyed
    // by FIELD NAME (never by position — a field inserted into the definition
    // must not silently shift what this graph authored; same contract as the
    // TypeRegistry's own name-keyed persistence). Resolved at seed time in
    // variableDefaultValue: the definition's defaults first, then these on top;
    // a name the definition no longer has simply doesn't apply.
    std::unordered_map<std::string, Value> structDefaults;

    ContainerKind kind() const { return containerKindOf(isArray, container); }
};

struct Node
{
    int         id = 0;
    NodeType    type = NodeType::Event;
    int         elem = 0;                 // target element id (Event/Get/Set)
    std::string s;                        // event/function/property name or string literal
    PinType     propType = PinType::Float;// value type for Get/Set; arg type for Event
    bool        hasArg = false;           // Event carries a data arg output
    int         access = 0;               // FunctionEntry: 0 public, 1 private
    // Event / FunctionEntry: may a class DERIVED from this one replace it?
    // C++'s `virtual`, and opt-in for the same reason: a base author decides
    // what is meant to be replaced. A derived class's add menu lists its
    // ancestors' overridable members; inserting one drops an override into
    // the child's own graph, and from then on ONLY the override runs — for
    // events exactly as for functions.
    bool        overridable = false;
    float       f[4] = {};                // literal payload
    // ConstTransform literal payload (rotation in euler degrees, identity scale).
    glm::vec3   tpos{ 0.0f }, trot{ 0.0f }, tscl{ 1.0f };
    // Get/SetVariable: whether the bound variable is an array (mirrors it so the
    // pins resolve). Array-op nodes (Make/Length/Get/Add): propType is the element
    // type; the array pins are marked in signatureOf.
    bool        isArray = false;
    float       x = 0.0f, y = 0.0f;       // editor canvas position
    // Which sub-graph this node lives in: 0 = the main event graph, else the id
    // of the owning FunctionEntry (that function's own body sub-graph). Editor
    // scoping only — the interpreter follows exec links regardless.
    int         subgraph = 0;
    // Function interface. FunctionEntry: params = inputs (data-outs). FunctionCall
    // mirrors both (params = data-ins, results = data-outs). FunctionReturn mirrors
    // results (data-ins). Empty on every other node type.
    std::vector<FuncParam> params;
    std::vector<FuncParam> results;
    // Inline pin defaults: editor-authored constants for UNWIRED simple data
    // inputs (Bool/Int/Float/String), keyed by DATA-IN INDEX (stable across the
    // exec-pin prefix). A wired pin ignores its default; evalInput falls back to
    // it before the type's zero. Spares a literal node per constant.
    std::unordered_map<int, Value> pinDefaults;
    // Struct/Enum nodes (Make/Break/SetField/ConstEnum/SwitchOnEnum/IntToEnum…):
    // the definition asset's path (HE::TypeRegistry key).
    std::string typeName;
    // CallExternal / Get- + SetExternal: which HorizonCode class the Target is
    // expected to be (asset path). Purely EDITOR metadata, exactly like
    // Variable::className — it fills the member picker and lets a rename find the
    // nodes that meant this class. The runtime never reads it: a call still
    // resolves by name on whatever instance the Ref actually carries, so a
    // recorded class that drifts from the real one costs a stale dropdown and
    // nothing else. Empty = never picked (or authored before the pickers existed),
    // and then the rename falls back to reading the Target's wire.
    std::string className;
    // Which container the node's container pins carry (see ContainerKind).
    // Get/SetVariable mirror their variable's; the Array nodes leave it None
    // (which reads as Array) so nothing authored before Set/Map moves; the
    // Set/Map nodes set it explicitly. `keyType`/`keyTypeName` type a Map node's
    // KEY — its `propType`/`typeName` are the value type.
    ContainerKind container = ContainerKind::None;
    PinType       keyType = PinType::String;
    std::string   keyTypeName;

    ContainerKind kind() const { return containerKindOf(isArray, container); }
};

// Links connect unified pin indices (see pin ranges below).
struct Link { int srcNode = 0, srcPin = 0, dstNode = 0, dstPin = 0; };

// Pin metadata for one node instance (variable-pin nodes like Event/Get/Set
// depend on the node's own fields, so this is computed per node, not per type).
// typeName: the Enum/Struct definition asset behind a pin of those types —
// borrowed from the node's own strings (never the signature scratch), null for
// every built-in type. Graph::connect requires matching typeNames.
// container/keyType/keyTypeName mirror the pin's ContainerKind and — for a Map
// pin, whose `type` is the VALUE type — its key. Left at the defaults every pin
// that is a scalar or a plain array has always had, so the brace-initializer
// lists below (and in every other signatureOf-style builder) stay as they were.
struct PinDesc { const char* name; PinType type; bool isArray = false;
                 const char* typeName = nullptr;
                 ContainerKind container = ContainerKind::None;
                 PinType       keyType = PinType::String;
                 const char*   keyTypeName = nullptr;

                 ContainerKind kind() const { return containerKindOf(isArray, container); } };
struct NodeSig { std::vector<PinDesc> execIns, execOuts, dataIns, dataOuts; };
HE_API NodeSig signatureOf(const Node& n);

// signatureOf() materialises four heap vectors per call. The pin-range and
// pin-type queries in the hot paths need only the counts or a single pin —
// Graph::connect asks 6× per attempted link, the interpreter once per executed
// node — so they go through these ALLOCATION-FREE accessors instead. Same switch,
// same answers; they just fill a reusable scratch signature and copy out the
// scalar bits. (HcCodegen and the editor panels still build full NodeSigs; they
// can adopt these too.)
struct NodeSigCounts { int execIns = 0, execOuts = 0, dataIns = 0, dataOuts = 0; };
HE_API NodeSigCounts signatureCountsOf(const Node& n);
// One data pin's descriptor; false (leaving `out` untouched) when `index` is out
// of range. `out.name` points into `n` or at a string literal, exactly like
// signatureOf's, so it outlives the call.
HE_API bool dataPinDescOf(const Node& n, bool input, int index, PinDesc& out);

// Static metadata for the editor add-menu (category + display name).
HE_API const char* nodeDisplayName(NodeType t);
// Extra words the add-menu's search matches BESIDES the display name,
// space-separated, or "" — never null.
//
// It exists because the display name is the node's key on disk (see the boxed
// warning at nodeDisplayName's definition): "Map Set" cannot be renamed to
// "Map Add" without every saved graph losing that node on load. So the name
// stays and the SYNONYM moves here, where a user typing "map add" still finds
// it. Aliases are search-only — nothing serializes them, so unlike a display
// name they can be changed at any time.
HE_API const char* nodeSearchAliases(NodeType t);
HE_API const char* nodeCategory(NodeType t);
// One-or-two-sentence usage description for the editor's hover tooltips
// (what the node does, what its inputs expect, what comes out).
HE_API const char* nodeTooltip(NodeType t);
HE_API const std::vector<NodeType>& nodeRegistry();

// ── A declared event ─────────────────────────────────────────────────────────
// The events a class raises are part of its interface, like its public
// functions: another class binds to them, and this one emits them. Declaring
// them once — instead of spelling the same name into an Event node, an Emit and
// a Bind — is what makes a typo an error instead of silence, and what lets the
// editor offer a list where it used to offer a text field.
//
// The NAME stays the identity on disk (renaming a declaration has to rewrite the
// nodes, never the saved bytes of some id). At run time the name is interned
// once into an EventId; see eventId() below.
struct EventDecl
{
    std::string name;
    // One optional argument, matching what an Event node and Emit Event carry.
    bool        hasArg = false;
    PinType     argType = PinType::Float;
    std::string typeName;   // Enum/Struct argument: the definition asset
};

struct HE_API Graph
{
    std::vector<Node>      nodes;
    std::vector<Link>      links;
    std::vector<Variable>  variables;
    // Events this class declares (custom ones only — the engine's own are a
    // fixed list, see engineEvents()).
    std::vector<EventDecl> events;
    int nextId = 1;

    // ── What this graph INHERITS, for the editor only ────────────────────────
    // A class asset whose baseClass names another class asset can read and write
    // its ancestors' variables and call their public functions under the same
    // names — the runtime resolves both across the instance's levels. The editor
    // has to OFFER them, and the menus and pickers that would do the offering sit
    // deep in HcGraphHost behind a `const Graph&`, with no route back to the
    // content system or to the class panel's state.
    //
    // So they ride here. Deliberately NOT merged into `variables`/`nodes`: these
    // are declared somewhere else and must never be written back into this
    // class's asset, and keeping them in their own list makes that true by
    // construction rather than by remembering to strip them before every save.
    // Neither field is serialized; fromJson leaves them empty and the class panel
    // refills them whenever the base class changes.
    std::vector<Variable> inherited;      // ancestors' instance variables, nearest wins
    std::vector<Node>     inheritedFns;   // ancestors' PUBLIC FunctionEntry prototypes

    Node*       findNode(int id);
    const Node* findNode(int id) const;
    int  addNode(Node n);
    void removeNode(int id);
    // Validated connect: pins must exist, out→in, types must match (exec↔exec,
    // data type equal). Replaces an occupied exec-out / data-in.
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);

    Variable*       findVariable(const std::string& name);
    const Variable* findVariable(const std::string& name) const;
    // The same lookup widened to the inherited list — what a reader wants when it
    // asks "what type is the variable this node names", because a node in a
    // derived class may legitimately name one it did not declare. Never returns a
    // mutable pointer: an inherited declaration belongs to the ancestor's asset.
    const Variable* findVariableOrInherited(const std::string& name) const;
    EventDecl*       findEvent(const std::string& name);
    const EventDecl* findEvent(const std::string& name) const;
};

// ── Input Action nodes ───────────────────────────────────────────────────────
// The host fires input as ordinary named events ("Input.Jump.Pressed"), which is
// what keeps Lua, Python and pre-existing graphs working. These two translate
// between that wire format and an InputAction node, and they are the ONLY place
// that does — the interpreter, the runtime's dispatch and the C++ codegen all
// ask here rather than each spelling the convention out again.

// The event names this node answers to, one per exec-out, in pin order:
// { Pressed, Released } for a digital action, { Axis } for an axis one. Empty
// for any other node type or an unbound node.
HE_API std::vector<std::string> inputActionEventNames(const Node& n);
// Which exec-out `eventName` enters, or -1 when this node does not handle it.
HE_API int inputActionChainFor(const Node& n, const std::string& eventName);

HE_API std::string toJson(const Graph& g);
HE_API bool        fromJson(const std::string& json, Graph& out);

// ── Migration hint: Create Widget makes a HIDDEN widget ──────────────────────
// Create Widget used to put its widget on screen by itself; it does not any
// more (docs/he-apps-plan.md), and Show Widget is what makes one visible. There
// is deliberately NO automatic migration — rewriting somebody's graph while
// loading it is a change nobody can see happening — so the answer is a warning,
// and this is what it is built from: the ids of the Create Widget nodes whose
// widget never reaches anything that could show it.
//
// It stays QUIET wherever it cannot follow the reference: a widget handed to a
// function, to an engine row, or to any node this does not know about counts as
// shown. A hint that fires on a working graph is a hint people learn to skip.
// Followed: the direct wire, and one variable name (Set Variable → Get Variable),
// which is how a graph keeps a widget between two events.
HE_API std::vector<int> widgetCreatorsWithoutShow(const Graph& g);

// ── Item-level JSON ─────────────────────────────────────────────────────────
// One node / one variable, in EXACTLY the form toJson() puts into the document's
// arrays — toJson/fromJson are implemented on top of these. Collaboration
// addresses a single item at a time (see CollabDocSync), and giving it its own
// serializer would guarantee the two drift the first time a field is added to
// only one of them. false = the JSON does not describe a usable item (an unknown
// node type, a nameless variable), which callers skip.
//
// Links have no item form: they are already a 4-int array (GraphJson.h) and are
// identified by their endpoints, not by an id.
HE_API std::string nodeToJson(const Node& n);
HE_API bool        nodeFromJson(const std::string& json, Node& out);
HE_API std::string variableToJson(const Variable& v);
HE_API bool        variableFromJson(const std::string& json, Variable& out);

// Propagate every function's interface (params + results, owned by its
// FunctionEntry) onto the matching FunctionCall / FunctionReturn nodes by name,
// so their pins resolve correctly. Call after editing an interface or loading a
// graph. FunctionReturn mirrors results (its data-ins); a call with no matching
// entry keeps its own mirror (the entry may live in another class's graph).
HE_API void syncFunctionSignatures(Graph& g);

// Re-mirror every struct/enum node's pin lists (params) from the CURRENT
// HE::TypeRegistry definitions: MakeStruct/BreakStruct get the field list,
// SetStructField revalidates its chosen field, SwitchOnEnum gets the entry
// names. Call after loading a graph or after a definition changed. A node whose
// definition is missing keeps its stored mirror (the def may load later).
// Recover the Enum/Struct DEFINITION of pins that only know their kind. Array
// and ForEach nodes carry their element type in propType, but nothing ever
// writes the definition path into their typeName — the editor's element-type
// picker offers built-ins only. Without it such a pin is a user-defined type
// nobody can resolve: no field pins, no entry list, "definition missing" in
// every panel, and no C++ type for the codegen. Recovered from the wiring,
// which is sound because Graph::connect only lets a definition-less pin join a
// typed one (the generic boundary) — so a non-empty peer names THE definition.
// Peers that disagree leave the node alone. Runs as part of fromJson, so every
// loaded graph is repaired; idempotent, and never overwrites an existing name.
// ── the engine's own events ──────────────────────────────────────────────────
// Names the engine itself fires, each a string literal at exactly one call site
// (WidgetManager, GameInstanceHost, HorizonWorld, PlayerHost, Runtime::destroy).
// They are NOT declared per class: every class may handle any of them, and none
// may raise them. `hook` is the CompiledInstance method a compiled class
// overrides for it; `elem` says whether the call site carries an element id.
struct EngineEventDesc
{
    const char* name;
    const char* hook;
    PinType     arg;    // Exec = the event carries none
    bool        elem;
};
HE_API const std::vector<EngineEventDesc>& engineEvents();
HE_API const EngineEventDesc* findEngineEvent(const std::string& name);

// ── the engine's own class taxonomy ─────────────────────────────────────────
// What a HorizonCode class asset's `baseClass` may name, and what each name
// brings with it. A base class is not a label: it decides which engine events
// the class may handle, which components a new asset of that kind starts with,
// and which built-in members its references offer in the editor.
//
// The chain is Object → Entity → {PlayerCharacter, PlayerController}, mirroring
// Unreal's AActor → {AController, APawn}. `baseClass == ""` reads as "Object",
// so assets authored before this table — they carry no CHUNK_HCBC — keep
// loading and keep behaving exactly as they did.
//
// A member is spelled as an HE::api registry id rather than getting a dispatch
// of its own: the editor inserts an Engine Call node with `targetParam` already
// wired to the reference the menu was opened on. The member surface therefore
// costs no runtime machinery, and Lua/Python/codegen get the same function for
// free. Those ids are strings, so a typo here would be silent — a test in
// tests/test_engine_api.cpp asserts every one resolves and that its
// targetParam really is a Ref.
struct EngineClassMember
{
    const char* label;        // what the member menu shows ("Possess")
    const char* apiId;        // HE::api registry id ("player.possess")
    int         targetParam;  // which of its params takes the target reference
};
struct EngineClassDesc
{
    const char* name;
    const char* base;   // "" = the root (Object itself)
    // ADDITIONAL to whatever the base already carries — engineClassEvents /
    // engineClassMembers walk the chain and concatenate, so a row never
    // restates its ancestors.
    std::vector<const char*>       events;
    std::vector<EngineClassMember> members;
};
HE_API const std::vector<EngineClassDesc>& engineClasses();
HE_API const EngineClassDesc* findEngineClass(const std::string& name);
// Is `derived` the same class as `base`, or below it in the chain? Empty reads
// as "Object" on both sides. A name the table does not know is only ever equal
// to itself — an asset with a garbage baseClass stays castable to itself and to
// nothing else, rather than silently answering "yes" to everything.
HE_API bool engineClassIsA(const std::string& derived, const std::string& base);
// The events / members of `name` AND of everything it derives from, base-first.
// Unknown name → empty.
HE_API std::vector<const char*>       engineClassEvents(const std::string& name);
HE_API std::vector<EngineClassMember> engineClassMembers(const std::string& name);

// ── interned event ids ───────────────────────────────────────────────────────
// The name is the format; the id is what dispatch runs on. Interning is
// process-global and append-only, so an id stays valid for the whole session and
// two graphs naming the same event agree without ever comparing strings.
using EventId = std::uint32_t;
HE_API EventId            eventId(const std::string& name);
// BY VALUE, deliberately: the intern table is a growing vector<string> shared
// across threads (asset streaming + export intern too) — a returned reference
// would dangle the moment eventId() reallocates it, and it would escape the
// mutex besides.
HE_API std::string eventName(EventId id);

// May a wire carry `from` into `to`? Equal types always; beyond that exactly the
// conversions the interpreter's `coerce` performs and no others — Float/Int/Bool
// among themselves, and Enum against Float/Int (it is int-backed). Deliberately
// NOT String, whose coerce yields the zero value: allowing that at a wire would
// look like a conversion and silently be a data loss. Arrays never convert —
// coerce passes an array through untouched, so an element-wise reinterpretation
// would read the wrong field of every item.
HE_API bool canConvertPinType(PinType from, PinType to);

// The wire canConvertPinType just refused — is there ONE node that would carry
// it? "Float into a String pin" has an answer (To String) and "Object into a
// Float pin" has none, and today both look the same to whoever dragged them.
// Lives beside canConvertPinType on purpose: the two are halves of one
// question, and a rule that drifted from the other would either offer a node
// for a wire that already connects or leave a convertible pair looking
// impossible.
//
// FALSE whenever the implicit conversion already suffices — Float↔Int, Enum
// against a number, Vec3/Vec4/Color — otherwise a caller inserts a node for a
// wire that would have connected on its own.
//
// The container kinds are REQUIRED rather than defaulted because they are the
// whole of one row: Set→Array is the same element type on both sides, and a
// caller who forgot them would be told a Set<Float> reaches a String pin.
HE_API bool conversionNodeFor(PinType from, ContainerKind fromKind,
                              PinType to,   ContainerKind toKind,
                              NodeType& out);

// Connect through a conversion node: the fallback for a wire Graph::connect
// refused ON TYPE. Spawns the node conversionNodeFor names, half-way between
// the two on the canvas and in the source's sub-graph, wires src→conv→dst and
// re-infers user-type definitions (a fresh Enum to String learns its definition
// from the wire it was born on). False when there is no such node, or when
// either half is refused after all — and then the node goes with it, because
// half a conversion left on the canvas is worse than the wire not appearing.
// The reasons that are NOT about the type (an exec pin, a pin that is not an
// output/input, both ends on one node) are recognised here rather than read out
// of Graph::connect's single false, and never spawn anything.
//
// `excluded` is the frontend's hidden-node list. A type the palette does not
// offer must not appear through this door either: the Animator sync graph hides
// Delay and Sequence on purpose, and a wire that conjured one would be a way
// around a restriction the menu states. Same rule the quick-spawn keys follow.
HE_API bool connectWithConversion(Graph& g, int srcNode, int srcPin,
                                  int dstNode, int dstPin,
                                  const std::vector<NodeType>& excluded = {});

// Equality of two SCALAR values of type `t` — the rule behind Array Contains,
// Set membership and Map key identity. Public because the boundaries need the
// same answer the interpreter gives: the savegame decoder dedupes a set with
// it, and generated C++ mirrors it (hc::sameScalar). Types with no meaningful
// identity (Struct) compare unequal, which is also why they cannot key a map.
HE_API bool scalarValueEquals(const Value& a, const Value& b, PinType t);

HE_API void inferUserTypeNames(Graph& g);

// Harvest the custom event names a graph already uses into its declaration list
// (Event nodes it handles, Emit Event nodes it raises). Graphs authored before
// events were declarable carry them only on the nodes; this is how they arrive
// with an interface. Runs as part of fromJson; never removes a declaration.
HE_API void inferEventDecls(Graph& g);

// Bind-a-definition's other half: re-mirror `nodes` (syncTypeSignatures) and
// carry their links over to the pins that appear. Binding a definition CHANGES
// the pin layout — Make Struct gains one data-in per field, Switch on Enum one
// exec-out per entry — and the existing links still address the old one, so
// without this a wire quietly lands on a different pin. Callers pass nodes whose
// definition was just set (their "before" is the bare shape).
HE_API void remapLinksForMirror(Graph& g, const std::vector<int>& nodes);

// The general re-mirror problem: whenever a node's pin LAYOUT is rebuilt from a
// definition that changed shape (a function parameter removed mid-list, a struct
// field deleted, an enum entry gone), the persisted links still address the OLD
// pin indices — without a remap every wire below the removed pin silently slides
// onto its neighbour, which is a misroute nobody sees. Capture the named layout
// BEFORE the mirror, re-mirror, then remap: each wire follows its pin NAME; a
// wire whose pin name vanished is dropped (visible and safe, unlike the slide).
// Pins that lost their name but whose region kept its size stay by index — that
// is the in-place rename/retype case, which must NOT cost the user their wires.
struct LinkRemapSnapshot
{
    // Per node id: the pin names of each region, in pin order.
    struct Sig { std::vector<std::string> execIns, execOuts, dataIns, dataOuts; };
    std::unordered_map<int, Sig> sigs;
};
HE_API LinkRemapSnapshot captureLinkRemapSnapshot(const Graph& g,
                                                  const std::vector<int>& nodeIds);
// Remaps g.links against the snapshot (taken before the mirror) and prunes any
// link on a snapshot node that ends up without a pin. Safe to call with nodes
// whose layout did not change — those remap onto themselves.
HE_API void remapLinksFromSnapshot(Graph& g, const LinkRemapSnapshot& snap);

HE_API void syncTypeSignatures(Graph& g);

// Partition a flat (pre-sub-graph) graph in place: assign every function-body
// node the `subgraph` of its owning FunctionEntry, leaving event-graph nodes at
// 0. No-op if the graph is already partitioned or has no functions. Called on
// load so old assets open with functions in their own sub-graphs.
HE_API void assignSubgraphs(Graph& g);

// The runtime Value a variable starts at (from its stored default). Hosts seed
// their per-instance variable store with this.
HE_API Value variableDefaultValue(const Variable& v);

// Editor convenience: clone the given nodes (fresh ids, positions offset by
// dx/dy) plus every link whose BOTH endpoints are in the set. Event and
// FunctionEntry nodes are skipped (handler/function names must stay unique).
// Returns the new ids in input order (skipped/unknown ids are omitted).
HE_API std::vector<int> duplicateNodes(Graph& g, const std::vector<int>& ids,
                                       float dx = 28.0f, float dy = 28.0f);

// Editor convenience: a ForEach node is generic until wired. When (dstNode,
// dstPin) is a ForEach's Array input and (srcNode, srcPin) is an ARRAY data
// output, adopt the source's element type onto the ForEach (its Array/Element
// pins recolor + retype), dropping Element-out links that no longer typecheck.
// For object arrays the element class rides along in the ForEach's s (member
// menus on the Element pin). Call BEFORE Graph::connect. No-op otherwise.
HE_API void adoptForEachElementType(Graph& g, int srcNode, int srcPin,
                                    int dstNode, int dstPin);

// ── Interpreter ──────────────────────────────────────────────────────────────
// The host binds these so HorizonCode can read/write target state without
// knowing what the target is.
struct Context
{
    std::function<Value(int elem, const std::string& prop)>              getProperty;
    std::function<void(int elem, const std::string& prop, const Value&)> setProperty;
    // The same pair on a REFERENCED instance, by element NAME (Get/Set Property
    // (Ref)). No instance id in the ones above because they always mean "mine";
    // this one is handed the target the node's pin resolved to.
    std::function<Value(uint32_t target, const std::string& elem, const std::string& prop)>
        getPropertyOn;
    std::function<void(uint32_t target, const std::string& elem, const std::string& prop,
                       const Value&)> setPropertyOn;
    std::function<Value(const std::string& var)>              getVariable;
    std::function<void(const std::string& var, const Value&)> setVariable;
    std::function<void()> showSelf;
    std::function<void()> hideSelf;

    // Widget management services (world-level; bound by the app). createWidget
    // instantiates a widget asset and returns its id; the rest act on that id.
    std::function<int(const std::string& assetPath)> createWidget;
    std::function<void(int widgetId)> showWidget;
    std::function<void(int widgetId)> hideWidget;
    std::function<void(int widgetId)> destroyWidget;
    // Instantiate a HorizonCode class asset → the new instance's Ref (0 on fail);
    // destroyObject removes a live instance by Ref.
    //
    // position/rotationEuler are 3 floats each, or nullptr. nullptr means
    // "place it as the class authored it" — NOT the origin. The distinction is
    // the backward-compatibility anchor: a graph that never wired the Location
    // pin must keep spawning where it spawns today, and a zero vector would
    // silently teleport every such spawn to (0,0,0). Hence a pointer, not a value.
    std::function<uint32_t(const std::string& classPath,
                           const float* position,      // 3 floats, or nullptr
                           const float* rotationEuler)> createObject; // 3 floats, degrees, or nullptr
    std::function<void(uint32_t objectRef)>               destroyObject;

    // Reference-based delegation (bound by the Runtime). All optional.
    // emitEvent: broadcast an event from THIS instance to everyone bound to it.
    std::function<void(const std::string& event, const Value& arg)>        emitEvent;
    // Same two, by interned id: generated code holds the id in a constant and
    // never builds the string. Optional — an unbound one falls back to the
    // name-taking pair above.
    std::function<void(EventId event, const Value& arg)>        emitEventId;
    std::function<void(uint32_t target, EventId event)>         bindEventId;
    // bindEvent: subscribe THIS instance to `event` on the `target` instance.
    std::function<void(uint32_t target, const std::string& event)>         bindEvent;
    // callExternal: call a public function on the `target` instance, passing
    // `args` and returning its result values (empty if the fn has none / fails).
    std::function<std::vector<Value>(uint32_t target, const std::string& fn,
                                     const std::vector<Value>& args)>      callExternal;
    // get/setExternal: read/write a PUBLIC variable on the `target` instance.
    std::function<Value(uint32_t target, const std::string& var)>              getExternal;
    std::function<void(uint32_t target, const std::string& var, const Value&)> setExternal;
    // The target instance as a COMPILED object, or null when it is interpreted,
    // destroyed, or never existed. Generated code uses it to call another
    // compiled class directly instead of marshalling through callExternal; the
    // null answer is what keeps mixed populations working (see hc::as).
    std::function<CompiledInstance*(uint32_t target)> resolveCompiled;
    // The GameInstance as an object, skipping the handle→instance lookup the
    // general path needs: the Runtime already holds it. Null when none is set or
    // it runs interpreted.
    std::function<CompiledInstance*()> gameInstanceCompiled;
    // References resolvable from any graph.
    std::function<Value()> getSelf;         // this instance
    std::function<Value()> getGameInstance; // the app-wide GameInstance (Ref 0 if none)

    // Generic engine-API dispatch: the EngineCall node passes its registry id +
    // evaluated argument Values and gets back the function's result Values. The
    // app binds this to the HE::api registry (HE_Scene); empty when unbound or
    // the id is unknown. This is the seam that keeps the interpreter (HE_Core)
    // decoupled from the engine surface it drives.
    std::function<std::vector<Value>(const std::string& apiId, const std::vector<Value>& args)> callApi;

    // Call a function on THIS instance that the running graph does not itself
    // declare — an INHERITED one. A class's graph only holds its own members,
    // so a Call Function node in a derived class finds no entry locally and
    // would otherwise be a silent no-op. The Runtime resolves it against the
    // instance's other levels, nearest first, and only reaches PUBLIC ones: a
    // base class's private function is private to that class, as in C++.
    // False = no level declares it, which leaves the call the no-op it was
    // before inheritance existed.
    std::function<bool(const std::string& fn, const std::vector<Value>& args,
                       std::vector<Value>* results)> callOwn;

    // Latent flow (bound by the Runtime): schedule THIS instance's exec chain to
    // resume from `nodeId`'s exec-out after `seconds` (Delay node). Unbound →
    // the Delay is a dead end (never resumes).
    // `realTime` counts REAL seconds: the wait ignores the time scale and keeps
    // running while the game is paused — which is the only way a pause menu can
    // time anything, since the runtime itself is driven by the scaled clock.
    std::function<void(int nodeId, float seconds, bool realTime)> scheduleResume;
    // Is `target` a live instance? (Is Valid node.) Unbound → false.
    std::function<bool(uint32_t target)> isValid;
    // Is `target` an instance of `classKey` — that exact class, or one derived
    // from that engine base? (Cast node.) Unbound → false.
    //
    // This deliberately does NOT go through resolveCompiled/classTag: that pair
    // is an exact pointer comparison over compiled instances only, so a Cast
    // built on it would answer differently for a base class and for an
    // interpreted target than the interpreter does. Both backends ask the
    // Runtime, so both get the same answer. (Generated code may still take a
    // direct route when the whole build is compiled — see hc::castRef.)
    std::function<bool(uint32_t target, const std::string& classKey)> isA;
    // Per-instance NODE state (DoOnce fired?, FlipFlop side) — persistent across
    // runs like variables, but never part of the variable store/public surface.
    // Reset together with the variables (reseedVariables).
    std::function<Value(int nodeId)>              getNodeState;
    std::function<void(int nodeId, const Value&)> setNodeState;
};

class HE_API Runner
{
public:
    Runner(const Graph& graph, Context ctx);

    // Fire every Event node whose name matches and whose element matches (an
    // Event bound to elem 0 fires for any element). `arg` feeds the event's
    // data output when it has one.
    void fireEvent(const std::string& eventName, int elem = 0, const Value& arg = {});

    // Run a named FunctionEntry, passing `args` to its parameters and copying its
    // return values into `results` (when non-null). False when missing or (with
    // requirePublic) the function is private — the gameplay-script routing check.
    bool callFunction(const std::string& name, bool requirePublic,
                      const std::vector<Value>& args = {}, std::vector<Value>* results = nullptr);

    // Resume the exec chain from `nodeId`'s first exec-out — the second half of
    // a Delay (called by the Runtime when the timer expires). A FRESH run: event
    // arg and exec-output caches start empty.
    void resumeFrom(int nodeId);

private:
    void runExecChain(const Node& from, int execOutPin, int depth);
    void execNode(const Node& n, int depth);
    Value evalData(const Node& n, int dataOutPin, int depth);
    Value evalInput(const Node& n, int dataInIndex, int depth);
    // Is a data input actually WIRED? evalInput can't answer this: it quietly
    // falls back to the pin default and then to the type's zero, so an optional
    // pin (Create Object's Location) cannot tell "left alone" from "set to 0".
    bool inputLinked(const Node& n, int dataInIndex) const;
    const Link* execLinkFrom(int nodeId, int pin) const;

    // One active function invocation: the argument values the call passed in
    // (read by the FunctionEntry's data-outs), the return values a
    // FunctionReturn writes (read by the FunctionCall's data-outs), and the
    // function's LOCAL variables (Variable::scope == fnEntryId), seeded from
    // their declared defaults when the frame is pushed.
    struct CallFrame
    {
        int                                    fnEntryId = 0;
        std::vector<Value>                     args;
        std::vector<Value>                     results;
        std::unordered_map<std::string, Value> locals;
    };
    // The innermost frame of the function that declares a given local (by its
    // FunctionEntry id), or null when that function isn't on the call stack
    // (a local's Get/Set node executing outside its function).
    CallFrame* frameFor(int fnEntryId);

    const Graph& m_graph;
    Context      m_ctx;
    Value        m_eventArg;
    int          m_steps = 0;
    // Outputs produced by exec nodes with side effects, per data-out pin
    // (CreateWidget's id, a FunctionCall's return values), so a downstream data
    // read returns the value instead of re-running the node. Cleared each run.
    std::unordered_map<int, std::vector<Value>> m_execOutputs;
    // Active function-call frames (innermost on top) — params in, results out.
    std::vector<CallFrame> m_callStack;
};

} // namespace HorizonCode
