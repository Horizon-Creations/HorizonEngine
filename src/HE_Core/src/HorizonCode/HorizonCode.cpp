#include <HorizonCode/HorizonCode.h>
#include <Diagnostics/Logger.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <unordered_set>
#include <vector>

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
        for (const auto& p : n.params) s.dataOuts.push_back({ p.name.c_str(), p.type, p.isArray });
        break;
    case T::FunctionCall:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params)  s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray });
        for (const auto& r : n.results) s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray });
        break;
    case T::FunctionReturn:
        s.execIns = { { "", P::Exec } };
        for (const auto& r : n.results) s.dataIns.push_back({ r.name.c_str(), r.type, r.isArray });
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
        s.dataOuts = { { "Value", n.propType, n.isArray } };
        break;
    case T::SetVariable:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType, n.isArray } };
        s.dataOuts = { { "Value", n.propType, n.isArray } }; // pass the set value through
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
        for (const auto& p : n.params)  s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray });
        for (const auto& r : n.results) s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray });
        break;
    case T::EmitEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataIns = { { "Arg", n.propType } };
        break;
    case T::EngineCall:
        // hasArg = the registry entry's isExec: side-effecting calls get exec pins,
        // pure calls (getters/math) are compact data nodes. params → data-ins,
        // results → data-outs (mirrored on the node from the ApiFn descriptor).
        if (n.hasArg) { s.execIns = { { "", P::Exec } }; s.execOuts = { { "", P::Exec } }; }
        for (const auto& p : n.params)  s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray });
        for (const auto& r : n.results) s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray });
        break;
    case T::GetGameInstance: s.dataOuts = { { "Game Instance", P::Ref } }; break;
    case T::GetSelf:         s.dataOuts = { { "Self", P::Ref } };          break;
    case T::ConstFloat:  s.dataOuts = { { "", P::Float } };  break;
    case T::ConstBool:   s.dataOuts = { { "", P::Bool } };   break;
    case T::ConstInt:    s.dataOuts = { { "", P::Int } };    break;
    case T::ConstString: s.dataOuts = { { "", P::String } }; break;
    case T::ConstVec2:   s.dataOuts = { { "", P::Vec2 } };   break;
    case T::ConstColor:  s.dataOuts = { { "", P::Color } };  break;
    case T::ConstTransform: s.dataOuts = { { "", P::Transform } }; break;
    case T::ArrayMake:
        s.dataOuts = { { "Array", n.propType, true } };
        break;
    case T::ArrayLength:
        s.dataIns  = { { "Array", n.propType, true } };
        s.dataOuts = { { "Length", P::Int } };
        break;
    case T::ArrayGet:
        s.dataIns  = { { "Array", n.propType, true }, { "Index", P::Int } };
        s.dataOuts = { { "Element", n.propType } };
        break;
    case T::ArrayAdd:
        s.dataIns  = { { "Array", n.propType, true }, { "Value", n.propType } };
        s.dataOuts = { { "Array", n.propType, true } };
        break;
    case T::ArraySet:
        s.dataIns  = { { "Array", n.propType, true }, { "Index", P::Int }, { "Value", n.propType } };
        s.dataOuts = { { "Array", n.propType, true } };
        break;
    case T::ArrayInsert:
        s.dataIns  = { { "Array", n.propType, true }, { "Index", P::Int }, { "Value", n.propType } };
        s.dataOuts = { { "Array", n.propType, true } };
        break;
    case T::ArrayRemove:
        s.dataIns  = { { "Array", n.propType, true }, { "Index", P::Int } };
        s.dataOuts = { { "Array", n.propType, true } };
        break;
    case T::ArrayContains:
        s.dataIns  = { { "Array", n.propType, true }, { "Value", n.propType } };
        s.dataOuts = { { "Contains", P::Bool } };
        break;
    case T::ArrayIndexOf:
        s.dataIns  = { { "Array", n.propType, true }, { "Value", n.propType } };
        s.dataOuts = { { "Index", P::Int } };
        break;
    case T::ForEach:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Body", P::Exec }, { "Done", P::Exec } };
        s.dataIns  = { { "Array", n.propType, true } };
        s.dataOuts = { { "Element", n.propType }, { "Index", P::Int } };
        break;
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
        case T::ConstTransform: return "Transform";
        case T::ArrayMake:    return "Make Array";
        case T::ArrayLength:  return "Array Length";
        case T::ArrayGet:     return "Array Get";
        case T::ArrayAdd:     return "Array Append";
        case T::ArraySet:     return "Array Set";
        case T::ArrayInsert:  return "Array Insert";
        case T::ArrayRemove:  return "Array Remove";
        case T::ArrayContains:return "Array Contains";
        case T::ArrayIndexOf: return "Array Index Of";
        case T::ForEach:      return "For Each";
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
        case T::EngineCall:   return "Engine Call";
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
        case T::ConstString: case T::ConstVec2: case T::ConstColor:
        case T::ConstTransform: return "Literals";
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
        case T::EngineCall: return "Engine";
        case T::ArrayMake: case T::ArrayLength: case T::ArrayGet: case T::ArrayAdd:
        case T::ArraySet: case T::ArrayInsert: case T::ArrayRemove:
        case T::ArrayContains: case T::ArrayIndexOf: case T::ForEach: return "Array";
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
bool dataPinIsArray(const Node& n, bool input, int index)
{
    const NodeSig s = signatureOf(n);
    const auto& pins = input ? s.dataIns : s.dataOuts;
    if (index < 0 || index >= (int)pins.size()) return false;
    return pins[index].isArray;
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
    if (v.isArray)
    {
        // Seed from the editor-authored slots (each already a scalar of v.type).
        Value r; r.isArray = true; r.type = v.type;
        r.items = v.defaultItems;
        for (Value& it : r.items) { it.isArray = false; it.type = v.type; }
        return r;
    }
    switch (v.type)
    {
        case P::Float:  return Value::ofFloat(v.f[0]);
        case P::Bool:   return Value::ofBool(v.f[0] != 0.0f);
        case P::Int:    return Value::ofInt((int)v.f[0]);
        case P::String: return Value::ofString(v.s);
        case P::Vec2:   return Value::ofVec2({ v.f[0], v.f[1] });
        case P::Color:  return Value::ofColor({ v.f[0], v.f[1], v.f[2], v.f[3] });
        case P::Ref:    return Value::ofRef(0);
        case P::Transform: return Value::ofTransform(v.tpos, v.trot, v.tscl);
        default:        return Value::ofFloat(v.f[0]);
    }
}

