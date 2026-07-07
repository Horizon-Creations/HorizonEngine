#pragma once
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <functional>
#include <string>
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

enum class PinType : uint8_t { Exec = 0, Float, Bool, Int, String, Vec2, Color };

struct Value
{
    PinType     type = PinType::Float;
    float       f = 0.0f;
    bool        b = false;
    int         i = 0;
    glm::vec2   v2{ 0.0f };
    glm::vec4   col{ 0.0f, 0.0f, 0.0f, 1.0f };
    std::string s;

    static Value ofFloat(float v)            { Value r; r.type = PinType::Float;  r.f = v;  return r; }
    static Value ofBool(bool v)              { Value r; r.type = PinType::Bool;   r.b = v;  return r; }
    static Value ofInt(int v)                { Value r; r.type = PinType::Int;    r.i = v;  return r; }
    static Value ofString(std::string v)     { Value r; r.type = PinType::String; r.s = std::move(v); return r; }
    static Value ofVec2(const glm::vec2& v)  { Value r; r.type = PinType::Vec2;   r.v2 = v; return r; }
    static Value ofColor(const glm::vec4& v) { Value r; r.type = PinType::Color;  r.col = v; return r; }
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
    // Host actions on the running widget.
    ShowWidget, HideWidget,
    // Literals (f[]/s).
    ConstFloat, ConstBool, ConstInt, ConstString, ConstVec2, ConstColor,
    // Math / logic.
    Add, Subtract, Multiply, Divide,
    Greater, Less, Equals, And, Or, Not,
    // Strings.
    Concat, ToString,
    // Debug.
    Print,
    COUNT
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
    std::vector<Node> nodes;
    std::vector<Link> links;
    int nextId = 1;

    Node*       findNode(int id);
    const Node* findNode(int id) const;
    int  addNode(Node n);
    void removeNode(int id);
    // Validated connect: pins must exist, out→in, types must match (exec↔exec,
    // data type equal). Replaces an occupied exec-out / data-in.
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);
};

HE_API std::string toJson(const Graph& g);
HE_API bool        fromJson(const std::string& json, Graph& out);

// ── Interpreter ──────────────────────────────────────────────────────────────
// The host binds these so HorizonCode can read/write target state without
// knowing what the target is.
struct Context
{
    std::function<Value(int elem, const std::string& prop)>              getProperty;
    std::function<void(int elem, const std::string& prop, const Value&)> setProperty;
    std::function<void()> showSelf;
    std::function<void()> hideSelf;
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
};

} // namespace HorizonCode
