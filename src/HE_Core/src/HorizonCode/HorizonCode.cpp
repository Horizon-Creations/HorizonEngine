#include <HorizonCode/HorizonCode.h>
#include <cstdint>
#include <Diagnostics/Logger.h>
#include <Types/TypeRegistry.h>   // struct/enum definitions (field/entry resolution)
#include <GraphCommon/GraphJson.h>
#include <GraphCommon/GraphModel.h>
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

namespace
{
// The one switch that knows every node's pins. Fills `s` IN PLACE — clearing its
// vectors without shrinking them — so the allocation-free accessors below can
// reuse a single scratch NodeSig instead of building four heap vectors per query.
void signatureInto(const Node& n, NodeSig& s)
{
    s.execIns.clear(); s.execOuts.clear(); s.dataIns.clear(); s.dataOuts.clear();
    // A pin's user-defined type: the node's own definition path, or null when it
    // names none (PinDesc's contract — see HorizonCode.h). Spelled once here
    // instead of at every descriptor below; `defOf` is for the mirrored
    // param/result/field lists, which carry their own paths.
    auto defOf = [](const std::string& t) -> const char* { return t.empty() ? nullptr : t.c_str(); };
    const char* const tn = defOf(n.typeName);
    switch (n.type)
    {
    case T::Event:
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::FunctionEntry:
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params)
            s.dataOuts.push_back({ p.name.c_str(), p.type, p.isArray, defOf(p.typeName) });
        break;
    case T::FunctionCall:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        for (const auto& p : n.params)
            s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray, defOf(p.typeName) });
        for (const auto& r : n.results)
            s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray, defOf(r.typeName) });
        break;
    case T::FunctionReturn:
        s.execIns = { { "", P::Exec } };
        for (const auto& r : n.results)
            s.dataIns.push_back({ r.name.c_str(), r.type, r.isArray, defOf(r.typeName) });
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
        s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::SetProperty:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Value", n.propType, false, tn } }; // pass the set value through
        break;
    case T::GetVariable:
        s.dataOuts = { { "Value", n.propType, n.isArray, tn } };
        break;
    case T::SetVariable:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Value", n.propType, n.isArray, tn } };
        s.dataOuts = { { "Value", n.propType, n.isArray, tn } }; // pass the set value through
        break;
    case T::ShowSelf:
    case T::HideSelf:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        break;
    case T::CreateWidget:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataOuts = { { "Widget", P::Ref } };
        break;
    case T::ShowWidget:
    case T::HideWidget:
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
        s.dataOuts = { { "Value", n.propType, false, tn } };
        break;
    case T::SetExternal:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        s.dataIns  = { { "Target", P::Ref }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Value", n.propType, false, tn } }; // pass the set value through
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
        for (const auto& p : n.params)
            s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray, defOf(p.typeName) });
        for (const auto& r : n.results)
            s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray, defOf(r.typeName) });
        break;
    case T::EmitEvent:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "", P::Exec } };
        if (n.hasArg) s.dataIns = { { "Arg", n.propType, false, tn } };
        break;
    case T::EngineCall:
        // hasArg = the registry entry's isExec: side-effecting calls get exec pins,
        // pure calls (getters/math) are compact data nodes. params → data-ins,
        // results → data-outs (mirrored on the node from the ApiFn descriptor).
        if (n.hasArg) { s.execIns = { { "", P::Exec } }; s.execOuts = { { "", P::Exec } }; }
        for (const auto& p : n.params)
            s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray, defOf(p.typeName) });
        for (const auto& r : n.results)
            s.dataOuts.push_back({ r.name.c_str(), r.type, r.isArray, defOf(r.typeName) });
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
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayLength:
        s.dataIns  = { { "Array", n.propType, true, tn } };
        s.dataOuts = { { "Length", P::Int } };
        break;
    case T::ArrayGet:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int } };
        s.dataOuts = { { "Element", n.propType, false, tn } };
        break;
    case T::ArrayAdd:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArraySet:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayInsert:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayRemove:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Index", P::Int } };
        s.dataOuts = { { "Array", n.propType, true, tn } };
        break;
    case T::ArrayContains:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Contains", P::Bool } };
        break;
    case T::ArrayIndexOf:
        s.dataIns  = { { "Array", n.propType, true, tn }, { "Value", n.propType, false, tn } };
        s.dataOuts = { { "Index", P::Int } };
        break;
    case T::ForEach:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Body", P::Exec }, { "Done", P::Exec } };
        s.dataIns  = { { "Array", n.propType, true, tn } };
        s.dataOuts = { { "Element", n.propType, false, tn }, { "Index", P::Int } };
        break;
    case T::Delay:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Completed", P::Exec } };
        s.dataIns  = { { "Duration", P::Float } };
        break;
    case T::IsValid:
        s.dataIns  = { { "Target", P::Ref } };
        s.dataOuts = { { "Valid", P::Bool } };
        break;
    case T::DoOnce:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "Then", P::Exec } };
        break;
    case T::FlipFlop:
        s.execIns  = { { "", P::Exec } };
        s.execOuts = { { "A", P::Exec }, { "B", P::Exec } };
        s.dataOuts = { { "Is A", P::Bool } };
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
    case T::MakeStruct:
        // params mirror the struct's fields (synced from the TypeRegistry), so
        // pins resolve without the registry — same idea as EngineCall.
        for (const auto& p : n.params)
            s.dataIns.push_back({ p.name.c_str(), p.type, p.isArray,
                                  defOf(p.typeName) });
        s.dataOuts = { { "Struct", P::Struct, false, tn } };
        break;
    case T::BreakStruct:
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        for (const auto& p : n.params)
            s.dataOuts.push_back({ p.name.c_str(), p.type, p.isArray,
                                   defOf(p.typeName) });
        break;
    case T::GetStructField:
        // params[0] mirrors the chosen field; one output, so a graph reads a
        // single property without breaking the whole struct apart.
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        if (!n.params.empty())
            s.dataOuts.push_back({ n.params[0].name.c_str(), n.params[0].type,
                                   n.params[0].isArray,
                                   defOf(n.params[0].typeName) });
        break;
    case T::SetStructField:
        // params[0] mirrors the chosen field (name + type).
        s.dataIns = { { "Struct", P::Struct, false, tn } };
        if (!n.params.empty())
            s.dataIns.push_back({ n.params[0].name.c_str(), n.params[0].type,
                                  n.params[0].isArray,
                                  defOf(n.params[0].typeName) });
        s.dataOuts = { { "Struct", P::Struct, false, tn } };
        break;
    case T::ConstEnum:
        s.dataOuts = { { "", P::Enum, false, tn } };
        break;
    case T::SwitchOnEnum:
        // params mirror the entry names; the trailing Default catches values no
        // entry claims (or a missing definition).
        s.execIns = { { "", P::Exec } };
        for (const auto& p : n.params) s.execOuts.push_back({ p.name.c_str(), P::Exec });
        s.execOuts.push_back({ "Default", P::Exec });
        s.dataIns = { { "Value", P::Enum, false, tn } };
        break;
    case T::EnumToInt:
        s.dataIns  = { { "", P::Enum, false, tn } };
        s.dataOuts = { { "", P::Int } };
        break;
    case T::IntToEnum:
        s.dataIns  = { { "", P::Int } };
        s.dataOuts = { { "", P::Enum, false, tn } };
        break;
    case T::EnumToString:
        s.dataIns  = { { "", P::Enum, false, tn } };
        s.dataOuts = { { "", P::String } };
        break;
    default: break;
    }
}