std::vector<int> duplicateNodes(Graph& g, const std::vector<int>& ids, float dx, float dy)
{
    std::vector<int> fresh;
    std::unordered_map<int, int> remap;   // old id → clone id
    for (int id : ids)
    {
        const Node* src = g.findNode(id);
        if (!src) continue;
        // Handler/function names must stay unique per graph — don't clone those.
        if (src->type == NodeType::Event || src->type == NodeType::FunctionEntry) continue;
        Node copy = *src;                  // params/results/subgraph/payloads ride along
        copy.x += dx; copy.y += dy;
        const int nid = g.addNode(std::move(copy));
        remap[id] = nid;
        fresh.push_back(nid);
    }
    // Re-create the links INSIDE the duplicated set (external wires stay on the
    // originals — a duplicate shouldn't steal or share the source's inputs).
    std::vector<Link> cloned;
    for (const Link& l : g.links)
    {
        auto s = remap.find(l.srcNode), d = remap.find(l.dstNode);
        if (s != remap.end() && d != remap.end())
            cloned.push_back({ s->second, l.srcPin, d->second, l.dstPin });
    }
    for (const Link& l : cloned) g.links.push_back(l);
    return fresh;
}

void adoptForEachElementType(Graph& g, int srcNode, int srcPin, int dstNode, int dstPin)
{
    Node* dst = g.findNode(dstNode);
    const Node* src = g.findNode(srcNode);
    if (!dst || !src || dst->type != NodeType::ForEach) return;
    // ForEach unified pins: execIn 0, Body 1, Done 2, Array-in 3, Element-out 4.
    if (dstPin != 3) return;
    const NodeSig ss = signatureOf(*src);
    const int di = srcPin - (int)(ss.execIns.size() + ss.execOuts.size() + ss.dataIns.size());
    if (di < 0 || di >= (int)ss.dataOuts.size() || !ss.dataOuts[di].isArray) return;

    const PinType elem = ss.dataOuts[di].type;
    if (elem != dst->propType)
    {
        dst->propType = elem;
        // The Element output changed type: drop its links (they no longer typecheck).
        g.links.erase(std::remove_if(g.links.begin(), g.links.end(),
            [&](const Link& l){ return l.srcNode == dstNode && l.srcPin == 4; }),
            g.links.end());
    }
    // Object arrays: carry the element class along so the Element pin offers the
    // class's members. GetVariable/SetVariable → the variable's class; the array
    // ops keep their element class path in s.
    if (elem == PinType::Ref)
    {
        std::string cls;
        if (src->type == NodeType::GetVariable || src->type == NodeType::SetVariable)
        { if (const Variable* v = g.findVariable(src->s)) cls = v->className; }
        else if (src->type != NodeType::ForEach)
            cls = src->s;
        if (!cls.empty()) dst->s = cls;
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
        const int si = srcPin - sr.dataOut0, di = dstPin - dr.dataIn0;
        if (dataPinType(*s, false, si) != dataPinType(*d, true, di) ||
            dataPinIsArray(*s, false, si) != dataPinIsArray(*d, true, di)) // array ≠ scalar
            return false;
        links.erase(std::remove_if(links.begin(), links.end(),
            [&](const Link& l){ return l.dstNode == dstNode && l.dstPin == dstPin; }), links.end());
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    return false;
}

// ── JSON ─────────────────────────────────────────────────────────────────────

// ── Scalar Value ⇄ JSON (array-default slots) ────────────────────────────────
namespace
{
nlohmann::json scalarValueToJson(const Value& v, PinType t)
{
    switch (t)
    {
        case P::Float:  return v.f;
        case P::Bool:   return v.b;
        case P::Int:    return v.i;
        case P::String: return v.s;
        case P::Vec2:   return nlohmann::json::array({ v.v2.x, v.v2.y });
        case P::Color:  return nlohmann::json::array({ v.col.x, v.col.y, v.col.z, v.col.w });
        case P::Ref:    return v.ref;
        case P::Transform:
            return nlohmann::json::array({ v.tpos.x, v.tpos.y, v.tpos.z,
                                           v.trot.x, v.trot.y, v.trot.z,
                                           v.tscl.x, v.tscl.y, v.tscl.z });
        default:        return v.f;
    }
}
Value scalarValueFromJson(const nlohmann::json& j, PinType t)
{
    Value v; v.type = t;
    switch (t)
    {
        case P::Float:  if (j.is_number()) v.f = j.get<float>(); break;
        case P::Bool:   if (j.is_boolean()) v.b = j.get<bool>(); break;
        case P::Int:    if (j.is_number()) v.i = j.get<int>(); break;
        case P::String: if (j.is_string()) v.s = j.get<std::string>(); break;
        case P::Vec2:   if (j.is_array() && j.size() >= 2)
                            v.v2 = { j[0].get<float>(), j[1].get<float>() }; break;
        case P::Color:  if (j.is_array() && j.size() >= 4)
                            v.col = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>(), j[3].get<float>() }; break;
        case P::Ref:    if (j.is_number()) v.ref = j.get<uint32_t>(); break;
        case P::Transform:
            if (j.is_array() && j.size() >= 9)
            {
                v.tpos = { j[0].get<float>(), j[1].get<float>(), j[2].get<float>() };
                v.trot = { j[3].get<float>(), j[4].get<float>(), j[5].get<float>() };
                v.tscl = { j[6].get<float>(), j[7].get<float>(), j[8].get<float>() };
            }
            break;
        default: break;
    }
    return v;
}
} // namespace

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
        if (n.type == NodeType::ConstTransform)
            e["xform"] = { n.tpos.x, n.tpos.y, n.tpos.z, n.trot.x, n.trot.y, n.trot.z,
                           n.tscl.x, n.tscl.y, n.tscl.z };
        auto dumpParams = [](const std::vector<FuncParam>& ps)
        {
            nlohmann::json a = nlohmann::json::array();
            for (const auto& p : ps)
            {
                nlohmann::json pe = { { "name", p.name }, { "type", (int)p.type } };
                if (p.isArray) pe["arr"] = true;
                a.push_back(std::move(pe));
            }
            return a;
        };
        if (!n.params.empty())  e["params"]  = dumpParams(n.params);
        if (!n.results.empty()) e["results"] = dumpParams(n.results);
        if (n.subgraph)         e["subgraph"] = n.subgraph;
        if (n.isArray)          e["arr"]     = true;
        if (!n.pinDefaults.empty())
        {
            nlohmann::json pd = nlohmann::json::array();
            for (const auto& [idx, val] : n.pinDefaults)
                pd.push_back({ { "i", idx }, { "t", (int)val.type },
                               { "v", scalarValueToJson(val, val.type) } });
            e["pinDefaults"] = std::move(pd);
        }
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
        if (v.isArray)    e["arr"] = true;
        if (v.isArray && !v.defaultItems.empty())
        {
            nlohmann::json items = nlohmann::json::array();
            for (const Value& it : v.defaultItems) items.push_back(scalarValueToJson(it, v.type));
            e["items"] = std::move(items);
        }
        if (v.type == PinType::Transform)
            e["xform"] = { v.tpos.x, v.tpos.y, v.tpos.z, v.trot.x, v.trot.y, v.trot.z,
                           v.tscl.x, v.tscl.y, v.tscl.z };
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
        // Legacy display names (nodes serialize by name → renames need aliases).
        if (!known && typeName == "Array Add") { n.type = NodeType::ArrayAdd; known = true; }
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
        if (const auto& x = e.value("xform", nlohmann::json::array()); x.size() >= 9)
        {
            n.tpos = { x[0].get<float>(), x[1].get<float>(), x[2].get<float>() };
            n.trot = { x[3].get<float>(), x[4].get<float>(), x[5].get<float>() };
            n.tscl = { x[6].get<float>(), x[7].get<float>(), x[8].get<float>() };
        }
        auto loadParams = [](const nlohmann::json& a, std::vector<FuncParam>& ps)
        {
            for (const auto& pe : a)
            {
                FuncParam p;
                p.name = pe.value("name", std::string());
                p.type = (PinType)pe.value("type", (int)P::Float);
                p.isArray = pe.value("arr", false);
                ps.push_back(std::move(p));
            }
        };
        loadParams(e.value("params",  nlohmann::json::array()), n.params);
        loadParams(e.value("results", nlohmann::json::array()), n.results);
        n.subgraph = e.value("subgraph", 0);
        n.isArray  = e.value("arr", false);
        if (const auto& pd = e.value("pinDefaults", nlohmann::json::array()); pd.is_array())
            for (const auto& entry : pd)
            {
                if (!entry.is_object()) continue;
                const int idx = entry.value("i", -1);
                if (idx < 0) continue;
                const PinType t = (PinType)entry.value("t", (int)P::Float);
                n.pinDefaults[idx] = scalarValueFromJson(entry.value("v", nlohmann::json()), t);
            }
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
        v.isArray = e.value("arr", false);
        if (v.isArray)
            if (const auto& items = e.value("items", nlohmann::json::array()); items.is_array())
                for (const auto& it : items) v.defaultItems.push_back(scalarValueFromJson(it, v.type));
        v.className = e.value("className", std::string());
        if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
            for (int i = 0; i < 4; ++i) v.f[i] = f[i].get<float>();
        if (const auto& x = e.value("xform", nlohmann::json::array()); x.size() >= 9)
        {
            v.tpos = { x[0].get<float>(), x[1].get<float>(), x[2].get<float>() };
            v.trot = { x[3].get<float>(), x[4].get<float>(), x[5].get<float>() };
            v.tscl = { x[6].get<float>(), x[7].get<float>(), x[8].get<float>() };
        }
        g.variables.push_back(std::move(v));
    }
    syncFunctionSignatures(g); // reconcile call/return pins with their entries
    assignSubgraphs(g);        // migrate flat graphs → per-function sub-graphs
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

