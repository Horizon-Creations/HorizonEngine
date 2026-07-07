#include <UIWidget/UIWidgetGraph.h>
#include <Diagnostics/Logger.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace HE {

// ── Property metadata ────────────────────────────────────────────────────────

UIGraphPinType uiGraphPropType(UIWidgetProp p)
{
    switch (p)
    {
        case UIWidgetProp::Text:         return UIGraphPinType::String;
        case UIWidgetProp::Color:        return UIGraphPinType::Color;
        case UIWidgetProp::Visible:      return UIGraphPinType::Bool;
        case UIWidgetProp::PositionX:
        case UIWidgetProp::PositionY:
        case UIWidgetProp::Width:
        case UIWidgetProp::Height:
        case UIWidgetProp::FontSize:     return UIGraphPinType::Float;
        case UIWidgetProp::HoveredColor:
        case UIWidgetProp::PressedColor: return UIGraphPinType::Color;
        default:                         return UIGraphPinType::Float;
    }
}

const char* uiGraphPropName(UIWidgetProp p)
{
    switch (p)
    {
        case UIWidgetProp::Text:         return "Text";
        case UIWidgetProp::Color:        return "Color";
        case UIWidgetProp::Visible:      return "Visible";
        case UIWidgetProp::PositionX:    return "Position X";
        case UIWidgetProp::PositionY:    return "Position Y";
        case UIWidgetProp::Width:        return "Width";
        case UIWidgetProp::Height:       return "Height";
        case UIWidgetProp::FontSize:     return "Font Size";
        case UIWidgetProp::HoveredColor: return "Hovered Color";
        case UIWidgetProp::PressedColor: return "Pressed Color";
        default:                         return "?";
    }
}

// ── Node registry ────────────────────────────────────────────────────────────