// Scratch signature for the counting / single-pin queries below. Its vectors keep
// their capacity across calls, so after the first call signatureInto() only
// assigns and pushes into storage it already owns — zero allocations in the hot
// path (Graph::connect queries it 6× per attempted link, the Runner once per
// executed node, traceBody once per visited node). thread_local because the
// editor and the runtime can query concurrently; every accessor copies what it
// needs out before returning, so callers never alias the scratch.
NodeSig& sigScratch()
{
    static thread_local NodeSig s;
    return s;
}
} // namespace

NodeSig signatureOf(const Node& n)
{
    NodeSig s;
    signatureInto(n, s);
    return s;
}

NodeSigCounts signatureCountsOf(const Node& n)
{
    NodeSig& s = sigScratch();
    signatureInto(n, s);
    return { (int)s.execIns.size(), (int)s.execOuts.size(),
             (int)s.dataIns.size(), (int)s.dataOuts.size() };
}

bool dataPinDescOf(const Node& n, bool input, int index, PinDesc& out)
{
    if (index < 0) return false;
    NodeSig& s = sigScratch();
    signatureInto(n, s);
    const std::vector<PinDesc>& pins = input ? s.dataIns : s.dataOuts;
    if (index >= (int)pins.size()) return false;
    // PinDesc::name points at a string literal or at a string owned by `n` — never
    // at the scratch itself — so the copy stays valid after the next query.
    out = pins[(size_t)index];
    return true;
}

// ╔══════════════════════════════════════════════════════════════════════════╗
// ║  THESE STRINGS ARE AN ON-DISK FORMAT.                                    ║
// ║                                                                          ║
// ║  toJson() writes a node's type as its DISPLAY NAME and fromJson() looks  ║
// ║  it back up by that same string. Renaming an entry below therefore makes ║
// ║  every saved graph containing that node UNREADABLE — the node is silently ║
// ║  dropped on load (fromJson `continue`s on an unknown name) and every link ║
// ║  attached to it goes with it. "Array Add" → "Array Append" already cost   ║
// ║  us exactly that; it survives only through kLegacyNodeNames below.        ║
// ║                                                                          ║
// ║  So: if you rename a node, ADD ITS OLD STRING TO kLegacyNodeNames in the  ║
// ║  same commit. (The proper fix — a stable serialName per type, independent ║
// ║  of the UI label — is a format migration; see nodeTypeFromStoredName.)    ║
// ╚══════════════════════════════════════════════════════════════════════════╝
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
        case T::ShowSelf:   return "Show Self";
        case T::HideSelf:   return "Hide Self";
        case T::CreateWidget: return "Create Widget";
        case T::ShowWidget: return "Show Widget";
        case T::HideWidget: return "Hide Widget";
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
        case T::Delay:        return "Delay";
        case T::IsValid:      return "Is Valid";
        case T::DoOnce:       return "Do Once";
        case T::FlipFlop:     return "Flip Flop";
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
        case T::MakeStruct:     return "Make Struct";
        case T::BreakStruct:    return "Break Struct";
        case T::GetStructField: return "Get Struct Field";
        case T::SetStructField: return "Set Struct Field";
        case T::ConstEnum:      return "Enum Value";
        case T::SwitchOnEnum:   return "Switch on Enum";
        case T::EnumToInt:      return "Enum to Int";
        case T::IntToEnum:      return "Int to Enum";
        case T::EnumToString:   return "Enum to String";
        default:              return "?";
    }
}

