#include <HorizonCode/HorizonCode.h>
#include <Diagnostics/Logger.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

namespace HorizonCode {

using T = NodeType;
using P = PinType;

// ── Node signatures ──────────────────────────────────────────────────────────

NodeSig signatureOf(const Node& n)
{
    NodeSig s;
    switch (n.type)
    {
    case T::Event:
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataOuts = { { "Value", n.propType } };
        break;
    case T::FunctionEntry:
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params) s.dataOuts.push_back({ p.name.c_str(), p.type });
        break;
    case T::FunctionCall:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params)  s.dataIns.push_back({ p.name.c_str(), p.type });
        for (const auto& r : n.results) s.dataOuts.push_back({ r.name.c_str(), r.type });
        break;
    case T::FunctionReturn:
        s.execIns = { { "", P::Exec } };
        for (const auto& r : n.results) s.dataIns.push_back({ r.name.c_str(), r.type });
        break;
    case T::Branch:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "True", P::Exec }, { "False", P::Exec } };
        s.dataIns  = { { "Cond", P::Bool } };
        break;
    case T::Sequence:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Then 0", P::Exec }, { "Then 1", P::Exec } };
        break;
    case T::GetProperty:
        s.dataOuts = { { "Value", n.propType } };
        break;
    case T::SetProperty:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType } };
        s.dataOuts = { { "Value", n.propType } }; // pass the set value through
        break;
    case T::GetVariable:
        s.dataOuts = { { "Value", n.propType } };
        break;
    case T::SetVariable:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType } };
        s.dataOuts = { { "Value", n.propType } }; // pass the set value through
        break;
    case T::ShowWidget:
    case T::HideWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        break;
    case T::CreateWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataOuts = { { "Widget", P::Ref } };
        break;
    case T::ShowWidgetId:
    case T::HideWidgetId:
    case T::DestroyWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Widget", P::Ref } };
        break;
    case T::CreateObject:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataOuts = { { "Object", P::Ref } };
        break;
    case T::DestroyObject:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Object", P::Ref } };
        break;
    case T::GetExternal:
        s.dataIns  = { { "Target", P::Ref } };
        s.dataOuts = { { "Value", n.propType } };
        break;
    case T::SetExternal:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref }, { "Value", n.propType } };
        s.dataOuts = { { "Value", n.propType } }; // pass the set value through
        break;
    case T::BindEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref } };
        break;
    case T::CallExternal:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref } };
        for (const auto& p : n.params)  s.dataIns.push_back({ p.name.c_str(), p.type });
        for (const auto& r : n.results) s.dataOuts.push_back({ r.name.c_str(), r.type });
        break;
    case T::EmitEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataIns = { { "Arg", n.propType } };
        break;
    case T::GetGameInstance: s.dataOuts = { { "Game Instance", P::Ref } }; break;
    case T::GetSelf:         s.dataOuts = { { "Self", P::Ref } };          break;
    case T::ConstFloat:  s.dataOuts = { { "", P::Float } };  break;
    case T::ConstBool:   s.dataOuts = { { "", P::Bool } };   break;
    case T::ConstInt:    s.dataOuts = { { "", P::Int } };    break;
    case T::ConstString: s.dataOuts = { { "", P::String } }; break;
    case T::ConstVec2:   s.dataOuts = { { "", P::Vec2 } };   break;
    case T::ConstColor:  s.dataOuts = { { "", P::Color } };  break;
    case T::Add: case T::Subtract: case T::Multiply: case T::Divide:
        s.dataIns  = { { "A", P::Float }, { "B", P::Float } };
        s.dataOuts = { { "", P::Float } };
        break;
    case T::Greater: case T::Less: case T::Equals:
        s.dataIns  = { { "A", P::Float }, { "B", P::Float } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::And: case T::Or:
        s.dataIns  = { { "A", P::Bool }, { "B", P::Bool } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::Not:
        s.dataIns  = { { "", P::Bool } };
        s.dataOuts = { { "", P::Bool } };
        break;
    case T::Concat:
        s.dataIns  = { { "A", P::String }, { "B", P::String } };
        s.dataOuts = { { "", P::String } };
        break;
    case T::ToString:
        s.dataIns  = { { "", P::Float } };
        s.dataOuts = { { "", P::String } };
        break;
    case T::Print:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "", P::String } };
        break;
    default: break;
    }
    return s;
}