namespace
{
using T = UIGraphNodeType;
using P = UIGraphPinType;

const UIGraphNodeDesc& descFor(UIGraphNodeType t)
{
    // Note: Get/SetProperty declare their value pin as Float here; the editor
    // and interpreter retype it from the node's selected property
    // (uiGraphPropType), so the static desc is only a fallback.
    static const std::array<UIGraphNodeDesc, (size_t)T::COUNT> kDescs = { {
        /*EventConstruct*/ { "Event Construct",   "Events",  {}, {{"", P::Exec}}, {}, {} },
        /*EventTick*/      { "Event Tick",        "Events",  {}, {{"", P::Exec}}, {}, {{"Delta", P::Float}} },
        /*EventClick*/     { "Event OnClick",     "Events",  {}, {{"", P::Exec}}, {}, {} },
        /*EventHoverEnter*/{ "Event OnHoverEnter","Events",  {}, {{"", P::Exec}}, {}, {} },
        /*EventHoverExit*/ { "Event OnHoverExit", "Events",  {}, {{"", P::Exec}}, {}, {} },
        /*Branch*/         { "Branch",            "Flow",    {{"", P::Exec}}, {{"True", P::Exec}, {"False", P::Exec}}, {{"Cond", P::Bool}}, {} },
        /*Sequence*/       { "Sequence",          "Flow",    {{"", P::Exec}}, {{"Then 0", P::Exec}, {"Then 1", P::Exec}}, {}, {} },
        /*GetProperty*/    { "Get Property",      "Element", {}, {}, {}, {{"Value", P::Float}} },
        /*SetProperty*/    { "Set Property",      "Element", {{"", P::Exec}}, {{"", P::Exec}}, {{"Value", P::Float}}, {} },
        /*ShowSelf*/       { "Show Widget",       "Widget",  {{"", P::Exec}}, {{"", P::Exec}}, {}, {} },
        /*HideSelf*/       { "Hide Widget",       "Widget",  {{"", P::Exec}}, {{"", P::Exec}}, {}, {} },
        /*ConstFloat*/     { "Float",             "Literals",{}, {}, {}, {{"", P::Float}} },
        /*ConstBool*/      { "Bool",              "Literals",{}, {}, {}, {{"", P::Bool}} },
        /*ConstString*/    { "String",            "Literals",{}, {}, {}, {{"", P::String}} },
        /*ConstVec2*/      { "Vec2",              "Literals",{}, {}, {}, {{"", P::Vec2}} },
        /*ConstColor*/     { "Color",             "Literals",{}, {}, {}, {{"", P::Color}} },
        /*Add*/            { "Add",               "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Float}} },
        /*Subtract*/       { "Subtract",          "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Float}} },
        /*Multiply*/       { "Multiply",          "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Float}} },
        /*Divide*/         { "Divide",            "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Float}} },
        /*Greater*/        { "Greater",           "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Bool}} },
        /*Less*/           { "Less",              "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Bool}} },
        /*Equals*/         { "Equals",            "Math",    {}, {}, {{"A", P::Float}, {"B", P::Float}}, {{"", P::Bool}} },
        /*And*/            { "And",               "Logic",   {}, {}, {{"A", P::Bool}, {"B", P::Bool}}, {{"", P::Bool}} },
        /*Or*/             { "Or",                "Logic",   {}, {}, {{"A", P::Bool}, {"B", P::Bool}}, {{"", P::Bool}} },
        /*Not*/            { "Not",               "Logic",   {}, {}, {{"", P::Bool}}, {{"", P::Bool}} },
        /*Concat*/         { "Concat",            "String",  {}, {}, {{"A", P::String}, {"B", P::String}}, {{"", P::String}} },
        /*ToString*/       { "To String",         "String",  {}, {}, {{"", P::Float}}, {{"", P::String}} },
        /*FunctionEntry*/  { "Function",          "Functions", {}, {{"", P::Exec}}, {}, {} },
        /*FunctionCall*/   { "Call Function",     "Functions", {{"", P::Exec}}, {{"", P::Exec}}, {}, {} },
        /*Print*/          { "Print",             "Debug",   {{"", P::Exec}}, {{"", P::Exec}}, {{"", P::String}}, {} },
    } };
    return kDescs[(size_t)t];
}

// Unified pin index ranges for a node type.
struct PinRanges
{
    int execIn0, execOut0, dataIn0, dataOut0, end;
};
PinRanges pinRanges(UIGraphNodeType t)
{
    const UIGraphNodeDesc& d = descFor(t);
    PinRanges r;
    r.execIn0  = 0;
    r.execOut0 = r.execIn0  + (int)d.execIns.size();
    r.dataIn0  = r.execOut0 + (int)d.execOuts.size();
    r.dataOut0 = r.dataIn0  + (int)d.dataIns.size();
    r.end      = r.dataOut0 + (int)d.dataOuts.size();
    return r;
}

// Effective type of a data pin, honoring the property retype on Get/SetProperty.
UIGraphPinType dataPinType(const UIGraphNode& n, bool input, int index)
{
    if (n.type == T::GetProperty && !input && index == 0)
        return uiGraphPropType((UIWidgetProp)n.prop);
    if (n.type == T::SetProperty && input && index == 0)
        return uiGraphPropType((UIWidgetProp)n.prop);
    const UIGraphNodeDesc& d = descFor(n.type);
    const auto& pins = input ? d.dataIns : d.dataOuts;
    if (index < 0 || index >= (int)pins.size()) return P::Float;
    return pins[index].type;
}
} // namespace

const UIGraphNodeDesc& uiGraphNodeDesc(UIGraphNodeType t) { return descFor(t); }

const std::vector<UIGraphNodeType>& uiGraphNodeRegistry()
{
    static const std::vector<UIGraphNodeType> kAll = []
    {
        std::vector<UIGraphNodeType> v;
        for (int i = 0; i < (int)T::COUNT; ++i) v.push_back((UIGraphNodeType)i);
        return v;
    }();
    return kAll;
}

// ── Graph container ──────────────────────────────────────────────────────────

UIGraphNode*       UIWidgetGraph::findNode(int id)
{
    for (auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}
const UIGraphNode* UIWidgetGraph::findNode(int id) const
{
    for (const auto& n : nodes) if (n.id == id) return &n;
    return nullptr;
}

int UIWidgetGraph::addNode(UIGraphNode node)
{
    node.id = nextId++;
    nodes.push_back(std::move(node));
    return nodes.back().id;
}

void UIWidgetGraph::removeNode(int id)
{
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const UIGraphNode& n){ return n.id == id; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const UIGraphLink& l){ return l.srcNode == id || l.dstNode == id; }),
        links.end());
}

bool UIWidgetGraph::connect(int srcNode, int srcPin, int dstNode, int dstPin)
{
    const UIGraphNode* s = findNode(srcNode);
    const UIGraphNode* d = findNode(dstNode);
    if (!s || !d || srcNode == dstNode) return false;

    const PinRanges sr = pinRanges(s->type);
    const PinRanges dr = pinRanges(d->type);

    const bool srcIsExecOut = srcPin >= sr.execOut0 && srcPin < sr.dataIn0;
    const bool srcIsDataOut = srcPin >= sr.dataOut0 && srcPin < sr.end;
    const bool dstIsExecIn  = dstPin >= dr.execIn0  && dstPin < dr.execOut0;
    const bool dstIsDataIn  = dstPin >= dr.dataIn0  && dstPin < dr.dataOut0;

    if (srcIsExecOut && dstIsExecIn)
    {
        // One exec link per exec-out: replace an existing one.
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const UIGraphLink& l){ return l.srcNode == srcNode && l.srcPin == srcPin; }),
            links.end());
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    if (srcIsDataOut && dstIsDataIn)
    {
        const UIGraphPinType st = dataPinType(*s, false, srcPin - sr.dataOut0);
        const UIGraphPinType dt = dataPinType(*d, true,  dstPin - dr.dataIn0);
        if (st != dt) return false;
        // One incoming link per data-in: replace an existing one.
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const UIGraphLink& l){ return l.dstNode == dstNode && l.dstPin == dstPin; }),
            links.end());
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    return false;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

std::string uiWidgetGraphToJson(const UIWidgetGraph& g)
{
    nlohmann::json j;
    j["nextId"] = g.nextId;
    nlohmann::json jn = nlohmann::json::array();
    for (const auto& n : g.nodes)
    {
        // Type by NAME → schema-evolution safe (like the material graph).
        nlohmann::json e = {
            { "id",   n.id },
            { "type", descFor(n.type).name },
            { "pos",  { n.x, n.y } },
        };
        if (n.elem)     e["elem"]   = n.elem;
        if (n.prop)     e["prop"]   = n.prop;
        if (n.access)   e["access"] = n.access;
        if (!n.s.empty()) e["s"]    = n.s;
        if (n.f[0] || n.f[1] || n.f[2] || n.f[3])
            e["f"] = { n.f[0], n.f[1], n.f[2], n.f[3] };
        jn.push_back(std::move(e));
    }
    j["nodes"] = std::move(jn);

    nlohmann::json jl = nlohmann::json::array();
    for (const auto& l : g.links)
        jl.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin });
    j["links"] = std::move(jl);
    return j.dump(2);
}