namespace
{
// The exec-forward closure from `start` plus every data node feeding those exec
// nodes — i.e. the body of the graph rooted at `start` (an Event or a
// FunctionEntry). Used to partition a flat graph into per-function sub-graphs.
std::unordered_set<int> traceBody(const Graph& g, int start)
{
    auto pinRangesOf = [](const Node& n, int& execOut0, int& execOutEnd, int& dataIn0, int& dataInEnd)
    {
        const NodeSig s = signatureOf(n);
        execOut0  = (int)s.execIns.size();
        execOutEnd = execOut0 + (int)s.execOuts.size();
        dataIn0   = execOutEnd;
        dataInEnd = dataIn0 + (int)s.dataIns.size();
    };
    std::unordered_set<int> body; std::vector<int> stack;
    body.insert(start); stack.push_back(start);
    // Exec-forward.
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        int eo0, eoE, di0, diE; pinRangesOf(*n, eo0, eoE, di0, diE);
        for (const auto& l : g.links)
            if (l.srcNode == id && l.srcPin >= eo0 && l.srcPin < eoE)
                if (body.insert(l.dstNode).second) stack.push_back(l.dstNode);
    }
    // Data producers feeding any node already in the body.
    stack.assign(body.begin(), body.end());
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        int eo0, eoE, di0, diE; pinRangesOf(*n, eo0, eoE, di0, diE);
        for (const auto& l : g.links)
            if (l.dstNode == id && l.dstPin >= di0 && l.dstPin < diE)
                if (body.insert(l.srcNode).second) stack.push_back(l.srcNode);
    }
    return body;
}
} // namespace