const char* nodeTooltip(NodeType t)
{
    switch (t)
    {
        case T::Event:
            return "Entry point: starts an exec chain when the host fires the named event.\n"
                   "Events with a payload expose it as a data output.";
        case T::FunctionEntry:
            return "Entry of a user-defined function. Its declared inputs appear as data\n"
                   "outputs here; add a Return node to hand results back to the caller.";
        case T::FunctionCall:
            return "Calls a function declared in this graph. Wire its inputs, run it via the\n"
                   "exec pin, and read the declared results from the data outputs.";
        case T::FunctionReturn:
            return "Writes the owning function's results and ends the exec chain.\n"
                   "One data input per declared result.";
        case T::Branch:
            return "If/else: routes execution to True or False depending on the Bool input.";
        case T::Sequence:
            return "Runs its exec outputs in order (Then 0, Then 1, …), one after another.";
        case T::Delay:
            return "Pauses the exec chain and resumes from Completed after Duration seconds.\n"
                   "Retriggering while already pending is ignored.";
        case T::DoOnce:
            return "Lets execution through only the FIRST time it is reached;\n"
                   "every later trigger is swallowed.";
        case T::FlipFlop:
            return "Alternates between its A and B exec outputs on each trigger.\n"
                   "The Is A output tells which side just ran.";
        case T::GetProperty:
            return "Reads a property of the chosen target element; the value type follows\n"
                   "the property. Pure — evaluated whenever the output is used.";
        case T::SetProperty:
            return "Writes the wired value to a property of the chosen target element\n"
                   "when executed.";
        case T::GetVariable:
            return "Reads a graph variable (persistent per running instance).\n"
                   "Pure — evaluated whenever the output is used.";
        case T::SetVariable:
            return "Writes the wired value to a graph variable when executed.";
        case T::ShowSelf:
            return "Makes THIS widget visible (only meaningful inside a widget graph).";
        case T::HideSelf:
            return "Hides THIS widget (only meaningful inside a widget graph).";
        case T::CreateWidget:
            return "Instantiates the chosen widget asset and outputs its Widget id —\n"
                   "feed that into Show/Hide/Destroy Widget.";
        case T::ShowWidget:
            return "Shows the widget identified by the Widget input (from Create Widget).";
        case T::HideWidget:
            return "Hides the widget identified by the Widget input (from Create Widget).";
        case T::DestroyWidget:
            return "Destroys the widget identified by the Widget input; its id becomes invalid.";
        case T::CreateObject:
            return "Instantiates the chosen HorizonCode class as a live object and outputs\n"
                   "a reference to it (its Construct event fires).";
        case T::DestroyObject:
            return "Destroys the object referenced by the input (its Destruct event fires);\n"
                   "the reference becomes invalid.";
        case T::GetExternal:
            return "Reads a PUBLIC variable on the referenced instance.\n"
                   "Target expects an object reference; the output takes the variable's type.";
        case T::SetExternal:
            return "Writes a PUBLIC variable on the referenced instance when executed.\n"
                   "Target expects an object reference.";
        case T::ConstFloat:  return "Float literal. Edit the value on the node body.";
        case T::ConstBool:   return "Bool literal. Edit the value on the node body.";
        case T::ConstInt:    return "Int literal. Edit the value on the node body.";
        case T::ConstString: return "String literal. Edit the text on the node body.";
        case T::ConstVec2:   return "Vec2 literal (x, y). Edit the values on the node body.";
        case T::ConstColor:  return "Color literal (RGBA). Edit the swatch on the node body.";
        case T::ConstTransform:
            return "Transform literal: position, rotation (euler degrees) and scale,\n"
                   "edited on the node body.";
        case T::Add:      return "A + B (Float).";
        case T::Subtract: return "A - B (Float).";
        case T::Multiply: return "A * B (Float).";
        case T::Divide:   return "A / B (Float). Division by zero yields 0.";
        case T::Greater:  return "True when A > B.";
        case T::Less:     return "True when A < B.";
        case T::Equals:   return "True when A equals B.";
        case T::And:      return "True when both A and B are true.";
        case T::Or:       return "True when A or B (or both) is true.";
        case T::Not:      return "Inverts the Bool input.";
        case T::Concat:   return "Joins strings A and B into one string.";
        case T::ToString: return "Converts the input value to its text representation.";
        case T::Print:
            return "Writes the input string to the engine log when executed. Debug aid.";
        case T::BindEvent:
            return "Subscribes: when the Target instance emits the named event, this\n"
                   "instance's matching Event node fires.";
        case T::EmitEvent:
            return "Broadcasts the named event (with an optional payload) to every\n"
                   "instance bound to this one via Bind Event.";
        case T::CallExternal:
            return "Calls a PUBLIC function on the referenced instance. Target expects an\n"
                   "object reference; params and results mirror that function's signature.";
        case T::GetGameInstance:
            return "Outputs a reference to the app-wide GameInstance — global state and\n"
                   "functions that outlive scene changes.";
        case T::GetSelf:
            return "Outputs a reference to this running instance (e.g. to pass to Bind\n"
                   "Event or store in a variable).";
        case T::EngineCall:
            return "Calls an engine API function (transform, input, scene, math, …).\n"
                   "Pins mirror the function's parameters and results.";
        case T::ArrayMake:   return "Outputs a new empty array of the chosen element type.";
        case T::ArrayLength: return "Outputs the number of elements in the input array.";
        case T::ArrayGet:
            return "Outputs the element at Index (0-based). Out-of-range yields the\n"
                   "element type's default value.";
        case T::ArrayAdd:
            return "Outputs a COPY of the input array with Value appended at the end.\n"
                   "The original array is not modified.";
        case T::ArraySet:
            return "Outputs a COPY of the array with the element at Index replaced by Value.";
        case T::ArrayInsert:
            return "Outputs a COPY of the array with Value inserted at Index\n"
                   "(later elements shift right).";
        case T::ArrayRemove:
            return "Outputs a COPY of the array with the element at Index removed.";
        case T::ArrayContains:
            return "True when the array holds an element equal to Value.";
        case T::ArrayIndexOf:
            return "Outputs the index of the first element equal to Value, or -1 if absent.";
        case T::ForEach:
            return "Runs Body once per array element (Element + Index data outputs),\n"
                   "then continues from Done.";
        case T::IsValid:
            return "True when the Target reference points to a live instance — the guard\n"
                   "to run before touching an object that may have been destroyed.";
        case T::MakeStruct:
            return "Builds a value of the chosen Struct asset: one input per field,\n"
                   "unwired fields fall back to their declared defaults.";
        case T::BreakStruct:
            return "Splits a struct value into its fields — one output per field.";
        case T::GetStructField:
            return "Reads ONE field out of a struct — pick which on the node.\n"
                   "Use Break Struct when you want every field at once.";
        case T::SetStructField:
            return "Outputs a COPY of the struct with the chosen field replaced by Value.\n"
                   "The original struct is not modified.";
        case T::ConstEnum:
            return "Enum literal of the chosen Enum asset. Pick the entry on the node body.";
        case T::SwitchOnEnum:
            return "Routes execution to the exec output matching the enum value;\n"
                   "unmatched values (or a missing definition) take Default.";
        case T::EnumToInt:    return "Outputs the enum value's underlying Int.";
        case T::IntToEnum:    return "Reinterprets an Int as the chosen Enum asset's value.";
        case T::EnumToString: return "Outputs the NAME of the enum entry matching the value\n"
                   "(empty when no entry matches).";
        default: return "";
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
        case T::Sequence:
        case T::Delay:
        case T::DoOnce:
        case T::FlipFlop:      return "Flow";
        case T::IsValid:       return "Reference";
        case T::GetProperty:
        case T::SetProperty:   return "Property";
        case T::GetVariable:
        case T::SetVariable:   return "Variables";
        case T::ShowSelf:
        case T::HideSelf:    return "Widget";
        case T::CreateWidget:
        case T::ShowWidget:
        case T::HideWidget:
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
        case T::MakeStruct: case T::BreakStruct:
        case T::GetStructField: case T::SetStructField: return "Structs";
        case T::ConstEnum: case T::SwitchOnEnum:
        case T::EnumToInt: case T::IntToEnum: case T::EnumToString: return "Enums";
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
// All three go through the allocation-free accessors (see HorizonCode.h): they
// are the interpreter's and the editor's per-node hot path.
PinRanges pinRanges(const Node& n)
{
    const NodeSigCounts s = signatureCountsOf(n);
    PinRanges r;
    r.execIn0  = 0;
    r.execOut0 = r.execIn0  + s.execIns;
    r.dataIn0  = r.execOut0 + s.execOuts;
    r.dataOut0 = r.dataIn0  + s.dataIns;
    r.end      = r.dataOut0 + s.dataOuts;
    return r;
}
PinType dataPinType(const Node& n, bool input, int index)
{
    PinDesc d;
    return dataPinDescOf(n, input, index, d) ? d.type : P::Float;
}
bool dataPinIsArray(const Node& n, bool input, int index)
{
    PinDesc d;
    return dataPinDescOf(n, input, index, d) && d.isArray;
}
} // namespace

// ── Graph container ──────────────────────────────────────────────────────────

Node*       Graph::findNode(int id)       { return HE::graph::findNodeById(nodes, id); }
const Node* Graph::findNode(int id) const { return HE::graph::findNodeById(nodes, id); }

int Graph::addNode(Node n)
{
    return HE::graph::appendNode(nodes, nextId, std::move(n));
}

void Graph::removeNode(int id)
{
    // A deleted function takes its local variables with it. (HorizonCode has no
    // fixed sink node, so unlike Material/ParticleGraph nothing is un-removable.)
    if (const Node* n = findNode(id); n && n->type == NodeType::FunctionEntry)
        variables.erase(std::remove_if(variables.begin(), variables.end(),
            [&](const Variable& v){ return v.scope == id; }), variables.end());
    HE::graph::removeNodeAndLinks(nodes, links, id);
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
        case P::Enum:
        {
            // The stored default is the entry NAME in `s` (renumber-safe);
            // resolve against the current definition, first entry as fallback.
            Value r; r.type = P::Enum; r.typeName = v.typeName;
            HE::EnumDef def;
            if (HE::TypeRegistry::instance().getEnum(v.typeName, def))
            {
                if (const HE::EnumEntry* e = def.findEntry(v.s)) r.i = e->value;
                else if (!def.entries.empty())                   r.i = def.entries.front().value;
            }
            return r;
        }
        case P::Struct:
        {
            // The definition's own field defaults, then this graph's overrides
            // on top — matched BY NAME against the current definition.
            Value r = HE::TypeRegistry::instance().makeDefaultValue(v.typeName);
            if (!v.structDefaults.empty())
            {
                HE::StructDef def;
                if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
                    for (size_t i = 0; i < def.fields.size() && i < r.items.size(); ++i)
                        if (auto it = v.structDefaults.find(def.fields[i].name);
                            it != v.structDefaults.end())
                        {
                            Value ov = it->second;
                            if (def.fields[i].type == P::Enum)
                            {
                                // Stored as the entry NAME (renumber-safe).
                                HE::EnumDef ed;
                                ov.type = P::Enum; ov.typeName = def.fields[i].typeName;
                                if (HE::TypeRegistry::instance().getEnum(ov.typeName, ed))
                                {
                                    if (const HE::EnumEntry* en = ed.findEntry(it->second.s)) ov.i = en->value;
                                    else continue;   // stale entry name → keep the definition default
                                }
                            }
                            else
                            {
                                ov.type = def.fields[i].type;
                            }
                            r.items[i] = std::move(ov);
                        }
            }
            return r;
        }
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
    PinDesc sd;
    if (!dataPinDescOf(*src, /*input=*/false, srcPin - pinRanges(*src).dataOut0, sd) ||
        !sd.isArray)
        return;

    const PinType elem = sd.type;
    const std::string elemDef = sd.typeName ? sd.typeName : "";
    if (elem != dst->propType || (!elemDef.empty() && elemDef != dst->typeName))
    {
        dst->propType = elem;
        // Enum/Struct elements: carry the DEFINITION along, not just the kind —
        // without it the Element pin is a user-defined type nobody can resolve
        // (no field pins, "definition missing" in every panel that looks it up).
        if (elem == PinType::Enum || elem == PinType::Struct) dst->typeName = elemDef;
        else                                                  dst->typeName.clear();
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
        // An exec OUTPUT is a single "what runs next" pointer, so it is the SOURCE
        // side that gets replaced here — the one place HorizonCode's link semantics
        // differ from Material/ParticleGraph (which have no exec pins at all).
        HE::graph::disconnectOutput(links, srcNode, srcPin);
        links.push_back({ srcNode, srcPin, dstNode, dstPin });
        return true;
    }
    if (srcIsDataOut && dstIsDataIn)
    {
        const int si = srcPin - sr.dataOut0, di = dstPin - dr.dataIn0;
        if (dataPinType(*s, false, si) != dataPinType(*d, true, di) ||
            dataPinIsArray(*s, false, si) != dataPinIsArray(*d, true, di)) // array ≠ scalar
            return false;
        // User-defined types connect only to the SAME definition: a Struct pin
        // for PlayerStats must not accept an Inventory, and enums likewise. An
        // EMPTY typeName is the generic boundary (e.g. the save.setStruct
        // engine call, whose registry params carry no definition) — it accepts
        // anything, like an untyped Object reference; the callee validates.
        {
            const PinType pt = dataPinType(*s, false, si);
            if (pt == P::Enum || pt == P::Struct)
            {
                PinDesc sd{}, dd{};
                dataPinDescOf(*s, false, si, sd);
                dataPinDescOf(*d, true,  di, dd);
                const std::string_view a = sd.typeName ? sd.typeName : "";
                const std::string_view b = dd.typeName ? dd.typeName : "";
                if (!a.empty() && !b.empty() && a != b) return false;
            }
        }
        HE::graph::disconnectInput(links, dstNode, dstPin); // an input holds one link
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
        case P::Enum:   return v.s;   // the entry NAME (renumber-safe)
        case P::Struct:
        {
            // Name-keyed field object, like everything else about user types.
            nlohmann::json o = nlohmann::json::object();
            HE::StructDef def;
            if (HE::TypeRegistry::instance().getStruct(v.typeName, def))
                for (size_t i = 0; i < def.fields.size() && i < v.items.size(); ++i)
                    o[def.fields[i].name] = scalarValueToJson(v.items[i], def.fields[i].type);
            return o;
        }
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
        case P::Enum:   if (j.is_string()) v.s = j.get<std::string>(); break;  // entry NAME
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

// ── Stored node type name → NodeType ─────────────────────────────────────────
// Nodes serialize by DISPLAY NAME (see the banner at nodeDisplayName), so every
// rename of a node label leaves saved graphs referring to the OLD string. This
// table is where those old strings keep working — data-driven, one line per
// rename, forever. Never delete an entry: the graph that still uses it may be in
// a user's project, not in this repo.
//
// FOLLOW-UP (deliberately not done here): give every NodeType a stable
// serialName decoupled from the UI label and write THAT, keeping this table as
// the read-side alias map for everything written so far. That flips what
// toJson() emits, so it needs a format version on the graph and a shipped-editor
// compatibility story — too big to slip into a refactor. Until then the rule
// above ("rename ⇒ add an alias in the same commit") is the whole contract.
struct LegacyNodeName { const char* stored; NodeType type; };
constexpr LegacyNodeName kLegacyNodeNames[] = {
    { "Array Add", NodeType::ArrayAdd },   // renamed to "Array Append"
};

bool nodeTypeFromStoredName(const std::string& stored, NodeType& out)
{
    for (NodeType t : nodeRegistry())
        if (stored == nodeDisplayName(t)) { out = t; return true; }
    for (const LegacyNodeName& a : kLegacyNodeNames)
        if (stored == a.stored) { out = a.type; return true; }
    return false;
}
// ── Item-level JSON ─────────────────────────────────────────────────────────
// One node / variable as the object that goes into the document's array. The
// document serializers below are written on top of these, so the per-item form
// collaboration sends over the wire and the on-disk form are the SAME bytes by
// construction — a second, parallel serializer would drift the first time a
// field was added to only one of them.
nlohmann::json nodeToJsonObj(const Node& n)
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
            if (!p.typeName.empty()) pe["typeName"] = p.typeName;
            a.push_back(std::move(pe));
        }
        return a;
    };
    if (!n.params.empty())  e["params"]  = dumpParams(n.params);
    if (!n.results.empty()) e["results"] = dumpParams(n.results);
    if (n.subgraph)         e["subgraph"] = n.subgraph;
    if (n.isArray)          e["arr"]     = true;
    if (!n.typeName.empty()) e["typeName"] = n.typeName;
    if (!n.pinDefaults.empty())
    {
        nlohmann::json pd = nlohmann::json::array();
        for (const auto& [idx, val] : n.pinDefaults)
            pd.push_back({ { "i", idx }, { "t", (int)val.type },
                           { "v", scalarValueToJson(val, val.type) } });
        e["pinDefaults"] = std::move(pd);
    }
return e;
}