const char* nodeDisplayName(NodeType t)
{
    switch (t)
    {
        case T::Event:        return "Event";
        case T::FunctionEntry:return "Function";
        case T::FunctionCall: return "Call Function";
        case T::Branch:       return "Branch";
        case T::Sequence:     return "Sequence";
        case T::GetProperty:  return "Get Property";
        case T::SetProperty:  return "Set Property";
        case T::GetVariable:  return "Get Variable";
        case T::SetVariable:  return "Set Variable";
        case T::ShowWidget:   return "Show Self";
        case T::HideWidget:   return "Hide Self";
        case T::CreateWidget: return "Create Widget";
        case T::ShowWidgetId: return "Show Widget";
        case T::HideWidgetId: return "Hide Widget";
        case T::DestroyWidget:return "Destroy Widget";
        case T::CreateObject: return "Create Object";
        case T::DestroyObject:return "Destroy Object";
        case T::GetExternal:  return "Get (Ref)";
        case T::SetExternal:  return "Set (Ref)";
        case T::ConstFloat:   return "Float";
        case T::ConstBool:    return "Bool";
        case T::ConstInt:     return "Int";
        case T::ConstString:  return "String";
        case T::ConstVec2:    return "Vec2";
        case T::ConstColor:   return "Color";
        case T::Add:          return "Add";
        case T::Subtract:     return "Subtract";
        case T::Multiply:     return "Multiply";
        case T::Divide:       return "Divide";
        case T::Greater:      return "Greater";
        case T::Less:         return "Less";
        case T::Equals:       return "Equals";
        case T::And:          return "And";
        case T::Or:           return "Or";
        case T::Not:          return "Not";
        case T::Concat:       return "Concat";
        case T::ToString:     return "To String";
        case T::BindEvent:      return "Bind Event";
        case T::EmitEvent:      return "Emit Event";
        case T::CallExternal:   return "Call Function (Ref)";
        case T::GetGameInstance:return "Get Game Instance";
        case T::GetSelf:        return "Get Self";
        case T::Print:        return "Print";
        case T::FunctionReturn:return "Return";
        default:              return "?";
    }
}

const char* nodeCategory(NodeType t)
{
    switch (t)
    {
        case T::Event:         return "Events";
        case T::FunctionEntry: return "Functions";
        case T::FunctionCall:  return "Functions";
        case T::FunctionReturn:return "Functions";
        case T::Branch:
        case T::Sequence:      return "Flow";
        case T::GetProperty:
        case T::SetProperty:   return "Property";
        case T::GetVariable:
        case T::SetVariable:   return "Variables";
        case T::ShowWidget:
        case T::HideWidget:    return "Widget";
        case T::CreateWidget:
        case T::ShowWidgetId:
        case T::HideWidgetId:
        case T::DestroyWidget: return "UI";
        case T::ConstFloat: case T::ConstBool: case T::ConstInt:
        case T::ConstString: case T::ConstVec2: case T::ConstColor: return "Literals";
        case T::Add: case T::Subtract: case T::Multiply: case T::Divide:
        case T::Greater: case T::Less: case T::Equals: return "Math";
        case T::And: case T::Or: case T::Not: return "Logic";
        case T::Concat: case T::ToString: return "String";
        case T::BindEvent:
        case T::EmitEvent:      return "Events";
        case T::CallExternal:
        case T::GetGameInstance:
        case T::GetSelf:
        case T::CreateObject:
        case T::DestroyObject:
        case T::GetExternal:
        case T::SetExternal:    return "Reference";
        case T::Print: return "Debug";
        default: return "Misc";
    }
}