void assignSubgraphs(Graph& g)
{
    for (const Node& n : g.nodes) if (n.subgraph != 0) return; // already partitioned
    bool hasFn = false;
    for (const Node& n : g.nodes) if (n.type == NodeType::FunctionEntry) { hasFn = true; break; }
    if (!hasFn) return;

    // Nodes reachable from an Event stay in the event graph (subgraph 0).
    std::unordered_set<int> eventOwned;
    for (const Node& n : g.nodes)
        if (n.type == NodeType::Event)
        { auto b = traceBody(g, n.id); eventOwned.insert(b.begin(), b.end()); }

    // Each function claims its own body (minus anything an event already owns).
    for (Node& e : g.nodes)
    {
        if (e.type != NodeType::FunctionEntry) continue;
        e.subgraph = e.id;
        for (int m : traceBody(g, e.id))
        {
            if (m == e.id || eventOwned.count(m)) continue;
            if (Node* mn = g.findNode(m); mn && mn->subgraph == 0) mn->subgraph = e.id;
        }
    }
}

// ── Interpreter ──────────────────────────────────────────────────────────────

namespace
{
constexpr int kMaxSteps = 4096;
constexpr int kMaxDepth = 64;

// Element-type equality for Contains / IndexOf (scalar values only).
bool valueEquals(const Value& a, const Value& b, PinType t)
{
    switch (t)
    {
        case P::Float:  return a.f == b.f;
        case P::Bool:   return a.b == b.b;
        case P::Int:    return a.i == b.i;
        case P::String: return a.s == b.s;
        case P::Vec2:   return a.v2 == b.v2;
        case P::Color:  return a.col == b.col;
        case P::Ref:    return a.ref == b.ref;
        case P::Transform: return a.tpos == b.tpos && a.trot == b.trot && a.tscl == b.tscl;
        default:        return false;
    }
}

Value coerce(Value v, PinType want)
{
    if (v.isArray) return v;   // arrays are never scalar-coerced (pass through whole)
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
        if (n->type == T::Branch || n->type == T::Sequence || n->type == T::ForEach)
            return; // they steer their own exec-outs internally
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
    case T::EngineCall:
        // Side-effecting engine call: evaluate the argument pins, dispatch through
        // the registry, and cache the results for downstream data reads (like a
        // FunctionCall). Pure engine calls have no exec pin and never reach here.
        if (m_ctx.callApi)
        {
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
            m_execOutputs[n.id] = m_ctx.callApi(n.s, args);
        }
        break;
    case T::ForEach:
    {
        // Evaluate the array once, then run the Body chain per element with the
        // current Element + Index cached as this node's data outputs (read back
        // by evalData, like a call's results). Done fires after the last element.
        const Value arr = evalInput(n, 0, depth + 1);
        const PinRanges r = pinRanges(n);
        for (size_t i = 0; i < arr.items.size(); ++i)
        {
            m_execOutputs[n.id] = { arr.items[i], Value::ofInt((int)i) };
            runExecChain(n, r.execOut0 + 0, depth + 1);   // Body
        }
        runExecChain(n, r.execOut0 + 1, depth + 1);        // Done
        break;
    }
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
    // Unwired: the pin's inline default (editor-authored) before the type's zero.
    if (auto it = n.pinDefaults.find(dataInIndex); it != n.pinDefaults.end())
        return coerce(it->second, dataPinType(n, true, dataInIndex));
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
    case T::ConstTransform: return Value::ofTransform(n.tpos, n.trot, n.tscl);
    case T::ArrayMake:
    {
        Value r; r.isArray = true; r.type = n.propType; return r;   // empty array of the element type
    }
    case T::ArrayLength:
        return Value::ofInt((int)evalInput(n, 0, depth + 1).items.size());
    case T::ArrayGet:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size()) return arr.items[idx];
        Value def; def.type = n.propType; return def;   // out of range → element default
    }
    case T::ArrayAdd:
    {
        Value arr = evalInput(n, 0, depth + 1);          // a copy (pure: returns a new array)
        arr.isArray = true; arr.type = n.propType;
        arr.items.push_back(coerce(evalInput(n, 1, depth + 1), n.propType));
        return arr;
    }
    case T::ArraySet:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size())
            arr.items[idx] = coerce(evalInput(n, 2, depth + 1), n.propType);
        return arr;                                       // out of range → unchanged copy
    }
    case T::ArrayInsert:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        int idx = evalInput(n, 1, depth + 1).i;
        if (idx < 0) idx = 0;
        if (idx > (int)arr.items.size()) idx = (int)arr.items.size();  // clamp → append
        arr.items.insert(arr.items.begin() + idx, coerce(evalInput(n, 2, depth + 1), n.propType));
        return arr;
    }
    case T::ArrayRemove:
    {
        Value arr = evalInput(n, 0, depth + 1);
        arr.isArray = true; arr.type = n.propType;
        const int idx = evalInput(n, 1, depth + 1).i;
        if (idx >= 0 && idx < (int)arr.items.size())
            arr.items.erase(arr.items.begin() + idx);
        return arr;                                       // out of range → unchanged copy
    }
    case T::ArrayContains:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const Value key = coerce(evalInput(n, 1, depth + 1), n.propType);
        for (const Value& v : arr.items)
            if (valueEquals(v, key, n.propType)) return Value::ofBool(true);
        return Value::ofBool(false);
    }
    case T::ArrayIndexOf:
    {
        const Value arr = evalInput(n, 0, depth + 1);
        const Value key = coerce(evalInput(n, 1, depth + 1), n.propType);
        for (size_t i = 0; i < arr.items.size(); ++i)
            if (valueEquals(arr.items[i], key, n.propType)) return Value::ofInt((int)i);
        return Value::ofInt(-1);                           // not found
    }
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
    case T::ForEach: // Element + Index of the current iteration (cached per pass)
    {
        // A (local or cross-instance) call's return value: read the cached results.
        auto it = m_execOutputs.find(n.id);
        if (it != m_execOutputs.end() && dataOutPin >= 0 && dataOutPin < (int)it->second.size())
            return it->second[dataOutPin];
        return {};
    }
    case T::EngineCall:
    {
        // Exec engine call: return the value cached when the node ran. Pure engine
        // call (no exec pin): evaluate the inputs and dispatch now — re-evaluatable
        // because it has no side effect.
        if (n.hasArg)
        {
            auto it = m_execOutputs.find(n.id);
            if (it != m_execOutputs.end() && dataOutPin >= 0 && dataOutPin < (int)it->second.size())
                return it->second[dataOutPin];
            return {};
        }
        if (m_ctx.callApi)
        {
            std::vector<Value> args(n.params.size());
            for (size_t i = 0; i < n.params.size(); ++i)
                args[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
            std::vector<Value> res = m_ctx.callApi(n.s, args);
            if (dataOutPin >= 0 && dataOutPin < (int)res.size()) return res[dataOutPin];
        }
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