nlohmann::json variableToJsonObj(const Variable& v)
{
    nlohmann::json e = { { "name", v.name }, { "type", (int)v.type } };
    if (v.f[0] || v.f[1] || v.f[2] || v.f[3]) e["f"] = { v.f[0], v.f[1], v.f[2], v.f[3] };
    if (!v.s.empty()) e["s"] = v.s;
    if (v.access)     e["access"] = v.access;
    if (v.scope)      e["scope"] = v.scope;   // function-local (FunctionEntry id)
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
    if (!v.typeName.empty())  e["typeName"]  = v.typeName;
    if (!v.structDefaults.empty())
    {
        // Name-keyed, and each value tagged with its own type so a definition
        // edit can't reinterpret what was authored here.
        nlohmann::json sd = nlohmann::json::object();
        for (const auto& [field, val] : v.structDefaults)
            sd[field] = { { "t", (int)val.type }, { "v", scalarValueToJson(val, val.type) } };
        e["structDefaults"] = std::move(sd);
    }
return e;
}

// False = the object does not describe a node we know (unknown/removed type),
// which the document loader skips and the delta layer treats as "ignore".
bool nodeFromJsonObj(const nlohmann::json& e, Node& n)
{
    n.id = e.value("id", 0);
    if (!nodeTypeFromStoredName(e.value("type", std::string()), n.type)) return false;
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
            p.typeName = pe.value("typeName", std::string());
            ps.push_back(std::move(p));
        }
    };
    loadParams(e.value("params",  nlohmann::json::array()), n.params);
    loadParams(e.value("results", nlohmann::json::array()), n.results);
    n.subgraph = e.value("subgraph", 0);
    n.isArray  = e.value("arr", false);
    n.typeName = e.value("typeName", std::string());
    if (const auto& pd = e.value("pinDefaults", nlohmann::json::array()); pd.is_array())
        for (const auto& entry : pd)
        {
            if (!entry.is_object()) continue;
            const int idx = entry.value("i", -1);
            if (idx < 0) continue;
            const PinType t = (PinType)entry.value("t", (int)P::Float);
            n.pinDefaults[idx] = scalarValueFromJson(entry.value("v", nlohmann::json()), t);
        }
