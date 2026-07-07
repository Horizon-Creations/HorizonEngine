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
// InstanceId). Appended last so existing serialized propType ints stay stable.
enum class PinType : uint8_t { Exec = 0, Float, Bool, Int, String, Vec2, Color, Ref };

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

    static Value ofFloat(float v)            { Value r; r.type = PinType::Float;  r.f = v;  return r; }
    static Value ofBool(bool v)              { Value r; r.type = PinType::Bool;   r.b = v;  return r; }
    static Value ofInt(int v)                { Value r; r.type = PinType::Int;    r.i = v;  return r; }
    static Value ofString(std::string v)     { Value r; r.type = PinType::String; r.s = std::move(v); return r; }
    static Value ofVec2(const glm::vec2& v)  { Value r; r.type = PinType::Vec2;   r.v2 = v; return r; }
    static Value ofColor(const glm::vec4& v) { Value r; r.type = PinType::Color;  r.col = v; return r; }
    static Value ofRef(uint32_t id)          { Value r; r.type = PinType::Ref;    r.ref = id; return r; }
};

enum class NodeType : uint8_t
{
    // Host-fired entry points.
    Event = 0,      // s = event name, elem = target (0 = any); optional arg out
    FunctionEntry,  // s = function name; access = 0 public / 1 private
    // Control flow.
    Branch, Sequence, FunctionCall,
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
    COUNT
};

// A user-defined graph variable: named, typed, persistent per running instance.
// The default seeds the instance's variable store; Get/SetVariable nodes read
// and write it. The default value lives in f[]/s like a literal node.
struct Variable
{
    std::string name;
    PinType     type = PinType::Float;
    float       f[4] = {};
    std::string s;
    int         access = 0;   // 0 public (readable via a reference), 1 private
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
    float       x = 0.0f, y = 0.0f;       // editor canvas position
};

// Links connect unified pin indices (see pin ranges below).
struct Link { int srcNode = 0, srcPin = 0, dstNode = 0, dstPin = 0; };

// Pin metadata for one node instance (variable-pin nodes like Event/Get/Set
// depend on the node's own fields, so this is computed per node, not per type).
struct PinDesc { const char* name; PinType type; };
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

// The runtime Value a variable starts at (from its stored default). Hosts seed
// their per-instance variable store with this.
HE_API Value variableDefaultValue(const Variable& v);

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
    // callExternal: call a public function on the `target` instance.
    std::function<void(uint32_t target, const std::string& fn)>            callExternal;
    // get/setExternal: read/write a PUBLIC variable on the `target` instance.
    std::function<Value(uint32_t target, const std::string& var)>              getExternal;
    std::function<void(uint32_t target, const std::string& var, const Value&)> setExternal;
    // References resolvable from any graph.
    std::function<Value()> getSelf;         // this instance
    std::function<Value()> getGameInstance; // the app-wide GameInstance (Ref 0 if none)
};

class HE_API Runner
{
public:
    Runner(const Graph& graph, Context ctx);

    // Fire every Event node whose name matches and whose element matches (an
    // Event bound to elem 0 fires for any element). `arg` feeds the event's
    // data output when it has one.
    void fireEvent(const std::string& eventName, int elem = 0, const Value& arg = {});

    // Run a named FunctionEntry. False when missing or (with requirePublic) the
    // function is private — the gameplay-script routing check.
    bool callFunction(const std::string& name, bool requirePublic);

private:
    void runExecChain(const Node& from, int execOutPin, int depth);
    void execNode(const Node& n, int depth);
    Value evalData(const Node& n, int dataOutPin, int depth);
    Value evalInput(const Node& n, int dataInIndex, int depth);
    const Link* execLinkFrom(int nodeId, int pin) const;

    const Graph& m_graph;
    Context      m_ctx;
    Value        m_eventArg;
    int          m_steps = 0;
    // Outputs produced by exec nodes with side effects (e.g. CreateWidget's id),
    // so a downstream data read returns the value instead of re-running the node.
    // Cleared at the start of every run.
    std::unordered_map<int, Value> m_execOutputs;
};

} // namespace HorizonCode