const std::vector<NodeType>& nodeRegistry()
{
    static const std::vector<NodeType> kAll = []
    {
        std::vector<NodeType> v;
        for (int i = 0; i < (int)T::COUNT; ++i) v.push_back((NodeType)i);
        return v;
    }();
    return kAll;
}

// ── Pin ranges (unified index space: [execIns][execOuts][dataIns][dataOuts]) ──

namespace
{
struct PinRanges { int execIn0, execOut0, dataIn0, dataOut0, end; };
PinRanges pinRanges(const Node& n)
{
    const NodeSig s = signatureOf(n);
    PinRanges r;
    r.execIn0  = 0;
    r.execOut0 = r.execIn0  + (int)s.execIns.size();
    r.dataIn0  = r.execOut0 + (int)s.execOuts.size();
    r.dataOut0 = r.dataIn0  + (int)s.dataIns.size();
    r.end      = r.dataOut0 + (int)s.dataOuts.size();
    return r;
}
PinType dataPinType(const Node& n, bool input, int index)
{
    const NodeSig s = signatureOf(n);
    const auto& pins = input ? s.dataIns : s.dataOuts;
    if (index < 0 || index >= (int)pins.size()) return P::Float;
    return pins[index].type;
}
} // namespace

// ── Graph container ──────────────────────────────────────────────────────────

Node*       Graph::findNode(int id)       { for (auto& n : nodes) if (n.id == id) return &n; return nullptr; }
const Node* Graph::findNode(int id) const { for (const auto& n : nodes) if (n.id == id) return &n; return nullptr; }

int Graph::addNode(Node n)
{
    n.id = nextId++;
    nodes.push_back(std::move(n));
    return nodes.back().id;
}

void Graph::removeNode(int id)
{
    nodes.erase(std::remove_if(nodes.begin(), nodes.end(),
        [&](const Node& n){ return n.id == id; }), nodes.end());
    links.erase(std::remove_if(links.begin(), links.end(),
        [&](const Link& l){ return l.srcNode == id || l.dstNode == id; }), links.end());
}

Variable*       Graph::findVariable(const std::string& name)
{ for (auto& v : variables) if (v.name == name) return &v; return nullptr; }
const Variable* Graph::findVariable(const std::string& name) const
{ for (const auto& v : variables) if (v.name == name) return &v; return nullptr; }

Value variableDefaultValue(const Variable& v)
{
    switch (v.type)
    {
        case P::Float:  return Value::ofFloat(v.f[0]);
        case P::Bool:   return Value::ofBool(v.f[0] != 0.0f);
        case P::Int:    return Value::ofInt((int)v.f[0]);
        case P::String: return Value::ofString(v.s);
        case P::Vec2:   return Value::ofVec2({ v.f[0], v.f[1] });
        case P::Color:  return Value::ofColor({ v.f[0], v.f[1], v.f[2], v.f[3] });
        case P::Ref:    return Value::ofRef(0);
        default:        return Value::ofFloat(v.f[0]);
    }
}

bool Graph::connect(int srcNode, int srcPin, int dstNode, int dstPin)
{
    const Node* s = findNode(srcNode);
    const Node* d = findNode(dstNode);
    if (!s || !d || srcNode == dstNode) return false;

    const PinRanges sr = pinRanges(*s);
    const PinRanges dr = pinRanges(*d);

    const bool srcIsExecOut = srcPin >= sr.execOut0 && srcPin < sr.dataIn0;
    const bool srcIsDataOut = srcPin >= sr.dataOut0 && srcPin < sr.end;
    const bool dstIsExecIn  = dstPin >= dr.execIn0  && dstPin < dr.execOut0;
    const bool dstIsDataIn  = dstPin >= dr.dataIn0  && dstPin < dr.dataOut0;

    if (srcIsExecOut && dstIsExecIn)
    {
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l){ return l.srcNode == srcNode && l.srcPin == srcPin; }), links.end());
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    if (srcIsDataOut && dstIsDataIn)
    {
        if (dataPinType(*s, false, srcPin - sr.dataOut0) != dataPinType(*d, true, dstPin - dr.dataIn0))
            return false;
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l){ return l.dstNode == dstNode && l.dstPin == dstPin; }), links.end());
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    return false;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