bool uiWidgetGraphFromJson(const std::string& json, UIWidgetGraph& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    UIWidgetGraph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& e : j.value("nodes", nlohmann::json::array()))
    {
        UIGraphNode n;
        n.id = e.value("id", 0);
        const std::string typeName = e.value("type", std::string());
        bool known = false;
        for (UIGraphNodeType t : uiGraphNodeRegistry())
            if (typeName == descFor(t).name) { n.type = t; known = true; break; }
        if (!known) continue; // forward-compat: drop unknown node types
        if (const auto& p = e.value("pos", nlohmann::json::array()); p.size() >= 2)
        { n.x = p[0].get<float>(); n.y = p[1].get<float>(); }
        n.elem   = e.value("elem", 0);
        n.prop   = e.value("prop", 0);
        n.access = e.value("access", 0);
        n.s      = e.value("s", std::string());
        if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
            for (int i = 0; i < 4; ++i) n.f[i] = f[i].get<float>();
        if (n.id >= g.nextId) g.nextId = n.id + 1;
        g.nodes.push_back(std::move(n));
    }
    for (const auto& e : j.value("links", nlohmann::json::array()))
    {
        if (!e.is_array() || e.size() < 4) continue;
        UIGraphLink l{ e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>() };
        if (g.findNode(l.srcNode) && g.findNode(l.dstNode))
            g.links.push_back(l);
    }
    out = std::move(g);
    return true;
}