return true;
}

// False = no usable variable in this object (a variable is keyed by its name,
// so a nameless one has no identity).
bool variableFromJsonObj(const nlohmann::json& e, Variable& v)
{
    v.name = e.value("name", std::string());
    if (v.name.empty()) return false;
    v.type = (PinType)e.value("type", (int)P::Float);
    v.s    = e.value("s", std::string());
    v.access = e.value("access", 0);
    v.scope  = e.value("scope", 0);
    v.isArray = e.value("arr", false);
    if (v.isArray)
        if (const auto& items = e.value("items", nlohmann::json::array()); items.is_array())
            for (const auto& it : items) v.defaultItems.push_back(scalarValueFromJson(it, v.type));
    v.className = e.value("className", std::string());
    v.typeName  = e.value("typeName", std::string());
    if (const auto& sd = e.value("structDefaults", nlohmann::json::object()); sd.is_object())
        for (auto it = sd.begin(); it != sd.end(); ++it)
        {
            if (!it->is_object()) continue;
            const PinType t = (PinType)it->value("t", (int)P::Float);
            v.structDefaults[it.key()] = scalarValueFromJson(it->value("v", nlohmann::json()), t);
        }
    if (const auto& f = e.value("f", nlohmann::json::array()); f.size() >= 4)
        for (int i = 0; i < 4; ++i) v.f[i] = f[i].get<float>();
    if (const auto& x = e.value("xform", nlohmann::json::array()); x.size() >= 9)
    {
        v.tpos = { x[0].get<float>(), x[1].get<float>(), x[2].get<float>() };
        v.trot = { x[3].get<float>(), x[4].get<float>(), x[5].get<float>() };
        v.tscl = { x[6].get<float>(), x[7].get<float>(), x[8].get<float>() };
    }
return true;
}
} // namespace

// ── Document JSON — assembled from the item writers above ───────────────────
std::string toJson(const Graph& g)
{
    nlohmann::json j;
    j["nextId"] = g.nextId;

    nlohmann::json jn = nlohmann::json::array();
    for (const Node& n : g.nodes) jn.push_back(nodeToJsonObj(n));
    j["nodes"] = std::move(jn);

    nlohmann::json jl = nlohmann::json::array();
    for (const Link& l : g.links) // ARRAY link form — see GraphJson.h
        jl.push_back(HE::graph::linkToArray(l.srcNode, l.srcPin, l.dstNode, l.dstPin));
    j["links"] = std::move(jl);

    nlohmann::json jv = nlohmann::json::array();
    for (const Variable& v : g.variables) jv.push_back(variableToJsonObj(v));
    j["variables"] = std::move(jv);
    return j.dump(2);
}