std::string toJson(const Graph& g)
{
    nlohmann::json j;
    j["nextId"] = g.nextId;
    nlohmann::json jn = nlohmann::json::array();
    for (const auto& n : g.nodes)
    {
        nlohmann::json e = {
            { "id",   n.id },
            { "type", nodeDisplayName(n.type) }, // by name → schema-evolution safe
            { "pos",  { n.x, n.y } },
        };
        if (n.elem)              e["elem"]     = n.elem;
        if (!n.s.empty())        e["s"]        = n.s;
        if (n.propType != P::Float) e["propType"] = (int)n.propType;
        if (n.hasArg)            e["hasArg"]   = n.hasArg;
        if (n.access)            e["access"]   = n.access;
        if (n.f[0] || n.f[1] || n.f[2] || n.f[3])
            e["f"] = { n.f[0], n.f[1], n.f[2], n.f[3] };
        auto dumpParams = [](const std::vector<FuncParam>& ps)
        {
            nlohmann::json a = nlohmann::json::array();
            for (const auto& p : ps) a.push_back({ { "name", p.name }, { "type", (int)p.type } });
            return a;
        };
        if (!n.params.empty())  e["params"]  = dumpParams(n.params);
        if (!n.results.empty()) e["results"] = dumpParams(n.results);
        jn.push_back(std::move(e));
    }
    j["nodes"] = std::move(jn);

    nlohmann::json jl = nlohmann::json::array();
    for (const auto& l : g.links)
        jl.push_back({ l.srcNode, l.srcPin, l.dstNode, l.dstPin });
    j["links"] = std::move(jl);

    nlohmann::json jv = nlohmann::json::array();
    for (const auto& v : g.variables)
    {
        nlohmann::json e = { { "name", v.name }, { "type", (int)v.type } };
        if (v.f[0] || v.f[1] || v.f[2] || v.f[3]) e["f"] = { v.f[0], v.f[1], v.f[2], v.f[3] };
        if (!v.s.empty()) e["s"] = v.s;
        if (v.access)     e["access"] = v.access;
        if (!v.className.empty()) e["className"] = v.className;
        jv.push_back(std::move(e));
    }
    j["variables"] = std::move(jv);
    return j.dump(2);
}

