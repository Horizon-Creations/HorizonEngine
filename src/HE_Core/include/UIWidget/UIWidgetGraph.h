#pragma once
#include <UIWidget/UIWidgetTree.h>
#include <Types/Defines.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>

namespace HE {

// ── UI widget logic graph ────────────────────────────────────────────────────
// Blueprint-style event/logic graph attached to a UI Widget asset (JSON in
// CHUNK_UIWG, edited in the widget editor's Graph mode). Event nodes fire from
// pointer input (click/hover) or the frame tick; execution follows white exec
// links; data flows through typed pins evaluated on demand. The interpreter
// mutates a live UIWidgetTree instance copy directly (text, colors, layout,
// visibility), so no scene/entity coupling exists.

enum class UIGraphPinType : uint8_t { Exec = 0, Float, Bool, String, Vec2, Color };

// Element properties addressable by Get/SetProperty (typed via uiGraphPropType).
enum class UIWidgetProp : uint8_t
{
    Text = 0,     // String
    Color,        // Color (tint / text color / button normal color)
    Visible,      // Bool
    PositionX,    // Float
    PositionY,    // Float
    Width,        // Float
    Height,       // Float
    FontSize,     // Float
    HoveredColor, // Color (buttons)
    PressedColor, // Color (buttons)
    COUNT
};
HE_API UIGraphPinType uiGraphPropType(UIWidgetProp p);
HE_API const char*    uiGraphPropName(UIWidgetProp p);

enum class UIGraphNodeType : uint8_t
{
    // Events (exec source; `elem` = widget-tree node id, 0 = any element).
    EventConstruct = 0, // fires once when the widget is created/shown
    EventTick,          // fires every frame; data out 0 = dt (Float)
    EventClick,
    EventHoverEnter,
    EventHoverExit,

    // Flow
    Branch,             // exec in; Cond (Bool); exec outs: True, False
    Sequence,           // exec in; exec outs: Then 0, Then 1

    // Element access (`elem` = tree node id, `prop` = UIWidgetProp)
    GetProperty,        // data out 0: value (type = prop type)
    SetProperty,        // exec; data in 0: value

    // Widget-level (the instance running this graph)
    ShowSelf,           // exec: make this widget visible
    HideSelf,           // exec: hide this widget

    // Literals (value in f[4] / s)
    ConstFloat, ConstBool, ConstString, ConstVec2, ConstColor,

    // Math / logic (Floats and Bools)
    Add, Subtract, Multiply, Divide,
    Greater, Less, Equals,
    And, Or, Not,

    // Strings
    Concat,             // String + String → String
    ToString,           // Float → String (trimmed decimals)

    // Functions (name in `s`; access on the entry: 0 = public, 1 = private).
    FunctionEntry,      // exec source; the definition head
    FunctionCall,       // exec; runs the named FunctionEntry's chain inline

    // Debug
    Print,              // exec; data in 0: String → engine log

    COUNT
};

struct UIGraphPinDesc
{
    const char*    name;
    UIGraphPinType type;
};

struct UIGraphNodeDesc
{
    const char* name;      // display + JSON identity
    const char* category;  // add-menu grouping ("Events", "Flow", "Element", …)
    // Exec pins are listed separately from data pins; a node's pin index space
    // is [execIns][execOuts][dataIns][dataOuts] in that order.
    std::vector<UIGraphPinDesc> execIns;   // usually 0 or 1 ("")
    std::vector<UIGraphPinDesc> execOuts;  // labeled for Branch/Sequence
    std::vector<UIGraphPinDesc> dataIns;
    std::vector<UIGraphPinDesc> dataOuts;
};
HE_API const UIGraphNodeDesc& uiGraphNodeDesc(UIGraphNodeType t);
HE_API const std::vector<UIGraphNodeType>& uiGraphNodeRegistry();

struct UIGraphNode
{
    int             id = 0;
    UIGraphNodeType type = UIGraphNodeType::EventConstruct;
    int             elem = 0;      // widget-tree node id (events / property access)
    int             prop = 0;      // UIWidgetProp for Get/SetProperty
    int             access = 0;    // FunctionEntry: 0 = public, 1 = private
    float           f[4] = {};     // literal payload
    std::string     s;             // literal string / function name
    float           x = 0.0f, y = 0.0f; // canvas position (editor-only)
};

// Links connect pin indices in the unified pin space described above.
struct UIGraphLink
{
    int srcNode = 0, srcPin = 0;   // an exec-out or data-out of src
    int dstNode = 0, dstPin = 0;   // an exec-in or data-in of dst
};

struct UIWidgetGraph
{
    std::vector<UIGraphNode> nodes;
    std::vector<UIGraphLink> links;
    int nextId = 1;

