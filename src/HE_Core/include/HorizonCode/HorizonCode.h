#pragma once
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

// Ref = a reference/handle to another running script instance (a Runtime
// InstanceId). Transform = a position/rotation(euler)/scale bundle. Appended last
// so existing serialized propType ints stay stable.
enum class PinType : uint8_t { Exec = 0, Float, Bool, Int, String, Vec2, Color, Ref, Transform };

struct Value
{
    PinType     type = PinType::Float;
    float       f = 0.0f;
    bool        b = false;
    int         i = 0;
    glm::vec2   v2{ 0.0f };
    glm::vec4   col{ 0.0f, 0.0f, 0.0f, 1.0f };
    std::string s;
    uint32_t    ref = 0;   // instance handle when type == Ref (0 = none)
    // Transform payload (type == Transform): rotation in euler degrees, identity scale.
    glm::vec3   tpos{ 0.0f }, trot{ 0.0f }, tscl{ 1.0f };
    // Array payload: when isArray, `type` is the element type and `items` holds the
    // elements (each a scalar Value of `type`). An array is never scalar-coerced.
    bool               isArray = false;
    std::vector<Value> items;

    static Value ofFloat(float v)            { Value r; r.type = PinType::Float;  r.f = v;  return r; }
    static Value ofBool(bool v)              { Value r; r.type = PinType::Bool;   r.b = v;  return r; }
    static Value ofInt(int v)                { Value r; r.type = PinType::Int;    r.i = v;  return r; }
    static Value ofString(std::string v)     { Value r; r.type = PinType::String; r.s = std::move(v); return r; }
    static Value ofVec2(const glm::vec2& v)  { Value r; r.type = PinType::Vec2;   r.v2 = v; return r; }
    static Value ofColor(const glm::vec4& v) { Value r; r.type = PinType::Color;  r.col = v; return r; }
    static Value ofRef(uint32_t id)          { Value r; r.type = PinType::Ref;    r.ref = id; return r; }
    static Value ofTransform(const glm::vec3& p, const glm::vec3& r_, const glm::vec3& s_)
    { Value r; r.type = PinType::Transform; r.tpos = p; r.trot = r_; r.tscl = s_; return r; }
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
    ShowWidget, HideWidget,
    // Create + manage widgets by id (from any graph — level, GameInstance, …).
    CreateWidget,    // s = widget asset path; dataOut Widget (Int id)
    ShowWidgetId, HideWidgetId, DestroyWidget, // dataIn Widget (Int id)
    // Instantiate a HorizonCode class asset as a live runtime object.
    CreateObject,    // s = HorizonCode class asset path; dataOut Object (Ref)
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
    bool        isArray = false;   // the pin carries an array of `type`
};

// A user-defined graph variable: named, typed, persistent per running instance.
// The default seeds the instance's variable store; Get/SetVariable nodes read
// and write it. The default value lives in f[]/s like a literal node.
struct Variable
{
    std::string name;
    PinType     type = PinType::Float;
    bool        isArray = false;   // when true the variable holds an array of `type`
    float       f[4] = {};
    std::string s;
    // Transform default (type == Transform): rotation in euler degrees, identity scale.
    glm::vec3   tpos{ 0.0f }, trot{ 0.0f }, tscl{ 1.0f };
    // Array default (isArray): the editor-authored slots that seed the instance's
    // array on creation. Each item is a scalar Value of `type`.
    std::vector<Value> defaultItems;
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
};

// Links connect unified pin indices (see pin ranges below).
struct Link { int srcNode = 0, srcPin = 0, dstNode = 0, dstPin = 0; };

// Pin metadata for one node instance (variable-pin nodes like Event/Get/Set
// depend on the node's own fields, so this is computed per node, not per type).
struct PinDesc { const char* name; PinType type; bool isArray = false; };
struct NodeSig { std::vector<PinDesc> execIns, execOuts, dataIns, dataOuts; };
HE_API NodeSig signatureOf(const Node& n);

// Static metadata for the editor add-menu (category + display name).
HE_API const char* nodeDisplayName(NodeType t);
HE_API const char* nodeCategory(NodeType t);
HE_API const std::vector<NodeType>& nodeRegistry();

struct HE_API Graph
{
    std::vector<Node>     nodes;
    std::vector<Link>     links;
    std::vector<Variable> variables;
    int nextId = 1;

    Node*       findNode(int id);
    const Node* findNode(int id) const;
    int  addNode(Node n);
    void removeNode(int id);
    // Validated connect: pins must exist, out→in, types must match (exec↔exec,
    // data type equal). Replaces an occupied exec-out / data-in.
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);

    Variable*       findVariable(const std::string& name);
    const Variable* findVariable(const std::string& name) const;
};

HE_API std::string toJson(const Graph& g);
HE_API bool        fromJson(const std::string& json, Graph& out);

// Propagate every function's interface (params + results, owned by its
// FunctionEntry) onto the matching FunctionCall / FunctionReturn nodes by name,
// so their pins resolve correctly. Call after editing an interface or loading a
// graph. FunctionReturn mirrors results (its data-ins); a call with no matching
// entry keeps its own mirror (the entry may live in another class's graph).
HE_API void syncFunctionSignatures(Graph& g);

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
    std::function<uint32_t(const std::string& classPath)> createObject;
    std::function<void(uint32_t objectRef)>               destroyObject;

    // Reference-based delegation (bound by the Runtime). All optional.
    // emitEvent: broadcast an event from THIS instance to everyone bound to it.
    std::function<void(const std::string& event, const Value& arg)>        emitEvent;
    // bindEvent: subscribe THIS instance to `event` on the `target` instance.
    std::function<void(uint32_t target, const std::string& event)>         bindEvent;
    // callExternal: call a public function on the `target` instance, passing
    // `args` and returning its result values (empty if the fn has none / fails).
    std::function<std::vector<Value>(uint32_t target, const std::string& fn,
                                     const std::vector<Value>& args)>      callExternal;
    // get/setExternal: read/write a PUBLIC variable on the `target` instance.
    std::function<Value(uint32_t target, const std::string& var)>              getExternal;
    std::function<void(uint32_t target, const std::string& var, const Value&)> setExternal;
    // References resolvable from any graph.
    std::function<Value()> getSelf;         // this instance
    std::function<Value()> getGameInstance; // the app-wide GameInstance (Ref 0 if none)

    // Generic engine-API dispatch: the EngineCall node passes its registry id +
    // evaluated argument Values and gets back the function's result Values. The
    // app binds this to the HE::api registry (HE_Scene); empty when unbound or
    // the id is unknown. This is the seam that keeps the interpreter (HE_Core)
    // decoupled from the engine surface it drives.
    std::function<std::vector<Value>(const std::string& apiId, const std::vector<Value>& args)> callApi;

    // Latent flow (bound by the Runtime): schedule THIS instance's exec chain to
    // resume from `nodeId`'s exec-out after `seconds` (Delay node). Unbound →
    // the Delay is a dead end (never resumes).
    std::function<void(int nodeId, float seconds)> scheduleResume;
    // Is `target` a live instance? (Is Valid node.) Unbound → false.
    std::function<bool(uint32_t target)> isValid;
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