bool fromJson(const std::string& json, Graph& out)
{
    nlohmann::json j = nlohmann::json::parse(json, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return false;

    Graph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& e : j.value("nodes", nlohmann::json::array()))
    {
        Node n;
        n.id = e.value("id", 0);
        const std::string typeName = e.value("type", std::string());
        bool known = false;
        for (NodeType t : nodeRegistry())
            if (typeName == nodeDisplayName(t)) { n.type = t; known = true; break; }
        if (!known) continue;
        if (const auto& p = e.value("pos", nlohmann::json::array()); p.size() >= 2)
        { n.x = p[0].get<float>(); n.y = p[1].get<float>(); }
        n.elem     = e.value("elem", 0);
        n.s        = e.value("s", std::string());
        n.propType = (PinType)e.value("propType", (int)P::Float);
        n.hasArg   = e.value("hasArg", false);
        n.access   = e.value("access", 0);
        if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
            for (int i = 0; i < 4; ++i) n.f[i] = f[i].get<float>();
        auto loadParams = [](const nlohmann::json& a, std::vector<FuncParam>& ps)
        {
            for (const auto& pe : a)
            {
                FuncParam p;
                p.name = pe.value("name", std::string());
                p.type = (PinType)pe.value("type", (int)P::Float);
                ps.push_back(std::move(p));
            }
        };
        loadParams(e.value("params",  nlohmann::json::array()), n.params);
        loadParams(e.value("results", nlohmann::json::array()), n.results);
        if (n.id >= g.nextId) g.nextId = n.id + 1;
        g.nodes.push_back(std::move(n));
    }
    for (const auto& e : j.value("links", nlohmann::json::array()))
    {
        if (!e.is_array() || e.size() < 4) continue;
        Link l{ e[0].get<int>(), e[1].get<int>(), e[2].get<int>(), e[3].get<int>() };
        if (g.findNode(l.srcNode) && g.findNode(l.dstNode))
            g.links.push_back(l);
    }
    for (const auto& e : j.value("variables", nlohmann::json::array()))
    {
        Variable v;
        v.name = e.value("name", std::string());
        if (v.name.empty()) continue;
        v.type = (PinType)e.value("type", (int)P::Float);
        v.s    = e.value("s", std::string());
        v.access = e.value("access", 0);
        v.className = e.value("className", std::string());
        if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
            for (int i = 0; i < 4; ++i) v.f[i] = f[i].get<float>();
        g.variables.push_back(std::move(v));
    }
    syncFunctionSignatures(g); // reconcile call/return pins with their entries
    out = std::move(g);
    return true;
}

void syncFunctionSignatures(Graph& g)
{
    // The FunctionEntry owns each function's interface; mirror it onto the calls
    // and returns of the same name so their pins match. Calls/returns whose
    // function lives in another graph (no local entry) keep their own mirror.
    for (Node& n : g.nodes)
    {
        if (n.type != NodeType::FunctionCall && n.type != NodeType::FunctionReturn) continue;
        const Node* entry = nullptr;
        for (const Node& e : g.nodes)
            if (e.type == NodeType::FunctionEntry && e.s == n.s) { entry = &e; break; }
        if (!entry) continue;
        n.params  = entry->params;
        n.results = entry->results;
    }
}

// ── Interpreter ──────────────────────────────────────────────────────────────

namespace
{
constexpr int kMaxSteps = 4096;
constexpr int kMaxDepth = 64;

Value coerce(Value v, PinType want)
{
    if (v.type == want) return v;
    Value r; r.type = want;
    switch (want)
    {
        case P::Float:  r.f = v.type == P::Bool ? (v.b ? 1.0f : 0.0f)
                            : v.type == P::Int ? (float)v.i : 0.0f; break;
        case P::Int:    r.i = v.type == P::Float ? (int)v.f
                            : v.type == P::Bool ? (v.b ? 1 : 0) : 0; break;
        case P::Bool:   r.b = v.type == P::Float ? v.f != 0.0f
                            : v.type == P::Int ? v.i != 0 : false; break;
        default: break;
    }
    return r;
}
} // namespace

Runner::Runner(const Graph& graph, Context ctx) : m_graph(graph), m_ctx(std::move(ctx)) {}

const Link* Runner::execLinkFrom(int nodeId, int pin) const
{
    for (const auto& l : m_graph.links)
        if (l.srcNode == nodeId && l.srcPin == pin) return &l;
    return nullptr;
}

void Runner::fireEvent(const std::string& eventName, int elem, const Value& arg)
{
    m_steps = 0;
    m_eventArg = arg;
    m_execOutputs.clear();
    m_callStack.clear();
    for (const auto& n : m_graph.nodes)
    {
        if (n.type != T::Event || n.s != eventName) continue;
        if (n.elem != 0 && n.elem != elem) continue;
        runExecChain(n, pinRanges(n).execOut0, 0);
    }
}