bool fromJson(const std::string& json, Graph& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;

    Graph g;
    g.nextId = j.value("nextId", 1);
    for (const auto& e : j.value("nodes", nlohmann::json::array()))
    {
        Node n;
        if (!nodeFromJsonObj(e, n)) continue;   // unknown/removed node type
        HE::graph::bumpNextId(g.nextId, n.id);
        g.nodes.push_back(std::move(n));
    }
    for (const auto& e : j.value("links", nlohmann::json::array()))
    {
        Link l;
        if (!HE::graph::linkFromArray(e, l.srcNode, l.srcPin, l.dstNode, l.dstPin)) continue;
        // Unlike Material/ParticleGraph, HorizonCode drops links whose endpoints
        // didn't survive the node loop (an unknown/removed node type) — a dangling
        // exec link would otherwise stall the interpreter's chain walk.
        if (g.findNode(l.srcNode) && g.findNode(l.dstNode))
            g.links.push_back(l);
    }
    for (const auto& e : j.value("variables", nlohmann::json::array()))
    {
        Variable v;
        if (!variableFromJsonObj(e, v)) continue;
        g.variables.push_back(std::move(v));
    }
    syncFunctionSignatures(g); // reconcile call/return pins with their entries
    inferUserTypeNames(g);     // recover Enum/Struct definitions from the wiring
    syncTypeSignatures(g);     // re-mirror struct/enum pins from the TypeRegistry
    assignSubgraphs(g);        // migrate flat graphs → per-function sub-graphs
    out = std::move(g);
    return true;
}

// ── Item-level JSON, public (collaboration addresses single items) ──────────
std::string nodeToJson(const Node& n)     { return nodeToJsonObj(n).dump(); }
std::string variableToJson(const Variable& v) { return variableToJsonObj(v).dump(); }

bool nodeFromJson(const std::string& json, Node& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    Node n;
    if (!nodeFromJsonObj(j, n)) return false;
    out = std::move(n);
    return true;
}

bool variableFromJson(const std::string& json, Variable& out)
{
    nlohmann::json j;
    if (!HE::graph::parseGraphObject(json, j)) return false;
    Variable v;
    if (!variableFromJsonObj(j, v)) return false;
    out = std::move(v);
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

void inferUserTypeNames(Graph& g)
{
    // Declared variables are authoritative for their own Get/Set nodes.
    for (Node& n : g.nodes)
        if ((n.type == NodeType::GetVariable || n.type == NodeType::SetVariable) &&
            n.typeName.empty())
            if (const Variable* v = g.findVariable(n.s)) n.typeName = v->typeName;

    // Then propagate along wires until nothing new is learned (a chain of array
    // ops picks its element definition up one hop at a time).
    for (bool changed = true; changed; )
    {
        changed = false;
        for (Node& n : g.nodes)
        {
            if (!n.typeName.empty()) continue;
            if (n.propType != P::Struct && n.propType != P::Enum) continue;
            std::string found;
            bool ambiguous = false;
            for (const Link& l : g.links)
            {
                const bool weAreSrc = l.srcNode == n.id;
                if (!weAreSrc && l.dstNode != n.id) continue;
                const Node* peer = g.findNode(weAreSrc ? l.dstNode : l.srcNode);
                if (!peer || peer->id == n.id) continue;
                const PinRanges pr = pinRanges(*peer);
                const int idx = weAreSrc ? l.dstPin - pr.dataIn0 : l.srcPin - pr.dataOut0;
                PinDesc pd{};
                if (!dataPinDescOf(*peer, /*input=*/weAreSrc, idx, pd)) continue;
                if (pd.type != n.propType || !pd.typeName || !*pd.typeName) continue;
                if (found.empty())            found = pd.typeName;
                else if (found != pd.typeName) { ambiguous = true; break; }
            }
            if (ambiguous || found.empty()) continue;
            n.typeName = found;
            changed = true;
        }
    }
}

void syncTypeSignatures(Graph& g)
{
    auto& reg = HE::TypeRegistry::instance();
    auto fieldsToParams = [](const HE::StructDef& def)
    {
        std::vector<FuncParam> ps;
        ps.reserve(def.fields.size());
        for (const HE::StructField& f : def.fields)
            ps.push_back({ f.name, f.type, f.isArray, f.typeName });
        return ps;
    };
    for (Node& n : g.nodes)
    {
        switch (n.type)
        {
        case NodeType::MakeStruct:
        case NodeType::BreakStruct:
        {
            HE::StructDef def;
            if (!n.typeName.empty() && reg.getStruct(n.typeName, def))
                n.params = fieldsToParams(def);
            break;   // missing def: keep the stored mirror (may load later)
        }
        case NodeType::GetStructField:
        case NodeType::SetStructField:
        {
            // Revalidate the chosen field (params[0]) against the current def:
            // retype a renamed-type field, keep the mirror if the def is gone,
            // drop the choice entirely if the FIELD is gone.
            HE::StructDef def;
            if (n.typeName.empty() || !reg.getStruct(n.typeName, def)) break;
            if (n.params.empty()) break;
            if (const HE::StructField* f = def.findField(n.params[0].name))
                n.params[0] = { f->name, f->type, f->isArray, f->typeName };
            else
                n.params.clear();
            break;
        }
        case NodeType::SwitchOnEnum:
        {
            HE::EnumDef def;
            if (n.typeName.empty() || !reg.getEnum(n.typeName, def)) break;
            std::vector<FuncParam> ps;
            ps.reserve(def.entries.size());
            for (const HE::EnumEntry& e : def.entries)
                ps.push_back({ e.name, PinType::Exec, false, {} });
            n.params = std::move(ps);
            break;
        }
        case NodeType::ConstEnum:
        {
            // Clamp a stale stored value onto a live entry so the dropdown and
            // the emitted literal never disagree.
            HE::EnumDef def;
            if (n.typeName.empty() || !reg.getEnum(n.typeName, def) || def.entries.empty()) break;
            if (!def.findValue((int)n.f[0])) n.f[0] = (float)def.entries.front().value;
            break;
        }
        default: break;
        }
    }
}

namespace
{
// The exec-forward closure from `start` plus every data node feeding those exec
// nodes — i.e. the body of the graph rooted at `start` (an Event or a
// FunctionEntry). Used to partition a flat graph into per-function sub-graphs.
std::unordered_set<int> traceBody(const Graph& g, int start)
{
    std::unordered_set<int> body; std::vector<int> stack;
    body.insert(start); stack.push_back(start);
    // Exec-forward.
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        const PinRanges r = pinRanges(*n);
        for (const auto& l : g.links)
            if (l.srcNode == id && l.srcPin >= r.execOut0 && l.srcPin < r.dataIn0)
                if (body.insert(l.dstNode).second) stack.push_back(l.dstNode);
    }
    // Data producers feeding any node already in the body.
    stack.assign(body.begin(), body.end());
    while (!stack.empty())
    {
        const int id = stack.back(); stack.pop_back();
        const Node* n = g.findNode(id); if (!n) continue;
        const PinRanges r = pinRanges(*n);
        for (const auto& l : g.links)
            if (l.dstNode == id && l.dstPin >= r.dataIn0 && l.dstPin < r.dataOut0)
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
        case P::Enum:   return a.i == b.i;
        default:        return false;
    }
}

// ── THE coercion rule. Two other places implement it and MUST match: ─────────
//   • HorizonCodeGenSupport.h `hc::coerce*` — the generated-C++ backend. It is a
//     DELIBERATE duplicate, not an oversight: generated code must produce the
//     byte-identical result the interpreter would, without linking the
//     interpreter. Change one → change the other, or a packaged build silently
//     diverges from what the editor previewed.
//   • UIWidgetBinding.cpp `uiHcValueToProp` — the widget-property bridge, which
//     coerces into UIPropValue instead of Value but follows the same rule.
// Only Float↔Int↔Bool convert (an Enum counts as its Int); any other mismatch
// yields the target's zero. Coercing INTO Enum/Struct never invents a typeName —
// wiring already type-checked the definition, so a same-type value passes
// through above and a mismatch degrades to a typed empty value.
Value coerce(Value v, PinType want)
{
    if (v.isArray) return v;   // arrays are never scalar-coerced (pass through whole)
    if (v.type == want) return v;
    Value r; r.type = want;
    switch (want)
    {
        case P::Float:  r.f = v.type == P::Bool ? (v.b ? 1.0f : 0.0f)
                            : v.type == P::Int ? (float)v.i
                            : v.type == P::Enum ? (float)v.i : 0.0f; break;
        case P::Int:    r.i = v.type == P::Float ? (int)v.f
                            : v.type == P::Bool ? (v.b ? 1 : 0)
                            : v.type == P::Enum ? v.i : 0; break;
        case P::Bool:   r.b = v.type == P::Float ? v.f != 0.0f
                            : v.type == P::Int ? v.i != 0 : false; break;
        case P::Enum:   r.i = v.type == P::Int ? v.i
                            : v.type == P::Float ? (int)v.f : 0; break;
        default: break;
    }
    return r;
}
} // namespace