// ── Interpreter ──────────────────────────────────────────────────────────────

namespace
{
constexpr int kMaxSteps = 4096; // runaway guard (exec steps + data evals per run)
constexpr int kMaxDepth = 64;   // recursion guard (data eval / nested calls)

UIGraphValue coerce(UIGraphValue v, UIGraphPinType want)
{
    if (v.type == want) return v;
    // Minimal, predictable coercions; anything else falls back to a default.
    UIGraphValue r; r.type = want;
    switch (want)
    {
        case UIGraphPinType::Float:  r.f = v.type == UIGraphPinType::Bool ? (v.b ? 1.0f : 0.0f) : 0.0f; break;
        case UIGraphPinType::Bool:   r.b = v.type == UIGraphPinType::Float ? v.f != 0.0f : false; break;
        case UIGraphPinType::String: break;
        case UIGraphPinType::Vec2:   break;
        case UIGraphPinType::Color:  break;
        default: break;
    }
    return r;
}
} // namespace

UIWidgetGraphRunner::UIWidgetGraphRunner(const UIWidgetGraph& graph, UIWidgetTree& tree,
                                         UIWidgetSelfState& self)
    : m_graph(graph), m_tree(tree), m_self(self)
{
}

const UIGraphLink* UIWidgetGraphRunner::execLinkFrom(int nodeId, int pin) const
{
    for (const auto& l : m_graph.links)
        if (l.srcNode == nodeId && l.srcPin == pin)
            return &l;
    return nullptr;
}

void UIWidgetGraphRunner::fireEvent(UIWidgetEvent ev, int elem, float dt)
{
    m_steps = 0;
    m_tickDt = dt;
    const UIGraphNodeType want =
        ev == UIWidgetEvent::Construct  ? T::EventConstruct :
        ev == UIWidgetEvent::Tick       ? T::EventTick :
        ev == UIWidgetEvent::Click      ? T::EventClick :
        ev == UIWidgetEvent::HoverEnter ? T::EventHoverEnter : T::EventHoverExit;

    for (const auto& n : m_graph.nodes)
    {
        if (n.type != want) continue;
        // Pointer events bind to an element; elem 0 on the node = any element.
        const bool pointerEvent = want == T::EventClick ||
                                  want == T::EventHoverEnter ||
                                  want == T::EventHoverExit;
        if (pointerEvent && n.elem != 0 && n.elem != elem) continue;
        runExecChain(n, pinRanges(n.type).execOut0, 0);
    }
}

bool UIWidgetGraphRunner::callFunction(const std::string& name, bool requirePublic)
{
    for (const auto& n : m_graph.nodes)
    {
        if (n.type != T::FunctionEntry || n.s != name) continue;
        if (requirePublic && n.access != 0) return false; // private: not routable
        m_steps = 0;
        runExecChain(n, pinRanges(n.type).execOut0, 0);
        return true;
    }
    return false;
}

void UIWidgetGraphRunner::runExecChain(const UIGraphNode& from, int execOutPin, int depth)
{
    if (depth > kMaxDepth) return;
    const UIGraphLink* l = execLinkFrom(from.id, execOutPin);
    while (l)
    {
        if (++m_steps > kMaxSteps)
        {
            Logger::Log(Logger::LogLevel::Warning,
                "UIWidgetGraph: execution step limit hit — aborting run");
            return;
        }
        const UIGraphNode* n = m_graph.findNode(l->dstNode);
        if (!n) return;
        execNode(*n, depth);
        // Branch/Sequence steer inside execNode; linear nodes continue on
        // their single exec-out.
        if (n->type == T::Branch || n->type == T::Sequence) return;
        l = execLinkFrom(n->id, pinRanges(n->type).execOut0);
    }
}