bool Runner::callFunction(const std::string& name, bool requirePublic,
                          const std::vector<Value>& args, std::vector<Value>* results)
{
    for (const auto& n : m_graph.nodes)
    {
        if (n.type != T::FunctionEntry || n.s != name) continue;
        if (requirePublic && n.access != 0) return false;
        m_steps = 0;
        m_execOutputs.clear();
        m_callStack.clear();
        // Seed the frame: passed args coerced to the param types (missing ones fall
        // back to a typed default), and typed result slots for any Return to fill.
        CallFrame frame;
        frame.args.resize(n.params.size());
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            if (i < args.size()) frame.args[i] = coerce(args[i], n.params[i].type);
            else                 frame.args[i].type = n.params[i].type; // typed default
        }
        frame.results.resize(n.results.size());
        for (size_t i = 0; i < n.results.size(); ++i) frame.results[i].type = n.results[i].type;
        m_callStack.push_back(std::move(frame));
        runExecChain(n, pinRanges(n).execOut0, 0);
        if (results) *results = m_callStack.back().results;
        m_callStack.pop_back();
        return true;
    }
    return false;
}

void Runner::runExecChain(const Node& from, int execOutPin, int depth)
{
    if (depth > kMaxDepth) return;
    const Link* l = execLinkFrom(from.id, execOutPin);
    while (l)
    {
        if (++m_steps > kMaxSteps)
        {
            Logger::Log(Logger::LogLevel::Warning,
                "HorizonCode: execution step limit hit — aborting run");
            return;
        }
        const Node* n = m_graph.findNode(l->dstNode);
        if (!n) return;
        execNode(*n, depth);
        if (n->type == T::Branch || n->type == T::Sequence) return; // they steer internally
        l = execLinkFrom(n->id, pinRanges(*n).execOut0);
    }
}