Runner::Runner(const Graph& graph, Context ctx) : m_graph(graph), m_ctx(std::move(ctx)) {}

Runner::CallFrame* Runner::frameFor(int fnEntryId)
{
    for (auto it = m_callStack.rbegin(); it != m_callStack.rend(); ++it)
        if (it->fnEntryId == fnEntryId) return &*it;
    return nullptr;
}

namespace
{
// Seed a fresh frame's local-variable store from the function's declared
// locals (Variable::scope == the FunctionEntry's id) — per invocation, like
// Runtime::add seeds the instance store from the scope-0 variables.
void seedLocals(const Graph& g, int fnEntryId,
                std::unordered_map<std::string, Value>& locals)
{
    for (const auto& v : g.variables)
        if (v.scope == fnEntryId) locals[v.name] = variableDefaultValue(v);
}
} // namespace

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

void Runner::resumeFrom(int nodeId)
{
    const Node* n = m_graph.findNode(nodeId);
    if (!n) return;
    // A fresh run, exactly like a fire: the original run's event arg and
    // exec-output caches are gone (the chain re-reads live state instead).
    m_steps = 0;
    m_eventArg = Value{};
    m_execOutputs.clear();
    m_callStack.clear();
    runExecChain(*n, pinRanges(*n).execOut0, 0);
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
        // back to a typed default), typed result slots for any Return to fill, and
        // the function's locals at their declared defaults.
        CallFrame frame;
        frame.fnEntryId = n.id;
        seedLocals(m_graph, n.id, frame.locals);
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
            HE_LOG_WARN(HorizonCode, "%s",
                "HorizonCode: execution step limit hit — aborting run");
            return;
        }
        const Node* n = m_graph.findNode(l->dstNode);
        if (!n) return;
        execNode(*n, depth);
        if (n->type == T::Branch || n->type == T::Sequence || n->type == T::ForEach ||
            n->type == T::Delay || n->type == T::DoOnce || n->type == T::FlipFlop ||
            n->type == T::SwitchOnEnum)
            return; // they steer their own exec-outs internally (Delay: later)
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
        // A function-local writes the innermost frame of its owning function;
        // outside that function the write is dropped. Everything else goes to
        // the instance store (which also creates undeclared names, as before).
        if (const Variable* v = m_graph.findVariable(n.s); v && v->scope != 0)
        {
            if (CallFrame* f = frameFor(v->scope))
                f->locals[n.s] = coerce(evalInput(n, 0, depth + 1), n.propType);
        }
        else if (m_ctx.setVariable)
            m_ctx.setVariable(n.s, coerce(evalInput(n, 0, depth + 1), n.propType));
        break;
    case T::ShowSelf: if (m_ctx.showSelf) m_ctx.showSelf(); break;
    case T::HideSelf: if (m_ctx.hideSelf) m_ctx.hideSelf(); break;
    case T::CreateWidget:
    {
        // The widget id doubles as its runtime reference (widget id == scriptId),
        // so a created widget is a first-class Ref object.
        const int id = m_ctx.createWidget ? m_ctx.createWidget(n.s) : 0;
        m_execOutputs[n.id] = { Value::ofRef((uint32_t)id) }; // cached for the data output
        break;
    }
    case T::ShowWidget:  if (m_ctx.showWidget)    m_ctx.showWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::HideWidget:  if (m_ctx.hideWidget)    m_ctx.hideWidget((int)evalInput(n, 0, depth + 1).ref);    break;
    case T::DestroyWidget: if (m_ctx.destroyWidget) m_ctx.destroyWidget((int)evalInput(n, 0, depth + 1).ref); break;
    case T::CreateObject:
    {
        const uint32_t ref = m_ctx.createObject ? m_ctx.createObject(n.s) : 0u;
        if (ref == 0u)
            HE_LOG_ERROR(HorizonCode, "%s",
                ("HorizonCode: Create Object failed — class '" + n.s + "' not found").c_str());
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
        // pushing, so the caller's own params still resolve), seed typed results
        // and the callee's locals at their declared defaults.
        CallFrame frame;
        frame.fnEntryId = entry->id;
        seedLocals(m_graph, entry->id, frame.locals);
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
        HE_LOG_INFO(HorizonCode, "%s",
            ("[Widget] " + coerce(evalInput(n, 0, depth + 1), P::String).s).c_str());
        break;
    case T::Delay:
        // Latent: hand the continuation to the host scheduler and stop the
        // chain here — Runtime::update resumes from our exec-out later.
        if (m_ctx.scheduleResume)
            m_ctx.scheduleResume(n.id, evalInput(n, 0, depth + 1).f);
        break;
    case T::DoOnce:
        // Let the chain through only the first time (per instance; node state
        // persists across runs and resets with reseedVariables).
        if (m_ctx.getNodeState && m_ctx.getNodeState(n.id).b) break;
        if (m_ctx.setNodeState) m_ctx.setNodeState(n.id, Value::ofBool(true));
        runExecChain(n, pinRanges(n).execOut0, depth + 1);
        break;
    case T::FlipFlop:
    {
        // Alternate A/B, starting with A. The IsA data-out reports which side
        // THIS execution took (stored state = the side just taken).
        const bool wasA = m_ctx.getNodeState && m_ctx.getNodeState(n.id).b;
        const bool isA  = !wasA;
        if (m_ctx.setNodeState) m_ctx.setNodeState(n.id, Value::ofBool(isA));
        runExecChain(n, pinRanges(n).execOut0 + (isA ? 0 : 1), depth + 1);
        break;
    }
    case T::SwitchOnEnum:
    {
        // Match the value against the CURRENT definition, then route to the
        // exec-out whose mirrored entry name matches — renumber-safe. No match
        // (or missing def) → the trailing Default.
        const int val = evalInput(n, 0, depth + 1).i;
        const PinRanges r = pinRanges(n);
        int branch = (int)n.params.size();                 // Default
        HE::EnumDef def;
        if (HE::TypeRegistry::instance().getEnum(n.typeName, def))
            if (const HE::EnumEntry* e = def.findValue(val))
                for (size_t i = 0; i < n.params.size(); ++i)
                    if (n.params[i].name == e->name) { branch = (int)i; break; }
        runExecChain(n, r.execOut0 + branch, depth + 1);
        break;
    }
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
    case T::ConstEnum:
    {
        Value v; v.type = P::Enum; v.typeName = n.typeName; v.i = (int)n.f[0];
        return v;
    }
    case T::EnumToInt: return Value::ofInt(evalInput(n, 0, depth + 1).i);
    case T::IntToEnum:
    {
        Value v; v.type = P::Enum; v.typeName = n.typeName;
        v.i = evalInput(n, 0, depth + 1).i;
        return v;
    }
    case T::EnumToString:
    {
        const int val = evalInput(n, 0, depth + 1).i;
        HE::EnumDef def;
        if (HE::TypeRegistry::instance().getEnum(n.typeName, def))
            if (const HE::EnumEntry* e = def.findValue(val))
                return Value::ofString(e->name);
        return Value::ofString("");
    }
    case T::MakeStruct:
    {
        // Field values in the node's mirrored order — which syncTypeSignatures
        // keeps equal to DEFINITION order, the layout every consumer resolves
        // against. Unwired fields fall back to their declared defaults (the
        // whole-struct default is built first, then wired pins overwrite).
        Value v = HE::TypeRegistry::instance().makeDefaultValue(n.typeName);
        v.typeName = n.typeName;
        if (v.items.size() < n.params.size()) v.items.resize(n.params.size());
        const PinRanges r = pinRanges(n);
        for (size_t i = 0; i < n.params.size(); ++i)
        {
            // Only overwrite fields that are actually wired or carry an inline
            // default — an untouched pin keeps the definition's default.
            const int pin = r.dataIn0 + (int)i;
            bool wired = false;
            for (const auto& l : m_graph.links)
                if (l.dstNode == n.id && l.dstPin == pin) { wired = true; break; }
            if (wired || n.pinDefaults.count((int)i))
                v.items[i] = coerce(evalInput(n, (int)i, depth + 1), n.params[i].type);
        }
        return v;
    }
    case T::BreakStruct:
    {
        // Resolve the requested field BY NAME against the current definition —
        // the value's items are in def order, so a def edit mid-session can't
        // silently hand back the wrong field.
        if (dataOutPin < 0 || dataOutPin >= (int)n.params.size()) return {};
        const Value v = evalInput(n, 0, depth + 1);
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[dataOutPin].name)
                    return i < v.items.size()
                        ? coerce(v.items[i], n.params[dataOutPin].type)
                        : coerce(Value{}, n.params[dataOutPin].type);
        }
        // Def missing: trust the mirrored order (best effort).
        return dataOutPin < (int)v.items.size()
            ? coerce(v.items[dataOutPin], n.params[dataOutPin].type)
            : coerce(Value{}, n.params[dataOutPin].type);
    }
    case T::GetStructField:
    {
        // Resolve BY NAME against the current definition (like BreakStruct), so
        // a field inserted since this graph was authored can't shift the read.
        if (n.params.empty()) return {};
        const Value v = evalInput(n, 0, depth + 1);
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[0].name)
                    return i < v.items.size()
                        ? coerce(v.items[i], n.params[0].type)
                        : coerce(Value{}, n.params[0].type);
        }
        return coerce(Value{}, n.params[0].type);          // def missing → typed zero
    }
    case T::SetStructField:
    {
        Value v = evalInput(n, 0, depth + 1);              // a copy (pure)
        if (n.params.empty()) return v;
        if (v.type != P::Struct || v.typeName != n.typeName)
        {
            v = HE::TypeRegistry::instance().makeDefaultValue(n.typeName);
            v.typeName = n.typeName;
        }
        HE::StructDef def;
        if (HE::TypeRegistry::instance().getStruct(n.typeName, def))
        {
            for (size_t i = 0; i < def.fields.size(); ++i)
                if (def.fields[i].name == n.params[0].name)
                {
                    if (v.items.size() < def.fields.size()) v.items.resize(def.fields.size());
                    v.items[i] = coerce(evalInput(n, 1, depth + 1), n.params[0].type);
                    break;
                }
        }
        return v;
    }
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
        HE_LOG_WARN(HorizonCode, "%s",
            ("HorizonCode: Array Get index " + std::to_string(idx) + " out of range (size " +
             std::to_string(arr.items.size()) + ")").c_str());
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
        // Function-local: read the innermost frame of the owning function;
        // no frame (node ran outside its function) → the type's default.
        if (const Variable* var = m_graph.findVariable(n.s); var && var->scope != 0)
        {
            if (CallFrame* f = frameFor(var->scope))
                if (auto it = f->locals.find(n.s); it != f->locals.end())
                    return coerce(it->second, n.propType);
            return coerce(Value{}, n.propType);
        }
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
    case T::IsValid:
        return Value::ofBool(m_ctx.isValid && m_ctx.isValid(evalInput(n, 0, depth + 1).ref));
    case T::FlipFlop:
        // Which side the last execution took (A = true); false before any run.
        return Value::ofBool(m_ctx.getNodeState ? m_ctx.getNodeState(n.id).b : false);
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