    UIGraphNode*       findNode(int id);
    const UIGraphNode* findNode(int id) const;
    int addNode(UIGraphNode node);          // assigns id, returns it
    void removeNode(int id);                // also drops its links
    // Validated connect: pins must exist, direction out→in, types must match
    // (exec↔exec, data type equal); an occupied exec-out/data-in is replaced.
    bool connect(int srcNode, int srcPin, int dstNode, int dstPin);
};

HE_API std::string uiWidgetGraphToJson(const UIWidgetGraph& g);
HE_API bool        uiWidgetGraphFromJson(const std::string& json, UIWidgetGraph& out);

// ── Interpreter ──────────────────────────────────────────────────────────────

struct UIGraphValue
{
    UIGraphPinType type = UIGraphPinType::Float;
    float       f = 0.0f;
    bool        b = false;
    glm::vec2   v2{ 0.0f };
    glm::vec4   v4{ 0.0f, 0.0f, 0.0f, 1.0f };
    std::string s;

    static UIGraphValue ofFloat(float v)              { UIGraphValue r; r.type = UIGraphPinType::Float;  r.f = v;  return r; }
    static UIGraphValue ofBool(bool v)                { UIGraphValue r; r.type = UIGraphPinType::Bool;   r.b = v;  return r; }
    static UIGraphValue ofString(std::string v)       { UIGraphValue r; r.type = UIGraphPinType::String; r.s = std::move(v); return r; }
    static UIGraphValue ofVec2(const glm::vec2& v)    { UIGraphValue r; r.type = UIGraphPinType::Vec2;   r.v2 = v; return r; }
    static UIGraphValue ofColor(const glm::vec4& v)   { UIGraphValue r; r.type = UIGraphPinType::Color;  r.v4 = v; return r; }
};

// Per-instance flags the graph may flip (ShowSelf/HideSelf); owned by the
// widget runtime, passed by reference into each run.
struct UIWidgetSelfState
{
    bool visible = true;
};

enum class UIWidgetEvent : uint8_t { Construct = 0, Tick, Click, HoverEnter, HoverExit };

// Executes the graph against a live tree instance. Stateless between runs —
// all mutable state lives in the tree copy and self flags.
class HE_API UIWidgetGraphRunner
{
public:
    UIWidgetGraphRunner(const UIWidgetGraph& graph, UIWidgetTree& tree,
                        UIWidgetSelfState& self);

    // Fire every matching event node. `elem` = source element for pointer
    // events (event nodes bound to elem 0 fire for any element); dt feeds
    // EventTick's data output.
    void fireEvent(UIWidgetEvent ev, int elem = 0, float dt = 0.0f);

    // Run a named FunctionEntry. Returns false when the function is missing
    // or (with requirePublic) not public — the script-call routing check.
    bool callFunction(const std::string& name, bool requirePublic);

private:
    void runExecChain(const UIGraphNode& from, int execOutPin, int depth);
    void execNode(const UIGraphNode& n, int depth);
    UIGraphValue evalData(const UIGraphNode& n, int dataOutPin, int depth);
    UIGraphValue evalInput(const UIGraphNode& n, int dataInIndex, int depth);
    const UIGraphLink* execLinkFrom(int nodeId, int pin) const;

    const UIWidgetGraph& m_graph;
    UIWidgetTree&        m_tree;
    UIWidgetSelfState&   m_self;
    float                m_tickDt = 0.0f;
    int                  m_steps = 0;     // runaway guard across one fire/call
};

} // namespace HE