void Runner::execNode(const Node& n, int depth)
{
    if (depth > kMaxDepth) return;
    switch (n.type)
    {
    case T::Branch:
    {
        const bool cond = evalInput(n, 0, depth + 1).b;
        const PinRanges r = pinRanges(n);
        runExecChain(n, r.execOut0 + (cond ? 0 : 1), depth + 1);
        break;
    }
    case T::Sequence:
    {
        const PinRanges r = pinRanges(n);
        runExecChain(n, r.execOut0 + 0, depth + 1);
        runExecChain(n, r.execOut0 + 1, depth + 1);
        break;
    }
    case T::SetProperty:
        if (m_ctx.setProperty)
            m_ctx.setProperty(n.elem, n.s, coerce(evalInput(n, 0, depth + 1), n.propType));
        break;
    case T::SetVariable:
        if (m_ctx.setVariable)
            m_ctx.setVariable(n.s, coerce(evalInput(n, 0, depth + 1), n.propType));
        break;
    case T::ShowWidget: if (m_ctx.showSelf) m_ctx.showSelf(); break;
    case T::HideWidget: if (m_ctx.hideSelf) m_ctx.hideSelf(); break;
    case T::CreateWidget:
    {
        // The widget id doubles as its runtime reference (widget id == scriptId),
        // so a created widget is a first-class Ref object.
        const int id = m_ctx.createWidget ? m_ctx.createWidget(n.s) : 0;
        m_execOutputs[n.id] = { Value::ofRef((uint32_t)id) }; // cached for the data output
        break;
    }
    case T::ShowWidgetId:  if (m_ctx.showWidget)    m_ctx.showWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::HideWidgetId:  if (m_ctx.hideWidget)    m_ctx.hideWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::DestroyWidget: if (m_ctx.destroyWidget) m_ctx.destroyWidget((int)evalInput(n, 0, depth + 1).ref); break;
    case T::CreateObject:
    {
        const uint32_t ref = m_ctx.createObject ? m_ctx.createObject(n.s) : 0u;
        m_execOutputs[n.id] = { Value::ofRef(ref) }; // cached for the data output
        break;
    }
    case T::DestroyObject: if (m_ctx.destroyObject) m_ctx.destroyObject(evalInput(n, 0, depth + 1).ref); break;
    case T::SetExternal:
        if (m_ctx.setExternal)
            m_ctx.setExternal(evalInput(n, 0, depth + 1).ref, n.s,
                              coerce(evalInput(n, 1, depth + 1), n.propType));
        break;
    case T::BindEvent:
        if (m_ctx.bindEvent)
            m_ctx.bindEvent(evalInput(n, 0, depth + 1).ref, n.s);
        break;
    case T::EmitEvent:
        if (m_ctx.emitEvent)
            m_ctx.emitEvent(n.s, n.hasArg ? coerce(evalInput(n, 0, depth + 1), n.propType) : Value{});
        break;
    case T::CallExternal:
        if (m_ctx.callExternal)
        {
            // dataIn 0 = Target (Ref); 1.. = the callee's parameters.
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i + 1, depth + 1), n.params[i].type);
            m_execOutputs[n.id] = m_ctx.callExternal(evalInput(n, 0, depth + 1).ref, n.s, args);
        }
        break;
    case T::FunctionCall:
    {
        const Node* entry = nullptr;
        for (const auto& fn : m_graph.nodes)
            if (fn.type == T::FunctionEntry && fn.s == n.s) { entry = &fn; break; }
        if (!entry) break;
        // Build the call frame: evaluate arguments in the CALLER's context (before
        // pushing, so the caller's own params still resolve), seed typed results.
        CallFrame frame;
        frame.args.resize(n.params.size());
        for (size_t i = 0; i < n.params.size(); ++i)
            frame.args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
        frame.results.resize(n.results.size());
        for (size_t i = 0; i < n.results.size(); ++i) frame.results[i].type = n.results[i].type;
        m_callStack.push_back(std::move(frame));
        runExecChain(*entry, pinRanges(*entry).execOut0, depth + 1);
        // Cache the returned values as this call's data outputs, then pop.
        m_execOutputs[n.id] = std::move(m_callStack.back().results);
        m_callStack.pop_back();
        break;
    }
    case T::FunctionReturn:
        // Write the current invocation's return values (read back by the call).
        if (!m_callStack.empty())
        {
            CallFrame& f = m_callStack.back();
            for (size_t i = 0; i < n.results.size() && i < f.results.size(); ++i)
                f.results[i] = coerce(evalInput(n, (int)i, depth + 1), n.results[i].type);
        }
        break;
    case T::Print:
        Logger::Log(Logger::LogLevel::Info,
            ("[Widget] " + coerce(evalInput(n, 0, depth + 1), P::String).s).c_str());
        break;
    default: break;
    }
}

Value Runner::evalInput(const Node& n, int dataInIndex, int depth)
{
    const PinRanges r = pinRanges(n);
    const int pin = r.dataIn0 + dataInIndex;
    for (const auto& l : m_graph.links)
    {
        if (l.dstNode != n.id || l.dstPin != pin) continue;
        const Node* src = m_graph.findNode(l.srcNode);
        if (!src) break;
        return evalData(*src, l.srcPin - pinRanges(*src).dataOut0, depth);
    }
    Value v; v.type = dataPinType(n, true, dataInIndex);
    return v;
}