void UIWidgetGraphRunner::execNode(const UIGraphNode& n, int depth)
{
    if (depth > kMaxDepth) return;
    switch (n.type)
    {
    case T::Branch:
    {
        const bool cond = evalInput(n, 0, depth + 1).b;
        const PinRanges r = pinRanges(n.type);
        // exec-outs: True = r.execOut0, False = r.execOut0 + 1
        runExecChain(n, r.execOut0 + (cond ? 0 : 1), depth + 1);
        break;
    }
    case T::Sequence:
    {
        const PinRanges r = pinRanges(n.type);
        runExecChain(n, r.execOut0 + 0, depth + 1);
        runExecChain(n, r.execOut0 + 1, depth + 1);
        break;
    }
    case T::SetProperty:
    {
        UIWidgetNode* e = m_tree.findNode(n.elem);
        if (!e) break;
        const UIWidgetProp prop = (UIWidgetProp)n.prop;
        const UIGraphValue v = coerce(evalInput(n, 0, depth + 1), uiGraphPropType(prop));
        switch (prop)
        {
            case UIWidgetProp::Text:      e->text = v.s; break;
            case UIWidgetProp::Color:     for (int i = 0; i < 4; ++i) e->color[i] = v.v4[i]; break;
            case UIWidgetProp::Visible:   e->visible = v.b; break;
            case UIWidgetProp::PositionX: e->posX = v.f; break;
            case UIWidgetProp::PositionY: e->posY = v.f; break;
            case UIWidgetProp::Width:     e->sizeX = v.f; break;
            case UIWidgetProp::Height:    e->sizeY = v.f; break;
            case UIWidgetProp::FontSize:  e->fontSize = v.f; break;
            case UIWidgetProp::HoveredColor: for (int i = 0; i < 4; ++i) e->hoveredColor[i] = v.v4[i]; break;
            case UIWidgetProp::PressedColor: for (int i = 0; i < 4; ++i) e->pressedColor[i] = v.v4[i]; break;
            default: break;
        }
        break;
    }
    case T::ShowSelf: m_self.visible = true;  break;
    case T::HideSelf: m_self.visible = false; break;
    case T::FunctionCall:
    {
        // Inline-run the named function's chain (its access modifier gates
        // only EXTERNAL script calls, not graph-internal ones).
        for (const auto& fn : m_graph.nodes)
            if (fn.type == T::FunctionEntry && fn.s == n.s)
            {
                runExecChain(fn, pinRanges(fn.type).execOut0, depth + 1);
                break;
            }
        break;
    }
    case T::Print:
    {
        const UIGraphValue v = coerce(evalInput(n, 0, depth + 1), UIGraphPinType::String);
        Logger::Log(Logger::LogLevel::Info, ("[Widget] " + v.s).c_str());
        break;
    }
    default:
        break; // pure-data nodes have no exec behavior
    }
}

UIGraphValue UIWidgetGraphRunner::evalInput(const UIGraphNode& n, int dataInIndex, int depth)
{
    const PinRanges r = pinRanges(n.type);
    const int pin = r.dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)
    {
        if (l.dstNode != n.id || l.dstPin != pin) continue;
        const UIGraphNode* src = m_graph.findNode(l.srcNode);
        if (!src) break;
        const PinRanges sr = pinRanges(src->type);
        return evalData(*src, l.srcPin - sr.dataOut0, depth);
    }
    // Unconnected input → the pin type's default (literal defaults live in the
    // node's own payload only for literal node types).
    UIGraphValue v; v.type = dataPinType(n, true, dataInIndex);
    return v;
}