Value Runner::evalData(const Node& n, int dataOutPin, int depth)
{
    if (depth > kMaxDepth || ++m_steps > kMaxSteps) return {};
    switch (n.type)
    {
    case T::Event:       return coerce(m_eventArg, n.propType);
    case T::ConstFloat:  return Value::ofFloat(n.f[0]);
    case T::ConstBool:   return Value::ofBool(n.f[0] != 0.0f);
    case T::ConstInt:    return Value::ofInt((int)n.f[0]);
    case T::ConstString: return Value::ofString(n.s);
    case T::ConstVec2:   return Value::ofVec2({ n.f[0], n.f[1] });
    case T::ConstColor:  return Value::ofColor({ n.f[0], n.f[1], n.f[2], n.f[3] });
    case T::GetProperty:
    {
        Value v = m_ctx.getProperty ? m_ctx.getProperty(n.elem, n.s) : Value{};
        return coerce(v, n.propType);
    }
    case T::GetVariable:
    {
        Value v = m_ctx.getVariable ? m_ctx.getVariable(n.s) : Value{};
        return coerce(v, n.propType);
    }
    // Set-node pass-through: the value output re-reads the Value input (like a
    // C++ assignment expression returning the assigned value). No side effect.
    case T::SetVariable:
    case T::SetProperty:
        return coerce(evalInput(n, 0, depth + 1), n.propType);
    case T::SetExternal:
        return coerce(evalInput(n, 1, depth + 1), n.propType); // dataIn 1 = Value (0 = Target)
    case T::GetGameInstance: return m_ctx.getGameInstance ? m_ctx.getGameInstance() : Value::ofRef(0);
    case T::GetSelf:         return m_ctx.getSelf ? m_ctx.getSelf() : Value::ofRef(0);
    case T::CreateWidget:
    case T::CreateObject:
    {
        // Return the ref produced when this node ran (don't create again).
        auto it = m_execOutputs.find(n.id);
        return (it != m_execOutputs.end() && !it->second.empty()) ? it->second[0] : Value::ofRef(0);
    }
    case T::FunctionEntry:
    {
        // A function's input parameter: read it from the active call frame.
        if (!m_callStack.empty() && dataOutPin >= 0 &&
            dataOutPin < (int)m_callStack.back().args.size())
            return m_callStack.back().args[dataOutPin];
        return {};
    }
    case T::FunctionCall:
    case T::CallExternal:
    {
        // A (local or cross-instance) call's return value: read the cached results.
        auto it = m_execOutputs.find(n.id);
        if (it != m_execOutputs.end() && dataOutPin >= 0 && dataOutPin < (int)it->second.size())
            return it->second[dataOutPin];
        return {};
    }
    case T::GetExternal:
    {
        Value v = m_ctx.getExternal ? m_ctx.getExternal(evalInput(n, 0, depth + 1).ref, n.s) : Value{};
        return coerce(v, n.propType);
    }
    case T::Add:      return Value::ofFloat(evalInput(n, 0, depth + 1).f + evalInput(n, 1, depth + 1).f);
    case T::Subtract: return Value::ofFloat(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f);
    case T::Multiply: return Value::ofFloat(evalInput(n, 0, depth + 1).f * evalInput(n, 1, depth + 1).f);
    case T::Divide:
    {
        const float b = evalInput(n, 1, depth + 1).f;
        return Value::ofFloat(b != 0.0f ? evalInput(n, 0, depth + 1).f / b : 0.0f);
    }
    case T::Greater:  return Value::ofBool(evalInput(n, 0, depth + 1).f >  evalInput(n, 1, depth + 1).f);
    case T::Less:     return Value::ofBool(evalInput(n, 0, depth + 1).f <  evalInput(n, 1, depth + 1).f);
    case T::Equals:   return Value::ofBool(std::fabs(evalInput(n, 0, depth + 1).f - evalInput(n, 1, depth + 1).f) < 1e-6f);
    case T::And:      return Value::ofBool(evalInput(n, 0, depth + 1).b && evalInput(n, 1, depth + 1).b);
    case T::Or:       return Value::ofBool(evalInput(n, 0, depth + 1).b || evalInput(n, 1, depth + 1).b);
    case T::Not:      return Value::ofBool(!evalInput(n, 0, depth + 1).b);
    case T::Concat:   return Value::ofString(evalInput(n, 0, depth + 1).s + evalInput(n, 1, depth + 1).s);
    case T::ToString:
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "%g", evalInput(n, 0, depth + 1).f);
        return Value::ofString(buf);
    }
    default:
        (void)dataOutPin;
        return {};
    }
}

} // namespace HorizonCode