UIGraphValue UIWidgetGraphRunner::evalData(const UIGraphNode& n, int dataOutPin, int depth)
{
    if (depth > kMaxDepth || ++m_steps > kMaxSteps) return {};
    switch (n.type)
    {
    case T::EventTick:   return UIGraphValue::ofFloat(m_tickDt);
    case T::ConstFloat:  return UIGraphValue::ofFloat(n.f[0]);
    case T::ConstBool:   return UIGraphValue::ofBool(n.f[0] != 0.0f);
    case T::ConstString: return UIGraphValue::ofString(n.s);
    case T::ConstVec2:   return UIGraphValue::ofVec2({ n.f[0], n.f[1] });
    case T::ConstColor:  return UIGraphValue::ofColor({ n.f[0], n.f[1], n.f[2], n.f[3] });
    case T::GetProperty:
    {
        const UIWidgetNode* e = m_tree.findNode(n.elem);
        const UIWidgetProp prop = (UIWidgetProp)n.prop;
        if (!e) { UIGraphValue v; v.type = uiGraphPropType(prop); return v; }
        switch (prop)
        {
            case UIWidgetProp::Text:      return UIGraphValue::ofString(e->text);
            case UIWidgetProp::Color:     return UIGraphValue::ofColor({ e->color[0], e->color[1], e->color[2], e->color[3] });
            case UIWidgetProp::Visible:   return UIGraphValue::ofBool(e->visible);
            case UIWidgetProp::PositionX: return UIGraphValue::ofFloat(e->posX);
            case UIWidgetProp::PositionY: return UIGraphValue::ofFloat(e->posY);
            case UIWidgetProp::Width:     return UIGraphValue::ofFloat(e->sizeX);
            case UIWidgetProp::Height:    return UIGraphValue::ofFloat(e->sizeY);
            case UIWidgetProp::FontSize:  return UIGraphValue::ofFloat(e->fontSize);
            case UIWidgetProp::HoveredColor: return UIGraphValue::ofColor({ e->hoveredColor[0], e->hoveredColor[1], e->hoveredColor[2], e->hoveredColor[3] });
            case UIWidgetProp::PressedColor: return UIGraphValue::ofColor({ e->pressedColor[0], e->pressedColor[1], e->pressedColor[2], e->pressedColor[3] });
            default: return {};
        }
    }
    case T::Add:      return UIGraphValue::ofFloat(evalInput(n, 0, depth + 1).f + evalInput(n, 1, depth + 1).f);
    case T::Subtract: return UIGraphValue::ofFloat(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f);
    case T::Multiply: return UIGraphValue::ofFloat(evalInput(n, 0, depth + 1).f * evalInput(n, 1, depth + 1).f);
    case T::Divide:
    {
        const float b = evalInput(n, 1, depth + 1).f;
        return UIGraphValue::ofFloat(b != 0.0f ? evalInput(n, 0, depth + 1).f / b : 0.0f);
    }
    case T::Greater:  return UIGraphValue::ofBool(evalInput(n, 0, depth + 1).f >  evalInput(n, 1, depth + 1).f);
    case T::Less:     return UIGraphValue::ofBool(evalInput(n, 0, depth + 1).f <  evalInput(n, 1, depth + 1).f);
    case T::Equals:   return UIGraphValue::ofBool(std::fabs(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f) < 1e-6f);
    case T::And:      return UIGraphValue::ofBool(evalInput(n, 0, depth + 1).b && evalInput(n, 1, depth + 1).b);
    case T::Or:       return UIGraphValue::ofBool(evalInput(n, 0, depth + 1).b || evalInput(n, 1, depth + 1).b);
    case T::Not:      return UIGraphValue::ofBool(!evalInput(n, 0, depth + 1).b);
    case T::Concat:   return UIGraphValue::ofString(evalInput(n, 0, depth + 1).s + evalInput(n, 1, depth + 1).s);
    case T::ToString:
    {
        char buf[48];
        const float v = evalInput(n, 0, depth + 1).f;
        // Trim trailing zeros for HUD-friendly numbers ("3", "3.5").
        std::snprintf(buf, sizeof buf, "%g", v);
        return UIGraphValue::ofString(buf);
    }
    default:
        (void)dataOutPin;
        return {};
    }
}

} // namespace HE
